"""Find a checkpoint's own crop in a screenshot at a few scales (for screens whose surroundings changed too much
for relocate_one.py's context search), and optionally write the new region and crop:
  .venv\\Scripts\\python analysis\\find_crop.py recipes\\<Recipe>.json <checkpoint> <screenshot.png> [--write] [--min 0.85]
"""
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
ap.add_argument("--write", action="store_true")
ap.add_argument("--min", type=float, default=0.85)
ap.add_argument("--scales", default="1.0,1.1111,0.9")
a = ap.parse_args()

rp = os.path.abspath(a.recipe)
recipe = json.load(open(rp, encoding="utf-8"))
cp = recipe["checkpoints"][a.name]
folder = gp.checkpoint_dir(rp, recipe)
live = Image.open(a.shot).convert("RGB")
mw = recipe.get("max_width", 1280)
if live.width != mw:
    live = live.resize((mw, int(live.height * mw / live.width)), Image.LANCZOS)
ref = Image.open(os.path.join(folder, cp["image"])).convert("RGB")
hay = relearn.gray(live, 1.0)
best = None
for s in (float(x) for x in a.scales.split(",")):
    t = ref.resize((max(2, round(ref.width * s)), max(2, round(ref.height * s))), Image.LANCZOS)
    sc, x, y = relearn.ncc_search(hay, relearn.gray(t, 1.0))
    print(f"scale {s:.3f}: ncc {sc:.3f} at {x},{y} size {t.size}")
    if best is None or sc > best[0]:
        best = (sc, s, [x, y, t.width, t.height])
sc, s, region = best
crop = live.crop((region[0], region[1], region[0] + region[2], region[1] + region[3]))
v = gp.match_score(live, ref.resize(tuple(region[2:]), Image.LANCZOS), region, cp.get("margin", 8), cp.get("mode", "shape"))
print(json.dumps({"name": a.name, "ncc": round(sc, 3), "scale": round(s, 3), "region": region, "old_region": cp["region"], "verify": round(v, 2)}))
if a.write:
    if sc < a.min:
        raise SystemExit("not written: ncc below --min")
    crop.save(os.path.join(folder, cp["image"]))
    with open(rp, encoding="utf-8") as f:
        text = f.read()
    pat = re.compile(r'("' + re.escape(a.name) + r'": \{[^\n]*?"region": )\[[^\]]*\]')
    if len(pat.findall(text)) != 1:
        raise SystemExit("region not found in the recipe file")
    with open(rp, "w", encoding="utf-8", newline="") as f:
        f.write(pat.sub(lambda m: m.group(1) + json.dumps(region), text))
    print("written")
