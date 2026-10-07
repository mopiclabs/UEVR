"""Re-record a recipe's checkpoints after the game's layout changed (e.g. the Mopic display going from a pillarboxed
16:10 picture to a 16:9 fullscreen game), by replaying the recipe against the running game.

When a checkpoint doesn't match, its screen from an old passing run (runs/*/pilot/NN-<name>.png, before --before)
is located in the live screenshot: a wide patch around the old region is searched at a few scales, which gives
where the region went; the old crop is then checked there (shape, or colour for highlights), and if it fits, the
live pixels at the new region become the checkpoint image and region. Everything else runs as gamepilot's `run`.

  .venv\\Scripts\\python analysis\\relearn.py <process> recipes\\<Recipe>.json --before 20261006-180000
      [--var k=v ...] [--steps A-B] [--out DIR]

Changed images are written into the recipe's image folder and the regions into the recipe file at the end
(and after each relearned checkpoint, so a failed run keeps what it learned). Check with `gamepilot.py - matrix`.
"""
import argparse, glob, json, math, os, re, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
import gamepilot as gp  # noqa: E402

SCALES = (1.0, 10 / 9, 0.9, 1.25, 0.8)


def old_screens(before, game_glob):
    """checkpoint name -> newest old screenshot of it (runs before `before`)."""
    found = {}
    for d in sorted(glob.glob(os.path.join(ROOT, "runs", f"*{game_glob}*"))):
        stamp = os.path.basename(d)[:15]
        if not re.match(r"\d{8}-\d{6}", stamp) or stamp >= before:
            continue
        for f in glob.glob(os.path.join(d, "pilot", "*.png")):
            m = re.match(r"\d+-(.+)\.png$", os.path.basename(f))
            if m and not m.group(1).startswith("FAILED"):
                found[m.group(1)] = f
    return found


def gray(img, k):
    g = img.convert("L")
    return np.asarray(g.resize((max(1, int(g.width * k)), max(1, int(g.height * k))), Image.BILINEAR), dtype=np.float64)


def ncc_search(hay, tpl):
    """Best normalized cross-correlation of tpl over hay (both 2-D arrays) -> (score, x, y)."""
    from numpy.lib.stride_tricks import sliding_window_view
    th, tw = tpl.shape
    if th > hay.shape[0] or tw > hay.shape[1]:
        return -1.0, 0, 0
    t = tpl - tpl.mean()
    tn = math.sqrt(float((t * t).sum())) or 1.0
    win = sliding_window_view(hay, (th, tw))
    num = np.einsum("ijkl,kl->ij", win, t)
    s1 = win.sum(axis=(2, 3))
    s2 = np.einsum("ijkl,ijkl->ij", win, win)
    var = np.maximum(s2 - s1 * s1 / (th * tw), 1e-9)
    score = num / (np.sqrt(var) * tn)
    y, x = np.unravel_index(int(np.argmax(score)), score.shape)
    return float(score[y, x]), int(x), int(y)


