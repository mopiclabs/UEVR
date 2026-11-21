"""Pass-marker captures (exp/right-eye-cost, vr/PassMarkers.hpp format UEVRPM02 / UEVRPG01): per-pass RHI-thread and
GPU time of both eye families, by phase of an in-run A/B (VR_ExperimentSecondFamilyCvars "N,g1,g2,...").

Families: on the CPU record timeline, the left family ends at the last bind of the left eye's output target, the right
family at the last bind of the right eye's output target (both found as the frame's widest render targets, see
--outputs). Records between a frame's right output and the next left output's frame go to "tail" (UI, UEVR).

  python passmarkers.py <run dir> [--window 10 --groups 2] [--shapes K] [--top 30] [--md out.md]
    --shapes K : print the render-target timeline of capture K (to check the family split) and stop
"""

import argparse
import glob
import os
import re
import struct
import sys
from collections import defaultdict

KIND_MARKER, KIND_PRESENT, KIND_PSO, KIND_OMSET, KIND_CLOSE, KIND_FAMILY = 0, 1, 2, 3, 4, 6
NOQ = 0xFFFFFFFF
FLAT = False  # --flat: frames without eye outputs are one family (L)
CLASSES = ["draw", "indirect", "dispatch", "rt", "barrier", "copy"]


def read_capture(path):
    b = open(path, "rb").read()
    if b[:8] != b"UEVRPM02":
        raise SystemExit(f"{path}: not UEVRPM02")
    freq = struct.unpack_from("<q", b, 8)[0]
    off = 16
    recs = []
    n = len(b)
    while off + 28 <= n:
        q, tid, kind, ctx, size = struct.unpack_from("<qIIQI", b, off)
        payload = b[off + 28: off + 28 + size]
        off += 28 + size
        recs.append((q, tid, kind, ctx, payload))
    return freq, recs


def read_gpu(path):
    if not os.path.exists(path):
        return None
    b = open(path, "rb").read()
    if b[:8] != b"UEVRPG01":
        return None
    freq, cg, cc = struct.unpack_from("<QQQ", b, 8)
    count = struct.unpack_from("<I", b, 32)[0]
    ts = struct.unpack_from(f"<{count}Q", b, 36)
    return {"freq": freq, "calib_gpu": cg, "calib_cpu": cc, "ts": ts}


def words(payload):
    return struct.unpack_from(f"<{len(payload) // 4}I", payload)


class Rec:
    __slots__ = ("q", "tid", "kind", "list", "calls", "ticks", "query", "ltype", "pso", "rt", "ds", "nrt")

    def __init__(self, q, tid, kind, ctx, w):
        self.q, self.tid, self.kind, self.list = q, tid, kind, ctx
        self.calls = w[0:6]
        self.ticks = w[6:12]
        self.query = w[12] if len(w) > 12 else NOQ
        self.ltype = w[13] if len(w) > 13 else 0
        self.pso = None
        self.rt = None
        self.ds = None
        self.nrt = 0
        x = w[14:]
        if kind == KIND_PSO and len(x) >= 2:
            self.pso = x[0] | (x[1] << 32)
        elif kind == KIND_OMSET and len(x) >= 11:
            self.nrt = x[0]
            self.rt = (x[5], x[6], x[7]) if x[5] else None
            self.ds = (x[8], x[9], x[10]) if x[8] else None


def shape_name(rt, ds, nrt):
    s = []
    if rt:
        s.append(f"{nrt}RT {rt[0]}x{rt[1]} f{rt[2]}")
    elif nrt:
        s.append(f"{nrt}RT ?")
    if ds:
        s.append(f"DS {ds[0]}x{ds[1]} f{ds[2]}")
    return " + ".join(s) if s else "none"


