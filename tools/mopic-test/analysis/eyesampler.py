"""eyesampler: does each eye on the Mopic display change when the other does? (monado-service in MOPIC_MODE=sbs)

    .venv\\Scripts\\python analysis\\eyesampler.py sample --out <run folder> [--status <run>\\pilot-status.json] [--pid N]
                                                   [--seconds S] [--stop-file PATH] [--hz 120] [--y 0.5] [--height 96]
    .venv\\Scripts\\python analysis\\eyesampler.py report <run folder or eyes.csv> [--threshold 1.0] [--print]
    .venv\\Scripts\\python analysis\\eyesampler.py shot [--out strip.png]     # the strip it samples, to check its place

With MOPIC_MODE=sbs, monado presents the left eye in the left half of the Mopic display and the right eye in the right
half (the monitor does the lenticular conversion itself), and a screen capture sees both. `sample` grabs one strip
across the whole display (default: 96 px high around the middle) about 120 times a second, so both halves come from
the same composed desktop frame, and writes per sample and per eye the mean luminance and how much the eye changed
since the previous sample (eyes.csv, eyes-meta.json). Both eyes come from one submitted frame, so they normally change
in the same refresh. `report` finds (eyes-report.json):

  one-eye lag    one eye changed and the other followed more than one display refresh later with a change about as
                 big (else "unmatched": a change only one eye's strip shows, then the camera moving), while the scene
                 was moving
  one-eye only   one eye changed and the other didn't change at all within --max-lag while the first kept changing
                 (or changed a lot), while the scene was moving
  one-eye black  one eye black while the other shows a picture

With --status (run-test.ps1 -EyeSampler passes the pilot's status file) it samples only while one of the recipe's
measured segments (play steps, "measure") is open, and each row carries the segment's index. Times are QPC ns
(time.perf_counter_ns()): the clock of UEVR's perf files, monado's frame stats and gamepilot's segments.

eyes.csv: t_ns (QPC ns at the start of the grab), grab_us, seg (pilot segment, -1 without --status), l_mean / r_mean
(mean luma 0-255 of each half of the strip), l_diff / r_diff (mean absolute luma change against the previous sample,
empty after a pause), lr_corr / lr_shift (normalized correlation of the halves' column profiles at the best
horizontal shift, and that shift in display px: right[x] ~ left[x + lr_shift]; empty when a half is flat).
eyes-shots\\: candidate frames (one eye changed strongly, the other not; one eye black) as grayscale PNGs, the
previous sample over the current one, left | right.
"""

import argparse
import bisect
import csv
import datetime
import json
import math
import os
import statistics
import sys
import time

import numpy as np

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CSV_HEADER = ["t_ns", "grab_us", "seg", "l_mean", "r_mean", "l_diff", "r_diff", "lr_corr", "lr_shift"]
STEP = 4                 # every 4th pixel of the strip, in both directions (24 x 480 per eye of a 96 px strip)
LUMA = np.array([19, 183, 54], np.uint16)   # B, G, R weights of Rec. 709 luma, /256
MAX_SHIFT_PX = 96        # the L/R profile match looks this far (display px) each way
STATUS_POLL_S = 0.25     # pilot-status.json, the stop file and the game process are checked this often
STATUS_POLL_IDLE_S = 0.05   # ... and this often while no segment is open (a segment's first frames are often the ones
                            # that matter: the field guide toggles a state right after the key press)
PAUSE_GAP_S = 0.25       # a sample after a longer gap (a closed segment, a stall) has no diff
MATCH_RATIO = 3.0        # a lagging eye's later change is within this factor of the first eye's change


# ---------------------------------------------------------------------------------------------------------------
# one sample

def eye_lumas(bgra, height, width):
    """A BGRA strip (mss's raw.bgra) -> (left, right): the luma (Rec. 709, 0-255) of every STEP-th pixel of each
    half, as int16 arrays (rows x columns)."""
    a = np.frombuffer(bgra, np.uint8).reshape(height, width, 4)[::STEP, ::STEP, :3]
    y = (a.astype(np.uint16) @ LUMA >> 8).astype(np.int16)
    half = y.shape[1] // 2
    return y[:, :half], y[:, half:2 * half]


