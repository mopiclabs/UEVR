"""Move one checkpoint to where its screen shows it now, from a screenshot of that screen (relearn.py's locate):
  .venv\\Scripts\\python analysis\\relocate_one.py recipes\\<Recipe>.json <checkpoint> <screenshot.png> --before 20261007-080000 [--write] [--min-context 0.6]
Prints the context score, scale, new region and the check of the old crop there; --write saves the crop and the
region (formatting of the recipe kept). For a screen confirmed by eye, where relearn's automatic threshold is too
strict (animated menu backgrounds)."""
import argparse, json, os, re, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import gamepilot as gp  # noqa: E402
import relearn  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("recipe")
ap.add_argument("name")
ap.add_argument("shot")
ap.add_argument("--before", required=True)
ap.add_argument("--game", default=None)
ap.add_argument("--write", action="store_true")
ap.add_argument("--min-context", type=float, default=0.6)
ap.add_argument("--pad", type=int, default=60)
a = ap.parse_args()

rp = os.path.abspath(a.recipe)
recipe = json.load(open(rp, encoding="utf-8"))
cp = recipe["checkpoints"][a.name]
game = a.game or os.path.basename(rp).split("-")[0].split(".")[0]
lib = relearn.old_screens(a.before, game)
if a.name not in lib:
    raise SystemExit(f"no old screenshot of {a.name}")
live = Image.open(a.shot).convert("RGB")
if live.width != recipe.get("max_width", 1280):
    live = live.resize((recipe.get("max_width", 1280), int(live.height * recipe.get("max_width", 1280) / live.width)), Image.LANCZOS)
old = Image.open(lib[a.name]).convert("RGB").resize(live.size, Image.LANCZOS)
folder = gp.checkpoint_dir(rp, recipe)
now = gp.match_score(live, Image.open(os.path.join(folder, cp["image"])).convert("RGB"), cp["region"], cp.get("margin", 8), cp.get("mode", "shape"))
ctx, scale, region = relearn.locate(old, live, cp["region"], pad=a.pad)
ref = Image.open(os.path.join(folder, cp["image"])).convert("RGB").resize(tuple(region[2:]), Image.LANCZOS)
v = gp.match_score(live, ref, region, cp.get("margin", 8), cp.get("mode", "shape"))
print(json.dumps({"name": a.name, "as_recorded": round(now, 2), "context": round(ctx, 2), "scale": round(scale, 3),
                  "region": region, "old_region": cp["region"], "verify": round(v, 2)}))
if a.write:
    if ctx < a.min_context or v < cp.get("threshold", 0.8) - 0.1:
        raise SystemExit("not written: context or verify too low")
    x, y, w, h = region
    live.crop((x, y, x + w, y + h)).save(os.path.join(folder, cp["image"]))
    with open(rp, encoding="utf-8") as f:
        text = f.read()
    pat = re.compile(r'("' + re.escape(a.name) + r'": \{[^\n]*?"region": )\[[^\]]*\]')
    if len(pat.findall(text)) != 1:
        raise SystemExit("region not found in the recipe file")
    with open(rp, "w", encoding="utf-8", newline="") as f:
        f.write(pat.sub(lambda m: m.group(1) + json.dumps(region), text))
    print("written")