def load(path):
    freq, raw = read_capture(path)
    gpu = read_gpu(path[:-4] + ".gpu")
    recs, presents, families = [], [], []
    for q, tid, kind, ctx, payload in raw:
        if kind == KIND_PRESENT:
            presents.append(q)
        elif kind == KIND_FAMILY:
            families.append((q, ctx, struct.unpack_from("<I", payload)[0] if len(payload) >= 4 else 0))
        elif kind in (KIND_PSO, KIND_OMSET, KIND_CLOSE):
            recs.append(Rec(q, tid, kind, ctx, words(payload)))
    recs.sort(key=lambda r: r.q)
    return freq, recs, presents, families, gpu


def find_outputs(recs, presents):
    """(eye, full): the eye output (W/2 x H) and the double-wide target (W x H) of the same format, both bound alone
    at least once per frame (Wukong NSF: each family's tonemap writes a 1920x2160 eye target, then 3840x2160)."""
    count = defaultdict(int)
    for r in recs:
        if r.kind == KIND_OMSET and r.rt and r.nrt == 1:
            count[r.rt] += 1
    frames = max(1, len(presents) - 1)
    shapes = {s for s, c in count.items() if c >= frames * 0.8}
    pairs = [((w // 2, h, f), (w, h, f)) for (w, h, f) in shapes if (w // 2, h, f) in shapes]
    pairs.sort(key=lambda p: p[1][0] * p[1][1], reverse=True)
    return list(pairs[0]) if pairs else []


def assign_families(recs, presents, eye, full):
    """Per record: 'L', 'R' or 'T', and per record the frame index (present segment). In a frame (present to
    present), a family ends at the first single-target bind of another target of the eye output's format after an
    eye-output bind: the first such end closes L, the last closes R; records after R's end are T. A frame with one end
    (right eye skipped) has L and T only.
    At OpenXR resolution scale 1.0 that bind is the double-wide target for both families (its size equals the display
    output's). At other scales only the left family copies into the double-wide (2 x eye width); the right family's
    eye-output bind is followed by the display-size output (3840x2160 on a 4K Mopic panel). Until 2026-11-22 only the
    double-wide bind counted, so at scales other than 1.0 the right eye fell into T."""
    fam = ["T"] * len(recs)
    seg = [-1] * len(recs)
    j = 0
    for k in range(len(presents) - 1):
        a, b = presents[k], presents[k + 1]
        while j < len(recs) and recs[j].q < a:
            j += 1
        idx = []
        while j < len(recs) and recs[j].q < b:
            idx.append(j)
            j += 1
        ends = []
        seen_eye = False
        for i in idx:
            r = recs[i]
            if r.kind != KIND_OMSET or r.nrt != 1 or not r.rt:
                continue
            if r.rt == eye:
                seen_eye = True
            elif seen_eye and r.rt[2] == eye[2]:
                ends.append(i)
                seen_eye = False
        l_end = ends[0] if ends else None
        r_end = ends[-1] if len(ends) >= 2 else None
        for i in idx:
            seg[i] = k
            if l_end is not None and i <= l_end:
                fam[i] = "L"
            elif r_end is not None and i <= r_end:
                fam[i] = "R"
            elif l_end is None:
                fam[i] = "?"
            else:
                fam[i] = "T"
    return fam, seg


def analyze(path, phase_of):
    """Per phase: sums over the capture's frames (each frame's phase from its present)."""
    freq, recs, presents, families, gpu = load(path)
    if len(presents) < 2 or not recs:
        return None
    outs = find_outputs(recs, presents)
    if len(outs) < 2:
        if not FLAT:
            return {"error": f"eye / double-wide outputs not found"}
        # flat reference (no VR session): the whole frame is one family, counted as L
        eye, full = None, None
        _, seg = assign_families(recs, presents, (-1, -1, -1), (-1, -1, -1))
        fam = ["L"] * len(recs)
    else:
        eye, full = outs
        fam, seg = assign_families(recs, presents, eye, full)
    seg_phase = [phase_of(presents[k], freq) for k in range(len(presents) - 1)]
    res = defaultdict(lambda: {"frames": 0, "dur": 0.0, "rhi": defaultdict(float), "gpu": defaultdict(float),
                               "calls": defaultdict(lambda: [0] * 6), "gq": defaultdict(float), "ends": defaultdict(int)})
    for k in range(len(presents) - 1):
        ph = seg_phase[k]
        if ph is None:
            continue
        res[ph]["frames"] += 1
        res[ph]["dur"] += (presents[k + 1] - presents[k]) / freq

    def ph_of(i):
        return seg_phase[seg[i]] if seg[i] >= 0 else None

    by_thread_prev = {}
    state_thread = {}
    for i, r in enumerate(recs):
        p = by_thread_prev.get(r.tid)
        st = state_thread.get(r.tid, ("?", 0))
        if p is not None:
            dt = (r.q - recs[p].q) * 1000.0 / freq
            ph = ph_of(p)
            # after a Close the thread holds no open list: that gap is not recording time
            if dt < 50 and ph is not None and recs[p].kind != KIND_CLOSE:
                key = (fam[p], st[0], st[1])
                res[ph]["rhi"][key] += dt
                c = res[ph]["calls"][key]
                for k in range(6):
                    c[k] += r.calls[k]
        if r.kind == KIND_OMSET:
            st = (shape_name(r.rt, r.ds, r.nrt), st[1])
        elif r.kind == KIND_PSO:
            st = (st[0], r.pso)
        elif r.kind == KIND_CLOSE:
            st = ("?", 0)
        state_thread[r.tid] = st
        by_thread_prev[r.tid] = i
    gpu_coverage = [0, 0]
    if gpu:
        ts = gpu["ts"]
        f = gpu["freq"] or 1
        last = {}
        lstate = {}
        for i, r in enumerate(recs):
            st = lstate.get(r.list, ("?", 0))
            t = ts[r.query] if r.query != NOQ and r.query < len(ts) else 0
            gpu_coverage[0] += 1
            if t:
                gpu_coverage[1] += 1
                pl = last.get(r.list)
                if pl is not None and t >= pl[1]:
                    dt = (t - pl[1]) * 1000.0 / f
                    ph = ph_of(pl[0])
                    if dt < 50 and ph is not None:
                        key = (fam[pl[0]], pl[2][0], pl[2][1])
                        res[ph]["gpu"][key] += dt
                        res[ph]["gq"][(fam[pl[0]], r.ltype)] += dt
            if r.kind == KIND_OMSET:
                st = (shape_name(r.rt, r.ds, r.nrt), st[1])
            elif r.kind == KIND_PSO:
                st = (st[0], r.pso)
            if r.kind == KIND_CLOSE:
                last.pop(r.list, None)
                lstate.pop(r.list, None)
                continue
            lstate[r.list] = st
            if t:
                last[r.list] = (i, t, st)
    return {"phases": res, "outputs": (eye, full), "has_gpu": gpu is not None, "gpu_coverage": gpu_coverage}


def print_shapes(path, limit=400):
    freq, recs, presents, families, gpu = load(path)
    outs = find_outputs(recs, presents)
    print(f"{os.path.basename(path)}: {len(recs)} records, {len(presents)} presents, families {len(families)}, gpu {'yes' if gpu else 'no'}")
    print(f"  outputs (eye, double-wide): {outs}")
    p0 = presents[1] if len(presents) > 2 else presents[0]
    p1 = presents[3] if len(presents) > 4 else presents[-1]
    fam = assign_families(recs, presents, *outs[:2])[0] if len(outs) >= 2 else ["?"] * len(recs)
    last = {}
    n = 0
    for i, r in enumerate(recs):
        if not (p0 <= r.q < p1) or r.kind != KIND_OMSET:
            continue
        s = shape_name(r.rt, r.ds, r.nrt)
        if last.get(r.tid) == s:
            continue
        last[r.tid] = s
        print(f"  {(r.q - p0) * 1000 / freq:8.3f} ms tid {r.tid:6d} {fam[i]} {s}")
        n += 1
        if n >= limit:
            break
    for q, ctx, fc in families:
        if p0 <= q < p1:
            print(f"  family render {(q - p0) * 1000 / freq:8.3f} ms target {ctx:x} frame {fc}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--window", type=int, default=10)
    ap.add_argument("--groups", type=int, default=2)
    ap.add_argument("--names", default="plain,secondary,skip")
    ap.add_argument("--shapes", type=int, default=0)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--md", default="")
    ap.add_argument("--guard", type=float, default=2)
    ap.add_argument("--flat", action="store_true")
    a = ap.parse_args()
    global FLAT
    FLAT = a.flat
    d = a.run if os.path.basename(a.run) == "passmarkers" else os.path.join(a.run, "passmarkers")
    files = sorted(glob.glob(os.path.join(d, "capture-*.bin")), key=lambda p: int(re.findall(r"(\d+)\.bin$", p)[0]))
    if a.shapes:
        print_shapes(os.path.join(d, f"capture-{a.shapes}.bin"))
        return
    names = a.names.split(",")

    def phase_of(q, freq):
        """The frame's phase from its present; None within --guard s of a window start (the switch lands a frame or
        two later on the render and RHI threads)."""
        if a.groups <= 0:
            return 0
        sec = q / freq
        w = int(sec // a.window)
        if sec - w * a.window < a.guard:
            return None
        return int(w % (a.groups + 1))

    agg = {}
    coverage = [0, 0]
    outputs = None
    has_gpu = False
    for p in files:
        r = analyze(p, phase_of)
        if r is None or "error" in r:
            print(f"{os.path.basename(p)}: {r and r.get('error')}")
            continue
        outputs = outputs or r["outputs"]
        has_gpu = has_gpu or r["has_gpu"]
        coverage[0] += r["gpu_coverage"][0]
        coverage[1] += r["gpu_coverage"][1]
        for ph, x in r["phases"].items():
            g = agg.setdefault(ph, {"frames": 0, "dur": 0.0, "captures": 0, "rhi": defaultdict(float), "gpu": defaultdict(float)})
            g["frames"] += x["frames"]
            g["dur"] += x["dur"]
            g["captures"] += 1
            for f in ("rhi", "gpu"):
                for k, v in x[f].items():
                    g[f][k] += v
    out = []

    def emit(s=""):
        print(s)
        out.append(s)

    emit(f"# Pass markers: {a.run}")
    emit(f"outputs (eye, double-wide) {outputs}; GPU timestamps on {coverage[1]} of {coverage[0]} records")
    per_phase = {ph: g for ph, g in agg.items() if g["frames"] > 0}
    for ph in sorted(per_phase):
        g = per_phase[ph]
        emit(f"phase {ph} ({names[ph] if ph < len(names) else ph}): {g['frames']} frames in {g['captures']} captures, "
             f"fps {g['frames'] / g['dur']:.1f}")

    def per_frame(g, field, fam_filter=None):
        frames = g["frames"]
        return {k: v / frames for k, v in g[field].items() if fam_filter is None or k[0] in fam_filter}, frames

    emit("\n## Family totals, ms per frame (RHI = all CPU threads recording lists; GPU = timestamp deltas)")
    emit("| phase | L rhi | R rhi | T rhi | L gpu | R gpu | T gpu |")
    emit("|---|---|---|---|---|---|---|")
    for ph in sorted(per_phase):
        rhi, _ = per_frame(per_phase[ph], "rhi")
        gpu, _ = per_frame(per_phase[ph], "gpu")
        t = lambda m, f: sum(v for k, v in m.items() if k[0] == f)
        emit(f"| {names[ph] if ph < len(names) else ph} | {t(rhi, 'L'):.2f} | {t(rhi, 'R'):.2f} | {t(rhi, 'T'):.2f} | "
             f"{t(gpu, 'L'):.2f} | {t(gpu, 'R'):.2f} | {t(gpu, 'T'):.2f} |")

    # per pass shape and PSO: right family, plain vs secondary; and left vs right in plain
    if 0 in per_phase:
        base_rhi, _ = per_frame(per_phase[0], "rhi")
        base_gpu, _ = per_frame(per_phase[0], "gpu")
        cmp_rhi, cmp_gpu = {}, {}
        if 1 in per_phase:
            cmp_rhi, _ = per_frame(per_phase[1], "rhi")
            cmp_gpu, _ = per_frame(per_phase[1], "gpu")

        def by_shape(m, f):
            acc = defaultdict(float)
            for (fa, shape, pso), v in m.items():
                if fa == f:
                    acc[shape] += v
            return acc

        emit("\n## Right family by render-target shape, ms per frame: plain (PRIMARY) vs secondary; left (plain) for reference")
        emit("| shape | R gpu plain | R gpu secondary | diff | R rhi plain | R rhi secondary | diff | L gpu plain | L rhi plain |")
        emit("|---|---|---|---|---|---|---|---|---|")
        bg, cg, br, cr = by_shape(base_gpu, "R"), by_shape(cmp_gpu, "R"), by_shape(base_rhi, "R"), by_shape(cmp_rhi, "R")
        lg, lr = by_shape(base_gpu, "L"), by_shape(base_rhi, "L")
        shapes = sorted(set(bg) | set(cg) | set(br) | set(cr), key=lambda s: -(bg.get(s, 0) + br.get(s, 0)))
        for s in shapes[: a.top]:
            emit(f"| {s} | {bg.get(s, 0):.3f} | {cg.get(s, 0):.3f} | {cg.get(s, 0) - bg.get(s, 0):+.3f} | {br.get(s, 0):.3f} | "
                 f"{cr.get(s, 0):.3f} | {cr.get(s, 0) - br.get(s, 0):+.3f} | {lg.get(s, 0):.3f} | {lr.get(s, 0):.3f} |")

        def by_pso(m, f):
            acc = defaultdict(float)
            shp = {}
            for (fa, shape, pso), v in m.items():
                if fa == f:
                    acc[pso] += v
                    shp.setdefault(pso, shape)
            return acc, shp

        emit("\n## Right-family PSOs that the SECONDARY render drops or shrinks (plain - secondary, GPU + RHI ms per frame)")
        emit("| pso | pass shape (first seen) | R gpu plain | R gpu sec | R rhi plain | R rhi sec | L gpu plain | L rhi plain |")
        emit("|---|---|---|---|---|---|---|---|")
        bgp, shp = by_pso(base_gpu, "R")
        cgp, _ = by_pso(cmp_gpu, "R")
        brp, shp2 = by_pso(base_rhi, "R")
        crp, _ = by_pso(cmp_rhi, "R")
        lgp, _ = by_pso(base_gpu, "L")
        lrp, _ = by_pso(base_rhi, "L")
        shp.update({k: v for k, v in shp2.items() if k not in shp})
        keys = set(bgp) | set(brp)
        drop = sorted(keys, key=lambda k: -((bgp.get(k, 0) - cgp.get(k, 0)) + (brp.get(k, 0) - crp.get(k, 0))))
        for k in drop[: a.top]:
            emit(f"| {k:x} | {shp.get(k, '?')} | {bgp.get(k, 0):.3f} | {cgp.get(k, 0):.3f} | {brp.get(k, 0):.3f} | {crp.get(k, 0):.3f} | "
                 f"{lgp.get(k, 0):.3f} | {lrp.get(k, 0):.3f} |")
        gone = [k for k in keys if cgp.get(k, 0) == 0 and crp.get(k, 0) == 0 and (bgp.get(k, 0) + brp.get(k, 0)) > 0]
        emit(f"\nPSOs present in the PRIMARY right family and absent in the SECONDARY one: {len(gone)}, "
             f"GPU {sum(bgp.get(k, 0) for k in gone):.3f} ms/f, RHI {sum(brp.get(k, 0) for k in gone):.3f} ms/f")
        ronly = [k for k in keys if lgp.get(k, 0) == 0 and lrp.get(k, 0) == 0]
        emit(f"PSOs in the right family never used by the left: {len(ronly)}, GPU {sum(bgp.get(k, 0) for k in ronly):.3f} ms/f")

    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    sys.exit(main())
