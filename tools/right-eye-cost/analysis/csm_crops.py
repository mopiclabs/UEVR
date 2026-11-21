"""Shadow-edge crops for the CSM share A/B (binocular frames of an SBS run): frames split into A/B phases by their QPC
time (phase = floor(qpc s / window) % 2; 0 = share for CSM share mode N >= 10, or --names), the right eye shifted onto
the left by the run's best horizontal offset, and for the chosen frames a montage of left | right | |left - right| x4
over the crop with the most edge energy in the lower two thirds (where cast shadows fall). Also prints, per phase,
the binocular-report.json metrics (one-eye shading, detail mismatch) if the harness's analysis has run.
  python csm_crops.py <run dir> --out <png> [--window 10 --names share,off --per-phase 3 --crop 360x240]"""
import argparse, glob, json, os, re
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser(); ap.add_argument("run"); ap.add_argument("--out", required=True)
ap.add_argument("--window", type=int, default=10); ap.add_argument("--names", default="share,off")
ap.add_argument("--per-phase", type=int, default=3); ap.add_argument("--crop", default="360x240")
ap.add_argument("--single-phase", action="store_true", help="no A/B: treat every frame as phase 0")
ap.add_argument("--coarse", type=int, default=1, help="pick crops by edges of the K x K block-mean image (cast-shadow "
                "and sun-patch boundaries) instead of full-resolution texture edges; e.g. 12")
ap.add_argument("--first-window", type=int, default=0, help="leave out frames before this window index (floor(qpc s / "
                "window)), e.g. the windows before the CSM share found its layout")
a = ap.parse_args()
names = a.names.split(",")
cw, ch = (int(x) for x in a.crop.split("x"))
frames = []
for p in sorted(glob.glob(os.path.join(a.run, "binocular", "*.png"))):
    q = int(re.match(r"(\d+)", os.path.basename(p)).group(1))
    if int(q / 1e9 // a.window) < a.first_window:
        continue
    ph = 0 if a.single_phase else int(q / 1e9 // a.window) % 2
    frames.append((q, ph, p))


def load(p):
    im = np.asarray(Image.open(p).convert("RGB"), np.float32)
    h, w, _ = im.shape
    return im[:, : w // 2], im[:, w // 2: w // 2 * 2]


def best_dx(L, R, rng=160):
    gl, gr = L.mean(axis=2), R.mean(axis=2)
    band = slice(gl.shape[0] // 3, gl.shape[0] * 5 // 6)
    best, bd = 1e18, 0
    for dx in range(-rng, rng + 1, 4):
        if dx >= 0:
            d = np.abs(gl[band, dx:] - gr[band, : gr.shape[1] - dx]).mean()
        else:
            d = np.abs(gl[band, : dx] - gr[band, -dx:]).mean()
        if d < best:
            best, bd = d, dx
    return bd


def shift(R, dx):
    out = np.zeros_like(R)
    if dx >= 0:
        out[:, dx:] = R[:, : R.shape[1] - dx]
    else:
        out[:, : dx] = R[:, -dx:]
    return out


def crop_pos(L):
    g = L.mean(axis=2)
    if a.coarse > 1:
        # large-scale light / shadow boundaries (sun patches, cast shadows) rather than texture: gradients of the
        # block-mean image, spread back to full resolution
        k = a.coarse
        h, w = g.shape[0] // k * k, g.shape[1] // k * k
        b = g[:h, :w].reshape(h // k, k, w // k, k).mean(axis=(1, 3))
        eb = np.zeros_like(b)
        eb[:-1, :-1] = np.abs(np.diff(b, axis=0))[:, :-1] + np.abs(np.diff(b, axis=1))[:-1, :]
        e = np.zeros((g.shape[0] - 1, g.shape[1] - 1), np.float32)
        e[:h - 1, :w - 1] = np.kron(eb, np.ones((k, k), np.float32))[:h - 1, :w - 1]
    else:
        gy, gx = np.abs(np.diff(g, axis=0))[:, :-1], np.abs(np.diff(g, axis=1))[:-1, :]
        e = gy + gx
    h, w = e.shape
    best, pos = -1, (0, 0)
    for y in range(h // 3, h - ch, ch // 3):
        for x in range(160, w - cw - 160, cw // 3):
            s = e[y:y + ch, x:x + cw].sum()
            if s > best:
                best, pos = s, (x, y)
    return pos, best


chosen = []
for ph in sorted({f[1] for f in frames}):
    cand = []
    for q, p_, path in frames:
        if p_ != ph:
            continue
        L, R = load(path)
        if L.mean() < 12:
            continue
        (x, y), en = crop_pos(L)
        cand.append((en, q, path, x, y))
    cand.sort(reverse=True)
    chosen += [(ph,) + c[1:] for c in cand[: a.per_phase]]
rows = []
for ph, q, path, x, y in chosen:
    L, R = load(path)
    dx = best_dx(L, R)
    Rs = shift(R, dx)
    lc, rc = L[y:y + ch, x:x + cw], Rs[y:y + ch, x:x + cw]
    diff = np.clip(np.abs(lc - rc) * 4, 0, 255)
    row = np.concatenate([lc, np.full((ch, 6, 3), 255, np.float32), rc, np.full((ch, 6, 3), 255, np.float32), diff], axis=1)
    rows.append(row)
    print(f"{names[ph] if ph < len(names) else ph}: {os.path.basename(path)} crop x{x} y{y} dx {dx} mean |L-R| {np.abs(lc - rc).mean():.2f}")
if rows:
    gap = np.full((8, rows[0].shape[1], 3), 255, np.float32)
    img = np.concatenate(sum([[r, gap] for r in rows], [])[:-1], axis=0)
    Image.fromarray(img.astype(np.uint8)).save(a.out)
    print(f"montage -> {a.out} ({len(rows)} rows: left | right shifted | |L-R| x4)")
rep = os.path.join(a.run, "binocular-report.json")
if os.path.exists(rep):
    fr = json.load(open(rep, encoding="utf-8")).get("frames", [])
    by = {}
    for f in fr:
        q = int(re.match(r"(\d+)", f["file"]).group(1))
        if int(q / 1e9 // a.window) < a.first_window:
            continue
        ph = 0 if a.single_phase else int(q / 1e9 // a.window) % 2
        by.setdefault(ph, []).append(f)
    for ph, fs in sorted(by.items()):
        sh = [x.get("shade_largest_pct") or 0 for x in fs]; mm = [x.get("largest_pct") or 0 for x in fs]
        print(f"binocular {names[ph] if ph < len(names) else ph}: {len(fs)} frames; one-eye shading max {max(sh):.1f} %, "
              f"median {np.median(sh):.1f} %; largest detail patch max {max(mm):.1f} %, median {np.median(mm):.1f} %")
