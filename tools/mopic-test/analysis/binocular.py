"""binocular: do both eyes show the same scene, apart from parallax? (frames from eyesampler.py --full-every)

    .venv\\Scripts\\python analysis\\binocular.py analyze <run folder | folder of PNGs | one PNG> [--top 6] [--print]
    .venv\\Scripts\\python analysis\\binocular.py selftest

The input is the whole Mopic display while monado-service runs with MOPIC_MODE=sbs (left eye | right eye, each
squeezed into half the width), as eyesampler.py --full-every saves it into <run>\\binocular\\. Things that hurt when
the eyes disagree, beyond the horizontal shift stereo is made of, are measured per frame:

  shade         shading one eye has and the other hasn't (a shadow, light, fog or reflection drawn in one eye
                only, one eye a frame behind): for every pixel the smallest local difference (9x9 box SAD) between
                the low-passed luma of the left eye and the right eye's moved by dx in [-MAX_DX, MAX_DX] and dy in
                [-MAX_DY, MAX_DY]; 16x16 blocks whose best match still differs by more than SHADE_THRESHOLD differ.
                shade_pct = their share of the valid blocks (the frame-edge strips only one eye sees and pure black
                left out), shade_largest_pct = the biggest connected group. Flags "one-eye shading" at SHADE_FLAG.
  mismatch      the same on luma as is (--threshold): also catches different detail, LOD or texture, but particles,
                sparkles and view-dependent highlights (water, metal) differ between real eyes too, so "one-eye
                detail" (largest_pct >= DETAIL_FLAG) is a hint to look at the heat maps, not a fault by itself.
  luma_diff     mean luma of the right eye minus the left (percent of the left's): one eye darker / brighter
  color_diff    the same per channel (B, G, R), the largest absolute
  contrast      std of luma right / left: one eye washed out
  sharpness     mean |Laplacian| right / left: one eye blurrier (a different upscaler / TAA history per eye)
  vertical_px   the vertical shift (display px) the eyes match best at: should be 0; >1 px strains

analyze writes binocular-report.json next to the frames' run folder (per frame and a summary with the worst frame of
each metric) and binocular-heat\\<frame>.png for the --top frames with the largest differing patch: left eye | right
eye, on the left eye shade blocks tinted red and detail-only blocks yellow. The frames are analysed in --jobs worker
processes. Unattended runs need MOPIC_PREACQ_FOLLOW_SDK=false too: with nobody in front of the display the eyes
converge (both eyes the same picture) and nothing can be compared.
"""

import argparse
import glob
import json
import os
import sys

import numpy as np

LUMA = np.array([0.0722, 0.7152, 0.2126])   # B, G, R (Rec. 709)
WORK_W = 480            # each eye is analysed at this width (display eye width 1920 -> 1/4)
MAX_DX = 96             # work px (= 384 display px): horizontal search each way (a fist in a close-up is ~80)
MAX_DY = 1              # work px (= 4 display px); larger vertical offsets: vertical_offset()
SAD_WIN = 9             # box window of the per-pixel cost
BLOCK = 16              # mismatch blocks, work px
EDGE = 36               # columns at each side left out of the vertical offset search
EDGE_PAD = 8            # work px left out at each side beyond the frame's best horizontal shift
SHADE_WIN = 15          # low-pass window of the shade band (work px)
SHADE_THRESHOLD = 10.0
TEX_FOUND = 0.6         # mean SAD of the contrast-normalized texture (0 same, ~1.1 unrelated) up to which a block's
                        # surface counts as found in the other eye
TEX_MIN = 3.0           # local luma contrast a block needs for its texture to say anything
SHADE_MIN_W = 3         # blocks: narrower horizontal runs of differing shading are half occlusions
SHADE_FLAG = 1.5        # % of the frame: the biggest patch of differing shading that flags a run
EDGE_FLAG = 5.0         # % of the frame: the same for patches touching the left / right edge (window violations)
DETAIL_FLAG = 10.0
LIT = 15.0              # mean luma both eyes need for a frame to count in the whole-picture flags
FRAME_SHARE = 0.1       # brightness / vertical / blur flags: share of the lit frames (luma diff >= 8%, |dy| >= 8 px,
                        # sharpness R/L outside 0.8-1.25)      # % of the frame: the same for detail  # mean best-match SAD of the low-passed luma above which a block's shading differs
