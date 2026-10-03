"""perfreport: the frame rate of one harness run, cut to the gameplay the pilot measured.

    .venv\\Scripts\\python perfreport.py run <run folder> [--no-compare] [--print]

run-test.ps1 runs it after every run that got the game started (unless -NoPerf): it writes <run folder>\\perf.json
(every segment and group) and perf-summary.json, whose "summary" becomes result.json's "perf" and whose "line" goes
into summary.txt. README.md ("Frame rate") has the fields. Every input is optional; whatever is missing leaves its
part null:

  pilot-status.json   gamepilot: the measured segments (QPC start/end of the play / play_until / "measure" steps), a
                      QPC/Unix clock pair, the displays and the game window's; pilot\\captures.csv: its screen captures
                      inside segments
  perf-frames.csv     UEVR (VR_PerfLog, src\\mods\\vr\\PerfLog.cpp): one row per xrEndFrame, P = a new engine frame
                      (newer than every one submitted before), R = one already submitted (the newest again, or an
                      older one: 2x frame generation's second Present resubmits the previous engine frame), E = empty,
                      F = failed. Files written before UEVR classified that way are reclassified from the frame column
  perf.csv            UEVR: one row per second, also while no frame arrives, with QPC/XrTime pairs
  monado-frames.csv   monado-service (comp_window_mopic.c, the run's rows of app_frame_stats.csv): one row per second,
                      the compositor's presents and how many of them showed a new frame of the game
  presentmon*.csv     PresentMon console app (--qpc_time --multi_csv): the game's own Presents, and monado-service's
  gpu.csv             nvidia-smi at 1 Hz
  game-settings\\      the game's GameUserSettings.ini before the run (and GameUserSettings.after.ini after it)
  perf-context.json   run-test.ps1: pids, clock pairs, end of the observation, power, PresentMon / nvidia-smi status,
                      the frame generation modules loaded in the game
  log.txt, config.txt UEVR's log (XR focus time) and the run's config (render settings fingerprint)

Times are QueryPerformanceCounter time in nanoseconds ("QPC ns": Python's time.perf_counter_ns(), MSVC's steady_clock,
monado's os_monotonic_get_ns()); PresentMon's raw QPC ticks, UEVR's XrTime display times and nvidia-smi's wall-clock
times are converted.

Runs without a recipe have no segments: "observe" is measured from XR focus (without UEVR: the game's start) + 30 s to
the end of the observation window, and is never used as a baseline.
"""

import argparse
import bisect
import csv
import datetime
import glob
import hashlib
import json
import math
import os
import re
import statistics
import sys

VERSION = 1

SETTLE_HOLD_S = 2.0      # a segment starts once the runtime paces at <= 2 refreshes for this long...
SETTLE_MAX_S = 10.0      # ...trimming at most this much (monado's period estimate collapses after stalls)
SETTLE_FIXED_S = 5.0     # without UEVR's predicted period: this much, at most a quarter of the segment
EXCLUDE_RUN_S = 3        # loading-like seconds (no tick, no new frame, mostly empty submits, no submit at all) in runs
                         # this long are left out; shorter ones stay in, as the stalls they are
OBSERVE_AFTER_S = 30.0   # runs without a recipe: measured from XR focus (or the game's start) + this
HITCH_MIN_MS = 50.0      # hitch: an interval >= max(this, 2x the median)
STALL_MS = 250.0         # stall: an interval >= this (UEVR's tick-gap warning threshold)
LOW01_MIN = 10000        # 0.1% lows (p99.9) only with this many intervals
LATE_MARGIN_MS = 4.0     # monado shows a frame 4 ms after its present (u_pacing_compositor_fake.c)
FG_RATIO = 1.7           # presents per simulated frame (PresentMon) or per engine tick (UEVR) at or above this = frame
                         # generation
OLDER_FRAME_WINDOW = 120 # PerfLog.hpp OLDER_FRAME_WINDOW: further back than this, the engine frame counter restarted
GPU_BOUND_UTIL = 90.0

# UEVR's files (PerfLog.cpp FRAMES_HEADER / SUMMARY_HEADER): the columns read here, which selftest_perf.py checks
# against PerfLog.cpp
FRAMES_COLUMNS = ("frame", "display_time", "period_ns", "end_us", "wait_us", "callsite", "nsf", "nsf_pair", "flags",
                  "presents", "dup_presents", "present_us", "ticks", "ticks_skipped", "tick_us", "uevr_gt_us",
                  "uevr_rt_us", "rt_blocked_us")
ONEHZ_COLUMNS = ("qpc_ns", "unix_ms", "window_ms", "xr_new", "xr_repeat", "xr_empty", "xr_fail", "wait_ok",
                 "engine_delta_ms", "frames_dropped", "frame_rows_unwritten", "xr_pair_qpc_ns", "xr_pair_time")
# monado's app_frame_stats.csv (comp_window_mopic.c MOPIC_AFS_CSV_HEADER)
MONADO_COLUMNS = ("qpc_ns", "unix_ms", "window_ms", "pid", "exe", "mode", "presents", "new", "repeated", "no_layer",
                  "dropped", "gap_max_ms")
# perf-frames.csv / perf.csv "flags" (PerfLog.hpp, namespace flag): bit numbers; sync stage and rendering method above
FLAG_BITS = {"afr": 0, "synced": 1, "skip_draw": 2, "nsf": 3, "nsf_active": 4, "nsf_array": 5, "async_wait": 6,
             "d3d12": 7, "framegen_sc": 8, "dlssg": 9, "menu": 10, "hmd": 11, "focused": 12, "foreground": 13, "mono": 14}
SYNC_STAGE_SHIFT, METHOD_SHIFT = 16, 20                              # flag::SYNC_STAGE_SHIFT, flag::METHOD_SHIFT
SYNC_STAGES = ("early", "late", "very_late")                         # (flags >> 16) & 3, VR::SynchronizeStage
# (flags >> 20) & 0xF, VR::RenderingMethod (5 = uevr::mono::method_id)
METHODS = ("native_stereo", "synchronized", "alternating", "synthetic_dibr", "synthetic_dibr_single_view", "mono")
# the "callsite" column: OpenXR.cpp's sync_frame_callsite_name(), or a VRRuntime::SyncFrameCallsite number
CALLSITES = ("unknown", "runtime_fix_frame", "vr_late_on_present", "vr_early_rhi_command", "vr_d3d11_initial_sync",
             "vr_post_present_initial_sync", "vr_very_late_post_present", "openxr_session_ready",
             "openxr_begin_frame_recovery")
# per VR frame sums of perf-frames.csv columns; uevr_rt is UEVR's own render-thread time, without rt_blocked (its time
# blocked in OpenXR calls and D3D12 fence waits)
ATTRIB = (("tick_ms", "tick_us"), ("present_ms", "present_us"), ("wait_ms", "wait_us"), ("uevr_gt_ms", "uevr_gt_us"),
          ("uevr_rt_ms", "uevr_rt_us"), ("rt_blocked_ms", "rt_blocked_us"))
# UEVR's log.txt: the session reaching FOCUSED ("VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5 (FOCUSED)",
# "[OpenXR] Session transitioned to FOCUSED")
FOCUS_LOG = re.compile(r"SESSION_STATE_CHANGED \d+ \(FOCUSED\)|Session transitioned to FOCUSED|XR_SESSION_STATE_FOCUSED")
# nvidia-smi clocks_event_reasons.active bits
CLOCK_REASONS = {0x1: "idle", 0x2: "app_clocks", 0x4: "sw_power_cap", 0x8: "hw_slowdown", 0x10: "sync_boost",
                 0x20: "sw_thermal", 0x40: "hw_thermal", 0x80: "hw_power_brake", 0x100: "display_clocks"}
# Windows 11 power mode overlays (HKLM\...\Power\User\PowerSchemes ActiveOverlay*PowerScheme; none = balanced)
POWER_OVERLAYS = {"961cc777-2547-4f9d-8174-7d86181b8a7a": "best_efficiency",
                  "ded574b5-45a0-4f42-8737-46345c09c238": "best_performance",
                  "00000000-0000-0000-0000-000000000000": "balanced"}
# GameUserSettings.ini: keys UE rewrites on every exit (left out of the fingerprint) / keys shown in the context
SETTINGS_VOLATILE = re.compile(r"^(Last\w*|Version|ChangeListVersion|\w*Benchmark\w*|\w*PlayTime\w*|\w*TimeStamp\w*)$", re.I)
SETTINGS_KEYS = re.compile(r"DLSS|FrameGen|Reflex|Upscal|Streamline|FSR|XeSS|VSync|FrameRate|Resolution|FullscreenMode|"
                           r"LatencyMode|Monitor|InsertFrame|ScreenPercentage|DynamicResolution|LockFrame|UISettingData|"
                           r"^sg\.", re.I)
SETTINGS_FRAMEGEN = re.compile(r"FrameGen|InsertFrame", re.I)
FG_LOG = re.compile(r"Found Streamline \(DLSSFG\) swapchain")


# ---------------------------------------------------------------------------------------------------------------
# small helpers

def num(s):
    """'12' -> 12, '1.5' -> 1.5, '0x1f' -> 31, '' / 'NA' / '[N/A]' -> None."""
    if s is None:
        return None
    s = s.strip()
    if not s:
        return None
    try:
        return int(s)
    except ValueError:
        pass
    try:
        if s[:2].lower() == "0x":
            return int(s, 16)
        v = float(s)
        return v if math.isfinite(v) else None
    except ValueError:
        return None


def pct(values, q):
    """Percentile q (0-100) of sorted values, interpolated like numpy's default; None when empty."""
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    k = (len(values) - 1) * q / 100.0
    lo, hi = math.floor(k), math.ceil(k)
    return values[lo] + (values[hi] - values[lo]) * (k - lo)


def median(values):
    return statistics.median(values) if values else None


def rnd(x, nd=1):
    return None if x is None else round(x, nd)


def top(counts):
    """The most frequent key of a {value: count} dict, None when empty."""
    return max(counts, key=counts.get) if counts else None


