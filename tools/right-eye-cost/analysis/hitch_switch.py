"""Hitches by time since the last window switch in an in-run A/B (GdfShare 2 / N: share in even windows of QPC time;
SecondFamilyCvars: phase = window % (groups + 1)), from perf-frames.csv P rows, play phase only, NO guard: per mode,
the present intervals and hitches (> 2x / 3x the run's median interval) in each second of the window, as counts and
as hitches per 1000 frames, so a switching artifact (hitches bunched right after a switch) shows apart from a
steady difference between the modes.
  python hitch_switch.py <label=run dir> [...] --window 10 [--groups 1 --names share,refresh --bins 10]"""
import argparse, csv, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_times import probe_qpc, near

ap = argparse.ArgumentParser(); ap.add_argument("runs", nargs="+"); ap.add_argument("--window", type=int, required=True)
ap.add_argument("--groups", type=int, default=1); ap.add_argument("--names", default="share,refresh")
ap.add_argument("--bins", type=int, default=0, help="bins per window (default: one per second, at most 12)")
ap.add_argument("--exclude-probe", type=float, default=0, help="leave out intervals from 6 s before to S s after each [GdfFind] probe-5 scan")
ap.add_argument("--base-ms", type=float, default=0, help="thresholds 2x / 3x this (default: median of all given runs pooled)")
a = ap.parse_args()
names = a.names.split(",")
bins = a.bins or min(a.window, 12)
loaded = []
for spec in a.runs:
    label, run = spec.split("=", 1)
    ph = {p["name"]: p["qpc_ns"] for p in json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))["phases"]}
    t0, t1 = ph["play"], ph.get("leave") or ph.get("exit") or 1 << 62
    qs = sorted(int(r["qpc_ns"]) for r in csv.DictReader(open(os.path.join(run, "perf-frames.csv"), encoding="utf-8", errors="replace")) if r["kind"] == "P")
    probes = probe_qpc(run) if a.exclude_probe > 0 else []
    iv = [(y, (y - x) / 1e6) for x, y in zip(qs, qs[1:]) if t0 <= y < t1 and not (probes and near(y, probes, a.exclude_probe))]
    loaded.append((label, iv, len(probes)))
med = a.base_ms if a.base_ms > 0 else float(np.median([v for _, iv, _ in loaded for _, v in iv]))
print(f"threshold base {med:.2f} ms ({'given' if a.base_ms > 0 else 'pooled median of the given runs'}): >2x {2 * med:.1f} ms, >3x {3 * med:.1f} ms")
for label, iv, nprobe in loaded:
    tab = {}  # (mode, bin) -> [frames, >2x, >3x]
    for y, v in iv:
        s = y / 1e9; w = int(s // a.window); m = w % (a.groups + 1)
        b = min(bins - 1, int((s - w * a.window) / a.window * bins))
        c = tab.setdefault((m, b), [0, 0, 0]); c[0] += 1; c[1] += v > 2 * med; c[2] += v > 3 * med
    print(f"\n{label}{f' (probe scans {nprobe} excluded, +{a.exclude_probe:g} s)' if nprobe else ''}: own median {float(np.median([v for _, v in iv])):.1f} ms, window {a.window} s, {bins} bins (bin = {a.window / bins:g} s since the switch)")
    for m in range(a.groups + 1):
        nm = names[m] if m < len(names) else str(m)
        row = [tab.get((m, b), [0, 0, 0]) for b in range(bins)]
        f = sum(r[0] for r in row); h2 = sum(r[1] for r in row); h3 = sum(r[2] for r in row)
        print(f"   {nm:<8} frames {f:6d}, >2x {h2:4d} ({1000 * h2 / max(1, f):.1f}/1000), >3x {h3:3d}")
        print("      >2x per bin: " + " ".join(f"{r[1]:3d}" for r in row))
        print("      >3x per bin: " + " ".join(f"{r[2]:3d}" for r in row))
        print("      frames/bin : " + " ".join(f"{r[0]:5d}" for r in row))