BLACK = 6.0             # luma below this in both eyes: not judged
THRESHOLD = 14.0        # mean best-match SAD (luma 0-255) above which a block is mismatched


def load_pair(path):
    """A side-by-side PNG -> (left, right) float32 BGR arrays of WORK_W wide, plus the display size."""
    from PIL import Image
    img = Image.open(path).convert("RGB")
    w, h = img.size
    half = w // 2
    eh = int(round(h * WORK_W / half))
    left = img.crop((0, 0, half, h)).resize((WORK_W, eh), Image.BILINEAR)
    right = img.crop((half, 0, half * 2, h)).resize((WORK_W, eh), Image.BILINEAR)
    lb = np.asarray(left, np.float32)[:, :, ::-1]
    rb = np.asarray(right, np.float32)[:, :, ::-1]
    return lb, rb, (w, h)


def luma(bgr):
    return bgr @ LUMA


def box(a, k):
    """Mean over a k x k window (same size, edges clamped) via cumulative sums."""
    p = k // 2
    a = np.pad(a, p, mode="edge")
    c = a.cumsum(0).cumsum(1)
    c = np.pad(c, ((1, 0), (1, 0)))
    s = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    return s / (k * k)


def shifted(a, dx, dy):
    """a moved so that out[y, x] = a[y - dy, x - dx] (edges clamped)."""
    h, w = a.shape
    ys = np.clip(np.arange(h) - dy, 0, h - 1)
    xs = np.clip(np.arange(w) - dx, 0, w - 1)
    return a[ys][:, xs]


def normalized(a, win):
    """a with its local mean removed and divided by its local contrast (win x win): a shadow or light that scales
    the brightness of a textured surface leaves this unchanged. Also the local contrast, to leave flat areas out."""
    hp = a - box(a, win)
    sd = np.sqrt(np.maximum(box(hp * hp, win), 0.0))
    return hp / (sd + 2.0), sd


def search(l, r, shifts, win, shade_win):
    """For every pixel of l, over the shifts (dx, dy) of r (a shift whose source falls outside r doesn't count):
    raw     the smallest box SAD of luma as is (the detail band)
    tex     the smallest box SAD of the contrast-normalized texture: how well the surface itself is found in r
    shade   at that texture-best shift, the box SAD of luma low-passed by shade_win: the lighting on the same
            surface in the two eyes
    shade_min  the smallest low-passed luma SAD over all shifts: no shift gives the same lighting
    and the mean raw cost per shift (for the global best shift)."""
    h, w = l.shape
    nl, _ = normalized(l, win)
    nr, _ = normalized(r, win)
    ls, rs = box(l, shade_win), box(r, shade_win)
    raw = np.full((h, w), np.inf, np.float32)
    tex = np.full((h, w), np.inf, np.float32)
    shade = np.full((h, w), np.inf, np.float32)
    shade_min = np.full((h, w), np.inf, np.float32)
    totals = {}
    x = np.arange(w)
    for dx, dy in shifts:
        out = (x - dx < 0) | (x - dx >= w)
        c = box(np.abs(l - shifted(r, dx, dy)), win).astype(np.float32)
        c[:, out] = np.inf
        np.minimum(raw, c, out=raw)
        totals[(dx, dy)] = float(c[:, ~out].mean())
        t = box(np.abs(nl - shifted(nr, dx, dy)), win).astype(np.float32)
        t[:, out] = np.inf
        better = t < tex
        tex[better] = t[better]
        sh = box(np.abs(ls - shifted(rs, dx, dy)), win).astype(np.float32)
        shade[better] = sh[better]
        sh[:, out] = np.inf
        np.minimum(shade_min, sh, out=shade_min)
    return raw, tex, shade, shade_min, totals


