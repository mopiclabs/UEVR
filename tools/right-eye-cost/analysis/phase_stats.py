"""Per-phase means of UEVR's 1-s perf.csv rows and nvidia-smi's gpu.csv for an in-run A/B run
(VR_ExperimentSecondFamilyCvars "N,g1,g2..."; phase = floor(qpc s / N) % (groups + 1)). Rows within --guard s of a
window start are left out; only the recipe's play phase from --settle s on counts.

  python phase_stats.py <run dir> [--window 10 --groups 2 --guard 2 --settle 20]
"""
import argparse
import csv
import datetime
import json
import os
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--window", type=int, default=10)
    ap.add_argument("--groups", type=int, default=2)
    ap.add_argument("--guard", type=float, default=2)
    ap.add_argument("--settle", type=float, default=20)
    ap.add_argument("--names", default="plain,secondary,skip")
    a = ap.parse_args()
    names = a.names.split(",")
    st = json.load(open(os.path.join(a.run, "pilot-status.json"), encoding="utf-8-sig"))
    ph = {p["name"]: p["qpc_ns"] for p in st.get("phases", [])}
    t0 = ph["play"] + a.settle * 1e9
    t1 = ph.get("leave") or ph.get("exit") or float("inf")
    W = a.window * 1e9

    def phase(q):
        if not (t0 <= q < t1):
            return None
        w = int(q // W)
        if q - w * W < a.guard * 1e9 + 1e9:  # the 1-s row ends at q: its start must be past the guard
            return None
        return w % (a.groups + 1)

    cols = ["vr_fps", "present_fps", "wait_avg_ms", "end_avg_ms", "new_int_p95_ms", "tick_fps"]
    acc = defaultdict(lambda: defaultdict(list))
    unix_to_phase = []
    with open(os.path.join(a.run, "perf.csv"), newline="") as f:
        for r in csv.DictReader(f):
            q = int(r["qpc_ns"])
            p = phase(q)
            unix_to_phase.append((int(r["unix_ms"]), q, p))
            if p is None:
                continue
            for c in cols:
                try:
                    acc[p][c].append(float(r[c]))
                except (ValueError, KeyError):
                    pass
    # gpu.csv: local time -> unix ms -> nearest perf row's phase
    gpath = os.path.join(a.run, "gpu.csv")
    if os.path.exists(gpath) and unix_to_phase:
        unix_to_phase.sort()
        with open(gpath, newline="") as f:
            rd = csv.reader(f)
            next(rd, None)
            for row in rd:
                try:
                    ts = datetime.datetime.strptime(row[0].strip(), "%Y/%m/%d %H:%M:%S.%f")
                    um = int(ts.timestamp() * 1000)
                    util = float(row[1].strip().split()[0])
                    power = float(row[5].strip().split()[0])
                except (ValueError, IndexError):
                    continue
                # perf row whose 1-s window contains um (row unix_ms = window end)
                best = min(unix_to_phase, key=lambda x: abs(x[0] - 500 - um))
                if abs(best[0] - 500 - um) < 600 and best[2] is not None:
                    acc[best[2]]["gpu_util"].append(util)
                    acc[best[2]]["gpu_w"].append(power)
    print(f"{os.path.basename(a.run)}: N={a.window}s, guard {a.guard}s, settle {a.settle}s")
    keys = cols + ["gpu_util", "gpu_w"]
    print(f"   {'metric':<16}" + "".join(f"{(names[p] if p < len(names) else p):>12}" for p in sorted(acc)))
    for k in keys:
        line = f"   {k:<16}"
        for p in sorted(acc):
            v = acc[p].get(k, [])
            line += f"{(sum(v) / len(v)) if v else float('nan'):>12.2f}"
        print(line + f"   (n {', '.join(str(len(acc[p].get(k, []))) for p in sorted(acc))})")


if __name__ == "__main__":
    main()
