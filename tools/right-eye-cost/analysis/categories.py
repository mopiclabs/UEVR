"""Right-eye GPU time by category (shader entry names + render-target shapes), left vs right vs SECONDARY, for the
"per pass: left ms, right ms, depends on the eye position?" table.
  python categories.py <run dir> [--md out.md]"""
import argparse, csv, glob, os, re, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm

ap = argparse.ArgumentParser(); ap.add_argument("run"); ap.add_argument("--md", default="")
ap.add_argument("--window", type=int, default=10); ap.add_argument("--groups", type=int, default=2); ap.add_argument("--guard", type=float, default=2)
ap.add_argument("--flat", action="store_true")
a = ap.parse_args()
pm.FLAT = a.flat
d = os.path.join(a.run, "passmarkers")

fn = {}
with open(os.path.join(d, "names.tsv"), encoding="utf-8") as f:
    for r in csv.DictReader(f, delimiter="\t"):
        fn[r["hash"]] = r["functions"]
stages = defaultdict(dict)
with open(os.path.join(d, "psos.tsv"), encoding="utf-8") as f:
    for line in f:
        p = line.rstrip("\n").split("\t")
        if len(p) >= 3:
            stages[int(p[0], 16)][p[1]] = fn.get(p[2], "")


def name_of(pso):
    st = stages.get(pso)
    if not st:
        return None
    return " ".join(st.get(k, "") for k in ("C", "V", "P", "M"))


# Internal render size (INT = the most common depth target "DS WxH f20") and the output size (OUT = the widest 1-RT
# target that is not an atlas), found per run so the shape rules fit VR (1116x1256 -> 1920x2160) and flat
# (1485x928 -> 2560x1600) alike.
INT = OUT = ""


def size_of(shape, prefix):
    m = re.match(prefix + r"(\d+)x(\d+)", shape)
    return (int(m.group(1)), int(m.group(2))) if m else None


def smaller_than_int(shape):
    sz = size_of(shape, r"[12]RT ")
    iw = int(INT.split("x")[0]) if INT else 0
    return sz is not None and iw and 16 <= sz[0] < iw


# (category, eye-dependent?, rule on (name, shape))
RULES = [
    ("GDF refresh (heightfield/page compose)", "no: world-space clipmaps around the camera", lambda n, s: n and re.search(r"Heightfield|ComposeHeightfields|GlobalDistanceField|CopyDistanceFieldAtlas", n)),
    ("CSM shadow depth (incl. Nanite shadow raster)", "barely: cascades from the camera, eyes 6 cm apart", lambda n, s: "4096x1024" in s or (n and re.search(r"EmitShadowMap|ShadowObjectCull|CullObjectsForShadow", n))),
    ("Nanite raster / cull (no target)", "yes for the main view; shadow views no", lambda n, s: n and re.search(r"HWRasterize|MicropolyRasterize|PersistentClusterCull|InstanceCull|NodeAndClusterCull|RasterBin", n)),
    ("Distance-field shadowing (screen)", "yes (per pixel)", lambda n, s: n and re.search(r"DistanceFieldShadowing", n)),
    ("Lumen screen probes / reflections", "yes (screen-space probes)", lambda n, s: n and re.search(r"ScreenProbe|Reflection(Trace|Resolve|Temporal|Clear|Generate)|ScreenSpaceBentNormal|LumenReflection", n)),
    ("Lumen scene lighting / radiance cache", "no: world-space (surface cache, probes)", lambda n, s: n and re.search(r"LumenScene|LumenCard|Radiosity|RadianceCache|MarkRadianceProbes", n)),
    ("Niagara GPU simulation", "no: world-space", lambda n, s: n and re.search(r"Simulate|Niagara", n)),
    ("HZB", "yes (per view depth)", lambda n, s: n and re.search(r"HZB", n)),
    ("Deferred lights / sky lighting", "yes (per pixel)", lambda n, s: n and re.search(r"DeferredLight|ReflectionEnvironment|SkyLighting", n)),
    ("Base pass (GBuffer)", "yes", lambda n, s: bool(INT) and re.match(r"[3-8]RT " + INT, s) is not None),
    ("Depth prepass / masked depth", "yes", lambda n, s: bool(INT) and s.startswith("DS " + INT)),
    ("Full-res colour passes (lighting, translucency, post)", "yes", lambda n, s: bool(INT) and re.match(r"[12]RT " + INT, s) is not None),
    ("Output (tonemap/upscale to the eye or window size)", "yes", lambda n, s: bool(OUT) and OUT in s),
    ("Half/quarter-res post (bloom, light shafts...)", "yes", lambda n, s: smaller_than_int(s) and (size_of(s, r"[12]RT ") or (0, 0))[0] > 64),
    ("Small targets (LUTs, atlases, sky)", "mostly no (LUTs) / mixed", lambda n, s: re.match(r"\dRT (\d{1,3})x(\d{1,3}) ", s) is not None or "4096x4096" in s or "512x512" in s or "264x264" in s or "256x" in s),
    ("Compute, unnamed (PSO made before injection)", "unknown", lambda n, s: (not n) and (s == "none" or s == "?")),
    ("Compute, named other", "see names", lambda n, s: bool(n) and (s == "none" or s == "?")),
]


