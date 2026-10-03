"""Self-test of the frame-rate measurement (no game): gamepilot's measured segments with the game, the screen and the
input stubbed out, and perfreport.py on synthetic run folders (UEVR perf-frames.csv / perf.csv, monado-frames.csv,
PresentMon, nvidia-smi files in the formats their writers use) whose numbers are known. UEVR's file names, headers
and flag bits and monado's header are read from their sources (PerfLog.cpp / PerfLog.hpp, comp_window_mopic.c), so a
change on either side that perfreport.py (or run-test.ps1) doesn't follow fails here."""
import datetime
import json
import os
import random
import re
import shutil
import subprocess
import sys

TOOLS = r"C:\Users\zzong\source\repos\UEVR-jh\tools\mopic-test"
REPO = os.path.dirname(os.path.dirname(TOOLS))
MONADO_SOURCE = r"C:\Users\zzong\source\repos\monado\src\xrt\compositor\main\comp_window_mopic.c"
sys.path.insert(0, TOOLS)
import gamepilot  # noqa: E402
import perfreport  # noqa: E402

work = sys.argv[1]
os.makedirs(work, exist_ok=True)
fails = 0


def check(name, ok, detail=""):
    global fails
    print(("ok    " if ok else "FAIL  ") + name + ("" if ok else f"  {detail}"))
    fails += 0 if ok else 1


def near(a, b, tol):
    return a is not None and b is not None and abs(a - b) <= tol