def best_match(l, r, max_dx=MAX_DX, max_dy=MAX_DY):
    """Per pixel of the left eye: raw (smallest luma SAD, the detail band), tex (how well its texture is found in the
    right eye) and shade (the low-passed luma difference on that same surface), plus the global best shift. Things
    right in front of the camera (a cat's ear, a phone held up, leaves) shift further than max_dx: a coarse pass at
    half resolution covers shifts up to 2 * max_dx."""
    shifts = [(dx, dy) for dy in range(-max_dy, max_dy + 1) for dx in range(-max_dx, max_dx + 1)]
    raw, tex, shade, shade_min, totals = search(l, r, shifts, SAD_WIN, SHADE_WIN)
    (gdx, gdy) = min(totals, key=totals.get)
    h, w = l.shape
    h2, w2 = h // 2, w // 2
    half = lambda a: a[:h2 * 2, :w2 * 2].reshape(h2, 2, w2, 2).mean((1, 3))
    far = [(dx, 0) for dx in range(-max_dx, max_dx + 1) if abs(dx) * 2 > max_dx]
    raw2, tex2, shade2, shade_min2, _ = search(half(l), half(r), far, SAD_WIN // 2 | 1, SHADE_WIN // 2 | 1)
    up = lambda a: np.repeat(np.repeat(a, 2, 0), 2, 1)
    sub = (slice(0, h2 * 2), slice(0, w2 * 2))
    np.minimum(raw[sub], up(raw2), out=raw[sub])
    np.minimum(shade_min[sub], up(shade_min2), out=shade_min[sub])
    better = up(tex2) < tex[sub]
    tex[sub][better] = up(tex2)[better]
    shade[sub][better] = up(shade2)[better]
    return raw, tex, shade, shade_min, gdx, gdy


def vertical_offset(l, r, max_dy=6, max_dx=EDGE - 4):
    """dy (work px) of the best global match over a wider vertical range, on textured rows."""
    gl, gr = l - box(l, 15), r - box(r, 15)      # high-pass: brightness differences don't decide it
    best, arg = None, 0
    for dy in range(-max_dy, max_dy + 1):
        costs = [np.abs(gl[:, EDGE:-EDGE] - shifted(gr, dx, dy)[:, EDGE:-EDGE]).mean()
                 for dx in range(-max_dx, max_dx + 1, 2)]
        c = min(costs)
        if best is None or c < best:
            best, arg = c, dy
    return arg


def laplacian_mean(a):
    lap = (np.roll(a, 1, 0) + np.roll(a, -1, 0) + np.roll(a, 1, 1) + np.roll(a, -1, 1) - 4 * a)[1:-1, 1:-1]
    return float(np.abs(lap).mean())


def largest_group(mask):
    """Sizes of the biggest 4-connected group of True cells away from the left and right edges, and of the biggest
    touching one: something near at the frame edge is seen by one eye only (a stereo window violation), which is
    geometry, not a rendering fault."""
    seen = np.zeros_like(mask, bool)
    best, best_edge = 0, 0
    h, w = mask.shape
    for y in range(h):
        for x in range(w):
            if mask[y, x] and not seen[y, x]:
                stack, n, edge = [(y, x)], 0, False
                seen[y, x] = True
                while stack:
                    cy, cx = stack.pop()
                    n += 1
                    edge = edge or cx == 0 or cx == w - 1
                    for ny, nx in ((cy + 1, cx), (cy - 1, cx), (cy, cx + 1), (cy, cx - 1)):
                        if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = True
                            stack.append((ny, nx))
                if edge:
                    best_edge = max(best_edge, n)
                else:
                    best = max(best, n)
    return best, best_edge


def wide(mask, n=SHADE_MIN_W):
    """Blocks in horizontal runs of at least n: a strip narrower than that along an object's edge is the background
    one eye sees past it (half occlusion, normal stereo), not a shadow or light missing in one eye."""
    out = np.zeros_like(mask)
    for i in range(mask.shape[1] - n + 1):
        run = mask[:, i:i + n].all(1)
        out[run, i:i + n] = True
    return out


def blocks(a, border, bh, bw):
    return a[:bh * BLOCK, border:border + bw * BLOCK].reshape(bh, BLOCK, bw, BLOCK).mean((1, 3))


def analyze_pair(lb, rb, threshold=THRESHOLD, shade_threshold=SHADE_THRESHOLD):
    l, r = luma(lb), luma(rb)
    best, tex, best_s, shade_min, gdx, gdy = best_match(l, r)
    h, w = l.shape
    # the strip at the left eye's edges that the right eye sees past its own frame edge: one-eye by geometry
    border = abs(gdx) + EDGE_PAD
    bh, bw = h // BLOCK, (w - 2 * border) // BLOCK
    cost, cost_s = blocks(best, border, bh, bw), blocks(best_s, border, bh, bw)
    bright = blocks(np.maximum(l, shifted(r, gdx, gdy)), border, bh, bw)
    valid = (bright >= BLACK) & np.isfinite(cost)
    mism = (cost > threshold) & valid
    # shading is judged where the surface itself is found in the other eye (its texture matches): background only
    # one eye sees beside a near object (half occlusion) and flat areas aren't, a shadow drawn in one eye only is
    _, contrast = normalized(l, SAD_WIN)
    found = (blocks(tex, border, bh, bw) <= TEX_FOUND) & (blocks(contrast, border, bh, bw) >= TEX_MIN)
    # and no shift at all gives the same lighting there (repeating textures, cobblestones, can be "found" at the
    # wrong place)
    unlit = blocks(shade_min, border, bh, bw) > shade_threshold
    shade = wide((cost_s > shade_threshold) & valid & found & unlit)
    nvalid = int(valid.sum())
    # whole-picture comparisons over what both eyes see: the right eye moved by the best shift, edge strips left out
    cols = slice(border, w - border)
    ro = shifted(r, gdx, gdy)[:, cols]
    lo = l[:, cols]
    rbo = np.stack([shifted(rb[:, :, c], gdx, gdy) for c in range(3)], 2)[:, cols]
    lm, rm = float(lo.mean()), float(ro.mean())
    cl, cr = lb[:, cols].reshape(-1, 3).mean(0), rbo.reshape(-1, 3).mean(0)
    sl, sr = laplacian_mean(lo), laplacian_mean(ro)

    shade_groups = largest_group(shade)

    def pct(n):
        return round(100.0 * n / nvalid, 2) if nvalid else None
    res = {
        "shade_pct": pct(shade.sum()), "shade_largest_pct": pct(shade_groups[0]), "shade_edge_pct": pct(shade_groups[1]),
        "mismatch_pct": pct(mism.sum()), "largest_pct": pct(largest_group(mism)[0]),
        "luma_left": round(lm, 1), "luma_right": round(rm, 1),
        "luma_diff_pct": round(100.0 * (rm - lm) / lm, 2) if lm > 1 else None,
        "color_diff": round(float(np.abs(cr - cl).max()), 2),
        "contrast": round(float(ro.std() / lo.std()), 3) if lo.std() > 1 else None,
        "sharpness": round(sr / sl, 3) if sl > 0.05 else None,
        "best_dx_display": gdx * 4, "vertical_px": vertical_offset(l, r) * 4,
        "valid_blocks": nvalid,
    }
    return res, (mism, shade), (border, bh, bw)


def heat_image(lb, rb, masks, geom, path):
    """left eye | right eye; on the left eye shade mismatches tinted red, detail-only mismatches yellow."""
    from PIL import Image
    border, bh, bw = geom
    mism, shade = masks
    tint = lb.copy()
    for y in range(bh):
        for x in range(bw):
            if mism[y, x] or shade[y, x]:
                y0, x0 = y * BLOCK, border + x * BLOCK
                blk = tint[y0:y0 + BLOCK, x0:x0 + BLOCK]
                color = [0, 0, 255] if shade[y, x] else [0, 220, 255]
                blk[:] = blk * 0.5 + np.array(color, np.float32) * 0.5
    both = np.hstack([tint, rb])[:, :, ::-1].clip(0, 255).astype(np.uint8)
    Image.fromarray(both, "RGB").save(path)


def frame_files(path):
    if os.path.isfile(path):
        return [path], os.path.dirname(os.path.abspath(path))
    d = os.path.join(path, "binocular") if os.path.isdir(os.path.join(path, "binocular")) else path
    files = sorted(glob.glob(os.path.join(d, "*.png")))
    out = path if d != path else os.path.dirname(os.path.abspath(d.rstrip("\\/")))
    return files, out


def analyze_file(args):
    """One frame (a process pool worker): its metrics and block masks."""
    f, threshold = args
    lb, rb, size = load_pair(f)
    res, masks, geom = analyze_pair(lb, rb, threshold)
    res["file"] = os.path.basename(f)
    return res, masks, geom


def analyze(path, top=6, threshold=THRESHOLD, jobs=0):
    files, out_dir = frame_files(path)
    jobs = jobs or max(1, min(8, (os.cpu_count() or 2) - 2))
    if jobs > 1 and len(files) > 1:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(jobs) as pool:
            results = list(pool.map(analyze_file, [(f, threshold) for f in files]))
    else:
        results = [analyze_file((f, threshold)) for f in files]
    frames = [r[0] for r in results]
    worst = sorted(zip(files, results), key=lambda k: (k[1][0]["shade_largest_pct"] or 0, k[1][0]["largest_pct"] or 0),
                   reverse=True)[:top]
    keep = []
    for f, (res, masks, geom) in worst:
        lb, rb, size = load_pair(f)
        keep.append((None, f, lb, rb, masks, geom))
    heat_dir = os.path.join(out_dir, "binocular-heat")
    for old in glob.glob(os.path.join(heat_dir, "*.png")):      # an earlier analysis's worst frames
        os.remove(old)
    if keep:
        os.makedirs(heat_dir, exist_ok=True)
    for pct, f, lb, rb, mism, geom in keep:
        heat_image(lb, rb, mism, geom, os.path.join(heat_dir, os.path.basename(f)))
    doc = {"version": 1, "summary": summarize(frames, threshold), "frames": frames,
           "heat": [os.path.join("binocular-heat", os.path.basename(k[1])) for k in keep]}
    with open(os.path.join(out_dir, "binocular-report.json"), "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1)
    return doc


def resummarize(path):
    """binocular-report.json's summary again from its per-frame values (after the flag rules changed)."""
    rp = path if path.endswith(".json") else os.path.join(path, "binocular-report.json")
    with open(rp, encoding="utf-8") as fh:
        doc = json.load(fh)
    doc["summary"] = summarize(doc["frames"], doc["summary"].get("threshold", THRESHOLD))
    with open(rp, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1)
    return doc


def summarize(frames, threshold):
    def stat(key, fn=max, absval=False):
        vals = [(abs(fr[key]) if absval else fr[key], fr["file"]) for fr in frames if fr.get(key) is not None]
        if not vals:
            return None
        v, f = fn(vals)
        return {"value": v, "frame": f}

    def median(key, absval=False):
        vals = sorted(abs(fr[key]) if absval else fr[key] for fr in frames if fr.get(key) is not None)
        return vals[len(vals) // 2] if vals else None

    lit = [fr for fr in frames if fr["luma_left"] >= LIT and fr["luma_right"] >= LIT]

    def share(test):
        return round(sum(1 for fr in lit if test(fr)) / len(lit), 3) if lit else None

    summary = {
        "frames": len(frames), "threshold": threshold,
        "shade_pct_median": median("shade_pct"), "shade_largest_pct_max": stat("shade_largest_pct"),
        "shade_edge_pct_max": stat("shade_edge_pct"),
        "mismatch_pct_median": median("mismatch_pct"), "mismatch_pct_max": stat("mismatch_pct"),
        "largest_pct_max": stat("largest_pct"),
        "luma_diff_pct_abs_max": stat("luma_diff_pct", absval=True),
        "color_diff_max": stat("color_diff"),
        "sharpness_min": stat("sharpness", fn=min), "sharpness_max": stat("sharpness"),
        "contrast_min": stat("contrast", fn=min), "contrast_max": stat("contrast"),
        "vertical_px_abs_max": stat("vertical_px", absval=True), "vertical_px_median": median("vertical_px", True),
        # whole-picture flags need the difference in a share of the lit frames, not one: a fade, a cut or a black
        # frame gives any single frame an extreme value
        "lit_frames": len(lit),
        "brightness_share": share(lambda f: f["luma_diff_pct"] is not None and abs(f["luma_diff_pct"]) >= 8.0),
        "vertical_share": share(lambda f: abs(f["vertical_px"]) >= 8),
        "blur_share": share(lambda f: f["sharpness"] is not None and not 0.8 <= f["sharpness"] <= 1.25),
    }
    summary["flags"] = flags(summary)
    summary["line"] = line(summary)
    return summary


def flags(s):
    out = []
    def v(k):
        x = s.get(k)
        return x["value"] if isinstance(x, dict) else x
    if (v("shade_largest_pct_max") or 0) >= SHADE_FLAG:
        out.append("one-eye shading")
    if (v("shade_edge_pct_max") or 0) >= EDGE_FLAG:
        out.append("one-eye at frame edge")
    if (v("largest_pct_max") or 0) >= DETAIL_FLAG:
        out.append("one-eye detail")
    if (s.get("brightness_share") or 0) >= FRAME_SHARE:
        out.append("brightness differs")
    if (s.get("vertical_share") or 0) >= FRAME_SHARE:
        out.append("vertical offset")
    if (s.get("blur_share") or 0) >= FRAME_SHARE:
        out.append("one eye blurrier")
    return out


def line(s):
    def v(k, f="{:.1f}"):
        x = s.get(k)
        x = x["value"] if isinstance(x, dict) else x
        return "-" if x is None else f.format(x)
    return (f"binocular[{s['frames']} frames]: one-eye shading median {v('shade_pct_median')}% (largest patch max "
            f"{v('shade_largest_pct_max')}%, at the frame edge {v('shade_edge_pct_max')}%) | detail median {v('mismatch_pct_median')}% (largest patch max "
            f"{v('largest_pct_max')}%) | luma diff max "
            f"{v('luma_diff_pct_abs_max')}% | sharpness R/L {v('sharpness_min', '{:.2f}')}-{v('sharpness_max', '{:.2f}')} | "
            f"vertical max {v('vertical_px_abs_max', '{:.0f}')} px"
            + (" | FLAGS: " + ", ".join(s["flags"]) if s["flags"] else ""))


# ---------------------------------------------------------------------------------------------------------------
# self-test: a synthetic stereo pair, then the same pair with one eye's shadow removed / darkened / blurred / raised

def synthetic(seed=1, w=1920, h=1080, disparity=24):
    """A textured scene as a side-by-side frame (each eye 960 wide): a noise texture with a dark 'shadow' rectangle;
    the right eye sees it shifted left by `disparity` display-frame px (half width per eye)."""
    rng = np.random.default_rng(seed)
    half = w // 2
    base = rng.normal(120, 40, (h // 4, (half + disparity) // 4, 3)).clip(0, 255)
    base = np.kron(base, np.ones((4, 4, 1)))                       # texture at a scale the analysis resolves
    shadow = np.zeros(base.shape[:2], bool)
    shadow[400:700, 300:600] = True
    scene = base.copy()
    scene[shadow] *= 0.35
    left = scene[:h, disparity:disparity + half]
    right = scene[:h, 0:half]
    return left, right, base


def to_png(left, right, path):
    from PIL import Image
    Image.fromarray(np.hstack([left, right]).clip(0, 255).astype(np.uint8)[:, :, ::-1], "RGB").save(path)


def selftest(work):
    os.makedirs(work, exist_ok=True)
    left, right, base = synthetic()
    cases = {}
    to_png(left, right, os.path.join(work, "0-same.png"))
    r2 = right.copy()
    disparity, half = 24, 960
    noshadow = base[:1080, 0:half]
    r2[:] = noshadow                                         # the right eye without the shadow
    to_png(left, r2, os.path.join(work, "1-shadow-one-eye.png"))
    to_png(left, right * 0.85, os.path.join(work, "2-right-darker.png"))
    from PIL import Image, ImageFilter
    blur = np.asarray(Image.fromarray(right.clip(0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(3)), np.float32)
    to_png(left, blur, os.path.join(work, "3-right-blurred.png"))
    to_png(left, np.roll(right, 24, 0), os.path.join(work, "4-right-raised.png"))
    r5 = right.copy()                                        # a small ground shadow (~2% of the eye) in the left eye only
    l5 = left.copy()
    l5[850:1000, 600:800] *= 0.45
    to_png(l5, r5, os.path.join(work, "5-small-shadow-left-only.png"))
    # a textured object close to the camera (160 px disparity, more than the background's 24) in front of the
    # background: the background beside it is seen by one eye only (half occlusion), which is not a fault
    rng = np.random.default_rng(7)
    obj = np.kron(rng.normal(150, 50, (75, 50, 3)).clip(0, 255), np.ones((4, 4, 1)))
    l6, r6 = left.copy(), right.copy()
    l6[300:600, 500:700] = obj
    r6[300:600, 340:540] = obj
    to_png(l6, r6, os.path.join(work, "6-near-object.png"))
    fails = 0
    # (frame, flags it must raise, flags it must not)
    for name, want, never in (("0-same.png", [], None), ("1-shadow-one-eye.png", ["one-eye shading"], []),
                              ("2-right-darker.png", ["brightness differs"], []),
                              ("3-right-blurred.png", ["one eye blurrier"], ["one-eye shading"]),
                              ("4-right-raised.png", ["vertical offset"], []),
                              ("5-small-shadow-left-only.png", ["one-eye shading"], ["brightness differs"]),
                              ("6-near-object.png", [], ["one-eye shading"])):
        d = os.path.join(work, name[:-4])
        os.makedirs(d, exist_ok=True)
        os.replace(os.path.join(work, name), os.path.join(d, name))
        doc = analyze(d)
        got = doc["summary"]["flags"]
        ok = all(w in got for w in want) and (not got if never is None else not any(n in got for n in never))
        fails += not ok
        print(("ok   " if ok else "FAIL ") + f" {name}: flags {got} | {doc['summary']['line']}")
    print("binocular self-test: " + ("all passed" if not fails else f"{fails} failed"))
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("analyze")
    s.add_argument("path")
    s.add_argument("--top", type=int, default=6)
    s.add_argument("--threshold", type=float, default=THRESHOLD)
    s.add_argument("--jobs", type=int, default=0, help="worker processes (0: CPUs - 2, at most 8)")
    s.add_argument("--print", action="store_true")
    s = sub.add_parser("resummarize", help="redo a binocular-report.json's summary and flags from its frames")
    s.add_argument("path")
    s.add_argument("--print", action="store_true")
    s = sub.add_parser("selftest")
    s.add_argument("--work", default=os.path.join(os.environ.get("TEMP", "."), "binocular-selftest"))
    args = ap.parse_args()
    if args.cmd == "selftest":
        sys.exit(1 if selftest(args.work) else 0)
    if args.cmd == "resummarize":
        doc = resummarize(args.path)
    else:
        doc = analyze(args.path, args.top, args.threshold, args.jobs)
    if args.print:
        for fr in doc["frames"]:
            print(json.dumps(fr))
    print(doc["summary"]["line"])


if __name__ == "__main__":
    main()
