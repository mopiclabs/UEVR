"""Frame-interval hitches per window phase from UEVR's perf-frames.csv (one row per frame; intervals between
consecutive frame rows' qpc_ns): frames, mean interval, the phase's own median, p99, and the count of intervals above
2x and 3x ONE base interval for all phases (--base-ms, else the median of all phases pooled; until 2026-11-22 each
phase used its own median, which gave the faster phase a lower bar), per window phase (floor(qpc s / N) % (groups + 1),
first --guard seconds of each window left out), in the play phase from play + 20 s.
  python hitches.py <run> [--window 60 --groups 1 --names share,refresh --guard 2 --base-ms 22]"""
import argparse, csv, json, os
import numpy as np

ap = argparse.ArgumentParser(); ap.add_argument("run"); ap.add_argument("--window", type=int, default=60)
ap.add_argument("--groups", type=int, default=1); ap.add_argument("--names", default="share,refresh"); ap.add_argument("--guard", type=float, default=2)
ap.add_argument("--base-ms", type=float, default=0, help="thresholds 2x / 3x this (default: pooled median of all phases)")
a = ap.parse_args()
st = json.load(open(os.path.join(a.run, "pilot-status.json"), encoding="utf-8-sig"))
ph = {p["name"]: p["qpc_ns"] for p in st.get("phases", [])}
t0, t1 = ph.get("play", 0) + 20e9, ph.get("leave") or ph.get("exit") or 1 << 62
qs = []
for r in csv.DictReader(open(os.path.join(a.run, "perf-frames.csv"), encoding="utf-8", errors="replace")):
    if r.get("kind") == "P":
        qs.append(int(r["qpc_ns"]))
if not qs:  # kind values differ by build: take every row with a frame number
    qs = [int(r["qpc_ns"]) for r in csv.DictReader(open(os.path.join(a.run, "perf-frames.csv"), encoding="utf-8", errors="replace")) if r.get("frame", "0") != "0"]
qs.sort()
names = a.names.split(",")
by = {}
for x, y in zip(qs, qs[1:]):
    if not (t0 <= y < t1):
        continue
    sec = y / 1e9; w = int(sec // a.window)
    if a.groups > 0 and sec - w * a.window < a.guard:
        continue
    k = w % (a.groups + 1) if a.groups > 0 else 0
    by.setdefault(k, []).append((y - x) / 1e6)
base = a.base_ms if a.base_ms > 0 else float(np.median([d for v in by.values() for d in v]))
print(f"{os.path.basename(a.run)}: frame intervals in the play phase (from play + 20 s), window {a.window} s; "
      f"threshold base {base:.2f} ms ({'given' if a.base_ms > 0 else 'pooled median'}): >2x {2 * base:.1f} ms, >3x {3 * base:.1f} ms")
print(f"   {'phase':<9}{'frames':>8}{'mean ms':>9}{'own med':>9}{'p99 ms':>8}{'>2x':>6}{'>3x':>6}{'per min >2x':>12}")
for k in sorted(by):
    v = np.array(by[k]); med = np.median(v)
    mins = v.sum() / 60000
    print(f"   {names[k] if k < len(names) else k:<9}{len(v):>8}{v.mean():>9.2f}{med:>9.1f}{np.percentile(v, 99):>8.1f}{(v > 2 * base).sum():>6}{(v > 3 * base).sum():>6}{(v > 2 * base).sum() / max(1e-9, mins):>12.1f}")
