"""Left-eye similarity of two side-by-side binocular frames: Pearson correlation of 64x36 grey thumbnails and mean luma.
Used to confirm a Wukong-walk run loaded the right save (forest shrine) from its first in-play frame.
  python frame_similar.py <reference.png> <frame.png>"""
import sys
import numpy as np
from PIL import Image


def thumb(p):
    im = Image.open(p).convert("L")
    w, h = im.size
    return np.asarray(im.crop((0, 0, w // 2, h)).resize((64, 36)), np.float32)


a, b = thumb(sys.argv[1]), thumb(sys.argv[2])
r = float(np.corrcoef(a.ravel(), b.ravel())[0, 1])
print(f"corr {r:.3f}; mean luma ref {a.mean():.1f}, frame {b.mean():.1f}")
