"""Right/left brightness of a run's binocular eye frames (EyeSampler PNGs named <qpc ns>-segN.png, side by side):
per frame, the luma of 24x24-pixel blocks in each eye, the right/left ratio per block (blocks darker than 8/255 in both
eyes left out), its median and 10th percentile; per window phase (floor(qpc s / N) % (groups + 1)) the means, and the
trend over the play phase. For the right-eye darkness that the GDF refresh was made for (10-28: Wukong chapter-1
forest, R/L median 0.87-0.95, p10 0.80 without it; 1.00 / 0.97 with it).
  python eyes_ratio.py <run dir> [--window 10 --groups 1 --names share,refresh --guard 2]"""
import argparse, glob, json, os, re
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser(); ap.add_argument("run")
ap.add_argument("--window", type=int, default=10); ap.add_argument("--groups", type=int, default=0)
ap.add_argument("--names", default=""); ap.add_argument("--guard", type=float, default=2)
a = ap.parse_args()
st = json.load(open(os.path.join(a.run, "pilot-status.json"), encoding="utf-8-sig"))
ph = {p["name"]: p["qpc_ns"] for p in st.get("phases", [])}
t_play, t_leave = ph.get("play", 0), ph.get("leave") or ph.get("exit") or 1 << 62
names = a.names.split(",") if a.names else [str(i) for i in range(a.groups + 1)]
rows = []
for p in sorted(glob.glob(os.path.join(a.run, "binocular", "*.png"))):
    q = int(re.match(r"(\d+)", os.path.basename(p)).group(1))
    if not (t_play + 20e9 <= q < t_leave):
        continue
    sec = q / 1e9
    w = int(sec // a.window)
    if a.groups > 0 and sec - w * a.window < a.guard:
        continue
    phase = w % (a.groups + 1) if a.groups > 0 else 0
    im = np.asarray(Image.open(p).convert("L"), np.float32)
    h, wd = im.shape
    L, R = im[:, : wd // 2], im[:, wd // 2: wd // 2 * 2]
    bh, bw = h // 24, (wd // 2) // 24
    Lb = L[: bh * 24, : bw * 24].reshape(bh, 24, bw, 24).mean(axis=(1, 3))
    Rb = R[: bh * 24, : bw * 24].reshape(bh, 24, bw, 24).mean(axis=(1, 3))
    m = (Lb > 8) | (Rb > 8)
    ratio = (Rb[m] + 1) / (Lb[m] + 1)
    rows.append((q, phase, float(np.median(ratio)), float(np.percentile(ratio, 10)), float(Rb.mean() / max(1e-6, Lb.mean()))))
print(f"{os.path.basename(a.run)}: {len(rows)} frames in the play phase (from play + 20 s)")
print(f"   {'phase':<10}{'n':>4}{'R/L mean luma':>15}{'block median':>14}{'block p10':>11}")
for k in sorted({r[1] for r in rows}):
    sel = [r for r in rows if r[1] == k]
    print(f"   {names[k] if k < len(names) else k:<10}{len(sel):>4}{np.mean([r[4] for r in sel]):>15.3f}{np.mean([r[2] for r in sel]):>14.3f}{np.mean([r[3] for r in sel]):>11.3f}")
if rows:
    t0 = rows[0][0]
    thirds = np.array_split(np.array([(r[0], r[2], r[3]) for r in rows]), 3)
    print("   trend (thirds of the play phase): " + ", ".join(f"{(t[0][0] - t0) / 1e9:.0f}-{(t[-1][0] - t0) / 1e9:.0f} s median {t[:, 1].mean():.3f} p10 {t[:, 2].mean():.3f}" for t in thirds if len(t)))