def load_json(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


# ---------------------------------------------------------------------------------------------------------------
# gamepilot: segments, captures, phases, per-step seeds (the game, the screen and all input stubbed)

from PIL import Image  # noqa: E402

pressed = []      # (step, key)
current = {"pilot": None}
gamepilot.press = lambda name, hold_ms=80: pressed.append((current["pilot"].status["step"], name))
gamepilot.focus = lambda hwnd: True
gamepilot.find_window = lambda proc: 1234
gamepilot.recipe_grab = lambda hwnd, recipe, source=None: (Image.new("RGB", (64, 36)), {})


def make_pilot(name, steps, matches=None):
    recipe = os.path.join(work, f"{name}.json")
    with open(recipe, "w", encoding="utf-8") as f:
        json.dump({"game": "t", "process": "t", "source": "window", "checkpoints": {"cp": {"image": "x.png", "region": [0, 0, 4, 4]}},
                   "steps": steps}, f)
    out = os.path.join(work, f"pilot-{name}")
    shutil.rmtree(out, ignore_errors=True)
    p = gamepilot.Pilot("mopicselftest_noproc", recipe, out, os.path.join(work, f"status-{name}.json"))
    p.game_running = lambda: True
    p.window = lambda: 1234
    p.input_ready = lambda strict=True: 1234
    calls = {"n": 0}

    def m(cp, img):
        calls["n"] += 1
        return matches(calls["n"]) if matches else True
    p.matches = m
    current["pilot"] = p
    return p, out


steps = [{"phase": "play"}, {"wait": 0.3, "measure": True}, {"play": 0.5, "keys": "a b c d", "every": 0.05},
         {"play": 0.2, "keys": "a b", "every": 0.05, "measure": False},
         {"play_until": "cp", "keys": "w s", "timeout": 5, "every": 0.05, "note": "a comment"},
         {"wait": 0.1, "measure": "movie"}, {"note": "x"}, {"key": "enter"}]
p, out = make_pilot("seg", steps, matches=lambda n: n >= 3)
before = gamepilot.qpc_ns()
ok = p.run(1, len(steps))
after = gamepilot.qpc_ns()
st = load_json(os.path.join(work, "status-seg.json"))
segs = st["segments"]
check("pilot: run ok", ok and st["state"] == "done", st.get("error"))
check("pilot: 4 segments (wait measure, play, play_until with a note, movie wait), not the measure:false play / note / key",
      [s["label"] for s in segs] == ["gameplay", "gameplay", "gameplay", "movie"] and [s["step"] for s in segs] == [2, 3, 5, 6],
      [(s["label"], s["step"]) for s in segs])
check("pilot: segments closed, ok, in order, inside the run",
      all(s["ok"] and before < s["start_qpc_ns"] < s["end_qpc_ns"] < after for s in segs)
      and all(segs[k]["end_qpc_ns"] <= segs[k + 1]["start_qpc_ns"] for k in range(len(segs) - 1)), segs)
check("pilot: segment lengths (wait 0.3 s, play 0.5 s)", near((segs[0]["end_qpc_ns"] - segs[0]["start_qpc_ns"]) / 1e9, 0.3, 0.15)
      and near((segs[1]["end_qpc_ns"] - segs[1]["start_qpc_ns"]) / 1e9, 0.5, 0.2), segs)
check("pilot: play's trailing screenshot is not in its segment (0 captures); play_until's 3 grabs are",
      segs[1]["captures"] == 0 and segs[2]["captures"] == 3 and segs[0]["captures"] == 0, [s["captures"] for s in segs])
cap = perfreport.Table(os.path.join(out, "captures.csv"))
rows = [(int(r[0]), int(r[1]), int(r[2])) for r in cap.rows]
check("pilot: captures.csv has play_until's 3 spans under segment 2, inside it",
      len(rows) == 3 and all(r[0] == 2 and segs[2]["start_qpc_ns"] <= r[1] <= r[2] <= segs[2]["end_qpc_ns"] for r in rows), rows)
check("pilot: phases with QPC times", st["phases"] and st["phases"][0]["name"] == "play" and st["phases"][0]["step"] == 1
      and before < st["phases"][0]["qpc_ns"] < segs[0]["start_qpc_ns"], st["phases"])
clock = st["clock"]
check("pilot: clock pair (QPC ns ~ perf_counter_ns, Unix ns now, frequency)",
      before - 10e9 < clock["qpc_ns"] < before and abs(clock["unix_ns"] / 1e9 - st["started"]) < 5 and clock["qpc_freq"] > 0, clock)
check("pilot: displays listed (one is the Mopic display), a segment has power and cpu time",
      isinstance(st["displays"], list) and any("MPL" in (d.get("monitor") or "") for d in st["displays"])
      and segs[0]["power"] is not None and segs[1]["cpu_s"] is not None, (st["displays"], segs[0]["power"]))


def keys_of(step, n):
    return [k for s_, k in pressed if s_ == step][:n]


run1 = {3: keys_of(3, 5), 5: keys_of(5, 2)}
pressed.clear()
p, out = make_pilot("seg", steps, matches=lambda n: n >= 3)
p.run(1, len(steps))
run2 = {3: keys_of(3, 5), 5: keys_of(5, 2)}
r3, r5 = random.Random("seg.json:3"), random.Random("seg.json:5")
expect = {3: [r3.choice("a b c d".split()) for _ in range(5)], 5: [r5.choice(["w", "s"]) for _ in range(2)]}
check("pilot: each step's keys come from Random('<recipe>:<step>'), the same in a second run",
      run1 == expect and run2 == expect, (run1, run2, expect))

p, out = make_pilot("segfail", [{"play_until": "cp", "timeout": 0.3, "every": 0.05}], matches=lambda n: False)
p.run()
st = load_json(os.path.join(work, "status-segfail.json"))
check("pilot: a play_until that times out closes its segment as not ok", st["state"] == "failed"
      and len(st["segments"]) == 1 and st["segments"][0]["ok"] is False and st["segments"][0]["end_qpc_ns"], st["segments"])

for step, want in (({"play": 1}, "gameplay"), ({"play_until": "x"}, "gameplay"), ({"play": 1, "measure": False}, None),
                   ({"wait": 1}, None), ({"wait": 1, "measure": True}, "gameplay"), ({"wait": 1, "measure": "movie"}, "movie"),
                   ({"phase": "play", "measure": True}, None), ({"wait_for": "x", "measure": " bench "}, "bench"),
                   ({"play": 1, "note": "n"}, "gameplay"), ({"wait": 1, "measure": " "}, None)):
    check(f"measure_label {step} -> {want}", p.measure_label(step) == want, p.measure_label(step))
for bad in ([1], 1):
    try:
        p.measure_label({"wait": 1, "measure": bad})
        check(f"measure_label refuses {bad!r}", False, "accepted")
    except gamepilot.Failed as e:
        check(f"measure_label refuses {bad!r}", e.kind == "recipe")

# ---------------------------------------------------------------------------------------------------------------
# perfreport on a synthetic injected run, in the formats of UEVR's PerfLog.cpp and monado's comp_window_mopic.c


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def c_string(path, name):
    """A string constant of a C/C++ source, its adjacent literals joined: `constexpr ... NAME = "a" "b";` or
    `#define NAME "a" \\ "b"`. None when the file or the constant isn't there."""
    text = read_text(path)
    m = re.search(r"constexpr[^=;\n]*\b%s\s*=|#define\s+%s\b" % (name, name), text)
    if not m:
        return None
    rest = text[m.end():]
    if m.group(0).startswith("#define"):
        body = []
        for line in rest.split("\n"):
            body.append(line)
            if not line.rstrip().endswith("\\"):
                break
        rest = "\n".join(body)
    else:
        rest = rest[:rest.index(";")]
    return "".join(s.encode("utf-8").decode("unicode_escape") for s in re.findall(r'"((?:[^"\\]|\\.)*)"', rest))


# what UEVR writes, from its source: file names, headers, flag bits (PerfLog.hpp namespace flag)
PERFLOG_CPP = os.path.join(REPO, "src", "mods", "vr", "PerfLog.cpp")
PERFLOG_HPP = os.path.join(REPO, "src", "mods", "vr", "PerfLog.hpp")
SUMMARY_FILE = c_string(PERFLOG_CPP, "SUMMARY_FILE")
FRAMES_FILE = c_string(PERFLOG_CPP, "FRAMES_FILE")
FRAMES_HEADER = (c_string(PERFLOG_CPP, "FRAMES_HEADER") or "").strip("\n")
SUMMARY_HEADER = (c_string(PERFLOG_CPP, "SUMMARY_HEADER") or "").strip("\n")
flag_ns = re.search(r"namespace flag \{(.*?)\n\}", read_text(PERFLOG_HPP), re.S)
UEVR_BITS = {n.lower(): int(b) for n, b in re.findall(r"constexpr uint32_t (\w+) = 1u << (\d+);", flag_ns.group(1) if flag_ns else "")}
UEVR_SHIFTS = {n: int(v) for n, v in re.findall(r"constexpr uint32_t (SYNC_STAGE_SHIFT|METHOD_SHIFT) = (\d+);", flag_ns.group(1) if flag_ns else "")}
MONADO_HEADER = (c_string(MONADO_SOURCE, "MOPIC_AFS_CSV_HEADER") or
                 "qpc_ns,unix_ms,window_ms,pid,exe,mode,presents,new,repeated,no_layer,present_fps,new_fps,gap_avg_ms,"
                 "gap_max_ms,dts_avg_ms,dts_max_ms,dropped,rebased\n").strip("\n")
check("producers: PerfLog.cpp's file names and headers, PerfLog.hpp's flag bits found",
      SUMMARY_FILE and FRAMES_FILE and FRAMES_HEADER.startswith("qpc_ns,kind,") and SUMMARY_HEADER.startswith("qpc_ns,")
      and len(UEVR_BITS) > 8 and len(UEVR_SHIFTS) == 2, (SUMMARY_FILE, FRAMES_FILE, FRAMES_HEADER, UEVR_BITS, UEVR_SHIFTS))
# perfreport.py's short names for two of them
named = {{"framegen_swapchain": "framegen_sc"}.get(n, n): b for n, b in UEVR_BITS.items()}
m = re.search(r"constexpr uint32_t OLDER_FRAME_WINDOW = (\d+);", read_text(PERFLOG_HPP))
check("producers: perfreport.py's older-frame window is PerfLog.hpp's OLDER_FRAME_WINDOW",
      m is not None and int(m.group(1)) == perfreport.OLDER_FRAME_WINDOW, (m and m.group(1), perfreport.OLDER_FRAME_WINDOW))
for args, want in (((5, 0), "newer"), ((11, 10), "newer"), ((10, 10), "same"), ((9, 10), "older"),
                   ((10, 10 + perfreport.OLDER_FRAME_WINDOW), "older"), ((9, 10 + perfreport.OLDER_FRAME_WINDOW), "restart"),
                   ((2, 0xFFFFFFFE), "newer"), ((0xFFFFFFFE, 2), "older"), ((0, 5), "unknown")):
    check(f"frame_order{args} -> {want} (PerfLog.hpp order_frame)", perfreport.frame_order(*args) == want,
          perfreport.frame_order(*args))
check("producers: perfreport.py's flag bits, sync stage and method shifts are PerfLog.hpp's",
      named == perfreport.FLAG_BITS and UEVR_SHIFTS == {"SYNC_STAGE_SHIFT": perfreport.SYNC_STAGE_SHIFT, "METHOD_SHIFT": perfreport.METHOD_SHIFT},
      (named, perfreport.FLAG_BITS, UEVR_SHIFTS))
harness = read_text(os.path.join(TOOLS, "run-test.ps1"))
reporter = read_text(os.path.join(TOOLS, "perfreport.py"))
check(f"producers: run-test.ps1 copies and perfreport.py reads UEVR's {SUMMARY_FILE} and {FRAMES_FILE}",
      all(f'"{n}"' in harness and f'"{n}"' in reporter for n in (SUMMARY_FILE, FRAMES_FILE)), (SUMMARY_FILE, FRAMES_FILE))
missing = [c for c in perfreport.FRAMES_COLUMNS if c not in FRAMES_HEADER.split(",")]
check("producers: every perf-frames.csv column perfreport.py reads is in PerfLog.cpp's FRAMES_HEADER", not missing, missing)
missing = [c for c in perfreport.ONEHZ_COLUMNS if c not in SUMMARY_HEADER.split(",")]
check("producers: every perf.csv column perfreport.py reads is in PerfLog.cpp's SUMMARY_HEADER", not missing, missing)
missing = [c for c in perfreport.MONADO_COLUMNS if c not in MONADO_HEADER.split(",")]
check(f"producers: every monado column perfreport.py reads is in app_frame_stats.csv's header"
      f"{'' if os.path.exists(MONADO_SOURCE) else ' (monado source not found, the known header)'}", not missing, missing)

MS = 1_000_000
S = 1_000_000_000_000_000          # QPC ns where the measured segment starts
U0 = 1_790_000_000_000_000_000     # Unix ns of QPC T0
T0 = S - 10_000 * MS
XR = 5_000_000_000                 # XrTime = QPC ns - this
P60, P30 = 16_666_667, 33_333_333
BIT = {n: 1 << b for n, b in perfreport.FLAG_BITS.items()}
VERY_LATE = 2 << perfreport.SYNC_STAGE_SHIFT
BASE = BIT["d3d12"] | BIT["hmd"] | BIT["focused"] | BIT["foreground"] | BIT["nsf"] | BIT["nsf_active"] | VERY_LATE
PM_HEADER = ("Application,ProcessID,SwapChainAddress,PresentRuntime,SyncInterval,PresentFlags,AllowsTearing,PresentMode,"
             "FrameType,MsBetweenSimulationStart,MsBetweenPresents,MsBetweenDisplayChange,MsInPresentAPI,MsRenderPresentLatency,"
             "MsUntilDisplayed,CPUStartQPC,MsBetweenAppStart,MsCPUBusy,MsCPUWait,MsGPULatency,MsGPUTime,MsGPUBusy,MsGPUWait")


def unix_of(q):
    return U0 + (q - T0)


def local(q, fmt):
    return datetime.datetime.fromtimestamp(unix_of(q) / 1e9).strftime(fmt)


def write(path, lines, final_newline=True):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + ("\n" if final_newline else ""))


