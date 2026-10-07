"""Move every checkpoint of a recipe family by one uniform scale about the screen center (a game whose UI grew from
a pillarboxed 16:10 picture to 16:9 fullscreen on the Mopic display: Tekken x10/9 about (640, 360)), resizing each
image to its new region. Checkpoints listed in --done (already re-recorded) are taken from --from-recipe instead.
  .venv\\Scripts\\python analysis\\rescale_recipes.py "recipes\\Tekken8Demo*.json" --scale 1.1111111 --done notice,title,main_menu --from-recipe recipes\\Tekken8Demo.json [--write]
"""
import argparse, glob, json, os, re, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import gamepilot as gp  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("pattern")
ap.add_argument("--scale", type=float, required=True)
ap.add_argument("--cx", type=float, default=640)
ap.add_argument("--cy", type=float, default=360)
ap.add_argument("--done", default="")
ap.add_argument("--from-recipe", default=None)
ap.add_argument("--write", action="store_true")
a = ap.parse_args()

done = set(x for x in a.done.split(",") if x)
src = json.load(open(a.from_recipe, encoding="utf-8"))["checkpoints"] if a.from_recipe else {}
k = a.scale


def move(r):
    x, y, w, h = r
    return [int(round((x - a.cx) * k + a.cx)), int(round((y - a.cy) * k + a.cy)), max(2, int(round(w * k))), max(2, int(round(h * k)))]


images = {}   # image path -> new size (must agree across recipes)
plans = []
for p in sorted(glob.glob(a.pattern)):
    recipe = json.load(open(p, encoding="utf-8"))
    folder = gp.checkpoint_dir(os.path.abspath(p), recipe)
    for name, cp in recipe["checkpoints"].items():
        new = src[name]["region"] if name in done else move(cp["region"])
        plans.append((p, name, cp["region"], new))
        path = os.path.join(folder, cp["image"])
        size = tuple(new[2:])
        if name not in done:
            if path in images and images[path] != size:
                raise SystemExit(f"{cp['image']} would get two sizes: {images[path]} and {size}")
            images[path] = size
for p, name, old, new in plans:
    print(f"{os.path.basename(p):32} {name:28} {old} -> {new}")
print(f"{len(images)} images to resize")
if not a.write:
    raise SystemExit(0)
for path, size in images.items():
    im = Image.open(path)
    im.convert("RGB").resize(size, Image.LANCZOS).save(path)
by_file = {}
for p, name, old, new in plans:
    by_file.setdefault(p, []).append((name, new))
for p, items in by_file.items():
    text = open(p, encoding="utf-8").read()
    for name, new in items:
        pat = re.compile(r'("' + re.escape(name) + r'": \{[^\n]*?"region": )\[[^\]]*\]')
        if len(pat.findall(text)) != 1:
            raise SystemExit(f"{p}: region of {name} not found once")
        text = pat.sub(lambda m: m.group(1) + json.dumps(new), text)
    open(p, "w", encoding="utf-8", newline="").write(text)
print("written")
