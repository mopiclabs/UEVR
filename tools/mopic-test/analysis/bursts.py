"""bursts: do both eyes stay together from one displayed frame to the next? (eyesampler.py --burst-every)

    .venv\\Scripts\\python analysis\\bursts.py analyze <run folder> [--jobs N] [--print]
    .venv\\Scripts\\python analysis\\bursts.py sheet <burst folder> [--out sheet.jpg]

The input is <run>\\bursts\\<n>-seg<s>\\: consecutive frames of the whole Mopic display (left eye | right eye under
MOPIC_MODE=sbs) that capture\\burstcap.exe grabbed for eyesampler.py, as fNN.png and burst.json (each frame's QPC
time and how many display refreshes it came after the previous one). A burst file a killed sampler left
(<n>-seg<s>.brst) is turned into PNGs first. Per frame, against the previous one:

  dL / dR        each eye's change: mean absolute luma difference (0-255) against its previous frame, on each eye
                 scaled to WORK_W
  frozen         one eye unchanged (d <= FROZEN_RATIO of the other's) while the other moved (>= MOVE): that eye
                 showed its previous picture again
  jump           one eye changed (>= JUMP) JUMP_RATIO times more than the other and than its own median in the
                 burst: a flash, a pop or a skipped picture in that eye only
  black          one eye's mean luma under BLACK_LEVEL while the other's is BRIGHT_MIN or more
  behind         the left eye matches the right eye's previous or next frame clearly better than the right eye's
                 frame of the same refresh (parallax search ALIGN_DX each way), while the scene moves, and the eyes'
                 change sizes line up one frame apart instead of in step: one eye a refresh behind the other
  lr spike       the same-refresh left/right match (cost_same) far worse than the burst's median: something drawn
                 in one eye for a moment; then binocular.py's comparison (shade / mismatch / luma / vertical offset)
                 of that frame against the burst's baseline (every BASELINE_EVERY-th frame) says what differs.
                 binocular.py takes seconds a frame, so it runs on flagged and baseline frames only

bursts-report.json gets the per-frame numbers, the flagged frames and a summary; bursts-flags\\ a picture per flag
(the frame before, the frame, the frame after; left | right; the eye named is outlined), and bursts-sheets\\ one
contact sheet per burst, all frames in order, for looking through by eye.
"""

import argparse
import glob
import json
import os
import statistics
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

WORK_W = 480             # each eye's width for the temporal checks (binocular.py's WORK_W too)
MOVE = 1.5               # an eye "moved" when its d is at least this
FROZEN_RATIO = 0.1       # ... and the other eye's d is at most this share of it: frozen
JUMP = 4.0               # a one-eye jump: d at least this,
JUMP_RATIO = 3.0         # this many times the other eye's d and this eye's median d in the burst
BLACK_LEVEL = 8.0
BRIGHT_MIN = 20.0
ALIGN_W = 240            # width for the "behind" match
ALIGN_DX = 48            # its horizontal search each way (ALIGN_W px; = 384 display px)
BEHIND_MARGIN = 0.85     # a neighbour's cost must be under this share of the same-refresh cost,
LAG_SHARE = 0.5          # and |this eye's d - the other's d one frame earlier| under this share of |dL - dR|
SPIKE_COST = 1.5         # lr spike: cost_same at least this many times the burst's median cost_same
SPIKE_COST_MIN = 3.0     # ... and this much above it (luma)
BASELINE_EVERY = 10      # binocular.py on every Nth frame of a burst (its median is what a flagged frame is held to)
SPIKE_SHADE = 3.0        # binocular on a flagged frame: shade_largest_pct this far above the baseline median,
SPIKE_MISMATCH = 10.0    # mismatch_pct this far above it,
SPIKE_LUMA = 4.0         # luma_diff_pct this far from it (percentage points),
SPIKE_VERTICAL = 8       # |vertical_px - median| at least this (display px)