def locate(old_full, live, region, pad=60, k=0.5):
    """Where `region` of old_full went in live: (context score, scale, new region)."""
    x, y, w, h = region
    cx0, cy0 = max(0, x - pad), max(0, y - pad)
    cx1, cy1 = min(old_full.width, x + w + pad), min(old_full.height, y + h + pad)
    ctx = old_full.crop((cx0, cy0, cx1, cy1))
    hay = gray(live, k)
    best = (-1.0, 1.0, region)
    for s in SCALES:
        tpl = gray(ctx.resize((max(4, int(ctx.width * s)), max(4, int(ctx.height * s))), Image.BILINEAR), k)
        sc, px, py = ncc_search(hay, tpl)
        if s != 1.0:
            sc -= 0.05  # another scale only when clearly better
        if sc > best[0]:
            nx, ny = px / k + (x - cx0) * s, py / k + (y - cy0) * s
            best = (sc, s, [int(round(nx)), int(round(ny)), max(2, int(round(w * s))), max(2, int(round(h * s)))])
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("process")
    ap.add_argument("recipe")
    ap.add_argument("--before", required=True, help="old-layout runs are those before this stamp (YYYYmmdd-HHMMSS)")
    ap.add_argument("--game", default=None, help="run-folder glob for old screenshots (default: the recipe's game)")
    ap.add_argument("--var", action="append", default=[])
    ap.add_argument("--steps", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--min-context", type=float, default=0.8)
    ap.add_argument("--eager", action="store_true",
                    help="wait_for steps relocate on every check (for waits that press keys meanwhile); seek never relocates")
    ap.add_argument("--first-timeout", type=float, default=0,
                    help="cap a wait's first (as recorded) attempt at this many seconds; 0 = the recipe's timeout")
    args = ap.parse_args()

    recipe_path = os.path.abspath(args.recipe)
    game = args.game or os.path.basename(recipe_path).split("-")[0].split(".")[0]
    lib = old_screens(args.before, game)
    print(f"old screenshots for {len(lib)} checkpoint names ({game})")
    done, changed = set(), {}
    orig_score = gp.checkpoint_score
    relocating = [False]

    def save_region(name, region):
        """Only that checkpoint's "region" in the file, its formatting kept."""
        with open(recipe_path, encoding="utf-8") as f:
            text = f.read()
        pat = re.compile(r'("' + re.escape(name) + r'": \{[^\n]*?"region": )\[[^\]]*\]')
        if len(pat.findall(text)) != 1:
            raise SystemExit(f"can't find the region of {name} in {recipe_path}")
        with open(recipe_path, "w", encoding="utf-8", newline="") as f:
            f.write(pat.sub(lambda m: m.group(1) + json.dumps(region), text))

    # Relocating only after a wait_for has failed with the recipe as it is: a first check that misses is normal
    # (a seek's cursor still elsewhere, a menu still fading in), and two identical-looking places (Stray's save
    # slots 1 and 3) must not be taken for each other. seek steps never relocate.
    orig_wait = gp.Pilot.wait_for

    def wait_for(self, names, timeout, *a, **kw):
        if args.eager:
            # relocate on every check of the wait: a wait that presses keys meanwhile (skipping movies) must
            # recognize its screen at once, or its presses act on that screen (Tekken: Enter at the main menu
            # opened STORY)
            relocating[0] = True
            try:
                return orig_wait(self, names, timeout, *a, **kw)
            finally:
                relocating[0] = False
        try:
            first = min(timeout, args.first_timeout) if args.first_timeout else timeout
            return orig_wait(self, names, first, *a, **kw)
        except gp.Failed as e:
            if e.kind != "checkpoint":
                raise
            self.log("  relearn: not found as recorded, locating it from the old screenshot")
            relocating[0] = True
            try:
                return orig_wait(self, names, min(timeout, 20), *a, **kw)
            finally:
                relocating[0] = False

    gp.Pilot.wait_for = wait_for

    def score(rp, recipe, name, img):
        s = orig_score(rp, recipe, name, img)
        cp = recipe["checkpoints"][name]
        if not relocating[0] or name in done or s >= cp.get("threshold", 0.8) or name not in lib:
            return s
        old_full = Image.open(lib[name]).convert("RGB")
        if old_full.size != img.size:
            old_full = old_full.resize(img.size, Image.LANCZOS)
        ctx, scale, region = locate(old_full, img, cp["region"])
        if ctx < args.min_context:
            return s
        folder = gp.checkpoint_dir(rp, recipe)
        ref = Image.open(os.path.join(folder, cp["image"])).convert("RGB").resize(tuple(region[2:]), Image.LANCZOS)
        v = gp.match_score(img, ref, region, cp.get("margin", 8), cp.get("mode", "shape"))
        if v < cp.get("threshold", 0.8) - 0.1:
            return s
        x, y, w, h = region
        img.crop((x, y, x + w, y + h)).save(os.path.join(folder, cp["image"]))
        cp["region"] = region
        changed[name] = region
        done.add(name)
        save_region(name, region)
        print(f"  relearned {name}: context {ctx:.2f} scale {scale:.3f} region {region} (verify {v:.2f})", flush=True)
        return 1.0

    gp.checkpoint_score = score
    variables = gp.parse_vars(args.var)
    first, last = 1, None
    if args.steps:
        a, _, b = args.steps.partition("-")
        first = int(a) if a else 1
        last = int(b) if b else (None if _ else first)
    out = args.out or os.path.join(ROOT, "runs", "discover", "relearn-" + os.path.splitext(os.path.basename(recipe_path))[0])
    os.makedirs(out, exist_ok=True)
    pilot = gp.Pilot(args.process, recipe_path, out, os.path.join(out, "status.json"), None, variables)
    ok = pilot.run(first, last)
    print(f"run {'ok' if ok else 'FAILED'}; relearned {len(changed)}: {sorted(changed)}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