def make_run(name):
    d = os.path.join(work, "runs", name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def row_of(header, values):
    """A CSV row in the header's column order; columns not given are 0."""
    return ",".join(str(values.get(h, 0)) for h in header.split(","))


def frame_row(q, kind, display, period, wait_us, flags, nsf, frame, ticks=1):
    return row_of(FRAMES_HEADER, {
        "qpc_ns": q, "kind": kind, "frame": frame, "display_time": display, "period_ns": period, "end_us": 300,
        "wait_us": wait_us, "callsite": "vr_very_late_post_present", "nsf": nsf, "flags": flags, "presents": 1,
        "present_us": 1000, "ticks": ticks, "tick_us": 5000 if ticks else 0, "uevr_gt_us": 300, "uevr_rt_us": 700,
        "rt_blocked_us": 200})


def summary_row(q, **kw):
    v = {"qpc_ns": q, "unix_ms": unix_of(q) // MS, "window_ms": "1000.000", "engine_delta_ms": "950.000",
         "callsite": "vr_very_late_post_present", "flags": BASE, "sync_stage": 2, "method": 0, "xr_pair_qpc_ns": q,
         "xr_pair_time": q - XR}
    v.update(kw)
    return row_of(SUMMARY_HEADER, v)


def summary_rows(frames, first, last, **overrides):
    """perf.csv rows at S + k s for k in [first, last), counted from the frames ((qpc, kind, ...) tuples) in each
    (q - 1 s, q]; overrides: {"<k>": {column: value}}."""
    out = [SUMMARY_HEADER]
    for k in range(first, last):
        q = S + k * 1000 * MS
        n = {kind: sum(1 for f in frames if q - 1000 * MS < f[0] <= q and f[1] == kind) for kind in "PREF"}
        v = {"xr_frames": sum(n.values()), "xr_new": n["P"], "xr_repeat": n["R"], "xr_empty": n["E"], "xr_fail": n["F"],
             "wait_calls": n["P"], "wait_ok": n["P"]}
        v.update(overrides.get(str(k), {}))
        out.append(summary_row(q, **v))
    return out


run = make_run("20261002-120000-Custom-perf-r1")
E = S + 70_000 * MS
frames = []   # (qpc, kind, period, wait_us, flags, nsf)
for k in range(1, 40):                        # 0-4 s: monado's collapsed period estimate (6 refreshes)
    frames.append((S + k * 100 * MS, "P", 100 * MS, 8000, BASE, 1))
late_q = set()
for k in range(1800):                         # 4-34 s: 60 fps, every 10th frame submitted after its display time
    q = S + 4000 * MS + k * P60
    frames.append((q, "P", P60, 8000, BASE, 2 if k % 50 == 0 else 1))
    if k % 10 == 0:
        late_q.add(q)
for k in range(600):                          # 34-44 s: the UEVR menu is open
    frames.append((S + 34000 * MS + k * P60, "P", P60, 8000, BASE | BIT["menu"], 1))
skip = {181, 182} | set(range(331, 339))      # 44-64 s: 30 fps on Streamline's swapchain, a 100 ms hitch, a 300 ms stall
for k in range(600):
    if k not in skip:
        frames.append((S + 44000 * MS + k * P30, "P", P30, 10000, BASE | BIT["framegen_sc"], 1))
for k in range(360):                          # 64-70 s: loading (empty submits, no ticks)
    frames.append((S + 64000 * MS + k * P60, "E", 0, 0, BASE, 0))
lines = [FRAMES_HEADER]
for i, (q, kind, period, wait, flags, nsf) in enumerate(frames):
    disp = (q - 1 * MS if q in late_q else q + 20 * MS) - XR
    lines.append(frame_row(q, kind, disp, period, wait, flags, nsf, 0 if kind == "E" else i + 1, 0 if kind == "E" else 1))
lines.append(f"{E + 1000},P,99")              # torn last row (the writer killed in the middle of it)
write(os.path.join(run, "perf-frames.csv"), lines, final_newline=False)
# one second without any xrEndFrame in the loading stretch (left out, still counted as such)
write(os.path.join(run, "perf.csv"), summary_rows(frames, -5, 80, **{"66": {"xr_frames": 0, "xr_empty": 0}}))

# PresentMon: the game presents at 120/s (60 simulated) on its main swap chain, a few presents on UEVR's dummy one;
# monado-service at 60/s
pm = [PM_HEADER]
q = S
while q < E:
    pm.append(f"testgame.exe,4242,0x000001A0,DXGI,0,512,1,Hardware: Independent Flip,Application,16.667,8.333,NA,0.20,5.0,NA,"
              f"{q // 100},8.333,6.0,2.3,1.0,7.0,7.5,0.8")
    q += 8_333_333
for k in range(10):
    pm.append(f"testgame.exe,4242,0x000001B0,DXGI,0,0,0,Composed: Flip,Application,NA,500.0,NA,0.1,1.0,NA,{(S + k * 500 * MS) // 100},1,1,1,1,1,1,1")
write(os.path.join(run, "presentmon-testgame.exe-4242.csv"), pm)
pm = [PM_HEADER]
q = S
while q < E:
    pm.append(f"monado-service.exe,777,0x000002A0,Other,1,0,0,Hardware: Independent Flip,Application,NA,16.667,16.667,0.5,2.0,3.0,"
              f"{q // 100},16.667,1,1,1,1,1,1")
    q += P60
write(os.path.join(run, "presentmon-monado-service.exe-777.csv"), pm)

# monado: one row per second (the window ends at qpc_ns). A new frame on every refresh at 60 fps, every other one at
# 30 fps, one frame dropped; another client's rows (blender) at the same times are not the game's
mon = [MONADO_HEADER]
for k in range(70):
    q = S + (k + 1) * 1000 * MS
    new = 60 if 4 <= k < 34 else 30 if 44 <= k < 64 else 0
    gap = 300.0 if k == 54 else 16.7 if new == 60 else 33.3 if new else 0
    mon.append(f"{q},{unix_of(q) // MS},1000.000,4242,testgame.exe,sbs,60,{new},{60 - new},0,60.00,{new:.2f},0,{gap},0,0,"
               f"{1 if k == 50 else 0},0")
    mon.append(f"{q + 3 * MS},{unix_of(q) // MS},1000.000,999,blender.exe,sbs,60,0,60,0,60.00,0.00,0,0,0,0,7,0")
write(os.path.join(run, "monado-frames.csv"), mon)

gpu = ["timestamp, utilization.gpu [%], clocks.current.graphics [MHz], clocks.current.memory [MHz], temperature.gpu, "
       "power.draw [W], enforced.power.limit [W], pstate, clocks_event_reasons.active"]
for k in range(-12, 75):
    q = S + k * 1000 * MS
    gpu.append(f"{local(q, '%Y/%m/%d %H:%M:%S.%f')[:-3]}, 95, 2100, 9000, {70 + (k % 10)}, 120.50, 160.00, P0, 0x0000000000000005")
gpu.append("2026/10/02 12:")                  # torn by the kill
write(os.path.join(run, "gpu.csv"), gpu, final_newline=False)

write(os.path.join(run, "game-settings", "GameUserSettings.ini"),
      ["[/Script/Engine.GameUserSettings]", "bUseVSync=False", "FrameRateLimit=0.000000", "ResolutionSizeX=2560",
       "LastConfirmedFullscreenMode=1", "CurrentSelectedFrameGenerationMode = 17", "[ScalabilityGroups]", "sg.ShadowQuality=3"])
write(os.path.join(run, "game-settings", "GameUserSettings.after.ini"),
      ["[/Script/Engine.GameUserSettings]", "bUseVSync=False", "FrameRateLimit=0.000000", "ResolutionSizeX=1920",
       "LastConfirmedFullscreenMode=2", "CurrentSelectedFrameGenerationMode = 17", "[ScalabilityGroups]", "sg.ShadowQuality=3"])
write(os.path.join(run, "config.txt"), ["VR_NativeStereoFix=true", "VR_PerfLog=true", "FrameworkConfig_LogLevel=2"])
write(os.path.join(run, "log.txt"), [f"[{local(S, '%Y-%m-%d %H:%M:%S.%f')[:-3]}] [UnrealVR] [info] [D3D12Hook.cpp:390] "
                                     "Found Streamline (DLSSFG) swapchain during dummy initialization: 1a0"])
write(os.path.join(run, "pilot", "captures.csv"), ["segment,start_qpc_ns,end_qpc_ns", f"0,{S + 50020 * MS},{S + 50050 * MS}",
                                                   f"0,{S + 60000 * MS},{S + 60010 * MS}"])
mopic = {"device": "\\\\.\\DISPLAY2", "adapter": "Intel(R) UHD Graphics", "monitor": "MPL0291", "primary": False,
         "width": 3840, "height": 2160, "refresh_hz": 60}
with open(os.path.join(run, "pilot-status.json"), "w", encoding="utf-8") as f:
    json.dump({"state": "done", "clock": {"qpc_ns": T0, "unix_ns": U0, "qpc_freq": 10_000_000}, "displays": [mopic],
               "updated": (unix_of(E) + 5e9) / 1e9,
               "segments": [{"label": "gameplay", "phase": "play", "step": 5, "start_qpc_ns": S, "end_qpc_ns": E, "ok": True,
                             "captures": 2, "capture_s": 0.04, "cpu_s": 1.5, "window": dict(mopic, window=[-7680, 0, -3840, 2160]),
                             "power": {"ac": True, "battery_pct": 100, "saver": False}},
                            {"label": "gameplay", "phase": "play", "step": 9, "start_qpc_ns": E + 1000 * MS,
                             "end_qpc_ns": None, "ok": None, "captures": 0}]}, f)
# run-test.ps1 writes it with a BOM (Set-Content -Encoding UTF8)
with open(os.path.join(run, "perf-context.json"), "w", encoding="utf-8-sig") as f:
    json.dump({"game": "Custom", "process": "testgame", "label": "perf", "recipe": r"C:\x\recipes\T.json", "recipe_vars": "",
               "injected": True, "dll_sha256": "AAAA", "qpc_freq": 10_000_000, "t0": {"qpc_ns": T0, "unix_ms": U0 // MS},
               "game_pids": [4242], "power": {"ac": True, "battery_pct": 100, "scheme": "381b4222", "overlay_ac": None},
               "presentmon": {"status": "ok"}, "nvidia_smi": {"status": "ok"},
               "modules": {"nvngx_dlssg": True, "fg_modules": ["nvngx_dlssg.dll", "sl.dlss_g.dll"], "probes": 3}}, f)

doc = perfreport.report(run, do_compare=False)
s = doc["summary"]
vr = s["vr"]
print("  line: " + doc["line"])
check("perf: headline label gameplay, one finished segment (the unfinished one left out with a warning)",
      s["label"] == "gameplay" and s["segments"] == 1 and len(doc["segments"]) == 2
      and any("did not finish" in w for w in doc["warnings"]), (s["label"], s["segments"], doc["warnings"]))
check("perf: settle 4.0 s (6-refresh period until then)", near(s["settle_s"], 4.0, 0.01) and not s["settle_capped"], s["settle_s"])
check("perf: excluded menu 10 s, loading 6 s; measured 50 s", near(s["excluded_s"]["menu"], 10, 0.01)
      and near(s["excluded_s"]["loading"], 6, 0.01) and near(s["measured_s"], 50, 0.01), (s["excluded_s"], s["measured_s"]))
# the window is (start, end]: the frame at the settle point only opens the first interval
check("perf: VR fps = 2389 new frames / 50 s", vr["frames"] == 2389 and near(vr["fps"], 47.78, 0.001), (vr["frames"], vr["fps"]))
check("perf: frame times p50 16.67, p99 33.33 ms, max 300, 1% low 30, no p99.9 (< 10000 intervals)",
      near(vr["frame_ms"]["p50"], 16.67, 0.01) and near(vr["frame_ms"]["p99"], 33.33, 0.01) and near(vr["low1_fps"], 30.0, 0.01)
      and near(vr["frame_ms"]["max"], 300, 0.01) and vr["frame_ms"]["p999"] is None and vr["low01_fps"] is None, vr["frame_ms"])
check("perf: 2 hitches (100 / 300 ms), 1 stall, 2.4 per minute, no interval across the menu",
      vr["hitches"] == 2 and vr["stalls"] == 1 and near(vr["hitch_per_min"], 2.4, 0.001) and vr["hitch_ms"] == 50.0, vr)
check("perf: one hitch overlaps a capture", vr["hitches_in_capture"] == 1, vr["hitches_in_capture"])
check("perf: paced 60 / 40 % at 1 / 2 refreshes, max period 33.3 ms", near(vr["paced_pct"]["1"], 60.0, 0.2)
      and near(vr["paced_pct"]["2"], 40.0, 0.2) and vr["paced_pct"]["3+"] == 0 and near(vr["period_max_ms"], 33.3, 0.05), vr["paced_pct"])
check("perf: work 8.67 ms (very-late sync, no async wait, not capped)", near(vr["work_ms"], 8.67, 0.01) and vr["work_note"] is None,
      (vr["work_ms"], vr["work_note"]))
check("perf: late 179 of 2389 (XrTime placed on QPC with perf.csv's pairs)", near(vr["late_pct"], 179 * 100 / 2389, 0.05), vr["late_pct"])
check("perf: predicted-slot intervals p50 16.67", near(vr["predicted_slot_ms"]["p50"], 16.67, 0.01), vr["predicted_slot_ms"])
check("perf: engine 47.78 ticks/s at 0.95 engine time, fresh = 2389 - 35 NSF reuses", near(vr["engine_fps"], 47.78, 0.001)
      and vr["engine_time_ratio"] == 0.95 and near(vr["fps_fresh"], (2389 - 35) / 50, 0.01)
      and near(vr["nsf_reused_pct"], 35 * 100 / 2389, 0.05), (vr["engine_time_ratio"], vr["fps_fresh"], vr["nsf_reused_pct"]))
check("perf: no repeats, 1 submit and 1 wait per frame, 1 s without submits (in the loading stretch)", vr["r_pct"] == 0
      and vr["submits_per_frame"] == 1.0 and vr["wait_ok_per_frame"] == 1.0 and vr["empty_submits"] == 0
      and vr["no_submit_s"] == 1.0 and vr["ring_dropped"] == 0, vr)
check("perf: perf.csv counts the same new frames (within a second's worth), no warning about it",
      abs(vr["frames_1hz"] - vr["frames"]) <= 60 and not any("perf.csv counted" in w for w in doc["warnings"]),
      (vr["frames_1hz"], vr["frames"], doc["warnings"]))
a = s["attrib"]
check("perf: attribution medians (UEVR's own render-thread time, its blocked time apart) and the 'paced' hint",
      a["tick_ms"] == 5.0 and a["present_ms"] == 1.0 and a["wait_ms"] == 8.0 and a["uevr_rt_ms"] == 0.7
      and a["rt_blocked_ms"] == 0.2 and a["uevr_gt_ms"] == 0.3 and a["presents_per_vr_frame"] == 1.0 and a["bound"] == "paced", a)
m = s["mode"]
check("perf: mode d3d12, very-late sync at its callsite, NSF, native stereo, not AFR, foreground 100 %",
      m["api"] == "d3d12" and m["sync"] == "very_late" and m["callsite"] == "vr_very_late_post_present"
      and m["method"] == "native_stereo" and m["nsf"] and m["afr"] is False and m["skip_draw"] is False
      and m["async_wait"] is False and m["framegen_sc"] is True and m["dlssg"] is False and m["foreground_pct"] == 100.0, m)
fl = s["flat"]
check("perf: game Presents 120 fps on the main swap chain, sim 60, 2.0 per simulated frame, GPU busy 7.5",
      near(fl["fps"], 120.0, 0.05) and near(fl["sim_fps"], 60.0, 0.05) and fl["presents_per_sim"] == 2.0
      and fl["gpu_busy_ms"] == 7.5 and fl["present_mode"] == "Hardware: Independent Flip" and fl["frame_types"] == {"Application": 100.0}, fl)
check("perf: compositor 60 presents/s", near(s["compositor"]["present_fps"], 60.0, 0.05), s["compositor"])
d = s["display"]
check("perf: monado: 50 measured seconds of the game's session, a new frame on 80 % of 3000 refreshes (48/s), 1 dropped",
      d["s"] == 50.0 and d["fps"] == 60.0 and d["new_fps"] == 48.0 and d["new_pct"] == 80.0 and d["dropped"] == 1
      and d["new_gap_max_ms"] == 300.0 and d["mode"] == "sbs" and d["uevr_vs_display_pct"] == -0.5, d)
g = s["gpu"]
check("perf: GPU 95 %, 2100 MHz, max 79 C, 120.5/160 W, power cap (idle bit ignored)", g["util_p50"] == 95 and g["clock_mhz_p50"] == 2100
      and g["temp_c_max"] == 79 and g["power_w_p50"] == 120.5 and g["power_limit_w"] == 160 and g["limits"] == ["sw_power_cap"], g)
fg = s["framegen"]
check("perf: one Present per engine tick, no older-frame resubmits, nothing reclassified", vr["presents_per_tick"] == 1.0
      and vr["older_submits"] == 0 and vr["older_pct"] == 0.0 and vr["nsf_snapshot_pct"] is None
      and not any("reclassif" in w or "counted as R" in w for w in doc["warnings"]), (vr["presents_per_tick"], vr["older_submits"]))
check("perf: frame generation measured (PresentMon's 2.0 presents per simulated frame; UEVR's 1.0 per tick doesn't fire), "
      "the swapchain flag, the module and the log listed after it (never changed)",
      fg["on"] is True and fg["measured"] is True and fg["presents_per_tick"] == 1.0 and len(fg["evidence"]) == 4
      and fg["evidence"][0] == "presentmon: 2.0 presents per simulated frame"
      and fg["setting"] == "CurrentSelectedFrameGenerationMode=17"
      and fg["modules"] == ["nvngx_dlssg.dll", "sl.dlss_g.dll"], fg)
check("perf: not capped (FrameRateLimit 0)", s["capped"]["on"] is False and s["capped"]["limit_fps"] is None, s["capped"])
check("perf: window on the Mopic display (Intel adapter), 60 Hz, AC, balanced", s["window"]["monitor"] == "MPL0291"
      and s["window"]["adapter"].startswith("Intel") and s["refresh_hz"] == 60 and s["power"]["ac"] is True
      and s["power"]["mode"] == "balanced", (s["window"], s["power"]))
st_ = s["settings"]
check("perf: settings fingerprint, changed keys without the volatile ones", st_["changed"] == ["ResolutionSizeX"]
      and len(st_["fingerprint"]) == 16, st_)
check("perf: sources", s["sources"] == {"uevr": "ok", "presentmon": "ok", "monado": "ok", "gpu": "ok"}, s["sources"])
h = s["headline"]
check("perf: headline", h == {"kind": "vr", "fps": 47.78, "low1_fps": 30.0, "hitches": 2, "hitch_per_min": 2.4, "fg": True,
                              "measured_s": 50.0}, h)
check("perf: summary line", doc["line"].startswith("perf[gameplay 50.0s, settle 4.0s, excluded loading 6s menu 10s]: VR 47.8 fps (1% low 30.0, p99 33.3 ms, 2 hitches, 1 stalls)")
      and "paced 60:60% 30:40%" in doc["line"] and "work 8.7 ms" in doc["line"] and "FG on" in doc["line"]
      and "display: new frame on 80% of refreshes (48.0/s), 1 dropped" in doc["line"] and "hint paced" in doc["line"]
      and "game Present 120.0 fps (sim 60.0)" in doc["line"], doc["line"])
json.dumps(doc)  # serializable
runv = make_run("20261002-120050-Custom-volatile-r1")
write(os.path.join(runv, "game-settings", "GameUserSettings.ini"),
      ["[ScalabilityGroups]", "sg.ShadowQuality=3", "[/Script/Engine.GameUserSettings]", "LastConfirmedFullscreenMode=0",
       "Version=9", "CurrentSelectedFrameGenerationMode=17", "ResolutionSizeX=2560", "FrameRateLimit=0.000000", "bUseVSync=False"])
check("perf: the fingerprint ignores volatile keys and the order of lines", perfreport.read_settings(runv)["fingerprint"] == st_["fingerprint"])
write(os.path.join(runv, "game-settings", "GameUserSettings.ini"), ["[/Script/Engine.GameUserSettings]", "bUseVSync=True"])
check("perf: a real change changes it", perfreport.read_settings(runv)["fingerprint"] != st_["fingerprint"])

# the CLI writes perf.json and perf-summary.json (what run-test.ps1 reads) and prints the line
r = subprocess.run([sys.executable, os.path.join(TOOLS, "perfreport.py"), "run", run, "--no-compare"], capture_output=True, text=True)
pj = load_json(os.path.join(run, "perf.json"))
ps = load_json(os.path.join(run, "perf-summary.json"))
check("perf CLI: exit 0, perf.json and perf-summary.json written, the line printed", r.returncode == 0
      and pj["summary"]["vr"]["fps"] == 47.78 and ps["summary"] == pj["summary"] and ps["line"] == pj["line"]
      and r.stdout.strip() == pj["line"], r.stdout + r.stderr)
lower = {}
dup = []


def collect_keys(o):
    if isinstance(o, dict):
        for k, v in o.items():
            lower.setdefault(id(o), set())
            if k.lower() in lower[id(o)]:
                dup.append(k)
            lower[id(o)].add(k.lower())
            collect_keys(v)
    elif isinstance(o, list):
        for v in o:
            collect_keys(v)


collect_keys(ps)
check("perf-summary.json has no keys that differ only in case (PowerShell's ConvertFrom-Json)", not dup, dup)


# variants of the run: work_ms is only valid with the very-late sync on one thread and no frame cap
FRAME_COLS = FRAMES_HEADER.split(",")


def variant(name, flag_fn=None, settings=None, drop_flags=False, callsite=None, keep=None, no_ticks=False, comment=False):
    """The run with its perf-frames.csv flags (or callsite) rewritten, the flags column dropped, rows left out
    (keep(row number) False), ticks zeroed, a "#" comment line before the header, or the game's settings replaced."""
    run2 = make_run(f"20261002-120100-Custom-{name}-r1")
    for f in ("perf.csv", "pilot-status.json", "perf-context.json", "monado-frames.csv"):
        shutil.copy(os.path.join(run, f), os.path.join(run2, f))
    fi, ci = FRAME_COLS.index("flags"), FRAME_COLS.index("callsite")
    out_lines = ["# a comment line before the header"] if comment else []
    with open(os.path.join(run, "perf-frames.csv"), encoding="utf-8") as f:
        source = f.read().split("\n")
    row = 0
    for ln in source:
        parts = ln.split(",")
        if len(parts) != len(FRAME_COLS):
            out_lines.append(ln)
            continue
        if parts[0].isdigit():
            row += 1
            if keep and not keep(row):
                continue
            if flag_fn:
                parts[fi] = str(flag_fn(int(parts[fi])))
            if callsite is not None:
                parts[ci] = str(callsite)
            if no_ticks:
                parts[FRAME_COLS.index("ticks")] = parts[FRAME_COLS.index("tick_us")] = "0"
        if drop_flags:
            del parts[fi]
        out_lines.append(",".join(parts))
    write(os.path.join(run2, "perf-frames.csv"), out_lines)
    if settings:
        write(os.path.join(run2, "game-settings", "GameUserSettings.ini"), ["[/Script/Engine.GameUserSettings]"] + settings)
    doc2 = perfreport.report(run2, do_compare=False)
    return dict(doc2["summary"], all_warnings=doc2["warnings"])


def variant_of(base, name, row_fn):
    """`base` copied to a new run with every perf-frames.csv data row passed through row_fn(fields, columns)."""
    run2 = make_run(f"20261002-120800-Custom-{name}-r1")
    for f in os.listdir(base):
        if os.path.isfile(os.path.join(base, f)) and f not in ("perf.json", "perf-summary.json"):
            shutil.copy(os.path.join(base, f), os.path.join(run2, f))
    cols = FRAMES_HEADER.split(",")
    with open(os.path.join(base, "perf-frames.csv"), encoding="utf-8") as f:
        src = [ln for ln in f.read().split("\n") if ln]
    out_lines = [src[0]]
    for ln in src[1:]:
        parts = ln.split(",")
        row_fn(parts, cols)
        out_lines.append(",".join(parts))
    write(os.path.join(run2, "perf-frames.csv"), out_lines)
    return perfreport.report(run2, do_compare=False)["summary"]


# monado leaves "dropped" empty (counting it would need a change to the upstream multi compositor): unknown, not 0
run3 = make_run("20261002-120200-Custom-nodrop-r1")
for f in ("perf.csv", "perf-frames.csv", "pilot-status.json", "perf-context.json"):
    shutil.copy(os.path.join(run, f), os.path.join(run3, f))
di = MONADO_HEADER.split(",").index("dropped")
with open(os.path.join(run, "monado-frames.csv"), encoding="utf-8") as f:
    mon3 = [ln if k == 0 else ",".join(p if j != di else "" for j, p in enumerate(ln.split(",")))
            for k, ln in enumerate(f.read().split("\n")) if ln]
write(os.path.join(run3, "monado-frames.csv"), mon3)
doc3 = perfreport.report(run3, do_compare=False)
d3 = doc3["summary"]["display"]
check("perf: monado rows with an empty dropped column report dropped as unknown and leave it out of the line",
      d3["dropped"] is None and "dropped" not in doc3["line"] and d3["new_fps"] == 48.0, (d3["dropped"], doc3["line"]))

STAGE_MASK = 3 << perfreport.SYNC_STAGE_SHIFT
s2 = variant("late", lambda f: (f & ~STAGE_MASK) | (1 << perfreport.SYNC_STAGE_SHIFT))
check("perf: work_ms null with the late sync (the wait is on another thread)", s2["vr"]["work_ms"] is None
      and "sync late" in s2["vr"]["work_note"] and near(s2["vr"]["fps"], 47.78, 0.01), (s2["vr"]["work_ms"], s2["vr"]["work_note"]))
s2 = variant("async", lambda f: f | BIT["async_wait"])
check("perf: work_ms null with the async wait thread", s2["vr"]["work_ms"] is None and "async wait" in s2["vr"]["work_note"], s2["vr"]["work_note"])
s2 = variant("callsite", callsite="vr_late_on_present")
check("perf: work_ms null when xrWaitFrame ran elsewhere (vr_late_on_present) though the stage says very late",
      s2["vr"]["work_ms"] is None and "vr_late_on_present" in s2["vr"]["work_note"], s2["vr"]["work_note"])
s2 = variant("callsitenum", callsite=6, comment=True)
check("perf: a numeric callsite (SyncFrameCallsite 6) is vr_very_late_post_present; a '#' line before the header is skipped",
      s2["mode"]["callsite"] == "vr_very_late_post_present" and s2["vr"]["work_ms"] == vr["work_ms"] and s2["vr"]["fps"] == vr["fps"],
      (s2["mode"], s2["vr"]["work_note"]))
s2 = variant("afrcap", lambda f: f | BIT["afr"] | BIT["synced"] | (1 << perfreport.METHOD_SHIFT),
             ["FrameRateLimit=60.000000", "FrameRateLimit_WhenBackgrounded=30.000000"])
check("perf: AFR with FrameRateLimit 60 caps VR at 30: capped, work_ms null, hint capped",
      s2["capped"]["on"] is True and s2["capped"]["vr_limit_fps"] == 30.0 and s2["capped"]["limit_fps"] == 60
      and s2["capped"]["other"] == {"FrameRateLimit_WhenBackgrounded": 30} and s2["vr"]["work_ms"] is None
      and "frame cap" in s2["vr"]["work_note"] and s2["attrib"]["bound"] == "capped" and s2["mode"]["method"] == "synchronized"
      and s2["mode"]["afr"] and s2["mode"]["synced"], s2["capped"])
s2 = variant("mono", lambda f: (f | BIT["mono"]) | (5 << perfreport.METHOD_SHIFT))
check("perf: rendering method 5 is mono", s2["mode"]["method"] == "mono" and s2["mode"]["mono"], s2["mode"])
s2 = variant("lostrows", keep=lambda k: not (100 <= k < 1000 and k % 3 == 0))
check("perf: rows missing from perf-frames.csv (perf.csv counted more): a warning",
      any("perf.csv counted" in w for w in s2["all_warnings"]), s2["all_warnings"])
s2 = variant("noticks", no_ticks=True)
check("perf: a Tick hook that never counted: ticks don't make every second 'loading', no engine fps",
      near(s2["measured_s"], 50, 0.01) and near(s2["excluded_s"]["loading"], 6, 0.01) and s2["vr"]["engine_fps"] is None
      and s2["vr"]["fps"] == vr["fps"], (s2["measured_s"], s2["excluded_s"], s2["vr"]["engine_fps"]))
s2 = variant("cap120", None, ["FrameRateLimit=120.000000"])
check("perf: FrameRateLimit 120 without AFR doesn't bind at 60 Hz", s2["capped"]["on"] is False and s2["capped"]["vr_limit_fps"] == 120.0
      and s2["vr"]["work_ms"] is not None, s2["capped"])
s2 = variant("noflags", drop_flags=True)
check("perf: without a flags column: no mode, work_ms null, no menu exclusion", s2["mode"] is None and s2["vr"]["work_ms"] is None
      and s2["vr"]["work_note"] == "sync mode not logged" and s2["excluded_s"]["menu"] == 0, (s2["mode"], s2["vr"]["work_note"], s2["excluded_s"]))
s2 = variant("unfocused", lambda f: f & ~BIT["focused"])
check("perf: an unfocused session is left out entirely (the menu seconds still count as menu)", s2["vr"] is None
      and near(s2["excluded_s"]["unfocused"], 56.0, 0.01) and near(s2["excluded_s"]["menu"], 10.0, 0.01), s2["excluded_s"])

# a segment whose period never settles: 10 s trimmed, flagged
run3 = make_run("20261002-120200-Custom-unsettled-r1")
lines = [FRAMES_HEADER]
for k in range(1, 300):
    lines.append(frame_row(S + k * 100 * MS, "P", 0, 100 * MS, 1000, BASE, 0, k))
write(os.path.join(run3, "perf-frames.csv"), lines)
shutil.copy(os.path.join(run, "pilot-status.json"), os.path.join(run3, "pilot-status.json"))
s3 = perfreport.report(run3, do_compare=False)["summary"]
check("perf: a period that never settles: 10 s trimmed, settle_capped, 3+ refreshes all the time, max period 100 ms",
      near(s3["settle_s"], 10.0, 0.01) and s3["settle_capped"] == 1 and s3["vr"]["paced_pct"]["3+"] == 100.0
      and s3["vr"]["s_at_3plus"] > 19 and s3["vr"]["period_max_ms"] == 100.0 and near(s3["vr"]["fps"], 10.0, 0.2), (s3["settle_s"], s3["vr"]))

# a 2 s stall inside gameplay stays in (a stall), a 4 s one without rows is left out as no_data: the 3 whole seconds
# (1 s bins from the settle point) it covers, and no interval across it
run4 = make_run("20261002-120300-Custom-stall-r1")
lines = [FRAMES_HEADER]
t = 0
while t < 60_000:
    if not (20_000 <= t < 22_000) and not (40_000 <= t < 44_000):
        lines.append(frame_row(S + t * MS, "P", 0, P60, 1000, BASE, 0, t + 1))
    t += 20
write(os.path.join(run4, "perf-frames.csv"), lines)
with open(os.path.join(run4, "pilot-status.json"), "w", encoding="utf-8") as f:
    json.dump({"clock": {"qpc_ns": T0, "unix_ns": U0, "qpc_freq": 10_000_000},
               "segments": [{"label": "gameplay", "step": 1, "start_qpc_ns": S, "end_qpc_ns": S + 60_000 * MS, "ok": True}]}, f)
s4 = perfreport.report(run4, do_compare=False)["summary"]
check("perf: 2 s gap = a stall inside the measurement, 4 s gap left out as no_data", s4["vr"]["stalls"] == 1
      and near(s4["vr"]["frame_ms"]["max"], 2020, 0.5) and near(s4["excluded_s"]["no_data"], 3, 0.01),
      (s4["vr"]["stalls"], s4["excluded_s"], s4["vr"]["frame_ms"]))

# 2x frame generation (XeFG / DLSS-G): UEVR's Present hook runs twice per engine frame, the second time (no Tick before
# it) resubmitting the previous engine frame with its pose. Written as UEVR wrote it before it counted those as repeats
# (every row P), with the Native Stereo Fix copying live targets for the first 4 s and frozen pairs after that.
run8 = make_run("20261002-120700-Custom-fg-r1")
lines = [FRAMES_HEADER]
engine_frame = 5000
for k in range(2400):                         # 40 s of Presents every 16.7 ms, a new engine frame every other one
    q = S + 100 * MS + k * P60
    if k % 2 == 0:
        engine_frame += 1
    lines.append(row_of(FRAMES_HEADER, {
        "qpc_ns": q, "kind": "P", "frame": engine_frame if k % 2 == 0 else engine_frame - 1, "display_time": q + 20 * MS - XR,
        "period_ns": P60, "end_us": 300, "wait_us": 8000, "callsite": "vr_very_late_post_present", "nsf": 1,
        "nsf_pair": 1 if k < 240 else 2, "flags": BASE, "presents": 1, "dup_presents": k % 2, "present_us": 1000,
        "ticks": 1 - k % 2, "tick_us": 5000 if k % 2 == 0 else 0, "uevr_gt_us": 300, "uevr_rt_us": 700, "rt_blocked_us": 200}))
write(os.path.join(run8, "perf-frames.csv"), lines)
# perf.csv of that UEVR counted every one of those submits as new
write(os.path.join(run8, "perf.csv"), summary_rows([(S + 100 * MS + k * P60, "P") for k in range(2400)], -1, 42))
with open(os.path.join(run8, "pilot-status.json"), "w", encoding="utf-8") as f:
    json.dump({"clock": {"qpc_ns": T0, "unix_ns": U0, "qpc_freq": 10_000_000},
               "segments": [{"label": "gameplay", "step": 1, "start_qpc_ns": S, "end_qpc_ns": S + 40_000 * MS, "ok": True}]}, f)
with open(os.path.join(run8, "perf-context.json"), "w", encoding="utf-8") as f:
    json.dump({"injected": True, "qpc_freq": 10_000_000, "t0": {"qpc_ns": T0, "unix_ms": U0 // MS},
               "modules": {"nvngx_dlssg": True, "fg_modules": ["nvngx_dlssg.dll"]}}, f)
doc8 = perfreport.report(run8, do_compare=False)
s8 = doc8["summary"]
v8 = s8["vr"]
print("  line: " + doc8["line"])
check("perf FG: only engine frames newer than all before are new: ~30 fps from 60 Presents/s, half the submits repeats",
      near(v8["fps"], 30.0, 0.2) and near(v8["r_pct"], 50.0, 0.2) and near(v8["frame_ms"]["p50"], 33.33, 0.01)
      and near(v8["submits_per_frame"], 2.0, 0.01), (v8["fps"], v8["r_pct"], v8["frame_ms"], v8["submits_per_frame"]))
check("perf FG: the resubmits carry an older engine frame (older_pct ~50)", near(v8["older_pct"], 50.0, 0.2)
      and v8["older_submits"] == v8["repeat_submits"], (v8["older_pct"], v8["older_submits"], v8["repeat_submits"]))
check("perf FG: 2.00 Presents per engine tick, engine fps = VR fps", v8["presents_per_tick"] == 2.0
      and near(v8["engine_fps"], v8["fps"], 0.1), (v8["presents_per_tick"], v8["engine_fps"]))
check("perf FG: the file's older-frame P rows reclassified, with a warning; perf.csv's count of them as new is not "
      "compared", any("1200 of its P rows" in w for w in doc8["warnings"]) and v8["frames_1hz"] > 2 * v8["frames"] - 5
      and not any("perf.csv counted" in w for w in doc8["warnings"]), (v8["frames_1hz"], doc8["warnings"]))
fg8 = s8["framegen"]
check("perf FG: frame generation measured from UEVR's Presents per engine tick, the module listed after it",
      fg8["on"] is True and fg8["measured"] is True and fg8["evidence"] == ["uevr: 2.0 presents per engine tick",
                                                                           "game: nvngx_dlssg.dll loaded"], fg8)
check("perf FG: Native Stereo Fix eye pairs from frozen snapshots on ~90% of the submits", near(v8["nsf_snapshot_pct"], 90.0, 0.2),
      v8["nsf_snapshot_pct"])
check("perf FG: line", "2.00 presents/tick" in doc8["line"] and "R 50%" in doc8["line"] and "FG on" in doc8["line"]
      and "frozen pairs 90%" in doc8["line"], doc8["line"])
# the same run as UEVR writes it now (older-frame resubmits already R): the same numbers, nothing reclassified
with open(os.path.join(run8, "perf-frames.csv"), encoding="utf-8") as f:
    fg_rows = f.read().split("\n")
ki = FRAMES_HEADER.split(",").index("kind")
fg_new = [fg_rows[0]] + [",".join(p[:ki] + ["R" if n % 2 else "P"] + p[ki + 1:]) for n, p in
                         enumerate(r.split(",") for r in fg_rows[1:] if r)]
write(os.path.join(run8, "perf-frames.csv"), fg_new)
doc8b = perfreport.report(run8, do_compare=False)
v8b = doc8b["summary"]["vr"]
check("perf FG: a file classified by UEVR gives the same numbers, without the reclassification warning",
      v8b["fps"] == v8["fps"] and v8b["r_pct"] == v8["r_pct"] and v8b["older_submits"] == v8["older_submits"]
      and not any("counted as R" in w for w in doc8b["warnings"]), (v8b["fps"], v8b["r_pct"], doc8b["warnings"]))
# without the Tick hook's counts (no ticks), and in AFR, Presents per tick can't tell: the module alone decides again
s8c = variant_of(run8, "fg-noticks", lambda parts, cols: parts.__setitem__(cols.index("ticks"), "0"))
check("perf FG: without Tick counts, no Presents per tick and no measurement: the module says on (unmeasured)",
      s8c["vr"]["presents_per_tick"] is None and s8c["framegen"]["measured"] is False and s8c["framegen"]["on"] is True,
      (s8c["vr"]["presents_per_tick"], s8c["framegen"]))

# ---------------------------------------------------------------------------------------------------------------
# a run without UEVR and without a recipe: "observe" from the game's start + 30 s, PresentMon only

run5 = make_run("20261002-120400-Custom-flat-r1")
pm = [PM_HEADER]
q, k = T0, 0
while q < T0 + 120_000 * MS:
    kind = "Application" if k % 2 == 0 else "Intel XeSS-FG"
    pm.append(f"testgame.exe,4243,0xAB,DXGI,1,0,0,Composed: Flip,{kind},NA,13.889,16.667,0.2,1,1,{q // 100},1,1,1,1,1,9.0,1")
    q += 13_888_889
    k += 1
write(os.path.join(run5, "presentmon-testgame.exe-4243.csv"), pm)
with open(os.path.join(run5, "perf-context.json"), "w", encoding="utf-8") as f:
    json.dump({"game": "Custom", "process": "testgame", "recipe": "", "recipe_vars": "", "injected": False,
               "qpc_freq": 10_000_000, "t0": {"qpc_ns": T0, "unix_ms": U0 // MS}, "game_start_unix_ms": U0 // MS + 2000,
               "observe_end": {"qpc_ns": T0 + 100_000 * MS, "unix_ms": (U0 // MS) + 100_000}, "game_pids": [4243],
               "presentmon": {"status": "ok"}, "nvidia_smi": {"status": "not_found"}, "modules": {"nvngx_dlssg": False, "fg_modules": []}}, f)
write(os.path.join(run5, "game-settings", "GameUserSettings.ini"), ["[/Script/Engine.GameUserSettings]", "bUseVSync=True", "FrameRateLimit=0"])
doc5 = perfreport.report(run5, do_compare=False)
s5 = doc5["summary"]
print("  line: " + doc5["line"])
check("perf flat: observe from the game's start + 30 s, 5 s settle, to the observation end",
      s5["label"] == "observe" and near(s5["window_s"], 68.0, 0.01) and near(s5["settle_s"], 5.0, 0.01)
      and near(s5["measured_s"], 63.0, 0.01), (s5["label"], s5["window_s"], s5["settle_s"], s5["measured_s"]))
check("perf flat: 72 fps, no VR numbers, headline flat", near(s5["flat"]["fps"], 72.0, 0.01) and s5["vr"] is None
      and s5["headline"]["kind"] == "flat" and s5["flat"]["sim_fps"] is None, s5["flat"])
check("perf flat: VSync caps the game without UEVR", s5["capped"]["on"] is True and s5["capped"]["vsync"] is True, s5["capped"])
check("perf flat: sources", s5["sources"] == {"uevr": "not injected", "presentmon": "ok", "monado": "missing", "gpu": "not_found"}, s5["sources"])
check("perf flat: frame generation from PresentMon's frame types (half of them XeSS-FG)", s5["framegen"]["on"] is True
      and s5["framegen"]["evidence"] == ["presentmon: 50.0% Intel XeSS-FG frames"], s5["framegen"])
check("perf flat: line", doc5["line"].startswith("perf[observe 63.0s, settle 5.0s]: flat 72.0 fps (1% low 72.0, 0 hitches) | FG on | capped vsync"), doc5["line"])

# nothing measured: a recipe run without segments, PresentMon refused, nvidia-smi gone
run6 = make_run("20261002-120500-Custom-none-r1")
with open(os.path.join(run6, "pilot-status.json"), "w", encoding="utf-8") as f:
    json.dump({"clock": {"qpc_ns": T0, "unix_ns": U0, "qpc_freq": 10_000_000}, "segments": []}, f)
with open(os.path.join(run6, "perf-context.json"), "w", encoding="utf-8") as f:
    json.dump({"injected": True, "presentmon": {"status": "access_denied"}, "nvidia_smi": {"status": "exited"}}, f)
doc6 = perfreport.report(run6, do_compare=False)
check("perf: no segment -> no numbers, the reason, PresentMon access denied kept",
      doc6["summary"]["headline"] is None and doc6["summary"]["note"] == "no measured segment"
      and doc6["summary"]["sources"] == {"uevr": "missing", "presentmon": "access_denied", "monado": "missing", "gpu": "exited"}
      and doc6["line"] == "perf: no measured segment (uevr missing, presentmon access_denied, monado missing, gpu exited)", doc6["line"])

# a UEVR run without a recipe: "observe" from the session's FOCUSED line in log.txt (as UEVR logs it) + 30 s
run7 = make_run("20261002-120600-Custom-observe-r1")
shutil.copy(os.path.join(run4, "perf-frames.csv"), os.path.join(run7, "perf-frames.csv"))
write(os.path.join(run7, "log.txt"), [
    f"[{local(S - 40_000 * MS, '%Y-%m-%d %H:%M:%S.%f')[:-3]}] [UnrealVR] [info] VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 4 (VISIBLE)",
    f"[{local(S - 30_000 * MS, '%Y-%m-%d %H:%M:%S.%f')[:-3]}] [UnrealVR] [info] VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED 5 (FOCUSED)",
    f"[{local(S - 30_000 * MS, '%Y-%m-%d %H:%M:%S.%f')[:-3]}] [UnrealVR] [info] [OpenXR] Session transitioned to FOCUSED. session_ready=true"])
with open(os.path.join(run7, "perf-context.json"), "w", encoding="utf-8") as f:
    json.dump({"injected": True, "qpc_freq": 10_000_000, "t0": {"qpc_ns": T0, "unix_ms": U0 // MS},
               "observe_end": {"qpc_ns": S + 60_000 * MS, "unix_ms": unix_of(S + 60_000 * MS) // MS}}, f)
s7 = perfreport.report(run7, do_compare=False)["summary"]
check("perf observe: UEVR's FOCUSED log line + 30 s to the observation end", s7["label"] == "observe"
      and near(s7["window_s"], 60.0, 0.01) and s7["vr"] and s7["vr"]["stalls"] == 1, (s7.get("label"), s7.get("window_s"), s7.get("note")))

# ---------------------------------------------------------------------------------------------------------------
# comparisons with other runs in the same runs folder


def fake_result(name, injected, verdict, dll, perf_extra):
    d = make_run(name)
    perf = {"label": "gameplay", "injected": injected, "settings": {"fingerprint": st_["fingerprint"]},
            "window": {"monitor": "MPL0291"}, "render_config": s.get("render_config")}
    perf.update(perf_extra)
    with open(os.path.join(d, "result.json"), "w", encoding="utf-8-sig") as f:
        json.dump({"verdict": verdict, "game": "Custom", "recipe": r"D:\other\recipes\T.json", "recipe_vars": "",
                   "started": name[:15], "dll_sha256": dll, "perf": perf}, f)


fake_result("20261001-100000-Custom-flat-r1", False, "PASS", "AAAA", {"flat": {"fps": 70.0, "sim_fps": None}})
fake_result("20261001-100100-Custom-flat-r2", False, "PASS", "AAAA", {"flat": {"fps": 74.0, "sim_fps": None}})
fake_result("20261001-100200-Custom-flat-r3", False, "CRASH", "AAAA", {"flat": {"fps": 10.0}})
fake_result("20261001-100300-Custom-flatobs-r1", False, "PASS", "AAAA", {"label": "observe", "flat": {"fps": 10.0}})
for i, fps in enumerate((50.0, 52.0, 51.0)):
    fake_result(f"20261001-11000{i}-Custom-old-r{i + 1}", True, "PASS", "BBBB",
                {"vr": {"fps": fps, "frame_ms": {"p99": 33.33}, "hitch_per_min": 2.0, "work_ms": 8.5}})
fake_result("20261001-090000-Custom-older-r1", True, "PASS", "CCCC", {"vr": {"fps": 20.0}})
fake_result("20261001-120000-Custom-same-r1", True, "PASS", "AAAA", {"vr": {"fps": 99.0}})
fake_result("20261001-120100-Custom-othermon-r1", True, "PASS", "DDDD", {"vr": {"fps": 99.0}, "window": {"monitor": "BOE0C87"}})
ctx = load_json(os.path.join(run, "perf-context.json"))
c = perfreport.compare(run, s, ctx)
check("compare: flat baseline = median of the PASS gameplay runs without UEVR (72), VR/flat60 = 47.78 / 60",
      c["flat"]["runs"] == 2 and c["flat"]["flat_fps"] == 72.0 and near(c["flat"]["vr_vs_flat60"], 47.78 / 60, 0.001)
      and near(c["flat"]["vr_vs_flat"], 47.78 / 72, 0.001) and c["flat"]["afr_factor"] == 1, c["flat"])
pb = c["prev_build"]
check("compare: previous build = the newest other DLL (51 median of 3), same monitor only, -6.3 %, a regression",
      pb["dll_sha256"] == "BBBB" and pb["vr_fps"] == 51.0 and pb["runs"] == 3 and pb["range"] == [50.0, 52.0]
      and near(pb["delta_pct"], -6.3, 0.01) and pb["noisy"] is False and pb["render_config_differs"] is False
      and pb["regression"] == ["VR fps 47.78 vs 51.0"], pb)
s_afr = dict(s, mode=dict(s["mode"], afr=True))
check("compare: AFR halves the zero-overhead VR rate (flat / 2)", perfreport.compare(run, s_afr, ctx)["flat"]["afr_factor"] == 2
      and near(perfreport.compare(run, s_afr, ctx)["flat"]["vr_vs_flat60"], 47.78 / 30, 0.001))
s_line = dict(s, compare=c)
check("compare: in the summary line", "VR/flat60 80% (2 flat runs)" in perfreport.summary_line(s_line)
      and "prev build 51.0 (-6.3%, REGRESSION)" in perfreport.summary_line(s_line), perfreport.summary_line(s_line))
# runs with the eye sampler or the game's settings changed (run-test.ps1's perf-context.json "variant") are compared
# only among themselves: the sampler's load and other settings make them another population
fake_result("20261001-130000-Custom-eyes-r1", True, "PASS", "EEEE", {"vr": {"fps": 30.0}})
with open(os.path.join(work, "runs", "20261001-130000-Custom-eyes-r1", "perf-context.json"), "w", encoding="utf-8-sig") as f:
    json.dump({"variant": {"eye_sampler": True, "game_ini": ""}}, f)
c2 = perfreport.compare(run, s, ctx)
check("compare: a newer build's run with the eye sampler is no baseline for a plain run",
      c2["prev_build"]["dll_sha256"] == "BBBB" and c2["flat"]["runs"] == 2, c2["prev_build"])
c3 = perfreport.compare(run, s, dict(ctx, variant={"eye_sampler": True, "game_ini": ""}))
check("compare: a run with the eye sampler compares only with such runs",
      c3["prev_build"] and c3["prev_build"]["dll_sha256"] == "EEEE" and c3["flat"] is None, c3)
c4 = perfreport.compare(run, s, dict(ctx, variant={"eye_sampler": False, "game_ini": "gameusersettings.ini|s|k=1"}))
check("compare: a run with -GameIni has no baseline among plain runs", c4["prev_build"] is None and c4["flat"] is None, c4)

print(f"python perf: {fails} failure(s)")
sys.exit(1 if fails else 0)
