"""Whole runs of one route compared by route time (seconds since the pilot's play phase began): per minute of route,
fps (perf.csv), hitches (present intervals above 2x / 3x the run's median, perf-frames.csv), right/left mean luma
(eyes.csv) and block-ratio median / p10 (binocular frames; frames darker than 10/255 left out); then every big hitch
(> 3x median) with its route time per run, and per run the GdfShare exchange log totals.
  python route_compare.py <label=run dir> [<label=run dir> ...] [--minute 60] [--md out.md]"""
import argparse, csv, glob, json, os, re
from collections import defaultdict
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser(); ap.add_argument("runs", nargs="+"); ap.add_argument("--minute", type=int, default=60)
ap.add_argument("--md", default="")
a = ap.parse_args()
out = []


def emit(s=""):
    print(s); out.append(s)


data = {}
for spec in a.runs:
    label, run = spec.split("=", 1)
    st = json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))
    ph = {p["name"]: p["qpc_ns"] for p in st.get("phases", [])}
    t0, t1 = ph["play"], ph.get("leave") or ph.get("exit") or 1 << 62
    rt = lambda q: (q - t0) / 1e9
    d = {"fps": defaultdict(list), "lum": defaultdict(list), "blk": defaultdict(list), "hit2": defaultdict(int),
         "hit3": defaultdict(int), "frames": defaultdict(int), "big": [], "len": (t1 - t0) / 1e9}
    last = t0
    for r in csv.DictReader(open(os.path.join(run, "perf.csv"), encoding="utf-8", errors="replace")):
        q = int(r["qpc_ns"])
        if t0 <= q < t1 and float(r.get("vr_fps") or 0) > 0:
            d["fps"][int(rt(q) // a.minute)].append(float(r["vr_fps"])); last = q
    if t1 == 1 << 62:  # no leave/exit phase (pilot failed): play ends at the last perf row with a frame rate
        t1 = last + 1; d["len"] = (t1 - t0) / 1e9
    if os.path.exists(os.path.join(run, "eyes.csv")):
        for r in csv.DictReader(open(os.path.join(run, "eyes.csv"))):
            q = int(r["t_ns"])
            if t0 <= q < t1:
                d["lum"][int(rt(q) // a.minute)].append((float(r["l_mean"]), float(r["r_mean"])))
    for p in sorted(glob.glob(os.path.join(run, "binocular", "*.png"))):
        q = int(re.match(r"(\d+)", os.path.basename(p)).group(1))
        if not (t0 <= q < t1):
            continue
        im = np.asarray(Image.open(p).convert("L"), np.float32); h, w = im.shape
        L, R = im[:, : w // 2], im[:, w // 2: w // 2 * 2]
        bh, bw = h // 24, (w // 2) // 24
        Lb = L[: bh * 24, : bw * 24].reshape(bh, 24, bw, 24).mean(axis=(1, 3))
        Rb = R[: bh * 24, : bw * 24].reshape(bh, 24, bw, 24).mean(axis=(1, 3))
        if Lb.mean() < 10:
            continue
        m = (Lb > 8) | (Rb > 8); ratio = (Rb[m] + 1) / (Lb[m] + 1)
        d["blk"][int(rt(q) // a.minute)].append((float(np.median(ratio)), float(np.percentile(ratio, 10))))
    qs = sorted(int(r["qpc_ns"]) for r in csv.DictReader(open(os.path.join(run, "perf-frames.csv"), encoding="utf-8", errors="replace"))
                if r.get("kind") == "P")
    iv = [(y, (y - x) / 1e6) for x, y in zip(qs, qs[1:]) if t0 <= y < t1]
    med = float(np.median([v for _, v in iv])) if iv else 0
    for y, v in iv:
        k = int(rt(y) // a.minute)
        d["frames"][k] += 1
        if v > 2 * med:
            d["hit2"][k] += 1
        if v > 3 * med:
            d["hit3"][k] += 1
            d["big"].append((rt(y), v))
    d["median"] = med
    ex = re.findall(r"\[GdfShare\] window \d+ \((\w+)\): right families (\d+), exchanged (\d+), not shared (\d+) .*?block not validated (\d+)\), not validated on the render thread (\d+)",
                    open(os.path.join(run, "log.txt"), encoding="utf-8", errors="replace").read())
    d["ex"] = [tuple(int(x) for x in e[1:]) for e in ex]
    data[label] = d

labels = list(data)
emit("runs: " + ", ".join(f"{l} ({data[l]['len']:.0f} s of play, median frame {data[l]['median']:.1f} ms)" for l in labels))
for l in labels:
    ex = data[l]["ex"]
    if ex:
        tot = [sum(e[i] for e in ex) for i in range(5)]
        emit(f"   {l}: GdfShare windows {len(ex)}: right families {tot[0]}, exchanged {tot[1]}, not shared {tot[2]} (block not validated {tot[3]}), render-thread not validated {tot[4]}")
mins = sorted(set().union(*[set(data[l]["fps"]) | set(data[l]["frames"]) for l in labels]))
emit(f"\n| route min | " + " | ".join(f"{l} fps / hitch 2x,3x / R/L / blk med,p10" for l in labels) + " |")
emit("|---|" + "---|" * len(labels))
for k in mins:
    cells = []
    for l in labels:
        d = data[l]
        f = np.mean(d["fps"][k]) if d["fps"][k] else float("nan")
        lv = d["lum"][k]
        rl = sum(b for _, b in lv) / sum(x for x, _ in lv) if lv else float("nan")
        bm = np.mean([x for x, _ in d["blk"][k]]) if d["blk"][k] else float("nan")
        bp = np.mean([y for _, y in d["blk"][k]]) if d["blk"][k] else float("nan")
        cells.append(f"{f:.1f} / {d['hit2'][k]},{d['hit3'][k]} / {rl:.3f} / {bm:.3f},{bp:.3f}")
    emit(f"| {k * a.minute}-{(k + 1) * a.minute} s | " + " | ".join(cells) + " |")
emit("\ntotals:")
for l in labels:
    d = data[l]; mins_play = sum(d["frames"].values()) * d["median"] / 60000 if d["median"] else 1
    allf = [x for v in d["fps"].values() for x in v]
    lv = [x for v in d["lum"].values() for x in v]
    bl = [x for v in d["blk"].values() for x in v]
    emit(f"   {l}: fps {np.mean(allf):.2f}; hitches 2x {sum(d['hit2'].values())} ({sum(d['hit2'].values()) / max(1e-9, mins_play):.1f}/min), "
         f"3x {sum(d['hit3'].values())}; R/L luma {sum(b for _, b in lv) / sum(x for x, _ in lv) if lv else float('nan'):.3f}; "
         f"block median {np.mean([x for x, _ in bl]) if bl else float('nan'):.3f}, p10 {np.mean([y for _, y in bl]) if bl else float('nan'):.3f} ({len(bl)} frames)")
emit("\nbig hitches (> 3x median), route time s (interval ms):")
for l in labels:
    emit(f"   {l}: " + ", ".join(f"{t:.0f}({v:.0f})" for t, v in data[l]["big"]))
if a.md:
    open(a.md, "w", encoding="utf-8").write("\n".join(out) + "\n")