def read_json(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


class Table:
    """A CSV with a header row; "#" comment lines before it are kept in `comments`, not taken for the header. A torn
    last line (the writer was killed in the middle of it) and rows with the wrong number of fields are dropped."""

    def __init__(self, path):
        self.path = path
        self.header = []
        self.comments = []
        self.rows = []
        with open(path, encoding="utf-8-sig", errors="replace", newline="") as f:
            text = f.read()
        lines = text.split("\n")
        if lines and not text.endswith("\n"):
            lines = lines[:-1]
        body = []
        for line in lines:
            line = line.rstrip("\r")
            if not self.header:
                if line.startswith("#"):
                    self.comments.append(line[1:].strip())
                elif line.strip():
                    self.header = [h.strip() for h in next(csv.reader([line]))]
                continue
            if line.strip():
                body.append(line)
        width = len(self.header)
        for row in csv.reader(body):
            if len(row) == width:
                self.rows.append(row)
        self.index = {h: i for i, h in enumerate(self.header)}

    def has(self, name):
        return name in self.index

    def col(self, name, conv=num):
        """A column as a list (conv applied), or None when the file has no such column."""
        i = self.index.get(name)
        if i is None:
            return None
        return [conv(r[i]) for r in self.rows]

    def find(self, *patterns):
        """The first header matching one of the regexes (nvidia-smi adds units: 'power.draw [W]')."""
        for p in patterns:
            for h in self.header:
                if re.fullmatch(p, h, re.I):
                    return h
        return None


def load_table(path, warnings):
    if not path or not os.path.exists(path):
        return None
    try:
        t = Table(path)
    except OSError as e:
        warnings.append(f"could not read {os.path.basename(path)}: {e}")
        return None
    return t if t.header else None


class Clock:
    """QPC ns <-> Unix ns, from pairs read at the same moment (the pilot's start, run-test.ps1's start / ends, UEVR's
    and monado's 1 Hz rows); a conversion uses the nearest pair (the two clocks drift apart slowly)."""

    def __init__(self):
        self.pairs = []

    def add(self, qpc, unix_ns):
        if qpc and unix_ns:
            self.pairs.append((int(qpc), int(unix_ns)))

    def add_table(self, t, every=20):
        """About `every` pairs spread over a 1 Hz CSV with qpc_ns and unix_ms columns."""
        if t is None:
            return
        q, u = t.col("qpc_ns"), t.col("unix_ms")
        if q and u:
            for k in range(0, len(q), max(1, len(q) // every)):
                if q[k] and u[k]:
                    self.add(q[k], u[k] * 1_000_000)

    def to_qpc(self, unix_ns):
        if not self.pairs:
            return None
        q, u = min(self.pairs, key=lambda p: abs(p[1] - unix_ns))
        return q + (unix_ns - u)


def local_to_unix_ns(text, fmt):
    """A local wall-clock time string -> Unix ns."""
    try:
        return int(datetime.datetime.strptime(text.strip(), fmt).timestamp() * 1e9)
    except ValueError:
        return None


# ---------------------------------------------------------------------------------------------------------------
# inputs

def callsite_name(s):
    """A "callsite" field: sync_frame_callsite_name()'s text, or a SyncFrameCallsite number -> its name; None empty."""
    s = (s or "").strip()
    if not s:
        return None
    if s.isdigit():
        return CALLSITES[int(s)] if int(s) < len(CALLSITES) else f"callsite {s}"
    return s


def frame_order(frame, newest):
    """PerfLog.hpp order_frame(): `frame` against the newest engine frame submitted so far (0 = none yet), wrap-safe
    on 32 bits. -> "unknown" (frame 0), "newer", "same", "older" or "restart" (far behind: the counter restarted)."""
    if not frame:
        return "unknown"
    if not newest:
        return "newer"
    ahead = (frame - newest) & 0xFFFFFFFF
    if ahead >= 0x80000000:
        ahead -= 1 << 32
    if ahead > 0:
        return "newer"
    if ahead == 0:
        return "same"
    return "older" if -ahead <= OLDER_FRAME_WINDOW else "restart"


class Frames:
    """UEVR's perf-frames.csv as columns (FRAMES_COLUMNS), sorted by time. Counts in a row (presents, ticks, *_us) are
    since the previous row of any kind. Projection submits are classified the way PerfLog.hpp does: P only for an
    engine frame newer than every one submitted before (`reclassified` counts the P rows of older files that this
    turned into R), and `older` marks the R rows that carried an older engine frame than the newest."""

    def __init__(self, t):
        q = t.col("qpc_ns")
        order = sorted((i for i in range(len(q)) if q[i] is not None), key=lambda i: q[i])
        pick = (lambda col: None if col is None else [col[i] for i in order])
        self.qpc = [q[i] for i in order]
        self.kind = pick(t.col("kind", lambda s: s.strip().upper()[:1])) or ["P"] * len(self.qpc)
        for name in FRAMES_COLUMNS:
            setattr(self, name, pick(t.col(name, callsite_name if name == "callsite" else num)))
        self.n = len(self.qpc)
        self.older = [False] * self.n
        self.reclassified = 0
        if self.frame is not None:
            newest = 0
            for i in range(self.n):
                kind, frame = self.kind[i], self.frame[i]
                if kind not in ("P", "R") or frame is None:
                    continue
                order = frame_order(int(frame), newest)
                if order in ("newer", "restart"):
                    newest = int(frame)
                elif order != "unknown":
                    self.older[i] = order == "older"
                    if kind == "P":
                        self.kind[i] = "R"
                        self.reclassified += 1
        self.any_flags = 0          # every flag bit seen in the file (frame generation is a property of the run)
        for f in self.flags or []:
            self.any_flags |= f or 0
        # a game whose Tick hook never counted anything: its ticks say nothing about loading screens
        self.ticks_seen = self.ticks is not None and any(self.ticks)

    def flag(self, k, name):
        f = self.flags[k] if self.flags is not None else None
        return None if f is None else bool(f & (1 << FLAG_BITS[name]))

    def has_flags(self):
        return self.flags is not None and any(f is not None for f in self.flags)


class Display:
    """monado-service's 1 Hz rows (monado-frames.csv) of the game's session: one of the game's pids or its exe name
    (monado writes it in lower case); every row when neither is known."""

    def __init__(self, t, pids, process):
        q, w, pid = t.col("qpc_ns"), t.col("window_ms"), t.col("pid")
        exe = t.col("exe", lambda s: s.strip().lower())
        want_exe = process.lower() + ".exe" if process else None
        by_pid, by_exe = bool(pids) and pid is not None, bool(want_exe) and exe is not None
        self.rows = []
        cols = {n: t.col(n) for n in ("presents", "new", "repeated", "no_layer", "dropped", "gap_max_ms")}
        mode = t.col("mode", lambda s: s.strip())
        for k in range(len(t.rows)):
            if q[k] is None:
                continue
            if (by_pid or by_exe) and not ((by_pid and pid[k] in pids) or (by_exe and exe[k] == want_exe)):
                continue
            row = {n: (c[k] if c is not None else None) for n, c in cols.items()}
            row.update(qpc=q[k], window_ms=(w[k] if w is not None and w[k] else 1000.0), mode=mode[k] if mode else None)
            self.rows.append(row)


class PresentMon:
    """PresentMon CSVs of the run: the game's main swap chain and monado-service's."""

    def __init__(self, paths, process, pids, freq, warnings):
        self.game = None        # (qpc ns list, row list, column index) of the game's main swap chain
        self.compositor = None
        self.rows_game = 0
        self.swapchains = 0
        self.no_qpc = False
        exe = (process or "").lower() + ".exe"
        groups = {}
        for path in paths:
            t = load_table(path, warnings)
            if t is None:
                continue
            if t.find("CPUStartQPC", "TimeInQPC") is None:
                self.no_qpc = True
                continue
            app, pid, sc = t.index.get("Application"), t.index.get("ProcessID"), t.index.get("SwapChainAddress")
            for r in t.rows:
                name = r[app].strip().lower() if app is not None else ""
                p = num(r[pid]) if pid is not None else None
                if name == exe or (p is not None and p in pids):
                    who = "game"
                elif name == "monado-service.exe":
                    who = "compositor"
                else:
                    continue
                key = (who, p, r[sc].strip() if sc is not None else "")
                groups.setdefault(key, (t, []))[1].append(r)
        best = {}
        for (who, _, _), (t, rows) in groups.items():
            if who == "game":
                self.swapchains += 1
            if who not in best or len(rows) > len(best[who][1]):
                best[who] = (t, rows)   # the swap chain with the most frames (UEVR's dummy one has few)
        for who, (t, rows) in best.items():
            qi = t.index[t.find("CPUStartQPC", "TimeInQPC")]
            data = sorted(((q * 1_000_000_000 // freq, r) for r in rows if (q := num(r[qi])) is not None),
                          key=lambda d: d[0])
            entry = ([d[0] for d in data], [d[1] for d in data], dict(t.index))
            if who == "game":
                self.game = entry
                self.rows_game = len(data)
            else:
                self.compositor = entry


def pm_value(entry, k, name):
    i = entry[2].get(name)
    return None if i is None else num(entry[1][k][i])


def pm_text(entry, k, name):
    i = entry[2].get(name)
    return None if i is None else entry[1][k][i].strip()


def load_gpu(t, clock):
    """nvidia-smi --format=csv,nounits rows -> [{qpc, util, clock, ...}] (its timestamps are local wall clock)."""
    if t is None:
        return None
    cols = {key: t.find(*patterns) for key, patterns in {
        "time": [r"timestamp"], "util": [r"utilization\.gpu.*"], "clock": [r"clocks\.(current\.graphics|gr).*"],
        "mem_clock": [r"clocks\.(current\.memory|mem).*"], "temp": [r"temperature\.gpu.*"],
        "power": [r"power\.draw.*"], "limit": [r"enforced\.power\.limit.*"], "pstate": [r"pstate"],
        "reasons": [r"clocks_(event|throttle)_reasons\.active"]}.items()}
    if cols["time"] is None:
        return None
    out = []
    for r in t.rows:
        stamp = r[t.index[cols["time"]]].strip()
        if stamp.lower() == "timestamp":
            continue  # a header line repeated by a restarted sampler
        unix = local_to_unix_ns(stamp, "%Y/%m/%d %H:%M:%S.%f")
        q = clock.to_qpc(unix) if unix else None
        if q is None:
            continue
        row = {"qpc": q}
        for key, h in cols.items():
            if key == "time" or h is None:
                continue
            v = r[t.index[h]].strip()
            row[key] = v if key == "pstate" else num(v)
        out.append(row)
    return out


def segment_captures(run_dir, index):
    path = os.path.join(run_dir, "pilot", "captures.csv")
    if not os.path.exists(path):
        return []
    t = Table(path)
    seg, s, e = t.col("segment"), t.col("start_qpc_ns"), t.col("end_qpc_ns")
    if seg is None or s is None or e is None:
        return []
    return [(s[k], e[k]) for k in range(len(seg)) if seg[k] == index and s[k] is not None and e[k] is not None]


# ---------------------------------------------------------------------------------------------------------------
# accumulators: one per segment, merged per label; finish_*() turn them into the reported numbers

class Acc:
    def __init__(self):
        self.segments = 0
        self.window_s = 0.0
        self.settle_s = 0.0
        self.settle_capped = 0
        self.measured_s = 0.0
        self.excluded = {"loading": 0.0, "no_data": 0.0, "menu": 0.0, "unfocused": 0.0}
        # UEVR perf-frames.csv, the measured seconds
        self.rows = 0
        self.kinds = {"P": 0, "R": 0, "E": 0, "F": 0}
        self.nsf = {}                  # nsf value -> new frames
        self.ticks = 0
        self.ticks_skipped = 0
        self.presents = 0
        self.dup_presents = 0
        self.has_ticks = False
        self.has_presents = False
        self.older = 0                 # R submits of an engine frame older than the newest one submitted
        self.nsf_pair = {}             # nsf_pair value (1 live targets, 2 frozen pair) -> projection submits
        self.intervals = []            # (ms, start qpc, end qpc) between measured new frames
        self.slots = []                # predicted display time steps between them (ms)
        self.work = []
        self.attrib = {k: [] for k, _ in ATTRIB}
        self.paced = {"1": 0.0, "2": 0.0, "3+": 0.0}
        self.period_max = None
        self.late = 0
        self.late_n = 0
        self.flag_rows = {}            # flag name -> measured rows with it set
        self.flag_known = 0            # measured rows with a flags value
        self.sync = {}                 # sync stage -> rows
        self.method = {}               # rendering method -> rows
        self.callsite = {}             # xrWaitFrame callsite -> new frames
        # UEVR perf.csv
        self.onehz_rows = 0
        self.wait_ok = 0
        self.xr_new = 0
        self.ring_dropped = 0
        self.no_submit_s = 0.0
        self.delta_ms = 0.0
        self.delta_window_ms = 0.0
        # pilot
        self.captures = 0
        self.capture_s = 0.0
        self.cpu_s = 0.0
        self.spans = []
        # PresentMon
        self.flat = {"between": [], "sim": [], "gpu_busy": [], "display": [], "in_present": [], "rows": 0,
                     "sync": {}, "mode": {}, "runtime": {}, "type": {}}
        self.comp = {"between": [], "display": [], "rows": 0, "mode": {}}
        # monado
        self.disp = {"rows": 0, "s": 0.0, "presents": 0, "new": 0, "repeated": 0, "no_layer": 0, "dropped": 0,
                     "dropped_known": False, "gap_max": [], "mode": {}}
        # nvidia-smi
        self.gpu = {"util": [], "clock": [], "mem_clock": [], "temp": [], "power": [], "limit": [], "pstate": {},
                    "reasons": 0}

    def merge(self, o):
        for k, v in o.__dict__.items():
            setattr(self, k, merged(k, getattr(self, k), v))
        return self


def merged(key, a, b):
    if key == "period_max":
        return b if a is None else a if b is None else max(a, b)
    if key == "reasons":
        return a | b
    if isinstance(a, bool):
        return a or b
    if isinstance(a, (int, float)):
        return a + b
    if isinstance(a, list):
        return a + b
    if isinstance(a, dict):
        out = dict(a)
        for k, v in b.items():
            out[k] = merged(k, out[k], v) if k in out else v
        return out
    return a


# ---------------------------------------------------------------------------------------------------------------
# one segment

def settle_end(fr, a, b, refresh_ns):
    """Where the segment's start is trimmed to: the first moment from which the runtime paced at <= 2 refreshes for
    SETTLE_HOLD_S (monado's period estimate collapses to 6-25 refreshes after a stall, and stays there for seconds).
    -> (time, capped: it never settled within SETTLE_MAX_S)."""
    limit = a + SETTLE_MAX_S * 1e9
    if fr is None or fr.period_ns is None:
        return a + min(SETTLE_FIXED_S, 0.25 * (b - a) / 1e9) * 1e9, False
    i = bisect.bisect_right(fr.qpc, a)
    run_start = None
    while i < fr.n and fr.qpc[i] <= b:
        q, period = fr.qpc[i], fr.period_ns[i]
        if fr.kind[i] in ("P", "R") and period and period > 2.5 * refresh_ns:
            run_start = None
        elif run_start is None:
            if q > limit:
                break
            run_start = q
        elif q - run_start >= SETTLE_HOLD_S * 1e9:
            return max(a, run_start), False
        i += 1
    return min(limit, b), True


def classify_bins(fr, a, b):
    """1 s bins from a to b: "ok", or why the second is left out: menu / unfocused (UEVR's state flags in most rows),
    loading (no engine tick, no new frame, or more empty submits than new frames) and no_data (no xrEndFrame at all),
    the last two only for EXCLUDE_RUN_S seconds or more in a row: shorter ones stay in, as the stalls they are. An
    interval across a left-out second is never counted."""
    n = max(1, math.ceil((b - a) / 1e9 - 1e-9))
    stats = [[0, 0, 0, 0, False, 0, 0] for _ in range(n)]  # rows, P, E, ticks, ticks known, menu, unfocused
    uevr = fr is not None and fr.n > 0
    if uevr:
        i = bisect.bisect_right(fr.qpc, a)
        while i < fr.n and fr.qpc[i] <= b:
            s = stats[min(n - 1, int((fr.qpc[i] - a) // 1_000_000_000))]
            s[0] += 1
            kind = fr.kind[i]
            s[1] += kind == "P"
            s[2] += kind == "E"
            if fr.ticks_seen and fr.ticks[i] is not None:
                s[3] += fr.ticks[i]
                s[4] = True
            if fr.flag(i, "menu"):
                s[5] += 1
            elif fr.flag(i, "focused") is False or fr.flag(i, "hmd") is False:
                s[6] += 1
            i += 1
    cls = []
    for rows, p, e, ticks, ticks_known, menu, unfocused in stats:
        if rows == 0:
            cls.append("no_data?" if uevr else "ok")
        elif menu * 2 > rows:
            cls.append("menu?")
        elif unfocused * 2 > rows:
            cls.append("unfocused?")
        elif (ticks_known and ticks == 0) or p == 0 or e > p:
            cls.append("loading?")
        else:
            cls.append("ok")
    k = 0
    while k < n:
        if not cls[k].endswith("?"):
            k += 1
            continue
        j = k
        while j < n and cls[j] == cls[k]:
            j += 1
        keep_out = j - k >= EXCLUDE_RUN_S or cls[k] in ("menu?", "unfocused?")
        for m in range(k, j):
            cls[m] = cls[m][:-1] if keep_out else "ok"
        k = j
    return cls


def bin_seconds(a, b, k, n):
    """Length of bin k of n (the last one is partial)."""
    return min(1.0, (b - a) / 1e9 - k) if k == n - 1 else 1.0


def bin_of(q, a, n):
    return min(n - 1, max(0, int((q - a) // 1_000_000_000)))


def acc_uevr(acc, fr, a, b, cls, refresh_ns, xr_offset):
    n = len(cls)
    excl_prefix = [0]
    for c in cls:
        excl_prefix.append(excl_prefix[-1] + (c != "ok"))
    i = bisect.bisect_right(fr.qpc, a)
    last_p = None                  # the previous measured new frame
    prev = None                    # the previous measured row (time slices for the pacing split)
    sums = dict.fromkeys((col for _, col in ATTRIB), 0)
    while i < fr.n and fr.qpc[i] <= b:
        q = fr.qpc[i]
        bi = bin_of(q, a, n)
        if cls[bi] != "ok":
            last_p, prev = None, None
            i += 1
            continue
        if prev is not None and excl_prefix[bin_of(fr.qpc[prev], a, n) + 1] != excl_prefix[bi + 1]:
            last_p, prev = None, None   # a left-out second without rows lies between
        acc.rows += 1
        kind = fr.kind[i]
        acc.kinds[kind] = acc.kinds.get(kind, 0) + 1
        acc.older += fr.older[i]
        pair = fr.nsf_pair[i] if fr.nsf_pair is not None and kind in ("P", "R") else None
        if pair:
            acc.nsf_pair[pair] = acc.nsf_pair.get(pair, 0) + 1
        for col, key in ((fr.ticks, "ticks"), (fr.ticks_skipped, "ticks_skipped"), (fr.presents, "presents"),
                         (fr.dup_presents, "dup_presents")):
            if col is not None and col[i] is not None:
                setattr(acc, key, getattr(acc, key) + col[i])
        acc.has_ticks |= fr.ticks_seen
        acc.has_presents |= fr.presents is not None
        f = fr.flags[i] if fr.flags is not None else None
        if f is not None:
            acc.flag_known += 1
            for name, bit in FLAG_BITS.items():
                if f & (1 << bit):
                    acc.flag_rows[name] = acc.flag_rows.get(name, 0) + 1
            stage = (f >> SYNC_STAGE_SHIFT) & 0x3
            stage = SYNC_STAGES[stage] if stage < len(SYNC_STAGES) else f"stage {stage}"
            acc.sync[stage] = acc.sync.get(stage, 0) + 1
            method = (f >> METHOD_SHIFT) & 0xF
            method = METHODS[method] if method < len(METHODS) else f"method {method}"
            acc.method[method] = acc.method.get(method, 0) + 1
        period = fr.period_ns[i] if fr.period_ns is not None else None
        if period and kind in ("P", "R"):
            acc.period_max = max(acc.period_max or 0, period)
            if prev is not None:
                steps = max(1, round(period / refresh_ns))
                acc.paced["1" if steps <= 1 else "2" if steps == 2 else "3+"] += (q - fr.qpc[prev]) / 1e9
        for key in sums:
            col = getattr(fr, key)
            if col is not None and col[i] is not None:
                sums[key] += col[i]
        if kind == "P":
            nsf = fr.nsf[i] if fr.nsf is not None else None
            if nsf is not None:
                acc.nsf[nsf] = acc.nsf.get(nsf, 0) + 1
            site = fr.callsite[i] if fr.callsite is not None else None
            if site is not None:
                acc.callsite[site] = acc.callsite.get(site, 0) + 1
            if last_p is not None:
                interval = q - fr.qpc[last_p]
                acc.intervals.append((interval / 1e6, fr.qpc[last_p], q))
                if fr.display_time is not None and fr.display_time[i] and fr.display_time[last_p]:
                    acc.slots.append((fr.display_time[i] - fr.display_time[last_p]) / 1e6)
                if fr.wait_us is not None:
                    acc.work.append((interval - sums["wait_us"] * 1000) / 1e6)
                for key, col in ATTRIB:
                    if getattr(fr, col) is not None:
                        acc.attrib[key].append(sums[col] / 1000)
            if xr_offset is not None and fr.display_time is not None and fr.display_time[i]:
                acc.late_n += 1
                acc.late += q > fr.display_time[i] + xr_offset - LATE_MARGIN_MS * 1e6
            last_p = i
            sums = dict.fromkeys(sums, 0)
        prev = i
        i += 1


def onehz_rows(q, w, a, b):
    """Indexes of 1 s rows (each covers (q - window, q]) whose middle lies in (a, b], with that middle."""
    for k, qk in enumerate(q):
        if qk is None:
            continue
        mid = qk - (w[k] if w is not None and w[k] else 1000.0) * 1e6 / 2
        if a < mid <= b:
            yield k, mid


def acc_onehz(acc, t, a, b, cls):
    """UEVR's perf.csv rows: seconds without any xrEndFrame (over the whole segment, left-out seconds too), waits per
    new frame, frame records lost (the ring full or contended, or perf-frames.csv at its size cap), engine time
    against wall time."""
    if t is None:
        return
    q, w = t.col("qpc_ns"), t.col("window_ms")
    cols = {n: t.col(n) for n in ("xr_new", "xr_repeat", "xr_empty", "xr_fail", "wait_ok", "frames_dropped",
                                  "frame_rows_unwritten", "engine_delta_ms")}
    get = (lambda name, k: cols[name][k] if cols[name] is not None else None)
    n = len(cls)
    for k, mid in onehz_rows(q, w, a, b):
        submits = sum(get(name, k) or 0 for name in ("xr_new", "xr_repeat", "xr_empty", "xr_fail"))
        window_ms = w[k] if w is not None and w[k] else 1000.0
        if submits == 0:
            acc.no_submit_s += window_ms / 1000
        acc.ring_dropped += (get("frames_dropped", k) or 0) + (get("frame_rows_unwritten", k) or 0)
        if cls[bin_of(mid, a, n)] != "ok":
            continue
        acc.onehz_rows += 1
        acc.wait_ok += get("wait_ok", k) or 0
        acc.xr_new += get("xr_new", k) or 0
        delta = get("engine_delta_ms", k)
        if delta is not None:
            acc.delta_ms += delta
            acc.delta_window_ms += window_ms


def acc_flat(acc, pm, a, b, included):
    """The game's own Presents (PresentMon) between a and b, and monado-service's."""
    for which, entry in (("game", pm.game), ("comp", pm.compositor)):
        if entry is None:
            continue
        qs = entry[0]
        for k in range(bisect.bisect_right(qs, a), bisect.bisect_right(qs, b)):
            if not included(qs[k]):
                continue
            between = pm_value(entry, k, "MsBetweenPresents")
            display = pm_value(entry, k, "MsBetweenDisplayChange")
            mode = pm_text(entry, k, "PresentMode")
            if which == "comp":
                acc.comp["rows"] += 1
                if between:
                    acc.comp["between"].append(between)
                if display:
                    acc.comp["display"].append(display)
                if mode:
                    acc.comp["mode"][mode] = acc.comp["mode"].get(mode, 0) + 1
                continue
            acc.flat["rows"] += 1
            if between:
                acc.flat["between"].append(between)
            for key, col in (("sim", "MsBetweenSimulationStart"), ("gpu_busy", "MsGPUBusy"), ("in_present", "MsInPresentAPI")):
                v = pm_value(entry, k, col)
                if v is not None and v > 0:
                    acc.flat[key].append(v)
            if display:
                acc.flat["display"].append(display)
            for key, col in (("sync", "SyncInterval"), ("mode", "PresentMode"), ("runtime", "PresentRuntime"),
                             ("type", "FrameType")):
                v = pm_text(entry, k, col)
                if v:
                    acc.flat[key][v] = acc.flat[key].get(v, 0) + 1


def acc_display(acc, disp, a, b, cls):
    """monado-service's 1 Hz rows whose middle lies in a measured second of (a, b]."""
    if disp is None:
        return
    n = len(cls)
    for row in disp.rows:
        mid = row["qpc"] - row["window_ms"] * 1e6 / 2
        if not a < mid <= b or cls[bin_of(mid, a, n)] != "ok":
            continue
        d = acc.disp
        d["rows"] += 1
        d["s"] += row["window_ms"] / 1000
        for key in ("presents", "new", "repeated", "no_layer", "dropped"):
            d[key] += row[key] or 0
        # monado leaves "dropped" empty: it would need a counter in the upstream multi compositor
        d["dropped_known"] = d["dropped_known"] or row["dropped"] is not None
        if row["gap_max_ms"] is not None:
            d["gap_max"].append(row["gap_max_ms"])
        if row["mode"]:
            d["mode"][row["mode"]] = d["mode"].get(row["mode"], 0) + 1


def acc_gpu(acc, gpu, a, b):
    if gpu is None:
        return
    for row in gpu:
        if not a < row["qpc"] <= b:
            continue
        for key in ("util", "clock", "mem_clock", "temp", "power", "limit"):
            if row.get(key) is not None:
                acc.gpu[key].append(row[key])
        if row.get("pstate"):
            acc.gpu["pstate"][row["pstate"]] = acc.gpu["pstate"].get(row["pstate"], 0) + 1
        acc.gpu["reasons"] |= (row.get("reasons") or 0) & ~0x1   # idle isn't a limit


# ---------------------------------------------------------------------------------------------------------------
# results

def interval_stats(intervals_ms):
    s = sorted(intervals_ms)
    return s, {"p50": rnd(pct(s, 50), 2), "p95": rnd(pct(s, 95), 2), "p99": rnd(pct(s, 99), 2),
               "p999": rnd(pct(s, 99.9), 2) if len(s) >= LOW01_MIN else None, "max": rnd(s[-1] if s else None, 2)}


def hitch_limit(p50):
    return max(HITCH_MIN_MS, 2 * p50) if p50 else HITCH_MIN_MS


def mode_of(acc):
    """UEVR's mode over the measured rows: a state bit set in most of them; frame generation and the async wait
    thread when set in any; the xrWaitFrame callsite of most new frames. None for everything without flags."""
    if not acc.flag_known:
        return None
    rows = acc.flag_rows
    out = {name: rows.get(name, 0) * 2 > acc.flag_known for name in
           ("afr", "synced", "skip_draw", "nsf", "nsf_active", "nsf_array", "d3d12", "menu", "hmd", "focused", "mono")}
    for name in ("async_wait", "framegen_sc", "dlssg"):
        out[name] = bool(rows.get(name))
    out["foreground_pct"] = rnd(rows.get("foreground", 0) * 100 / acc.flag_known, 1)
    out["sync"] = top(acc.sync)
    out["method"] = top(acc.method)
    out["callsite"] = top(acc.callsite)
    out["api"] = "d3d12" if out["d3d12"] else "d3d11"
    return out


def finish_vr(acc, capped, mode):
    p = acc.kinds.get("P", 0)
    if acc.rows == 0 or acc.measured_s <= 0:
        return None
    sec = acc.measured_s
    s, frame = interval_stats([iv[0] for iv in acc.intervals])
    limit = hitch_limit(frame["p50"])
    hitch_spans = [(st, en) for ms, st, en in acc.intervals if ms >= limit]
    in_capture = sum(1 for st, en in hitch_spans if any(cs < en and ce > st for cs, ce in acc.spans))
    paced_total = sum(acc.paced.values())
    slots = sorted(acc.slots)
    repeats = acc.kinds.get("R", 0)
    # Present passes per engine tick (a skipped tick of the synced sequential mode is still one engine frame): 2.00
    # with 2x frame generation, whose second Present of each engine frame UEVR's hook sees too
    engine_ticks = acc.ticks + acc.ticks_skipped
    pairs = sum(acc.nsf_pair.values())
    vr = {
        "fps": rnd(p / sec, 2),
        "fps_fresh": rnd(sum(v for k, v in acc.nsf.items() if k not in (2, 3)) / sec, 2) if acc.nsf.get(1) or acc.nsf.get(2) or acc.nsf.get(3) else None,
        "engine_fps": rnd(acc.ticks / sec, 2) if acc.has_ticks else None,
        "low1_fps": rnd(1000 / frame["p99"], 2) if frame["p99"] else None,
        "low01_fps": rnd(1000 / frame["p999"], 2) if frame["p999"] else None,
        "frame_ms": frame,
        "predicted_slot_ms": {"p50": rnd(pct(slots, 50), 2), "p99": rnd(pct(slots, 99), 2),
                              "max": rnd(slots[-1], 2)} if slots else None,
        "hitches": len(hitch_spans),
        "hitch_ms": rnd(limit, 1),
        "stalls": sum(1 for iv in acc.intervals if iv[0] >= STALL_MS),
        "hitch_per_min": rnd(len(hitch_spans) * 60 / sec, 2),
        "hitches_in_capture": in_capture if acc.spans else None,
        "paced_pct": {k: rnd(v * 100 / paced_total, 1) for k, v in acc.paced.items()} if paced_total else None,
        "s_at_3plus": rnd(acc.paced["3+"], 1) if paced_total else None,
        "period_max_ms": rnd(acc.period_max / 1e6, 1) if acc.period_max else None,
        "work_ms": None,
        "work_note": None,
        "late_pct": rnd(acc.late * 100 / acc.late_n, 1) if acc.late_n else None,
        "r_pct": rnd(repeats * 100 / (p + repeats), 1) if p + repeats else None,
        "older_pct": rnd(acc.older * 100 / (p + repeats), 1) if p + repeats else None,
        "presents_per_tick": rnd(acc.presents / engine_ticks, 2) if acc.has_presents and acc.has_ticks and engine_ticks else None,
        "nsf_snapshot_pct": rnd(acc.nsf_pair.get(2, 0) * 100 / pairs, 1) if pairs else None,
        "submits_per_frame": rnd((p + repeats + acc.kinds.get("E", 0)) / p, 3) if p else None,
        "wait_ok_per_frame": rnd(acc.wait_ok / acc.xr_new, 3) if acc.xr_new else None,
        "frames": p,
        "frames_1hz": acc.xr_new if acc.onehz_rows else None,
        "repeat_submits": repeats,
        "older_submits": acc.older,
        "empty_submits": acc.kinds.get("E", 0),
        "failed_submits": acc.kinds.get("F", 0),
        "nsf_reused_pct": rnd(acc.nsf.get(2, 0) * 100 / p, 1) if mode and mode["nsf"] and p else None,
        "nsf_fallback_pct": rnd(acc.nsf.get(3, 0) * 100 / p, 1) if mode and mode["nsf"] and p else None,
        "no_submit_s": rnd(acc.no_submit_s, 1) if acc.onehz_rows or acc.no_submit_s else None,
        "ring_dropped": acc.ring_dropped if acc.onehz_rows else None,
        "engine_time_ratio": rnd(acc.delta_ms / acc.delta_window_ms, 3) if acc.delta_window_ms and acc.has_ticks else None,
    }
    # work = submit interval - time blocked in xrWaitFrame. One thread does both only with the very-late wait right
    # after Present (callsite vr_very_late_post_present) and no async wait thread, and a frame cap's idle sleep would
    # count as work.
    if not acc.work:
        vr["work_note"] = "no wait times"
    elif not mode:
        vr["work_note"] = "sync mode not logged"
    elif mode["sync"] != "very_late" or mode["async_wait"]:
        vr["work_note"] = f"sync {mode['sync']}{', async wait' if mode['async_wait'] else ''}: the wait is on another thread"
    elif mode["callsite"] not in (None, "vr_very_late_post_present"):
        vr["work_note"] = f"xrWaitFrame at {mode['callsite']}: not the very-late wait after Present"
    elif capped:
        vr["work_note"] = "frame cap: idle time counts as work"
    else:
        vr["work_ms"] = rnd(median(acc.work), 2)
    return vr


def finish_attrib(acc, vr, gpu, capped):
    if not vr:
        return None
    med = {k: rnd(median(v), 2) for k, v in acc.attrib.items()}
    p = vr["frames"] or 0
    out = dict(med)
    out["presents_per_vr_frame"] = rnd(acc.presents / p, 2) if p and acc.has_presents else None
    out["dup_presents_per_vr_frame"] = rnd(acc.dup_presents / p, 2) if p and acc.has_presents else None
    out["ticks_per_vr_frame"] = rnd(acc.ticks / p, 2) if p and acc.has_ticks else None
    out["gpu_util_p50"] = gpu["util_p50"] if gpu else None
    frame = vr["frame_ms"]["p50"]
    bound = None
    if capped:
        bound = "capped"
    elif frame and med["wait_ms"] is not None and med["wait_ms"] >= 0.25 * frame:
        bound = "paced"            # the runtime holds the game back: headroom
    elif gpu and gpu["util_p50"] is not None and gpu["util_p50"] >= GPU_BOUND_UTIL:
        bound = "gpu"
    elif frame and med["tick_ms"] is not None and med["tick_ms"] >= 0.8 * frame:
        bound = "game_thread"
    out["bound"] = bound
    return out


def finish_flat(acc):
    f = acc.flat
    if not f["between"]:
        return None
    s, frame = interval_stats(f["between"])
    mean = statistics.fmean(f["between"])
    limit = hitch_limit(frame["p50"])
    hitches = sum(1 for v in f["between"] if v >= limit)
    # generated frames have no simulation start (NA): 1 in 2 presents carries one with 2x frame generation, 1 in 4
    # with 4x multi-frame generation; fewer than that means the Reflex markers didn't come through
    sim = statistics.fmean(f["sim"]) if len(f["sim"]) >= 0.2 * len(f["between"]) else None
    types = f["type"]
    return {
        "fps": rnd(1000 / mean, 2),
        "sim_fps": rnd(1000 / sim, 2) if sim else None,
        "presents_per_sim": rnd(sim / mean, 2) if sim else None,
        "low1_fps": rnd(1000 / frame["p99"], 2) if frame["p99"] else None,
        "low01_fps": rnd(1000 / frame["p999"], 2) if frame["p999"] else None,
        "frame_ms": frame,
        "hitches": hitches,
        "hitch_per_min": rnd(hitches * 60 / acc.measured_s, 2) if acc.measured_s else None,
        "gpu_busy_ms": rnd(median(f["gpu_busy"]), 2),
        "in_present_ms": rnd(median(f["in_present"]), 2),
        "displayed_fps": rnd(1000 / statistics.fmean(f["display"]), 2) if f["display"] else None,
        "frames": f["rows"],
        "frame_types": {k: rnd(v * 100 / sum(types.values()), 1) for k, v in types.items()} if types else None,
        "sync_interval": top(f["sync"]),
        "present_mode": top(f["mode"]),
        "runtime": top(f["runtime"]),
    }


def finish_compositor(acc):
    c = acc.comp
    if not c["between"]:
        return None
    return {"present_fps": rnd(1000 / statistics.fmean(c["between"]), 2),
            "displayed_fps": rnd(1000 / statistics.fmean(c["display"]), 2) if c["display"] else None,
            "presents": c["rows"], "present_mode": top(c["mode"])}


def finish_display(acc, vr):
    """monado-service: refreshes of the Mopic display and how many showed a new frame of the game (exact, where UEVR's
    own numbers can only count submits)."""
    d = acc.disp
    if not d["rows"] or d["s"] <= 0:
        return None
    shown = d["presents"] - d["no_layer"]
    new_fps = d["new"] / d["s"]
    return {"fps": rnd(d["presents"] / d["s"], 2), "new_fps": rnd(new_fps, 2),
            "new_pct": rnd(d["new"] * 100 / shown, 1) if shown > 0 else None,
            "repeated_pct": rnd(d["repeated"] * 100 / shown, 1) if shown > 0 else None,
            "dropped": d["dropped"] if d["dropped_known"] else None, "new_gap_max_ms": rnd(max(d["gap_max"]), 1) if d["gap_max"] else None,
            "mode": top(d["mode"]), "s": rnd(d["s"], 1),
            # UEVR's new frames against the ones monado showed: a gap means frames replaced before they were shown
            "uevr_vs_display_pct": rnd((vr["fps"] - new_fps) * 100 / new_fps, 1) if vr and vr["fps"] and new_fps else None}


def finish_gpu(acc):
    g = acc.gpu
    if not g["util"] and not g["clock"]:
        return None
    return {"util_p50": rnd(median(g["util"]), 1), "clock_mhz_p50": rnd(median(g["clock"]), 0),
            "mem_clock_mhz_p50": rnd(median(g["mem_clock"]), 0), "temp_c_max": rnd(max(g["temp"]) if g["temp"] else None, 0),
            "temp_c_p50": rnd(median(g["temp"]), 0), "power_w_p50": rnd(median(g["power"]), 1),
            "power_limit_w": rnd(median(g["limit"]), 1), "pstate": top(g["pstate"]),
            "limits": [name for bit, name in CLOCK_REASONS.items() if g["reasons"] & bit]}


def finish_caps(settings, mode, injected, refresh_hz):
    """Frame caps the game's settings set (configured, not necessarily in effect: UEVR doesn't read the engine's
    effective caps). With UEVR, VSync doesn't cap (UEVR presents with sync interval 0), and an engine cap binds the
    VR rate when it is at most the refresh rate, per stereo pair in AFR (2 engine frames per VR frame)."""
    if not settings:
        return {"on": None, "source": None, "limit_fps": None, "vr_limit_fps": None, "vsync": None, "other": None}
    limit = settings.get("frame_rate_limit")
    limit = limit if limit and limit > 0 else None
    vsync = settings.get("vsync")
    out = {"on": False, "source": "settings", "limit_fps": limit, "vr_limit_fps": None, "vsync": vsync,
           "other": settings.get("other_limits") or None}
    if injected is False:
        out["on"] = bool(limit or vsync)
    elif limit:
        out["vr_limit_fps"] = rnd(limit / (2 if mode and mode["afr"] else 1), 1)
        out["on"] = out["vr_limit_fps"] <= refresh_hz + 0.5
    return out


def finish(acc, refresh_hz, ctx, settings, label=None):
    mode = mode_of(acc)
    caps = finish_caps(settings, mode, (ctx or {}).get("injected"), refresh_hz)
    gpu = finish_gpu(acc)
    vr = finish_vr(acc, caps["on"], mode)
    return {
        "label": label,
        "segments": acc.segments,
        "window_s": rnd(acc.window_s, 2),
        "settle_s": rnd(acc.settle_s, 2),
        "settle_capped": acc.settle_capped,
        "measured_s": rnd(acc.measured_s, 2),
        "excluded_s": {k: rnd(v, 1) for k, v in acc.excluded.items()},
        "vr": vr,
        "attrib": finish_attrib(acc, vr, gpu, caps["on"]),
        "flat": finish_flat(acc),
        "compositor": finish_compositor(acc),
        "display": finish_display(acc, vr),
        "gpu": gpu,
        "mode": mode,
        "capped": caps,
        "captures": {"count": acc.captures, "s": rnd(acc.capture_s, 2), "pilot_cpu_s": rnd(acc.cpu_s, 2)} if acc.captures or acc.cpu_s else None,
    }


# ---------------------------------------------------------------------------------------------------------------
# context

def read_settings(run_dir):
    """The game's GameUserSettings.ini (copied before the run): a fingerprint of everything but the keys UE rewrites
    on every exit, the keys that matter for the frame rate, and what changed during the run."""
    before = os.path.join(run_dir, "game-settings", "GameUserSettings.ini")
    if not os.path.exists(before):
        return None

    def parse(path):
        values, section = {}, ""
        try:
            with open(path, encoding="utf-8-sig", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("[") and line.endswith("]"):
                        section = line[1:-1]
                    elif "=" in line and not line.startswith(";"):
                        k, _, v = line.partition("=")
                        values[f"{section}/{k.strip()}"] = v.strip()
        except OSError:
            pass
        return values

    def key(k):
        return k.rsplit("/", 1)[1]

    values = parse(before)
    stable = sorted((k, v) for k, v in values.items() if not SETTINGS_VOLATILE.match(key(k)))
    fp = hashlib.sha256("\n".join(f"{k}={v}" for k, v in stable).encode("utf-8")).hexdigest()[:16]
    out = {"fingerprint": fp, "values": {key(k): v[:300] for k, v in values.items() if SETTINGS_KEYS.search(key(k))},
           "frame_rate_limit": None, "vsync": None, "other_limits": {}, "framegen": None, "changed": None}
    for k, v in values.items():
        name = key(k)
        if name == "FrameRateLimit":
            out["frame_rate_limit"] = num(v)
        elif name.startswith("FrameRateLimit") and num(v):
            out["other_limits"][name] = num(v)   # Dead as Disco: _WhenBackgrounded, _InMenu, _OnBattery
        elif name == "bUseVSync":
            out["vsync"] = v.lower() == "true"
    fg = [f"{key(k)}={v[:120]}" for k, v in sorted(values.items()) if SETTINGS_FRAMEGEN.search(key(k))]
    out["framegen"] = "; ".join(fg) or None
    after = os.path.join(run_dir, "game-settings", "GameUserSettings.after.ini")
    if os.path.exists(after):
        later = parse(after)
        out["changed"] = sorted(key(k) for k in set(values) | set(later)
                                if values.get(k) != later.get(k) and not SETTINGS_VOLATILE.match(key(k)))
    return out


def render_fingerprint(run_dir):
    """sha256 of the run's VR_* config keys (UEVR render settings; a different value makes runs incomparable)."""
    path = os.path.join(run_dir, "config.txt")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        keys = sorted(line.strip() for line in f if line.startswith("VR_") and not line.startswith("VR_PerfLog"))
    return hashlib.sha256("\n".join(keys).encode("utf-8")).hexdigest()[:16] if keys else None


def log_facts(run_dir, clock):
    """From UEVR's log.txt: the first XR focus (QPC ns) and the Streamline frame-generation swapchain line."""
    path = os.path.join(run_dir, "log.txt")
    facts = {"focused_qpc": None, "fg_lines": []}
    if not os.path.exists(path):
        return facts
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if facts["focused_qpc"] is None and FOCUS_LOG.search(line):
                m = re.match(r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]", line)
                if m:
                    unix = local_to_unix_ns(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
                    facts["focused_qpc"] = clock.to_qpc(unix) if unix else None
            m = FG_LOG.search(line)
            if m and m.group(0) not in facts["fg_lines"]:
                facts["fg_lines"].append(m.group(0))
    return facts


def framegen(fr, flat, ctx, facts, settings, vr=None, mode=None):
    """Frame generation, measured and flagged, never changed. Measured: UEVR's Present passes per engine tick in the
    measured segments (2x frame generation presents twice per engine frame, and UEVR's Present hook sees both; not in
    AFR, which paces its own Presents), PresentMon's generated frame types and Presents per simulated frame. When one
    of them could be measured, they decide ("measured" true): on when one fired, False when none did. The rest only
    says frame generation is available (UEVR hooked Streamline's frame-generation swapchain, nvngx_dlssg.dll loaded,
    the log line), which a game carries while generating nothing; without a measurement those decide, as before, and
    None means nothing could tell. Fired signals are listed in "evidence", measured ones first. The game's own setting
    is context only."""
    measured, available = [], []
    measurable = False
    ppt = (vr or {}).get("presents_per_tick")
    if ppt is not None and not (mode or {}).get("afr"):
        measurable = True
        if ppt >= FG_RATIO:
            measured.append(f"uevr: {ppt} presents per engine tick")
    if flat and flat.get("frame_types"):
        measurable = True
        for kind, share in flat["frame_types"].items():
            if kind.lower() not in ("application", "app", "") and share >= 5:
                measured.append(f"presentmon: {share}% {kind} frames")
    if flat and flat.get("presents_per_sim") is not None:
        measurable = True
        if flat["presents_per_sim"] >= FG_RATIO:
            measured.append(f"presentmon: {flat['presents_per_sim']} presents per simulated frame")
    flags = fr.any_flags if fr is not None and fr.has_flags() else None
    if flags is not None and flags & (1 << FLAG_BITS["framegen_sc"]):
        available.append("uevr: Streamline frame-generation swapchain")
    if flags is not None and flags & (1 << FLAG_BITS["dlssg"]):
        available.append("uevr: nvngx_dlssg.dll loaded")
    modules = (ctx or {}).get("modules") or {}
    if modules.get("nvngx_dlssg"):
        available.append("game: nvngx_dlssg.dll loaded")
    for line in facts["fg_lines"]:
        available.append(f"log: {line}")
    if measurable:
        on = bool(measured)
    else:
        told = flags is not None or modules.get("nvngx_dlssg") is False or bool(flat and flat.get("sim_fps"))
        on = True if available else (False if told else None)
    return {"on": on, "measured": measurable, "presents_per_tick": ppt, "evidence": measured + available,
            "modules": modules.get("fg_modules") or None, "setting": (settings or {}).get("framegen")}


def mopic_refresh(displays):
    for d in displays or []:
        if "MPL" in (d.get("monitor") or "").upper() and d.get("refresh_hz"):
            return d["refresh_hz"]
    return None


def power_info(segments, ctx):
    p = None
    for s in segments:
        if s.get("power"):
            p = dict(s["power"])
            break
    c = (ctx or {}).get("power") or {}
    if p is None and c:
        p = {"ac": c.get("ac"), "battery_pct": c.get("battery_pct")}
    if p is None:
        return None
    overlay = c.get("overlay_ac") if p.get("ac") is not False else c.get("overlay_dc")
    p["mode"] = POWER_OVERLAYS.get((overlay or "").lower(), overlay) if overlay else ("balanced" if c else None)
    p["scheme"] = c.get("scheme")
    return p


# ---------------------------------------------------------------------------------------------------------------
# comparisons with earlier runs in runs\ (informational: verdicts never change)

def run_variant(ctx):
    """What run-test.ps1 changed beyond UEVR's config (perf-context.json "variant": the eye sampler running, the game's
    own settings files changed with -GameIni), as a comparable key; a run without it (older runs) is plain."""
    v = (ctx or {}).get("variant") if isinstance(ctx, dict) else None
    v = v if isinstance(v, dict) else {}
    return bool(v.get("eye_sampler")), v.get("game_ini") or ""


def compare(run_dir, summary, ctx):
    """The flat baseline (the newest PASS runs without UEVR) and the previous build (the newest PASS runs of another
    UEVRBackend.dll), both with the same game, recipe, vars, segment label, settings fingerprint and window monitor."""
    ctx = ctx or {}
    root = os.path.dirname(os.path.abspath(run_dir))
    me = os.path.basename(os.path.abspath(run_dir))
    keys = {"game": ctx.get("game"), "recipe": os.path.basename(ctx.get("recipe") or ""), "vars": ctx.get("recipe_vars") or "",
            "settings": (summary.get("settings") or {}).get("fingerprint"),
            "monitor": (summary.get("window") or {}).get("monitor"), "label": summary.get("label"),
            "variant": run_variant(ctx)}
    flat_runs, other_builds = [], {}
    for path in glob.glob(os.path.join(root, "*", "result.json")):
        name = os.path.basename(os.path.dirname(path))
        if name == me:
            continue
        r = read_json(path)
        if not isinstance(r, dict) or r.get("verdict") != "PASS" or not isinstance(r.get("perf"), dict):
            continue
        p = r["perf"]
        if (r.get("game") != keys["game"] or os.path.basename(r.get("recipe") or "") != keys["recipe"]
                or (r.get("recipe_vars") or "") != keys["vars"] or p.get("label") != keys["label"]
                or p.get("label") == "observe"):
            continue
        if keys["settings"] and (p.get("settings") or {}).get("fingerprint") not in (None, keys["settings"]):
            continue
        if keys["monitor"] and (p.get("window") or {}).get("monitor") not in (None, keys["monitor"]):
            continue
        if run_variant(read_json(os.path.join(os.path.dirname(path), "perf-context.json"))) != keys["variant"]:
            continue
        entry = (r.get("started") or "", name, r, p)
        if p.get("injected") is False and p.get("flat"):
            flat_runs.append(entry)
        elif p.get("injected") and p.get("vr") and r.get("dll_sha256") != ctx.get("dll_sha256"):
            other_builds.setdefault(r.get("dll_sha256"), []).append(entry)
    out = {"flat": None, "prev_build": None}
    vr = summary.get("vr")
    if flat_runs and vr and vr.get("fps"):
        flat_runs.sort(key=lambda e: e[:2], reverse=True)
        use = flat_runs[:5]
        values = [(p["flat"].get("sim_fps") if (p.get("framegen") or {}).get("on") and p["flat"].get("sim_fps") else p["flat"].get("fps"))
                  for _, _, _, p in use]
        values = [v for v in values if v]
        if values:
            flat = median(values)
            # AFR renders the two eyes in two engine frames: zero overhead is flat / 2
            k = 2 if (summary.get("mode") or {}).get("afr") else 1
            refresh = summary.get("refresh_hz") or 60
            out["flat"] = {"flat_fps": rnd(flat, 2), "runs": len(values), "dirs": [n for _, n, _, _ in use],
                           "vr_vs_flat": rnd(vr["fps"] / flat, 3),
                           "vr_vs_flat60": rnd(vr["fps"] / (min(flat, refresh) / k), 3), "afr_factor": k,
                           "engine_vs_flat": rnd(vr["engine_fps"] / flat, 3) if vr.get("engine_fps") else None}
    if other_builds and vr and vr.get("fps"):
        newest = max(other_builds.values(), key=lambda runs: max(e[0] for e in runs))
        newest.sort(key=lambda e: e[:2], reverse=True)
        use = newest[:5]
        fps = [p["vr"]["fps"] for _, _, _, p in use if p["vr"].get("fps")]
        if fps:
            base = median(fps)
            spread = (max(fps) - min(fps)) / base if base else None
            p99 = [x for x in (((p["vr"].get("frame_ms") or {}).get("p99")) for _, _, _, p in use) if x]
            hpm = [x for x in ((p["vr"].get("hitch_per_min")) for _, _, _, p in use) if x is not None]
            work = [x for x in ((p["vr"].get("work_ms")) for _, _, _, p in use) if x]
            render = {p.get("render_config") for _, _, _, p in use}
            pb = {"dll_sha256": use[0][2].get("dll_sha256"), "runs": len(fps), "dirs": [n for _, n, _, _ in use],
                  "vr_fps": rnd(base, 2), "range": [rnd(min(fps), 2), rnd(max(fps), 2)],
                  "delta_pct": rnd((vr["fps"] - base) * 100 / base, 1) if base else None,
                  "noisy": bool(spread is not None and spread > 0.10),
                  "render_config_differs": bool(render - {summary.get("render_config")}),
                  "regression": None}
            # the design's rule, against at least 3 runs of the other build whose own spread is within 10 %
            if len(fps) >= 3 and not pb["noisy"]:
                why = []
                if vr["fps"] <= 0.95 * base and vr["fps"] < min(fps):
                    why.append(f"VR fps {vr['fps']} vs {pb['vr_fps']}")
                mine_p99 = (vr.get("frame_ms") or {}).get("p99")
                if p99 and mine_p99 and mine_p99 >= 1.15 * median(p99):
                    why.append(f"p99 {mine_p99} ms vs {rnd(median(p99), 2)}")
                if hpm and vr.get("hitch_per_min") is not None and vr["hitch_per_min"] >= 2 * median(hpm) and vr["hitch_per_min"] >= median(hpm) + 2:
                    why.append(f"hitches/min {vr['hitch_per_min']} vs {rnd(median(hpm), 2)}")
                if work and vr.get("work_ms") and vr["work_ms"] >= 1.1 * median(work):
                    why.append(f"work {vr['work_ms']} ms vs {rnd(median(work), 2)}")
                pb["regression"] = why
            out["prev_build"] = pb
    return out


# ---------------------------------------------------------------------------------------------------------------

def summary_line(s):
    """The one line summary.txt gets."""
    if s.get("label") is None:
        return f"perf: {s.get('note')} (" + ", ".join(f"{k} {v}" for k, v in (s.get("sources") or {}).items()) + ")"
    head = f"perf[{s['label']} {s['measured_s']:.1f}s"
    if (s.get("settle_s") or 0) >= 0.05:
        head += f", settle {s['settle_s']:.1f}s" + (" (never settled)" if s.get("settle_capped") else "")
    if any(v for v in (s.get("excluded_s") or {}).values()):
        head += ", excluded " + " ".join(f"{k} {v:.0f}s" for k, v in s["excluded_s"].items() if v)
    parts = []
    vr, flat, refresh = s.get("vr"), s.get("flat"), s.get("refresh_hz") or 60
    if vr:
        p99 = (vr.get("frame_ms") or {}).get("p99")
        text = (f"VR {vr['fps']:.1f} fps (1% low {vr['low1_fps'] or 0:.1f}, p99 {p99 or 0:.1f} ms, "
                f"{vr['hitches']} hitches, {vr['stalls']} stalls)")
        if vr.get("r_pct") and vr["r_pct"] > 5:
            text += f" R {vr['r_pct']:.0f}%"
        if vr.get("submits_per_frame") and vr["submits_per_frame"] > 1.05:
            text += f" {vr['submits_per_frame']:.2f} submits/frame"
        if vr.get("presents_per_tick") and vr["presents_per_tick"] > 1.05:
            text += f" {vr['presents_per_tick']:.2f} presents/tick"
        if vr.get("nsf_snapshot_pct") is not None:
            text += f" frozen pairs {vr['nsf_snapshot_pct']:.0f}%"
        if vr.get("wait_ok_per_frame") and vr["wait_ok_per_frame"] > 1.05:
            text += f" {vr['wait_ok_per_frame']:.2f} waits/frame"
        if vr.get("fps_fresh") is not None and vr["fps"] and vr["fps_fresh"] < 0.95 * vr["fps"]:
            text += f" fresh {vr['fps_fresh']:.1f}"
        if vr.get("no_submit_s"):
            text += f" {vr['no_submit_s']:.0f}s without submits"
        parts.append(text)
        if vr.get("paced_pct"):
            pp = vr["paced_pct"]
            parts.append(f"paced {refresh:.0f}:{pp['1']:.0f}% {refresh / 2:.0f}:{pp['2']:.0f}% "
                         f"<={refresh / 3:.0f}:{pp['3+']:.0f}% (max period {vr['period_max_ms'] or 0:.0f} ms)")
        parts.append(f"work {vr['work_ms']:.1f} ms" if vr.get("work_ms") is not None else f"work n/a ({vr.get('work_note')})")
    d = s.get("display")
    if d:
        parts.append(f"display: new frame on {d['new_pct'] or 0:.0f}% of refreshes ({d['new_fps']:.1f}/s)"
                     + (f", {d['dropped']} dropped" if d.get("dropped") is not None else ""))
    if flat:
        text = f"{'game Present' if vr or s.get('injected') else 'flat'} {flat['fps']:.1f} fps"
        if flat.get("sim_fps"):
            text += f" (sim {flat['sim_fps']:.1f})"
        if not vr:
            text += f" (1% low {flat['low1_fps'] or 0:.1f}, {flat['hitches']} hitches)"
        parts.append(text)
    fg = s.get("framegen") or {}
    if fg.get("on"):
        parts.append("FG on")
    if (s.get("capped") or {}).get("on"):
        c = s["capped"]
        parts.append(f"capped {c.get('vr_limit_fps') or c.get('limit_fps') or 'vsync'}")
    g = s.get("gpu")
    if g:
        parts.append(f"GPU {g['util_p50'] or 0:.0f}% {g['clock_mhz_p50'] or 0:.0f} MHz {g['temp_c_max'] or 0:.0f}C"
                     + (f" {g['power_w_p50']:.0f}/{g['power_limit_w']:.0f} W" if g.get("power_w_p50") and g.get("power_limit_w") else "")
                     + (f" limits {','.join(g['limits'])}" if g.get("limits") else ""))
    c = s.get("compare") or {}
    if c.get("flat"):
        parts.append(f"VR/flat60 {c['flat']['vr_vs_flat60'] * 100:.0f}% ({c['flat']['runs']} flat runs)")
    if c.get("prev_build") and c["prev_build"].get("delta_pct") is not None:
        pb = c["prev_build"]
        parts.append(f"prev build {pb['vr_fps']:.1f} ({pb['delta_pct']:+.1f}%{', noisy' if pb['noisy'] else ''}"
                     f"{', REGRESSION' if pb.get('regression') else ''})")
    hint = (s.get("attrib") or {}).get("bound")
    if hint:
        parts.append(f"hint {hint}")
    pm = (s.get("sources") or {}).get("presentmon")
    if pm and pm not in ("ok", "off"):
        parts.append(f"PresentMon {pm.replace('_', ' ')}")
    if not vr and not flat:
        parts.append("no frame data (" + ", ".join(f"{k} {v}" for k, v in (s.get("sources") or {}).items()) + ")")
    return head + "]: " + " | ".join(parts)


def headline(s):
    """The few numbers the matrix and ladder tables show."""
    vr, flat = s.get("vr"), s.get("flat")
    src, kind = (vr, "vr") if vr else (flat, "flat") if flat else (None, None)
    if not src:
        return None
    return {"kind": kind, "fps": src["fps"], "low1_fps": src.get("low1_fps"), "hitches": src.get("hitches"),
            "hitch_per_min": src.get("hitch_per_min"), "fg": bool((s.get("framegen") or {}).get("on")),
            "measured_s": s.get("measured_s")}


def qpc_frequency():
    try:
        import ctypes
        f = ctypes.c_int64()
        ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(f))
        return f.value
    except Exception:  # noqa: BLE001 - not on Windows
        return 10_000_000


def report(run_dir, do_compare=True):
    warnings = []
    ctx = read_json(os.path.join(run_dir, "perf-context.json")) or {}
    warnings += [f"run-test.ps1 could not copy {e}" for e in ctx.get("copy_errors") or []]
    pilot = read_json(os.path.join(run_dir, "pilot-status.json"))
    freq = ctx.get("qpc_freq") or ((pilot or {}).get("clock") or {}).get("qpc_freq") or qpc_frequency()

    frames_t = load_table(os.path.join(run_dir, "perf-frames.csv"), warnings)
    onehz = load_table(os.path.join(run_dir, "perf.csv"), warnings)
    monado_t = load_table(os.path.join(run_dir, "monado-frames.csv"), warnings)
    clock = Clock()
    if pilot and pilot.get("clock"):
        clock.add(pilot["clock"].get("qpc_ns"), pilot["clock"].get("unix_ns"))
    for key in ("t0", "observe_end", "end"):
        pair = ctx.get(key) or {}
        if pair.get("qpc_ns") and pair.get("unix_ms"):
            clock.add(pair["qpc_ns"], pair["unix_ms"] * 1_000_000)
    clock.add_table(onehz)
    clock.add_table(monado_t)

    fr = Frames(frames_t) if frames_t is not None and frames_t.has("qpc_ns") else None
    if frames_t is not None and fr is None:
        warnings.append("perf-frames.csv has no qpc_ns column")
    # XrTime -> QPC ns, from UEVR's once-a-second pairs (XR_KHR_win32_convert_performance_counter_time; empty fields
    # until the first pair, or without the extension)
    xr_offset = None
    if onehz is not None:
        xq, xt = onehz.col("xr_pair_qpc_ns"), onehz.col("xr_pair_time")
        if xq and xt:
            xr_offset = median([xq[k] - xt[k] for k in range(len(xq)) if xq[k] and xt[k]])
    pm_paths = sorted(glob.glob(os.path.join(run_dir, "presentmon*.csv")))
    pids = set(ctx.get("game_pids") or [])
    pm = PresentMon(pm_paths, ctx.get("process"), pids, freq, warnings) if pm_paths else None
    disp = Display(monado_t, pids, ctx.get("process")) if monado_t is not None and monado_t.has("qpc_ns") else None
    gpu = load_gpu(load_table(os.path.join(run_dir, "gpu.csv"), warnings), clock)
    facts = log_facts(run_dir, clock)
    settings = read_settings(run_dir)

    displays = (pilot or {}).get("displays") or ctx.get("displays")
    refresh_hz = mopic_refresh(displays)
    if refresh_hz is None and fr is not None and fr.period_ns:
        periods = [p for p in fr.period_ns if p and 4e6 <= p <= 34e6]
        refresh_hz = round(1e9 / min(periods)) if periods else None
    refresh_hz = refresh_hz or 60
    refresh_ns = 1e9 / refresh_hz

    # the measured segments
    segs = []
    if pilot is not None:
        source = "pilot"
        for index, s in enumerate(pilot.get("segments") or []):
            if not s.get("start_qpc_ns"):
                continue
            end = s.get("end_qpc_ns")
            if not end:  # the pilot died in the middle: up to its last status update
                end = clock.to_qpc(int((pilot.get("updated") or 0) * 1e9)) or s["start_qpc_ns"]
            segs.append(dict(s, index=index, end_qpc_ns=end))
    else:
        source = "observe"
        end = (ctx.get("observe_end") or {}).get("qpc_ns")
        start = facts["focused_qpc"]
        if start is None and ctx.get("injected") is False:
            t = ctx.get("game_start_unix_ms")
            start = clock.to_qpc(t * 1_000_000) if t else None
        if start and end and end - start > (OBSERVE_AFTER_S + 5) * 1e9:
            segs.append({"label": "observe", "index": None, "start_qpc_ns": start + int(OBSERVE_AFTER_S * 1e9),
                         "end_qpc_ns": end, "ok": True, "step": None, "phase": None})

    out_segments = []
    groups = {}
    for s in segs:
        a0, b = int(s["start_qpc_ns"]), int(s["end_qpc_ns"])
        acc = Acc()
        acc.segments = 1
        acc.window_s = max(0.0, (b - a0) / 1e9)
        a, capped = settle_end(fr if fr is not None and fr.n else None, a0, b, refresh_ns)
        acc.settle_s = max(0.0, (min(a, b) - a0) / 1e9)
        acc.settle_capped = int(capped)
        if b > a:
            cls = classify_bins(fr, a, b)
            n = len(cls)
            for k, c in enumerate(cls):
                if c == "ok":
                    acc.measured_s += bin_seconds(a, b, k, n)
                else:
                    acc.excluded[c] += bin_seconds(a, b, k, n)
            included = (lambda q, a=a, cls=cls, n=n: cls[bin_of(q, a, n)] == "ok")
            if fr is not None:
                acc_uevr(acc, fr, a, b, cls, refresh_ns, xr_offset)
            acc_onehz(acc, onehz, a, b, cls)
            if pm is not None:
                acc_flat(acc, pm, a, b, included)
            acc_display(acc, disp, a, b, cls)
            acc_gpu(acc, gpu, a, b)
        if s.get("index") is not None:
            acc.spans = segment_captures(run_dir, s["index"])
        acc.captures = s.get("captures") or 0
        acc.capture_s = s.get("capture_s") or 0.0
        acc.cpu_s = s.get("cpu_s") or 0.0
        result = finish(acc, refresh_hz, ctx, settings, s["label"])
        result.update(step=s.get("step"), phase=s.get("phase"), ok=s.get("ok"), start_qpc_ns=a0, end_qpc_ns=b,
                      window=s.get("window"))
        out_segments.append(result)
        if s.get("ok"):
            groups.setdefault(s["label"], Acc()).merge(acc)
        else:
            warnings.append(f"segment {s['label']} (step {s.get('step')}) did not finish: left out of the totals")

    group_results = {label: finish(acc, refresh_hz, ctx, settings, label) for label, acc in groups.items()}
    main = None
    if group_results:
        main = ("gameplay" if "gameplay" in group_results else "observe" if "observe" in group_results
                else max(group_results, key=lambda k: group_results[k]["measured_s"] or 0))

    smi = (ctx.get("nvidia_smi") or {}).get("status")
    sources = {
        "uevr": "ok" if fr is not None and fr.n else ("not injected" if ctx.get("injected") is False else "missing"),
        "presentmon": None,
        "monado": "ok" if disp is not None and disp.rows else ("not the game's" if monado_t is not None else "missing"),
        "gpu": "ok" if gpu else ("no_rows" if smi == "ok" else smi or "missing"),
    }
    pm_ctx = (ctx.get("presentmon") or {}).get("status")
    if pm is not None and pm.rows_game:
        sources["presentmon"] = "ok"
    elif pm_ctx and pm_ctx != "ok":
        sources["presentmon"] = pm_ctx
    elif pm is not None and pm.no_qpc:
        sources["presentmon"] = "no_qpc"
    else:
        sources["presentmon"] = "no_rows" if pm_paths or pm_ctx == "ok" else "missing"

    if main is not None:
        summary = dict(group_results[main])
        window = next((s.get("window") for s in segs if s.get("window")), None)
        summary.update({
            "injected": ctx.get("injected"),
            "refresh_hz": refresh_hz,
            "framegen": framegen(fr, summary.get("flat"), ctx, facts, settings, summary.get("vr"), summary.get("mode")),
            "window": {k: window.get(k) for k in ("device", "adapter", "monitor", "refresh_hz", "width", "height", "window")} if window else None,
            "power": power_info(segs, ctx),
            "settings": {k: settings[k] for k in ("fingerprint", "frame_rate_limit", "vsync", "framegen", "changed")} if settings else None,
            "render_config": render_fingerprint(run_dir),
            "sources": sources,
        })
        mode = summary.get("mode") or {}
        vr = summary.get("vr") or {}
        if fr is not None and fr.reclassified:
            warnings.append(f"perf-frames.csv was written before UEVR counted resubmits of older engine frames as "
                            f"repeats: {fr.reclassified} of its P rows are counted as R here")
        if vr.get("ring_dropped"):
            warnings.append(f"UEVR lost {vr['ring_dropped']} frame records (buffer full, or perf-frames.csv at its size "
                            "cap): counts from perf-frames.csv are low")
        # perf-frames.csv's new frames against perf.csv's count of the same seconds (rows lost or cut off); not when
        # the rows were reclassified, as perf.csv of that UEVR counted the older-frame resubmits as new too
        if vr.get("frames_1hz") and vr.get("frames") is not None and summary.get("measured_s") and not (fr is not None and fr.reclassified):
            # (1 s rows and the measured seconds don't line up: up to a second's frames at each end of a segment)
            slack = max(0.05 * vr["frames_1hz"], 2 * (summary.get("segments") or 1) * vr["frames_1hz"] / summary["measured_s"])
            if abs(vr["frames"] - vr["frames_1hz"]) > slack:
                warnings.append(f"perf-frames.csv has {vr['frames']} new frames where perf.csv counted "
                                f"{vr['frames_1hz']}")
        if mode.get("foreground_pct") is not None and mode["foreground_pct"] < 90:
            warnings.append(f"the game had the Windows foreground in only {mode['foreground_pct']}% of the measured frames")
        other = (summary.get("capped") or {}).get("other")
        if other and mode.get("foreground_pct") is not None and mode["foreground_pct"] < 99:
            warnings.append(f"the game caps itself in some states ({', '.join(f'{k}={v:g}' for k, v in other.items())})")
        if do_compare:
            try:
                summary["compare"] = compare(run_dir, summary, ctx)
            except Exception as e:  # noqa: BLE001 - a broken old result.json must not lose this run's numbers
                warnings.append(f"compare failed: {e!r}")
        summary["headline"] = headline(summary)
    else:
        summary = {"label": None, "note": "no measured segment" if source == "pilot" else "observation too short or not timed",
                   "headline": None, "injected": ctx.get("injected"), "sources": sources}
    summary["warnings"] = warnings
    line = summary_line(summary)
    return {"version": VERSION, "run": os.path.basename(os.path.abspath(run_dir)), "source": source, "label": main,
            "refresh_hz": refresh_hz, "segments": out_segments, "groups": group_results,
            "context": {"settings": settings, "displays": displays, "framegen_log": facts["fg_lines"],
                        "presentmon": ctx.get("presentmon"), "nvidia_smi": ctx.get("nvidia_smi"),
                        "modules": ctx.get("modules"), "power": ctx.get("power"), "xr_time_offset_ns": xr_offset},
            "summary": summary, "line": line, "warnings": warnings}


def finite(o):
    """NaN / infinity -> None all through (json.dump would write NaN, which PowerShell's ConvertFrom-Json refuses)."""
    if isinstance(o, float):
        return o if math.isfinite(o) else None
    if isinstance(o, dict):
        return {k: finite(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [finite(v) for v in o]
    return o


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("run", help="perf.json for one run folder")
    s.add_argument("run_dir")
    s.add_argument("--no-compare", action="store_true", help="don't look for baselines in the other run folders")
    s.add_argument("--print", action="store_true", help="also print perf.json")
    args = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    doc = finite(report(args.run_dir, not args.no_compare))
    # perf.json: everything; perf-summary.json: what run-test.ps1 puts into result.json and summary.txt (no keys
    # from the game's files, which could differ only in case: PowerShell's ConvertFrom-Json refuses those)
    for name, data in (("perf.json", doc), ("perf-summary.json", {"line": doc["line"], "summary": doc["summary"]})):
        path = os.path.join(args.run_dir, name)
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1, ensure_ascii=True)
        os.replace(path + ".tmp", path)
    print(doc["line"])
    if args.print:
        print(json.dumps(doc, indent=1))


if __name__ == "__main__":
    main()