def lr_match(left, right, max_shift=MAX_SHIFT_PX // STEP):
    """Normalized correlation of the halves' column profiles at the best shift s (grid columns): right[x] ~ left[x + s].
    (None, None) when a profile is flat (a black eye, a loading screen)."""
    pl = left.mean(axis=0)
    pr = right.mean(axis=0)
    n = len(pl)
    if n < 4 or pl.std() < 0.5 or pr.std() < 0.5:
        return None, None
    best_c, best_s = -2.0, 0
    for s in range(-max_shift, max_shift + 1):
        a = pl[max(0, s):n + min(0, s)]
        b = pr[max(0, -s):n - max(0, s)]
        if len(a) < n // 2:
            continue
        a = a - a.mean()
        b = b - b.mean()
        d = math.sqrt(float((a * a).sum()) * float((b * b).sum()))
        c = float((a * b).sum()) / d if d > 1e-9 else 0.0
        if c > best_c:
            best_c, best_s = c, s
    return best_c, best_s


def measure(left, right, prev, match=True):
    """The row values of one sample (prev: the previous sample's (left, right), or None after a pause; match: also
    the left/right profile match, which costs as much as the grab)."""
    l_diff = r_diff = None
    if prev is not None:
        l_diff = float(np.abs(left - prev[0]).mean())
        r_diff = float(np.abs(right - prev[1]).mean())
    corr, shift = lr_match(left, right) if match else (None, None)
    return {"l_mean": float(left.mean()), "r_mean": float(right.mean()), "l_diff": l_diff, "r_diff": r_diff,
            "lr_corr": corr, "lr_shift": None if shift is None else shift * STEP}


def fmt(v, digits=3):
    return "" if v is None else f"{v:.{digits}f}"


# ---------------------------------------------------------------------------------------------------------------
# sampling

def mopic_rect():
    """(left, top, width, height) of the Mopic display, and its refresh rate (gamepilot finds it by its EDID)."""
    sys.path.insert(0, TOOLS)
    import gamepilot
    m = gamepilot.mopic_monitor()
    if m is None:
        raise SystemExit("no Mopic display found")
    refresh = None
    for d in gamepilot.displays():
        if "MPL" in (d.get("monitor") or "").upper() and d.get("refresh_hz"):
            refresh = d["refresh_hz"]
    return (m[0], m[1], m[2] - m[0], m[3] - m[1]), refresh


def strip_rect(display, y_frac, height):
    """The strip across the whole display, `height` px around y_frac of its height (even, so STEP divides it)."""
    l, t, w, h = display
    height = max(STEP, min(h, height - height % STEP))
    top = t + min(h - height, max(0, int(round(h * y_frac - height / 2))))
    return {"left": l, "top": top, "width": w - w % (2 * STEP), "height": height}


def mss_grabber():
    import mss
    sct = mss.MSS()
    return lambda rect: sct.grab(rect).bgra


def read_segments(status_path):
    """The pilot's segments [(start, end or None)] and state, or None while the file is missing or half written."""
    try:
        with open(status_path, encoding="utf-8") as f:
            st = json.load(f)
    except (OSError, ValueError):
        return None
    segs = [(s.get("start_qpc_ns"), s.get("end_qpc_ns")) for s in st.get("segments") or []]
    return {"segments": segs, "state": st.get("state"), "clock": st.get("clock")}


def segment_at(segments, t):
    for i in range(len(segments) - 1, -1, -1):
        start, end = segments[i]
        if start is not None and start <= t and (end is None or t < end):
            return i
    return None


class Sampler:
    """Grabs the strip at `hz` and writes eyes.csv; grab/clock/sleep/alive are replaceable (the self-test drives it
    with synthetic frames on a fake clock)."""

    def __init__(self, out_dir, rect, hz=120, status_path=None, seconds=None, stop_file=None, alive=None, shots=40,
                 shot_threshold=1.0, black_level=8.0, bright_min=20.0, lr_every=12, grab=None, clock=time.perf_counter_ns,
                 sleep=time.sleep):
        self.out_dir, self.rect, self.hz, self.lr_every = out_dir, rect, hz, max(1, lr_every)
        self.status_path, self.seconds, self.stop_file, self.alive = status_path, seconds, stop_file, alive
        self.shots_left, self.shot_threshold = shots, shot_threshold
        self.black_level, self.bright_min = black_level, bright_min
        self.grab, self.clock, self.sleep = grab or mss_grabber(), clock, sleep
        self.meta = {"version": 1, "rect": [rect["left"], rect["top"], rect["width"], rect["height"]], "hz": hz,
                     "step": STEP, "status": status_path, "samples": 0, "shots": [], "stopped_by": None}
        self._status_sig, self._status = None, None

    def read_status(self):
        """read_segments() of the pilot's status file, opened only when os.stat says it changed: the pilot replaces
        the file and that fails while another process has it open (os.stat doesn't hold it)."""
        try:
            st = os.stat(self.status_path)
        except OSError:
            return None
        sig = (st.st_mtime_ns, st.st_size)
        if sig != self._status_sig or self._status is None:
            s = read_segments(self.status_path)
            if s is None:
                return None
            self._status_sig, self._status = sig, s
        return self._status

    def write_meta(self):
        with open(os.path.join(self.out_dir, "eyes-meta.json"), "w", encoding="utf-8") as f:
            json.dump(self.meta, f, indent=2)

    def run(self):
        os.makedirs(self.out_dir, exist_ok=True)
        start = self.clock()
        self.meta["clock"] = {"qpc_ns": start, "unix_ns": time.time_ns()}
        self.meta["started_qpc_ns"] = start
        self.write_meta()   # again at the end; this one is what a killed sampler leaves
        period = int(1e9 / self.hz)
        deadline = start + int(self.seconds * 1e9) if self.seconds else None
        status, next_poll, seg = None, start, None
        prev, prev_t, prev_seg = None, None, None
        last_shot = None
        next_t = start
        w, h = self.rect["width"], self.rect["height"]
        with open(os.path.join(self.out_dir, "eyes.csv"), "w", newline="\n", encoding="utf-8") as f:
            out = csv.writer(f, lineterminator="\n")
            out.writerow(CSV_HEADER)
            last_flush = start
            while True:
                now = self.clock()
                if now >= next_poll:
                    idle = self.status_path and seg is None
                    next_poll = now + int((STATUS_POLL_IDLE_S if idle else STATUS_POLL_S) * 1e9)
                    if deadline and now >= deadline:
                        self.meta["stopped_by"] = "seconds"
                        break
                    if self.stop_file and os.path.exists(self.stop_file):
                        self.meta["stopped_by"] = "stop file"
                        break
                    if self.alive and not self.alive():
                        self.meta["stopped_by"] = "game exited"
                        break
                    if self.status_path:
                        status = self.read_status() or status
                        if status and status["state"] not in (None, "running"):
                            self.meta["stopped_by"] = f"pilot {status['state']}"
                            break
                if self.status_path:
                    seg = segment_at(status["segments"], now) if status else None
                    if seg is None:
                        prev = None
                        self.sleep(0.02)
                        next_t = self.clock()
                        continue
                t0 = self.clock()
                data = self.grab(self.rect)
                t1 = self.clock()
                left, right = eye_lumas(data, h, w)
                if prev is not None and (seg != prev_seg or t0 - prev_t > PAUSE_GAP_S * 1e9):
                    prev = None
                row = measure(left, right, prev, self.meta["samples"] % self.lr_every == 0)
                out.writerow([t0, (t1 - t0) // 1000, -1 if seg is None else seg, fmt(row["l_mean"]), fmt(row["r_mean"]),
                              fmt(row["l_diff"]), fmt(row["r_diff"]), fmt(row["lr_corr"]),
                              "" if row["lr_shift"] is None else row["lr_shift"]])
                self.meta["samples"] += 1
                kind = self.shot_kind(row)
                if kind and self.shots_left > 0 and prev is not None and (last_shot is None or t0 - last_shot > 0.5e9):
                    self.save_shot(t0, kind, prev, (left, right))
                    last_shot = t0
                prev, prev_t, prev_seg = (left, right), t0, seg
                if t1 - last_flush > 1e9:
                    f.flush()
                    last_flush = t1
                next_t += period
                now = self.clock()
                if next_t < now:
                    next_t = now   # behind (a slow grab): no burst to catch up
                else:
                    self.sleep((next_t - now) / 1e9)
        self.meta["ended_qpc_ns"] = self.clock()
        self.write_meta()
        return self.meta

    def shot_kind(self, row):
        thr = self.shot_threshold
        if row["l_mean"] < self.black_level <= self.bright_min <= row["r_mean"]:
            return "left-black"
        if row["r_mean"] < self.black_level <= self.bright_min <= row["l_mean"]:
            return "right-black"
        ld, rd = row["l_diff"], row["r_diff"]
        if ld is not None and rd is not None:
            if ld > 4 * thr and rd < thr / 2:
                return "left-only"
            if rd > 4 * thr and ld < thr / 2:
                return "right-only"
        return None

    def save_shot(self, t, kind, prev, cur):
        from PIL import Image
        img = np.vstack([np.hstack(prev), np.hstack(cur)]).clip(0, 255).astype(np.uint8)
        d = os.path.join(self.out_dir, "eyes-shots")
        os.makedirs(d, exist_ok=True)
        name = f"{t}-{kind}.png"
        Image.fromarray(img, "L").save(os.path.join(d, name))
        self.meta["shots"].append({"t_ns": t, "kind": kind, "file": f"eyes-shots\\{name}"})
        self.shots_left -= 1


# ---------------------------------------------------------------------------------------------------------------
# report

def num(v):
    if v is None or v == "":
        return None
    try:
        x = float(v)
    except ValueError:
        return None
    return None if math.isnan(x) else x


def load_csv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            t = num(r.get("t_ns"))
            if t is None or any(v is None for v in r.values()):
                continue   # a line cut short (a sampler killed between flushes)
            rows.append({"t": int(t), "seg": int(num(r.get("seg")) if num(r.get("seg")) is not None else -1),
                         "grab_us": num(r.get("grab_us")), "l_mean": num(r.get("l_mean")), "r_mean": num(r.get("r_mean")),
                         "l_diff": num(r.get("l_diff")), "r_diff": num(r.get("r_diff")),
                         "lr_corr": num(r.get("lr_corr")), "lr_shift": num(r.get("lr_shift"))})
    rows.sort(key=lambda r: r["t"])
    return rows


def detect(rows, refresh_hz=60.0, threshold=1.0, ratio=4.0, max_lag_ms=500.0, min_lag_ms=None,
           moving_window_ms=500.0, moving_min=3, black_level=8.0, bright_min=20.0, captures=()):
    """Events in the samples (dicts as load_csv gives). An eye "changed" in a sample when its diff exceeds
    `threshold`; a lone change needs the other eye's diff at most `threshold` and `ratio` times smaller. The scene is
    "moving" around a sample when at least `moving_min` samples within +-moving_window_ms changed in both eyes.
    A lag counts from `min_lag_ms` (default 1.5 refreshes: the samples come every ~8 ms, so a lag of one refresh
    measures 8-25 ms and one of two refreshes 25-42 ms); shorter ones are "one_eye_lag_brief".

    Not a lag: a lone change whose follower is a much bigger or smaller change (outside MATCH_RATIO; a small change
    only one eye's strip shows, then the camera moving: "one_eye_change"), and a single small lone change after which
    the scene stands still (dropped). A lagging eye makes the same step later, about as big as the first eye's;
    a frozen eye stays while the other keeps changing. `uniform` on an event: the change was mostly a brightness
    step of the whole strip (auto exposure of one view), not a different frame."""
    refresh_ms = 1000.0 / refresh_hz
    min_lag_ms = 1.5 * refresh_ms if min_lag_ms is None else min_lag_ms
    n = len(rows)
    t = [r["t"] for r in rows]
    intervals = [(t[i] - t[i - 1]) / 1e6 for i in range(1, n) if rows[i]["seg"] == rows[i - 1]["seg"]]
    med_iv = statistics.median(intervals) if intervals else 1000.0 / 120

    def changed(i, eye):
        d = rows[i][eye + "_diff"]
        return d is not None and d > threshold

    def mean_step(k, eye):
        """How far the eye's mean luma moved since the previous sample (|mean step| <= mean abs diff)."""
        if k < 1 or rows[k][eye + "_mean"] is None or rows[k - 1][eye + "_mean"] is None:
            return None
        return abs(rows[k][eye + "_mean"] - rows[k - 1][eye + "_mean"])

    def max_grab(i0, i1):
        g = [rows[k]["grab_us"] for k in range(max(0, i0), min(n, i1 + 1)) if rows[k].get("grab_us") is not None]
        return max(g) / 1000.0 if g else 0.0

    both = [t[i] for i in range(n) if changed(i, "l") and changed(i, "r")]
    win = moving_window_ms * 1e6

    def moving_count(i):
        return bisect.bisect_right(both, t[i] + win) - bisect.bisect_left(both, t[i] - win)

    cap_starts = [c[0] for c in captures]

    def in_capture(a, b):
        k = bisect.bisect_right(cap_starts, b + 50e6)
        return any(captures[j][1] >= a - 50e6 for j in range(max(0, k - 8), k))

    def max_gap(i0, i1):
        g = 0.0
        for k in range(max(1, i0), min(n, i1 + 1)):
            g = max(g, (t[k] - t[k - 1]) / 1e6)
        return g

    def one_black(k):
        """One eye black, the other showing a picture (reported as one_eye_black, below)."""
        if k < 0 or k >= n or rows[k]["l_mean"] is None or rows[k]["r_mean"] is None:
            return None
        lm, rm = rows[k]["l_mean"], rows[k]["r_mean"]
        if lm < black_level and rm >= bright_min:
            return "left"
        if rm < black_level and lm >= bright_min:
            return "right"
        return None

    events = []
    i = 0
    while i < n:
        r = rows[i]
        if one_black(i) or one_black(i - 1):
            i += 1   # going black or coming back is a one-eye change, but it is a black event
            continue
        lone = None
        for a, b in (("l", "r"), ("r", "l")):
            da, db = r[a + "_diff"], r[b + "_diff"]
            if da is not None and db is not None and da > threshold and db <= threshold and da >= ratio * max(db, threshold / ratio):
                lone = (a, b)
        if lone is None:
            i += 1
            continue
        a, b = lone
        mv = moving_count(i)
        j, a_changes, broken = None, 1, False
        k = i + 1
        while k < n and rows[k]["seg"] == r["seg"] and (t[k] - t[i]) / 1e6 <= max_lag_ms:
            if rows[k][b + "_diff"] is None:
                broken = True   # a pause: can't tell when the other eye followed
                break
            if changed(k, b):
                j = k
                break
            if changed(k, a):
                a_changes += 1
            k += 1
        end = j if j is not None else k - 1
        da = r[a + "_diff"]
        follow = rows[j][b + "_diff"] if j is not None else None
        # one small change of one eye and then a still scene: something only that eye's strip shows (a lagging or
        # frozen eye leaves the other one changing on)
        isolated = j is None and a_changes < 2 and da < 5 * threshold
        frozen_ms = None
        if j is None and not broken and not isolated:
            # a frozen eye is one event until it changes again, not one per max_lag_ms
            m = k
            while (m < n and rows[m]["seg"] == r["seg"] and rows[m][b + "_diff"] is not None and not changed(m, b)
                   and (t[m] - t[i]) / 1e6 <= 60e3):
                m += 1
            end = max(end, m - 1)
            frozen_ms = round((t[min(m, n - 1)] - t[i]) / 1e6, 1)
        if mv >= moving_min and not broken and not isolated:
            lag = (t[j] - t[i]) / 1e6 if j is not None else None
            matched = j is not None and da / MATCH_RATIO <= follow <= da * MATCH_RATIO
            gap = max_gap(i, end)
            grab = max_grab(i, end)
            step = mean_step(i, a)
            kind = ("one_eye_only" if lag is None else "one_eye_change" if not matched
                    else "one_eye_lag" if lag > min_lag_ms else "one_eye_lag_brief")
            events.append({
                "kind": kind,
                "eye": "left" if a == "l" else "right",           # the eye that changed first / alone
                "t_ns": t[i], "seg": r["seg"], "lag_ms": None if lag is None else round(lag, 1),
                "refreshes": None if lag is None else round(lag / refresh_ms, 1),
                "first_changes": a_changes, "diff": round(da, 2), "other_diff": round(r[b + "_diff"], 2),
                "follow_diff": None if follow is None else round(follow, 2), "frozen_ms": frozen_ms,
                "uniform": step is not None and step >= 0.8 * da,
                "moving": mv, "gap_ms": round(gap, 1), "grab_ms": round(grab, 1),
                # a stall inside it, or a grab so slow its picture could be from either side of a refresh
                "uncertain": gap > max(2.5 * med_iv, refresh_ms * 1.5) or grab > refresh_ms / 2,
                "pilot_capture": bool(captures) and in_capture(t[i], t[end])})
        i = max(i + 1, end + 1) if (j is not None or not broken) else i + 1

    # one eye black while the other shows a picture, consecutive samples of one segment grouped
    cur = None
    for k in range(n + 1):
        eye = one_black(k)
        ends = cur is not None and (k == n or eye != cur["eye"] or rows[k]["seg"] != cur["seg"])
        if ends:
            dur = (t[cur["last"]] - t[cur["first"]]) / 1e6 + med_iv
            events.append({"kind": "one_eye_black", "eye": cur["eye"], "t_ns": t[cur["first"]], "seg": cur["seg"],
                           "samples": cur["last"] - cur["first"] + 1, "ms": round(dur, 1),
                           "refreshes": round(dur / refresh_ms, 1), "gap_ms": round(max_gap(cur["first"], cur["last"]), 1),
                           "pilot_capture": bool(captures) and in_capture(t[cur["first"]], t[cur["last"]])})
            cur = None
        if eye and cur is None:
            cur = {"eye": eye, "first": k, "last": k, "seg": rows[k]["seg"]}
        elif eye and cur:
            cur["last"] = k
    events.sort(key=lambda e: e["t_ns"])
    return events, {"median_interval_ms": round(med_iv, 2), "moving": [moving_count(i) >= moving_min for i in range(n)]}


def segments_from_status(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            st = json.load(f)
    except (OSError, ValueError):
        return None, None
    return [(s.get("label"), s.get("start_qpc_ns"), s.get("end_qpc_ns")) for s in st.get("segments") or []], st


def load_captures(path):
    out = []
    try:
        with open(path, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                a, b = num(r.get("start_qpc_ns")), num(r.get("end_qpc_ns"))
                if a is not None and b is not None:
                    out.append((int(a), int(b)))
    except OSError:
        pass
    return sorted(out)


def local_time(clock, t_ns):
    if not clock or clock.get("qpc_ns") is None or clock.get("unix_ns") is None:
        return None
    unix = clock["unix_ns"] + (t_ns - clock["qpc_ns"])
    return datetime.datetime.fromtimestamp(unix / 1e9).strftime("%H:%M:%S.%f")[:-3]


def report(path, threshold=1.0, refresh_hz=None, max_lag_ms=500.0, keep_events=200):
    """eyes-report.json for a run folder (or an eyes.csv): {"summary", "events", "line"}."""
    run_dir = path if os.path.isdir(path) else os.path.dirname(os.path.abspath(path))
    csv_path = os.path.join(path, "eyes.csv") if os.path.isdir(path) else path
    meta = {}
    try:
        with open(os.path.join(run_dir, "eyes-meta.json"), encoding="utf-8") as f:
            meta = json.load(f)
    except (OSError, ValueError):
        pass
    rows = load_csv(csv_path)
    segs, status = segments_from_status(os.path.join(run_dir, "pilot-status.json"))
    if segs:
        # samples taken after a segment closed but before the sampler saw it (it polls the status 4 times a second
        # while sampling)
        def inside(r):
            if r["seg"] < 0 or r["seg"] >= len(segs):
                return True
            _, a, b = segs[r["seg"]]
            return (a is None or r["t"] >= a) and (b is None or r["t"] < b)
        rows = [r for r in rows if inside(r)]
    refresh_hz = refresh_hz or meta.get("refresh_hz") or 60.0
    captures = load_captures(os.path.join(run_dir, "pilot", "captures.csv"))
    events, info = detect(rows, refresh_hz=refresh_hz, threshold=threshold, max_lag_ms=max_lag_ms, captures=captures)
    clock = meta.get("clock") or (status or {}).get("clock")
    for e in events:
        e["time"] = local_time(clock, e["t_ns"])
        if segs and 0 <= e["seg"] < len(segs):
            e["label"] = segs[e["seg"]][0]
            if segs[e["seg"]][1] is not None:
                e["seg_s"] = round((e["t_ns"] - segs[e["seg"]][1]) / 1e9, 2)

    n = len(rows)
    by_seg = {}
    for r in rows:
        by_seg.setdefault(r["seg"], []).append(r["t"])
    measured_s = sum((ts[-1] - ts[0]) / 1e9 for ts in by_seg.values() if len(ts) > 1)
    corr = [r["lr_corr"] for r in rows if r["lr_corr"] is not None]
    shift = [r["lr_shift"] for r in rows if r["lr_shift"] is not None and r["lr_corr"] is not None and r["lr_corr"] >= 0.6]
    corr_p50 = statistics.median(corr) if corr else None
    sbs = None if corr_p50 is None else corr_p50 >= 0.6
    lag = [e for e in events if e["kind"] == "one_eye_lag"]
    brief = [e for e in events if e["kind"] == "one_eye_lag_brief"]
    unmatched = [e for e in events if e["kind"] == "one_eye_change"]
    only = [e for e in events if e["kind"] == "one_eye_only"]
    black = [e for e in events if e["kind"] == "one_eye_black"]
    gaps = []
    for k in range(1, n):
        if rows[k]["seg"] == rows[k - 1]["seg"]:
            gaps.append((rows[k]["t"] - rows[k - 1]["t"]) / 1e6)
    grabs = sorted(r["grab_us"] / 1000.0 for r in rows if r.get("grab_us") is not None)
    refresh_ms = 1000.0 / refresh_hz
    summary = {
        "samples": n, "rate_hz": round(1000.0 / info["median_interval_ms"], 1) if n > 1 else None,
        "measured_s": round(measured_s, 1), "segments": len([s for s in by_seg if s >= 0]),
        "refresh_hz": refresh_hz, "threshold": threshold,
        "moving_pct": round(100.0 * sum(info["moving"]) / n, 1) if n else None,
        "one_eye_lag": {"count": len(lag), "left_first": sum(e["eye"] == "left" for e in lag),
                        "right_first": sum(e["eye"] == "right" for e in lag),
                        "max_ms": max((e["lag_ms"] for e in lag), default=None),
                        "uncertain": sum(e["uncertain"] for e in lag), "uniform": sum(e["uniform"] for e in lag),
                        "brief": len(brief), "unmatched": len(unmatched)},
        "one_eye_only": {"count": len(only), "left": sum(e["eye"] == "left" for e in only),
                         "right": sum(e["eye"] == "right" for e in only), "uncertain": sum(e["uncertain"] for e in only),
                         "uniform": sum(e["uniform"] for e in only)},
        "one_eye_black": {"count": len(black), "left": sum(e["eye"] == "left" for e in black),
                          "right": sum(e["eye"] == "right" for e in black),
                          "max_ms": max((e["ms"] for e in black), default=None)},
        "lr": {"corr_p50": None if corr_p50 is None else round(corr_p50, 3),
               "shift_p50_px": None if not shift else statistics.median(shift), "sbs": sbs},
        "gaps": {"max_ms": round(max(gaps), 1) if gaps else None,
                 "over_2_refreshes": sum(g > 2 * refresh_ms for g in gaps)},
        # how long a grab took: its picture is from somewhere inside it (a slow one blurs the lag times)
        "grab_ms": {"p50": round(grabs[len(grabs) // 2], 2), "p95": round(grabs[int(len(grabs) * 0.95)], 2),
                    "max": round(grabs[-1], 2)} if grabs else None,
        "shots": len(meta.get("shots") or []), "stopped_by": meta.get("stopped_by"),
        "first_events": events[:5],
    }
    if n == 0:
        line = "eyes: no samples" + (f" (sampler stopped: {meta.get('stopped_by')})" if meta.get("stopped_by") else "")
    else:
        lag_max = summary["one_eye_lag"]["max_ms"]
        segs_text = f"{summary['segments']} segment(s)" if summary["segments"] else "no segments"
        line = (f"eyes[{summary['rate_hz']} Hz, {segs_text} {summary['measured_s']} s, moving "
                f"{summary['moving_pct']:.0f}%]: one-eye lag {len(lag)}"
                + (f" (max {lag_max:.0f} ms = {lag_max / refresh_ms:.1f} refreshes)" if lag_max is not None else "")
                + (f" + {len(brief)} of 1 refresh" if brief else "")
                + (f" + {len(unmatched)} unmatched" if unmatched else "")
                + f", one-eye only {len(only)}, one-eye black {len(black)}"
                + (f" (max {summary['one_eye_black']['max_ms']:.0f} ms)" if black else ""))
        uniform = summary["one_eye_lag"]["uniform"] + summary["one_eye_only"]["uniform"]
        if uniform:
            line += f" | {uniform} brightness step(s)"
        if corr_p50 is not None:
            line += f" | L/R corr {corr_p50:.2f}"
            if shift:
                line += f" shift {summary['lr']['shift_p50_px']:.0f} px"
        if sbs is False:
            line += " | WARN: not side by side (monado-service without MOPIC_MODE=sbs?)"
        if summary["gaps"]["over_2_refreshes"]:
            line += f" | {summary['gaps']['over_2_refreshes']} sampler gaps > 2 refreshes"
        if summary["grab_ms"] and summary["grab_ms"]["p95"] > refresh_ms / 2:
            line += f" | slow grabs (p95 {summary['grab_ms']['p95']:.0f} ms)"
    doc = {"summary": summary, "events": events[:keep_events], "line": line, "csv": csv_path}
    with open(os.path.join(run_dir, "eyes-report.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
    return doc


# ---------------------------------------------------------------------------------------------------------------

def process_alive_fn(pid):
    sys.path.insert(0, TOOLS)
    import gamepilot
    return lambda: gamepilot.process_alive(pid)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sample", help="sample the Mopic display into <out>\\eyes.csv")
    s.add_argument("--out", required=True, help="folder for eyes.csv, eyes-meta.json, eyes-shots\\ (the run folder)")
    s.add_argument("--status", default="", help="gamepilot's status file: sample only while a measured segment is open")
    s.add_argument("--pid", type=int, default=0, help="stop when this process (the game) is gone")
    s.add_argument("--seconds", type=float, default=0, help="stop after this long (0: no limit)")
    s.add_argument("--stop-file", default="", help="stop when this file exists")
    s.add_argument("--hz", type=float, default=120)
    s.add_argument("--y", type=float, default=0.5, help="the strip's middle, as a fraction of the display height")
    s.add_argument("--height", type=int, default=96, help="strip height in display px")
    s.add_argument("--rect", default="", help="left,top,width,height instead of the Mopic display")
    s.add_argument("--shots", type=int, default=40, help="at most this many candidate PNGs")
    s.add_argument("--lr-every", type=int, default=12, help="the left/right match on every Nth sample (it costs as much as a grab)")
    s = sub.add_parser("report", help="find one-eye changes and black eyes in a run folder's eyes.csv")
    s.add_argument("path")
    s.add_argument("--threshold", type=float, default=1.0, help="an eye changed when its mean luma diff exceeds this")
    s.add_argument("--refresh-hz", type=float, default=0, help="default: the Mopic display's, from eyes-meta.json (60)")
    s.add_argument("--max-lag-ms", type=float, default=500)
    s.add_argument("--print", action="store_true", help="print every event")
    s = sub.add_parser("shot", help="save the strip the sampler would read (grayscale, left | right)")
    s.add_argument("--out", default="eyes-strip.png")
    s.add_argument("--y", type=float, default=0.5)
    s.add_argument("--height", type=int, default=96)
    s.add_argument("--rect", default="")
    args = ap.parse_args()

    if args.cmd == "report":
        doc = report(args.path, args.threshold, args.refresh_hz or None, args.max_lag_ms)
        if args.print:
            for e in doc["events"]:
                print(json.dumps(e))
        print(doc["line"])
        return

    if args.rect:
        l, t, w, h = (int(v) for v in args.rect.split(","))
        display, refresh = (l, t, w, h), None
    else:
        display, refresh = mopic_rect()
    rect = strip_rect(display, args.y, args.height)
    if args.cmd == "shot":
        from PIL import Image
        left, right = eye_lumas(mss_grabber()(rect), rect["height"], rect["width"])
        Image.fromarray(np.hstack([left, right]).clip(0, 255).astype(np.uint8), "L").save(args.out)
        print(json.dumps(dict(measure(left, right, None), rect=rect, out=os.path.abspath(args.out))))
        return
    sampler = Sampler(args.out, rect, hz=args.hz, status_path=args.status or None, seconds=args.seconds or None,
                      stop_file=args.stop_file or None, alive=process_alive_fn(args.pid) if args.pid else None,
                      shots=args.shots, lr_every=args.lr_every)
    sampler.meta["display"] = list(display)
    sampler.meta["refresh_hz"] = refresh
    meta = sampler.run()
    print(json.dumps({"samples": meta["samples"], "stopped_by": meta["stopped_by"], "out": os.path.abspath(args.out)}))


if __name__ == "__main__":
    main()
