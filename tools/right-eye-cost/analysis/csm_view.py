"""FSceneView layout hints from the [Experiment][CsmView] dumps (VR_ExperimentCsmProbe, both eyes' FSceneView, raw
qwords): 4x4 double matrices that look affine (last column 0,0,0,1, as UE's row-vector view matrices) or projective,
where the left and right eye differ, and blocks that repeat at a second offset inside the same view (FViewMatrices
ViewMatrices and its ShadowViewMatrices copy). Offline only.
  python csm_view.py <run dir> [--pair 2400]"""
import argparse, os, re, struct
import numpy as np

ap = argparse.ArgumentParser(); ap.add_argument("run"); ap.add_argument("--pair", default="")
a = ap.parse_args()
log = open(os.path.join(a.run, "log.txt"), encoding="utf-8", errors="replace").read()
dumps = {}
for n, eye, view, rest in re.findall(r"\[CsmView\] #(\d+) ([LR]) view ([0-9a-f]+) qwords from \+0:(.*)", log):
    if a.pair and n != a.pair:
        continue
    q = [int(x, 16) if x != "?" else None for x in rest.split()]
    dumps.setdefault(n, {})[eye] = q
for n, d in sorted(dumps.items()):
    if "L" not in d or "R" not in d:
        continue
    L, R = d["L"], d["R"]
    size = min(len(L), len(R))
    as_d = lambda q, i: struct.unpack("<d", struct.pack("<Q", q[i]))[0] if q[i] is not None else float("nan")
    diff = [i for i in range(size) if L[i] != R[i]]
    print(f"pair #{n}: {size} qwords, {len(diff)} differ between eyes; differing ranges (byte offsets):")
    runs, start, prev = [], None, None
    for i in diff:
        if start is None: start = prev = i
        elif i == prev + 1: prev = i
        else: runs.append((start, prev)); start = prev = i
    if start is not None: runs.append((start, prev))
    print("   " + ", ".join(f"+{s * 8:x}..+{(e + 1) * 8:x}" for s, e in runs[:60]))
    mats = []
    for i in range(0, size - 16):
        m = np.array([as_d(L, i + k) for k in range(16)])
        if not np.all(np.isfinite(m)) or np.abs(m).max() > 1e7:
            continue
        M = m.reshape(4, 4)
        affine = abs(M[0, 3]) < 1e-9 and abs(M[1, 3]) < 1e-9 and abs(M[2, 3]) < 1e-9 and abs(M[3, 3] - 1) < 1e-9
        rot = M[:3, :3]
        ortho = affine and np.allclose(rot @ rot.T, np.eye(3), atol=1e-3)
        if ortho:
            same = all(L[i + k] == R[i + k] for k in range(16))
            mats.append((i * 8, "view-like (orthonormal)", same, M[3, :3]))
    print(f"   orthonormal affine 4x4 doubles in the left view: {len(mats)}")
    for off, kind, same, t in mats[:40]:
        print(f"     +{off:x}: {kind}, eyes {'equal' if same else 'differ'}, translation row {np.round(t, 2)}")
    # repeated 128-byte blocks inside the left view (ViewMatrices vs ShadowViewMatrices)
    seen = {}
    for i in range(0, size - 16):
        blk = tuple(L[i:i + 16])
        if None in blk or all(x == 0 for x in blk):
            continue
        seen.setdefault(blk, []).append(i * 8)
    reps = sorted({tuple(v) for v in seen.values() if len(v) > 1}, key=lambda v: v[0])
    print(f"   128-byte blocks found at two or more offsets: {len(reps)}; first: " + ", ".join(
        "/".join(f"+{o:x}" for o in v) for v in reps[:20]))
