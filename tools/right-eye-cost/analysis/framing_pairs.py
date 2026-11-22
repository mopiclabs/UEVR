"""R20 framing check: the flat game window (flatshots.py grabs of runs A / B(fg) / B) next to the VR left eye (binocular
frames of a Wukong-still VR run with the eye sampler, side by side, each eye squeezed 2:1) at the same time into the
recipe's measured segment (Wukong-still's idle scene evolves the same way every run: wolves approach). Writes one
PNG per offset: flat on top, VR left eye below, both at the same width. Local only (game pictures).

  python framing_pairs.py --flat "<run dir>|<shots dir>" [--flat ...] --vr <run dir> --out <png prefix>
      [--offsets 30,90,150] [--width 960]"""
import argparse
import glob
import json
import os

import numpy as np
from PIL import Image, ImageDraw


def seg_start(run):
    st = json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))
    segs = [s for s in st.get("segments", []) if s.get("start_qpc_ns") and s.get("end_qpc_ns")]
    s = max(segs, key=lambda s: s["end_qpc_ns"] - s["start_qpc_ns"])
    return s["start_qpc_ns"], s["end_qpc_ns"]


def frames(folder):
    out = []
    for p in glob.glob(os.path.join(folder, "*.png")):
        b = os.path.basename(p).split("-")[0].split(".")[0]
        if b.isdigit():
            out.append((int(b), p))
    return sorted(out)


def nearest(fr, q):
    return min(fr, key=lambda x: abs(x[0] - q)) if fr else None


def label(img, text):
    lab = Image.new("RGB", (img.width, 22), (255, 255, 255))
    ImageDraw.Draw(lab).text((6, 5), text, fill=(0, 0, 0))
    out = Image.new("RGB", (img.width, img.height + 22), (255, 255, 255))
    out.paste(lab, (0, 0))
    out.paste(img, (0, 22))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flat", action="append", required=True)
    ap.add_argument("--vr", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--offsets", default="30,90,150")
    ap.add_argument("--width", type=int, default=960)
    a = ap.parse_args()
    vs, _ = seg_start(a.vr)
    vf = frames(os.path.join(a.vr, "binocular"))
    for off in [float(x) for x in a.offsets.split(",")]:
        panels = []
        for spec in a.flat:
            run, _, shots = spec.partition("|")
            fs, _ = seg_start(run)
            f = nearest(frames(shots), fs + int(off * 1e9))
            if not f:
                continue
            im = Image.open(f[1]).convert("RGB")
            im = im.resize((a.width, int(im.height * a.width / im.width)))
            panels.append(label(im, "FLAT %s, %.0f s into the segment (%.1f s off)" % (
                os.path.basename(run).split("-Wukong-")[-1], (f[0] - fs) / 1e9, (f[0] - fs) / 1e9 - off)))
        v = nearest(vf, vs + int(off * 1e9))
        if v:
            im = np.asarray(Image.open(v[1]).convert("RGB"))
            left = im[:, : im.shape[1] // 2]
            left = np.repeat(left, 2, axis=1)  # side-by-side eyes are squeezed 2:1
            li = Image.fromarray(left)
            li = li.resize((a.width, int(li.height * a.width / li.width)))
            panels.append(label(li, "VR left eye (%s), %.0f s into the segment" % (os.path.basename(a.vr).split("-Wukong-")[-1], (v[0] - vs) / 1e9)))
        if not panels:
            continue
        h = sum(p.height for p in panels)
        out = Image.new("RGB", (a.width, h), (255, 255, 255))
        y = 0
        for p in panels:
            out.paste(p, (0, y))
            y += p.height
        path = "%s-%03ds.png" % (a.out, off)
        out.save(path)
        print(path, out.size)


if __name__ == "__main__":
    main()
