"""R21/R22: the profiler lines of a run's UEVR log over its measured segment (from --settle s after the segment start):
[HookProfiler] (the Present hook's parts per frame, per-mod on_present, the game window's swap chain), [D3D12][frame-profiler]
(D3D12Component::on_frame and its parts) and [OpenXR][frame-profiler] (xrWaitFrame / xrBeginFrame / xrEndFrame, pose
update). Prints the weighted mean of every "<name> avg=<ms>ms ... n=<count>" field and the largest max.
[HookProfiler] lines written per VR_ExperimentPresentHookAB window ("window N (skip|keep)", R22) are grouped by phase and
weighted by the window's presents (their avg fields are per frame); 5 s lines (R21) are weighted by n. The [D3D12] and
[OpenXR] lines are 5 s averages that straddle windows, so they are only reported for the whole segment.

  python r21_prof.py --run <run dir> [--settle 20]"""
import argparse
import datetime
import json
import os
import re
from collections import defaultdict

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
FIELD = re.compile(r"(\w+) avg=([\d.]+)ms max=([\d.]+)ms n=(\d+)")
WINDOW = re.compile(r"\[HookProfiler\] window (\d+) \((skip|keep)([^)]*)\) \(per frame, (\d+) presents\)")
SKIPS = re.compile(r"skipped presents (\d+), skipped UI copies (\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--settle", type=float, default=20)
    a = ap.parse_args()
    st = json.load(open(os.path.join(a.run, "pilot-status.json"), encoding="utf-8-sig"))
    clock = st["clock"]
    segs = [s for s in st.get("segments", []) if s.get("start_qpc_ns") and s.get("end_qpc_ns")]
    seg = max(segs, key=lambda s: s["end_qpc_ns"] - s["start_qpc_ns"])
    to_unix = lambda q: (clock["unix_ns"] + (q - clock["qpc_ns"])) / 1e9
    u0, u1 = to_unix(seg["start_qpc_ns"]) + a.settle, to_unix(seg["end_qpc_ns"])
    acc = defaultdict(lambda: defaultdict(lambda: [0.0, 0.0, 0.0]))  # group -> field -> [weighted sum, weight, max]
    counts = defaultdict(lambda: [0, 0, 0, 0])  # group -> windows, presents, skipped presents, skipped UI
    last = {}
    notes = []
    for line in open(os.path.join(a.run, "log.txt"), encoding="utf-8", errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        if "[PresentHookAB]" in line:
            notes.append(line.strip()[:220])
        tag = "[HookProfiler]" if "[HookProfiler]" in line else ("[D3D12][frame-profiler]" if "[D3D12][frame-profiler]" in line
              else ("[OpenXR][frame-profiler]" if "[OpenXR][frame-profiler]" in line else None))
        if not tag:
            continue
        t = datetime.datetime.strptime(m.group(1)[:26], "%Y-%m-%d %H:%M:%S.%f").timestamp()
        if not (u0 <= t <= u1):
            continue
        group, weight_all = tag, None
        w = WINDOW.search(line)
        if w:
            group = "[HookProfiler] %s windows" % w.group(2)
            weight_all = int(w.group(4))
            c = counts[group]
            c[0] += 1
            c[1] += weight_all
            s = SKIPS.search(line)
            if s:
                c[2] += int(s.group(1))
                c[3] += int(s.group(2))
        for name, avg, mx, n in FIELD.findall(line):
            wt = weight_all if weight_all is not None else int(n)
            x = acc[group][name]
            x[0] += float(avg) * wt
            x[1] += wt
            x[2] = max(x[2], float(mx))
        last[tag] = line.strip()
    print("%s: segment %.0f s after settle" % (os.path.basename(a.run), u1 - u0))
    for n in notes[:4]:
        print("  note:", n)
    for group in sorted(acc):
        c = counts.get(group)
        print(" ", group, ("(%d windows, %d presents, skipped presents %d, skipped UI copies %d)" % tuple(c)) if c else "")
        for name, (s, wsum, mx) in acc[group].items():
            if wsum:
                print("     %-28s avg %.3f ms  max %.2f ms" % (name, s / wsum, mx))
    if "[HookProfiler]" in last:
        l = last["[HookProfiler]"]
        i = l.find("; mods on_present")
        print("  last HookProfiler tail:", l[i + 2:] if i >= 0 else l[-300:])


if __name__ == "__main__":
    main()
