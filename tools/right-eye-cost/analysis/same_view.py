"""Same-view comparison of binocular frames between two conditions (e.g. CSM share vs off windows of an A/B run, or an
align-only run vs the off windows of another run of the same recipe). Wukong-still's idle camera comes back to the
same views, so frames of condition A and B with near-identical LEFT eyes exist; the left eye renders the same way in
both conditions, so |L_A - L_B| is the control (animation, sampling time), and |R_A - R_B| beyond it is what the
condition changed in the right eye.

For the --pairs closest pairs (by left-eye difference on a 1/8 image), prints the mean |L_A - L_B| and |R_A - R_B|
(full resolution, 0-255) and their ratio, overall and in the darker half of the left image (shadowed pixels), and
writes a montage per pair: L_A | L_B | |L_A - L_B| x4 over R_A | R_B | |R_A - R_B| x4 (each eye at half size).

  python same_view.py --a <run>[:<phase>] --b <run>[:<phase>] --out <png> [--window 10 --first-a W --first-b W --pairs 3]
  phase: 0 = even windows, 1 = odd windows, omitted = all frames of that run"""
import argparse, glob, os, re
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("--a", required=True); ap.add_argument("--b", required=True); ap.add_argument("--out", required=True)
ap.add_argument("--window", type=int, default=10); ap.add_argument("--first-a", type=int, default=0)
ap.add_argument("--first-b", type=int, default=0); ap.add_argument("--pairs", type=int, default=3)
ap.add_argument("--labels", default="A,B")
a = ap.parse_args()
la, lb = a.labels.split(",")


def frames(spec, first):
    run, _, ph = spec.rpartition(":")
    if ph not in ("0", "1"):  # no phase given (a drive letter's colon is not one)
        run, ph = spec, ""
    out = []
    for p in sorted(glob.glob(os.path.join(run, "binocular", "*.png"))):
        q = int(re.match(r"(\d+)", os.path.basename(p)).group(1))
        w = int(q / 1e9 // a.window)
        if w < first or (ph != "" and w % 2 != int(ph)):
            continue
        out.append(p)
    return out


def eyes(p):
    im = np.asarray(Image.open(p).convert("RGB"), np.float32)
    w = im.shape[1] // 2
    return im[:, :w], im[:, w:2 * w]


def small(x):
    h, w = x.shape[0] // 8 * 8, x.shape[1] // 8 * 8
    return x[:h, :w].mean(axis=2).reshape(h // 8, 8, w // 8, 8).mean(axis=(1, 3))


A, B = frames(a.a, a.first_a), frames(a.b, a.first_b)
sa = {p: small(eyes(p)[0]) for p in A}
sb = {p: small(eyes(p)[0]) for p in B}
cand = []
for pa in A:
    if sa[pa].mean() < 12:
        continue
    for pb in B:
        cand.append((np.abs(sa[pa] - sb[pb]).mean(), pa, pb))
cand.sort()
used, pairs = set(), []
for d, pa, pb in cand:
    if pa in used or pb in used:
        continue
    used |= {pa, pb}
    pairs.append((d, pa, pb))
    if len(pairs) >= a.pairs:
        break
rows = []
for d, pa, pb in pairs:
    La, Ra = eyes(pa); Lb, Rb = eyes(pb)
    dl, dr = np.abs(La - Lb), np.abs(Ra - Rb)
    dark = La.mean(axis=2) < np.median(La.mean(axis=2))
    darkr = Ra.mean(axis=2) < np.median(Ra.mean(axis=2))
    print(f"{la} {os.path.basename(pa)[:15]} vs {lb} {os.path.basename(pb)[:15]}: match {d:.2f} | mean |dL| {dl.mean():.2f} "
          f"|dR| {dr.mean():.2f} ratio {dr.mean() / max(dl.mean(), 1e-6):.2f} | darker half |dL| {dl.mean(axis=2)[dark].mean():.2f} "
          f"|dR| {dr.mean(axis=2)[darkr].mean():.2f} | luma L {La.mean():.1f}/{Lb.mean():.1f} R {Ra.mean():.1f}/{Rb.mean():.1f}")
    half = lambda x: np.asarray(Image.fromarray(np.clip(x, 0, 255).astype(np.uint8)).resize((x.shape[1] // 2, x.shape[0] // 2)), np.float32)
    sep = lambda h: np.full((h, 4, 3), 255, np.float32)
    top = [half(La), half(Lb), half(dl * 4)]
    bot = [half(Ra), half(Rb), half(dr * 4)]
    h = top[0].shape[0]
    rows.append(np.concatenate([top[0], sep(h), top[1], sep(h), top[2]], axis=1))
    rows.append(np.concatenate([bot[0], sep(h), bot[1], sep(h), bot[2]], axis=1))
    rows.append(np.full((10, rows[-1].shape[1], 3), 255, np.float32))
if rows:
    Image.fromarray(np.concatenate(rows[:-1], axis=0).astype(np.uint8)).save(a.out)
    print(f"montage -> {a.out}: per pair, top row left eye {la} | {lb} | |diff| x4, bottom row right eye the same")
