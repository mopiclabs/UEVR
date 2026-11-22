"""R21: the profiler lines of a run's UEVR log over its measured segment (from --settle s after the segment start):
[HookProfiler] (the Present hook's parts, per-mod on_present, the game window's swap chain), [D3D12][frame-profiler]
(D3D12Component::on_frame and its parts) and [OpenXR][frame-profiler] (xrWaitFrame / xrBeginFrame / xrEndFrame, pose
update). Each line is a 5 s average; prints the n-weighted mean of every "<name> avg=<ms>ms ... n=<count>" field, the
largest max, and the per-mod and swap-chain fields of the last [HookProfiler] line.

  python r21_prof.py --run <run dir> [--settle 20]"""
import argparse
import datetime
import json
import os
import re
from collections import defaultdict

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
FIELD = re.compile(r"(\w+) avg=([\d.]+)ms max=([\d.]+)ms n=(\d+)")


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
    acc = defaultdict(lambda: defaultdict(lambda: [0.0, 0, 0.0]))  # tag -> field -> [sum avg*n, n, max]
    last = {}
    for line in open(os.path.join(a.run, "log.txt"), encoding="utf-8", errors="replace"):
        m = TS.match(line)
        if not m:
            continue
        tag = "[HookProfiler]" if "[HookProfiler]" in line else ("[D3D12][frame-profiler]" if "[D3D12][frame-profiler]" in line
              else ("[OpenXR][frame-profiler]" if "[OpenXR][frame-profiler]" in line else None))
        if not tag:
            continue
        t = datetime.datetime.strptime(m.group(1)[:26], "%Y-%m-%d %H:%M:%S.%f").timestamp()
        if not (u0 <= t <= u1):
            continue
        for name, avg, mx, n in FIELD.findall(line):
            x = acc[tag][name]
            x[0] += float(avg) * int(n)
            x[1] += int(n)
            x[2] = max(x[2], float(mx))
        last[tag] = line.strip()
    print("%s: segment %.0f s after settle" % (os.path.basename(a.run), u1 - u0))
    for tag in ("[HookProfiler]", "[D3D12][frame-profiler]", "[OpenXR][frame-profiler]"):
        if tag not in acc:
            print(" ", tag, "no lines in the segment")
            continue
        print(" ", tag)
        for name, (s, n, mx) in acc[tag].items():
            if n:
                print("     %-28s avg %.3f ms  max %.2f ms  n %d" % (name, s / n, mx, n))
    if "[HookProfiler]" in last:
        l = last["[HookProfiler]"]
        i = l.find("; mods on_present")
        print("  last HookProfiler tail:", l[i + 2:] if i >= 0 else l[-300:])


if __name__ == "__main__":
    main()