def luma(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def load_eyes(path, width):
    """A side-by-side PNG -> (left, right) luma arrays, each eye scaled to `width`."""
    from PIL import Image
    img = Image.open(path).convert("RGB")
    w, h = img.size
    half = w // 2
    eh = int(round(h * width / half))
    out = []
    for box in ((0, 0, half, h), (half, 0, half * 2, h)):
        e = img.crop(box).resize((width, eh), Image.BILINEAR)
        out.append(luma(np.asarray(e, np.float32)))
    return out


def shift_cost(a, b, max_dx):
    """Smallest mean |a - b moved by dx| over dx in [-max_dx, max_dx], on the columns both cover."""
    w = a.shape[1]
    best = None
    for dx in range(-max_dx, max_dx + 1):
        if dx >= 0:
            c = np.abs(a[:, dx:] - b[:, :w - dx])[:, max_dx - dx:w - max_dx] if w - dx > 0 else None
        else:
            c = np.abs(a[:, :w + dx] - b[:, -dx:])[:, max_dx + dx:w - max_dx] if w + dx > 0 else None
        if c is None or c.size == 0:
            continue
        v = float(c.mean())
        if best is None or v < best:
            best = v
    return best


def ensure_pngs(burst_dir_or_raw):
    """A leftover .brst -> its PNG folder (eyesampler.write_burst_pngs); the folder."""
    if burst_dir_or_raw.endswith(".brst"):
        import eyesampler
        d = burst_dir_or_raw[:-5]
        eyesampler.write_burst_pngs(burst_dir_or_raw, d)
        os.remove(burst_dir_or_raw)
        return d
    return burst_dir_or_raw


def pair_metrics(path):
    import binocular
    lb, rb, _ = binocular.load_pair(path)
    res, _, _ = binocular.analyze_pair(lb, rb)
    return res


def analyze_burst(d, jobs_pool=None):
    with open(os.path.join(d, "burst.json"), encoding="utf-8") as f:
        meta = json.load(f)
    frames = meta["frames"]
    files = [os.path.join(d, fr["file"]) for fr in frames]
    eyes = [load_eyes(p, WORK_W) for p in files]
    small = [load_eyes(p, ALIGN_W) for p in files]
    rows = []
    for i, (l, r) in enumerate(eyes):
        row = {"i": i, "file": frames[i]["file"], "present_ns": frames[i]["present_ns"],
               "refreshes": frames[i]["refreshes"], "l_mean": round(float(l.mean()), 2),
               "r_mean": round(float(r.mean()), 2), "dL": None, "dR": None}
        if i:
            pl, pr = eyes[i - 1]
            row["dL"] = round(float(np.abs(l - pl).mean()), 3)
            row["dR"] = round(float(np.abs(r - pr).mean()), 3)
        sl, sr = small[i]
        row["cost_same"] = round(shift_cost(sl, sr, ALIGN_DX), 3)
        row["cost_prev"] = round(shift_cost(sl, small[i - 1][1], ALIGN_DX), 3) if i else None
        row["cost_next"] = round(shift_cost(sl, small[i + 1][1], ALIGN_DX), 3) if i + 1 < len(small) else None
        rows.append(row)

    def med(key, which=None):
        vals = [r[key] for r in (which or rows) if r.get(key) is not None]
        return statistics.median(vals) if vals else None

    medL, medR, m_cost = med("dL"), med("dR"), med("cost_same")
    # binocular.py on the baseline frames and on every frame whose same-refresh match is far off
    spiky = {r["i"] for r in rows if m_cost and r["cost_same"] >= SPIKE_COST * m_cost and
             r["cost_same"] - m_cost >= SPIKE_COST_MIN}
    todo = sorted(spiky | set(range(0, len(rows), BASELINE_EVERY)))
    pairs = (jobs_pool.map(pair_metrics, [files[i] for i in todo]) if jobs_pool is not None else
             [pair_metrics(files[i]) for i in todo])
    keys = ("shade_pct", "shade_largest_pct", "mismatch_pct", "largest_pct", "luma_diff_pct", "vertical_px",
            "best_dx_display")
    for i, p in zip(todo, pairs):
        rows[i].update({k: p.get(k) for k in keys})
        rows[i]["baseline"] = i % BASELINE_EVERY == 0
    base = [r for r in rows if r.get("baseline")]
    m_shade, m_mis = med("shade_largest_pct", base), med("mismatch_pct", base)
    m_luma, m_vert = med("luma_diff_pct", base), med("vertical_px", base)
    flags = []
    for r in rows:
        kinds = []
        dL, dR = r["dL"], r["dR"]
        if dL is not None:
            if dL >= MOVE and dR <= FROZEN_RATIO * dL:
                kinds.append(("right-frozen", "R"))
            if dR >= MOVE and dL <= FROZEN_RATIO * dR:
                kinds.append(("left-frozen", "L"))
            if dL >= JUMP and dL >= JUMP_RATIO * max(dR, 0.01) and dL >= JUMP_RATIO * (medL or 0.01):
                kinds.append(("left-jump", "L"))
            if dR >= JUMP and dR >= JUMP_RATIO * max(dL, 0.01) and dR >= JUMP_RATIO * (medR or 0.01):
                kinds.append(("right-jump", "R"))
        if r["l_mean"] < BLACK_LEVEL <= BRIGHT_MIN <= r["r_mean"]:
            kinds.append(("left-black", "L"))
        if r["r_mean"] < BLACK_LEVEL <= BRIGHT_MIN <= r["l_mean"]:
            kinds.append(("right-black", "R"))
        # one eye a refresh behind: L matches R's next (or previous) frame better than R's frame of the same refresh,
        # and the eyes' change sizes line up shifted by one frame rather than in step. A sideways camera move matches
        # a neighbour frame too (the right eye's earlier place is the left eye's later one), but both eyes then still
        # change by the same amount in the same frame (Tekken's orbiting camera, 2026-11-01)
        moving = (dL or 0) >= MOVE and (dR or 0) >= MOVE
        i = r["i"]
        if moving and r["cost_same"] and i >= 1:
            pL, pR = rows[i - 1]["dL"], rows[i - 1]["dR"]
            if (r["cost_next"] is not None and r["cost_next"] < BEHIND_MARGIN * r["cost_same"] and pL is not None and
                    abs(dR - pL) < LAG_SHARE * abs(dR - dL)):
                kinds.append(("right-behind", "R"))
            if (r["cost_prev"] is not None and r["cost_prev"] < BEHIND_MARGIN * r["cost_same"] and pR is not None and
                    abs(dL - pR) < LAG_SHARE * abs(dL - dR)):
                kinds.append(("left-behind", "L"))
        if r["i"] in spiky:
            spikes = []
            if m_shade is not None and r.get("shade_largest_pct") is not None and r["shade_largest_pct"] - m_shade >= SPIKE_SHADE:
                spikes.append("shade")
            if m_mis is not None and r.get("mismatch_pct") is not None and r["mismatch_pct"] - m_mis >= SPIKE_MISMATCH:
                spikes.append("mismatch")
            if m_luma is not None and r.get("luma_diff_pct") is not None and abs(r["luma_diff_pct"] - m_luma) >= SPIKE_LUMA:
                spikes.append("luma")
            if m_vert is not None and r.get("vertical_px") is not None and abs(r["vertical_px"] - m_vert) >= SPIKE_VERTICAL:
                spikes.append("vertical")
            kinds.append(("lr-spike-" + ("+".join(spikes) or "cost"), "LR"))
        for kind, eye in kinds:
            flags.append({"burst": os.path.basename(d), "i": r["i"], "kind": kind, "eye": eye})
    gaps = sum(1 for r in rows[1:] if r["refreshes"] > 1)
    summary = {"burst": os.path.basename(d), "frames": len(rows), "refresh_gaps": gaps,
               "span_ms": round((rows[-1]["present_ns"] - rows[0]["present_ns"]) / 1e6, 1) if rows else 0,
               "median_dL": medL, "median_dR": medR, "median_cost_same": m_cost, "median_shade_largest_pct": m_shade,
               "median_mismatch_pct": m_mis, "median_luma_diff_pct": m_luma, "median_vertical_px": m_vert,
               "flags": len(flags)}
    return {"summary": summary, "rows": rows, "flags": flags}


def flag_image(d, i, n, eye, out):
    """frames i-1, i, i+1 of a burst stacked (left | right as grabbed, half size), the named eye of frame i outlined."""
    from PIL import Image, ImageDraw
    tiles = []
    for k in (i - 1, i, i + 1):
        if 0 <= k < n:
            im = Image.open(os.path.join(d, f"f{k:02d}.png")).convert("RGB")
            im = im.resize((im.width // 2, im.height // 2), Image.BILINEAR)
            if k == i:
                dr = ImageDraw.Draw(im)
                hw = im.width // 2
                for e, x0 in (("L", 0), ("R", hw)):
                    if e in eye:
                        dr.rectangle([x0 + 2, 2, x0 + hw - 3, im.height - 3], outline=(255, 0, 0), width=4)
            tiles.append(im)
    w, h = tiles[0].size
    sheet = Image.new("RGB", (w, h * len(tiles) + 4 * (len(tiles) - 1)), (40, 40, 40))
    for k, t in enumerate(tiles):
        sheet.paste(t, (0, k * (h + 4)))
    sheet.save(out, quality=90)


def contact_sheet(d, out, cols=5, tile_w=480):
    """Every frame of a burst in order, left | right, numbered."""
    from PIL import Image, ImageDraw
    files = sorted(glob.glob(os.path.join(d, "f*.png")))
    if not files:
        return
    first = Image.open(files[0])
    th = int(round(first.height * tile_w / first.width))
    rows = (len(files) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tile_w + (cols - 1) * 4, rows * th + (rows - 1) * 4), (40, 40, 40))
    for k, p in enumerate(files):
        im = Image.open(p).convert("RGB").resize((tile_w, th), Image.BILINEAR)
        ImageDraw.Draw(im).text((6, 4), f"{k:02d}", fill=(255, 255, 0))
        sheet.paste(im, ((k % cols) * (tile_w + 4), (k // cols) * (th + 4)))
    sheet.save(out, quality=85)


def analyze(run, jobs=0, sheets=True):
    bdir = os.path.join(run, "bursts")
    for raw in glob.glob(os.path.join(bdir, "*.brst")):
        try:
            ensure_pngs(raw)
        except (OSError, ValueError) as e:
            print(f"bursts: {raw}: {e}", file=sys.stderr)
    dirs = sorted(p for p in glob.glob(os.path.join(bdir, "*")) if os.path.isfile(os.path.join(p, "burst.json")))
    pool = None
    if jobs != 1 and dirs:
        import multiprocessing
        pool = multiprocessing.Pool(jobs or max(1, (os.cpu_count() or 2) // 2))
    results = []
    try:
        for d in dirs:
            results.append(analyze_burst(d, pool))
    finally:
        if pool is not None:
            pool.close()
            pool.join()
    flags = [f for r in results for f in r["flags"]]
    fdir = os.path.join(run, "bursts-flags")
    for f in flags:
        os.makedirs(fdir, exist_ok=True)
        d = os.path.join(bdir, f["burst"])
        n = len(glob.glob(os.path.join(d, "f*.png")))
        f["image"] = f"bursts-flags\\{f['burst']}-f{f['i']:02d}-{f['kind']}.jpg"
        flag_image(d, f["i"], n, f["eye"], os.path.join(run, f["image"]))
    if sheets:
        sdir = os.path.join(run, "bursts-sheets")
        os.makedirs(sdir, exist_ok=True)
        for d in dirs:
            contact_sheet(d, os.path.join(sdir, os.path.basename(d) + ".jpg"))
    summ = summarize(results, flags)
    doc = {"summary": summ, "bursts": [r["summary"] for r in results], "flags": flags,
           "frames": {r["summary"]["burst"]: r["rows"] for r in results}, "line": line(summ)}
    with open(os.path.join(run, "bursts-report.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return doc


def summarize(results, flags):
    frames = sum(r["summary"]["frames"] for r in results)
    kinds = {}
    for f in flags:
        k = f["kind"].split("-spike-")[0] + "-spike" if "-spike-" in f["kind"] else f["kind"]
        kinds[k] = kinds.get(k, 0) + 1
    rows = [row for r in results for row in r["rows"]]

    def med(key):
        vals = [x[key] for x in rows if x.get(key) is not None]
        return round(statistics.median(vals), 2) if vals else None
    return {"bursts": len(results), "frames": frames,
            "refresh_gaps": sum(r["summary"]["refresh_gaps"] for r in results),
            "flags": len(flags), "by_kind": kinds, "median_luma_diff_pct": med("luma_diff_pct"),
            "median_shade_largest_pct": med("shade_largest_pct"), "median_mismatch_pct": med("mismatch_pct"),
            "median_vertical_px": med("vertical_px")}


def line(s):
    kinds = ", ".join(f"{k} {v}" for k, v in sorted(s["by_kind"].items())) or "none"
    return (f"bursts[{s['bursts']} bursts, {s['frames']} frames, {s['refresh_gaps']} refresh gaps]: flags {s['flags']} "
            f"({kinds}) | R-L luma {s['median_luma_diff_pct']}%, one-eye shading patch {s['median_shade_largest_pct']}%, "
            f"mismatch {s['median_mismatch_pct']}%, vertical {s['median_vertical_px']} px")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("analyze")
    s.add_argument("run")
    s.add_argument("--jobs", type=int, default=0)
    s.add_argument("--no-sheets", action="store_true")
    s.add_argument("--print", action="store_true")
    s = sub.add_parser("sheet")
    s.add_argument("burst")
    s.add_argument("--out", default="sheet.jpg")
    args = ap.parse_args()
    if args.cmd == "sheet":
        contact_sheet(ensure_pngs(args.burst), args.out)
        return
    doc = analyze(args.run, args.jobs, not args.no_sheets)
    if args.print:
        for f in doc["flags"]:
            print(json.dumps(f))
    print(doc["line"])


if __name__ == "__main__":
    main()
