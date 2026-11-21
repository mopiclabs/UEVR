"""In-run A/B of a VR_ExperimentABCvars cell: frames and per-thread CPU split into "on" and "off" windows.

A window is on when floor(qpc seconds / N) is even (CVarManager::process_ab_cvars). The first --guard seconds of
every window are left out (the switch happens on the next engine tick; the frames in flight finish the old state).
Only the recipe's play phase from --settle s on counts. Windows pair up (on, the off after it) for a paired
difference with a 95 % interval over the pairs.

python abcpu.py <cell dir> <N> [--guard 2] [--settle 60] [--classes GameThread,RenderThread,RHIThread,TaskGraph]
"""

import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import classify  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cell")
    ap.add_argument("N", type=int)
    ap.add_argument("--guard", type=float, default=2)
    ap.add_argument("--settle", type=float, default=60)
    ap.add_argument("--classes", default="GameThread,RenderThread,RHIThread,TaskGraph,Other")
    ap.add_argument("--groups", type=int, default=0, help="VR_ExperimentSecondFamilyCvars groups: phase = window % (groups + 1), 0 = plain")
    a = ap.parse_args()
    cell = a.cell
    meta = json.load(open(os.path.join(cell, "meta.json")))
    hz = meta["tsc_hz"]
    rundir = open(os.path.join(cell, "rundir.txt")).read().strip()
    status = json.load(open(os.path.join(rundir, "pilot-status.json"), encoding="utf-8-sig"))
    phases = {p["name"]: p["qpc_ns"] for p in status.get("phases", [])}
    t0 = phases["play"] + int(a.settle * 1e9)
    t1 = phases.get("leave") or phases.get("exit")
    N = a.N * 1_000_000_000
    G = int(a.guard * 1e9)

    def win(q):
        """(window index, on) or None inside the guard / outside the measured span."""
        if not (t0 <= q < t1):
            return None
        w = q // N
        if q - w * N < G:
            return None
        if a.groups > 0:
            return w, w % (a.groups + 1)
        return w, (w % 2 == 0)

    frames = defaultdict(int)
    span = defaultdict(lambda: [None, None])
    frame_ms = defaultdict(list)
    prev_q = None
    with open(os.path.join(rundir, "perf-frames.csv"), newline="") as f:
        for r in csv.DictReader(f):
            if r["kind"] != "P":
                continue
            q = int(r["qpc_ns"])
            k = win(q)
            if k is not None:
                frames[k] += 1
                s = span[k]
                s[0] = q if s[0] is None else min(s[0], q)
                s[1] = q if s[1] is None else max(s[1], q)
                if prev_q is not None and win(prev_q) == k:
                    frame_ms[k[1]].append((q - prev_q) / 1e6)
            prev_q = q

    threads = {}
    with open(os.path.join(cell, "threads.csv"), newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        threads[r["tid"]] = classify(r["name"], r["start"])
    # UE4 leaves the game thread (the main thread, listed first) unnamed
    if rows and not any(r["name"] == "GameThread" for r in rows) and not rows[0]["name"]:
        threads[rows[0]["tid"]] = "GameThread"
    cpu = defaultdict(lambda: defaultdict(float))
    with open(os.path.join(cell, "samples.csv"), newline="") as f:
        for r in csv.DictReader(f):
            q = int(r["qpc_ns"])
            k = win(q)
            if k is not None:
                cpu[k][threads.get(r["tid"], "Other")] += int(r["cycles"]) / hz
    tick_s = defaultdict(float)
    with open(os.path.join(cell, "ticks.csv"), newline="") as f:
        for r in csv.DictReader(f):
            k = win(int(r["qpc_ns"]))
            if k is not None:
                tick_s[k] += int(r["dt_ns"]) / 1e9

    classes = a.classes.split(",")
    per_window = {}
    for k in frames:
        s0, s1 = span[k]
        dur = (s1 - s0) / 1e9 if s1 and s0 and s1 > s0 else 0
        if dur < 1 or frames[k] < 10:
            continue
        fps = (frames[k] - 1) / dur
        row = {"fps": fps, "ms": 1000 / fps}
        T = tick_s.get(k, 0)
        for c in classes:
            row[c + " busy"] = cpu[k][c] / T if T else float("nan")   # cores
            row[c + " ms/f"] = cpu[k][c] / T * 1000 / fps if T else float("nan")
        row["total cores"] = sum(cpu[k].values()) / T if T else float("nan")
        per_window[k] = row

    keys = list(next(iter(per_window.values())).keys()) if per_window else []
    m = lambda v: sum(v) / len(v) if v else float("nan")
    ci = lambda d: 1.96 * math.sqrt(sum((x - m(d)) ** 2 for x in d) / (len(d) - 1)) / math.sqrt(len(d)) if len(d) > 2 else float("nan")
    if a.groups > 0:
        P = a.groups + 1
        print(f"{os.path.basename(cell)}: N={a.N}s guard {a.guard}s, {a.groups} groups, windows per phase "
              + " ".join(str(sum(1 for k in per_window if k[1] == p)) for p in range(P)))
        base = {k[0] // P: row for k, row in per_window.items() if k[1] == 0}
        print(f"   {'metric':<20}{'plain':>9}" + "".join(f"{'g' + str(p) + '-plain':>14}" for p in range(1, P)))
        for key in keys:
            line = f"   {key:<20}{m([r[key] for k, r in per_window.items() if k[1] == 0]):>9.2f}"
            for p in range(1, P):
                d = [row[key] - base[k[0] // P][key] for k, row in per_window.items() if k[1] == p and k[0] // P in base]
                line += f"{m(d):>8.2f}+-{ci(d):<4.2f}"
            print(line)
        return
    by_state = {True: [], False: []}
    for k, row in per_window.items():
        by_state[k[1]].append(row)
    pairs = []
    for (w, on), row in per_window.items():
        if on and (w + 1, False) in per_window:
            pairs.append((row, per_window[(w + 1, False)]))
    print(f"{os.path.basename(cell)}: N={a.N}s guard {a.guard}s, windows on {len(by_state[True])} off {len(by_state[False])}, pairs {len(pairs)}")
    p50 = {s: sorted(v)[len(v) // 2] if v else float("nan") for s, v in frame_ms.items()}
    print(f"   frame ms p50: on {p50.get(True, float('nan')):.2f}  off {p50.get(False, float('nan')):.2f}")
    print(f"   {'metric':<20}{'on':>9}{'off':>9}{'on-off':>9}{'95% +-':>9}")
    for key in keys:
        on = [r[key] for r in by_state[True]]
        off = [r[key] for r in by_state[False]]
        d = [p[0][key] - p[1][key] for p in pairs]
        print(f"   {key:<20}{m(on):>9.2f}{m(off):>9.2f}{m(d):>9.2f}{ci(d):>9.2f}")


if __name__ == "__main__":
    sys.exit(main())
