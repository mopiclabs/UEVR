"""Per-thread context switches per second over a run's measured play (threadsample.py cells), to find a thread that
switches once per frame (or once per present) in a run whose frame rate UEVR counts, and read the same thread in a
run without UEVR. Prints the threads by name with their switches/s, CPU busy %, and the ratio to --ref (a known rate).

  python thread_rate.py --cell <cells dir> --run <run dir> [--settle 20] [--ref 50.29] [--top 25]"""
import argparse
import csv
import json
import os
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cell", required=True)
    ap.add_argument("--run", required=True)
    ap.add_argument("--settle", type=float, default=20)
    ap.add_argument("--ref", type=float, default=0)
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()
    st = json.load(open(os.path.join(a.run, "pilot-status.json"), encoding="utf-8-sig"))
    segs = [s for s in st.get("segments", []) if s.get("start_qpc_ns") and s.get("end_qpc_ns")]
    seg = max(segs, key=lambda s: s["end_qpc_ns"] - s["start_qpc_ns"])
    t0, t1 = seg["start_qpc_ns"] + int(a.settle * 1e9), seg["end_qpc_ns"]
    hz = json.load(open(os.path.join(a.cell, "meta.json")))["tsc_hz"]
    names = {r["tid"]: (r.get("name") or "?") for r in csv.DictReader(open(os.path.join(a.cell, "threads.csv"), encoding="utf-8", errors="replace"))}
    sw = defaultdict(int)
    cyc = defaultdict(int)
    for r in csv.DictReader(open(os.path.join(a.cell, "samples.csv"), encoding="utf-8")):
        q = int(r["qpc_ns"])
        if t0 <= q < t1:
            sw[r["tid"]] += int(r["ctxsw"])
            cyc[r["tid"]] += int(r["cycles"])
    dur = (t1 - t0) / 1e9
    rows = sorted(sw, key=lambda t: -cyc[t])[: a.top]
    print("%s: %.0f s of play" % (os.path.basename(a.run), dur))
    for t in rows:
        rate = sw[t] / dur
        print("  %-32s tid %-6s switches %8.2f/s  busy %5.1f %%%s" % (names.get(t, "?")[:32], t, rate, cyc[t] / hz / dur * 100,
              ("  x%.3f of ref" % (rate / a.ref)) if a.ref else ""))


if __name__ == "__main__":
    main()