def categorize(name, shape):
    for cat, dep, rule in RULES:
        try:
            if rule(name or "", shape):
                return cat
        except Exception:
            pass
    return "other"


def phase_of(q, freq):
    if a.groups <= 0:
        return 0
    sec = q / freq; w = int(sec // a.window)
    return None if sec - w * a.window < a.guard else int(w % (a.groups + 1))


raw = defaultdict(float); frames = defaultdict(int); shape_w = defaultdict(float)
for p in glob.glob(os.path.join(d, "capture-*.bin")):
    r = pm.analyze(p, phase_of)
    if not r or "error" in r:
        continue
    for ph, x in r["phases"].items():
        frames[ph] += x["frames"]
        for (fam, sh, pso), v in x["gpu"].items():
            raw[(ph, fam, sh, pso)] += v
            shape_w[sh] += v
# internal size: the largest-area scene depth target (DS f20) among those with at least 10 % of the busiest one's time
ds = [(v, sh) for sh, v in shape_w.items() if re.match(r"DS \d+x\d+ f20$", sh)]
if ds:
    top = max(v for v, _ in ds)
    area = lambda sh: (lambda m: int(m.group(1)) * int(m.group(2)))(re.match(r"DS (\d+)x(\d+)", sh))
    INT = re.match(r"DS (\d+x\d+)", max((sh for v, sh in ds if v >= 0.1 * top), key=area)).group(1)
else:
    INT = ""
# output: the 8-bit BGRA (f87) single target with the most GPU time (the tonemap/upscale output)
outs = [(v, m.group(1) + "x" + m.group(2)) for sh, v in shape_w.items() for m in [re.match(r"1RT (\d+)x(\d+) f87$", sh)] if m]
OUT = max(outs)[1] if outs else ""
print(f"internal size {INT}, output size {OUT}")
agg = defaultdict(lambda: defaultdict(float))
for (ph, fam, sh, pso), v in raw.items():
    agg[(ph, fam)][categorize(name_of(pso), sh)] += v
dep = {c: dd for c, dd, _ in RULES}; dep["other"] = "?"
cats = sorted({c for k in agg for c in agg[k]}, key=lambda c: -agg[(0, "R")].get(c, 0))
out = []
def emit(s):
    print(s); out.append(s)
g = lambda ph, fam, c: agg[(ph, fam)].get(c, 0) / frames[ph] if frames[ph] else 0
emit(f"frames plain {frames[0]}, secondary {frames[1]}, skip {frames[2]}; GPU ms per frame")
emit("| category | left | right | right/left | right SECONDARY | left (skip phase) | depends on eye position? |")
emit("|---|---|---|---|---|---|---|")
for c in cats:
    l, r_, s = g(0, "L", c), g(0, "R", c), g(1, "R", c)
    emit(f"| {c} | {l:.2f} | {r_:.2f} | {r_ / l if l else float('inf'):.2f} | {s:.2f} | {g(2, 'L', c):.2f} | {dep.get(c, '?')} |")
emit(f"| total | {sum(g(0, 'L', c) for c in cats):.2f} | {sum(g(0, 'R', c) for c in cats):.2f} | | {sum(g(1, 'R', c) for c in cats):.2f} | {sum(g(2, 'L', c) for c in cats):.2f} | |")
if a.md:
    open(a.md, "w", encoding="utf-8").write("\n".join(out) + "\n")
