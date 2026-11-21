"""Hitches split by what dominates the hitch frame, from UEVR's perf-frames.csv (rows of kind "P", one per present;
columns qpc_ns, tick_us, rt_blocked_us, present_us, uevr_rt_us, uevr_gt_us, ticks), play phase only (pilot-status.json
play .. leave).

A hitch is a present interval (between consecutive P rows) above 2x or 3x ONE base interval shared by every run and
phase of the invocation: --base-ms if given, else the median of all their intervals pooled. (Until 2026-11-22 each phase
used its own median: a phase with faster frames got a lower bar and counted more hitches.) Each phase's own median is
printed for information. Each hitch frame is classed by the largest of game tick (tick_us), render thread blocked
(rt_blocked_us) and present call (present_us): that class if it covers more than half of the interval, else "other"
(none dominating: GPU or elsewhere). Also prints the frame-interval list summary (frames, median, p90, p99) and the
median of each column on hitch frames vs all frames.

Whole runs:      python hitch_kinds.py <label=run dir> [...] [--base-ms 22]
Window A/B runs: python hitch_kinds.py <label=run dir> --window 10 --groups 1 --names share,refresh [--guard 2]
                 (phase = floor(qpc s / window) % (groups + 1); intervals ending within --guard s of a window start are
                 left out)
--exclude-probe S leaves out intervals from 6 s before to S s after each [GdfFind] probe-5 scan (builds up to 28dedeef)."""
import argparse, csv, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_times import probe_qpc, near

ap = argparse.ArgumentParser(); ap.add_argument("runs", nargs="+"); ap.add_argument("--window", type=int, default=0)
ap.add_argument("--groups", type=int, default=1); ap.add_argument("--names", default=""); ap.add_argument("--guard", type=float, default=2)
ap.add_argument("--exclude-probe", type=float, default=0, help="leave out intervals from 6 s before to S s after each [GdfFind] probe-5 scan (log.txt)")
ap.add_argument("--base-ms", type=float, default=0, help="hitch thresholds are 2x / 3x this (default: pooled median of every run and phase given)")
a = ap.parse_args()
FIELDS = ["tick_us", "rt_blocked_us", "present_us", "uevr_rt_us", "uevr_gt_us", "ticks"]
f = lambda r, k: float(r.get(k) or 0)


def phase_of(q):
    if a.window <= 0:
        return 0
    s = q / 1e9; w = int(s // a.window)
    return None if s - w * a.window < a.guard else w % (a.groups + 1)


groups = []  # (label, phase name, [(interval ms, row)], probes excluded)
names = a.names.split(",") if a.names else []
for spec in a.runs:
    label, run = spec.split("=", 1)
    ph = {p["name"]: p["qpc_ns"] for p in json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))["phases"]}
    t0, t1 = ph["play"], ph.get("leave") or ph.get("exit") or 1 << 62
    rows = sorted((r for r in csv.DictReader(open(os.path.join(run, "perf-frames.csv"), encoding="utf-8", errors="replace")) if r["kind"] == "P"),
                  key=lambda r: int(r["qpc_ns"]))
    probes = probe_qpc(run) if a.exclude_probe > 0 else []
    by_phase = {}
    for x, y in zip(rows, rows[1:]):
        q = int(y["qpc_ns"])
        if not (t0 <= q < t1) or (probes and near(q, probes, a.exclude_probe)):
            continue
        p = phase_of(q)
        if p is not None:
            by_phase.setdefault(p, []).append(((q - int(x["qpc_ns"])) / 1e6, y))
    for p in sorted(by_phase):
        pname = names[p] if p < len(names) else (str(p) if a.window > 0 else "run")
        groups.append((label, pname, by_phase[p], len(probes)))

pooled = np.array([d for _, _, iv, _ in groups for d, _ in iv])
base = a.base_ms if a.base_ms > 0 else float(np.median(pooled))
print(f"threshold base {base:.2f} ms ({'given' if a.base_ms > 0 else 'pooled median of ' + str(len(groups)) + ' run/phase groups'}): >2x = {2 * base:.1f} ms, >3x = {3 * base:.1f} ms")
for label, pname, iv, nprobe in groups:
    v = np.array([d for d, _ in iv]); med = float(np.median(v))
    print(f"{label} [{pname}]{f' (probe scans {nprobe} excluded, +{a.exclude_probe:g} s)' if nprobe else ''}: frames {len(v)}, own median {med:.1f} ms, p90 {np.percentile(v, 90):.1f}, p99 {np.percentile(v, 99):.1f}")
    allm = {k: float(np.median([f(r, k) for _, r in iv])) for k in FIELDS}
    for mult in (2, 3):
        hs = [(d, r) for d, r in iv if d > mult * base]
        kinds = {"game tick": 0, "rt blocked": 0, "present": 0, "other (GPU or elsewhere)": 0}
        for d, r in hs:
            ms = {"game tick": f(r, "tick_us") / 1e3, "rt blocked": f(r, "rt_blocked_us") / 1e3, "present": f(r, "present_us") / 1e3}
            k = max(ms, key=ms.get)
            kinds[k if ms[k] > 0.5 * d else "other (GPU or elsewhere)"] += 1
        hm = {k: float(np.median([f(r, k) for _, r in hs])) if hs else float("nan") for k in FIELDS}
        print(f"   >{mult}x ({mult * base:.0f} ms): {len(hs)} ({len(hs) / (v.sum() / 60000):.1f}/min) | " + ", ".join(f"{k} {n}" for k, n in kinds.items())
              + " | median on hitch / all: " + ", ".join(f"{k} {hm[k]:.0f}/{allm[k]:.0f}" for k in FIELDS))
