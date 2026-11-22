"""Where the GPU's direct queue waits, from pass-marker captures (UEVRPM02 + UEVRPG01 GPU timestamps).

Per command list, consecutive timestamped records bound a span of that list's GPU work. The union of the direct
lists' spans is the busy part of the timeline that markers cover; the rest is either idle or uncovered work (list
starts before their first marker, ~20 % of the work in 11.2). Gaps are classified by the families of the spans on
either side (L / R / T, passmarkers.assign_families; a gap from one frame's last span to the next frame's first is
"frame boundary"), and by size. A true wait shows up as a few long gaps at one position; uncovered work as many short
ones everywhere. For every gap, whether any thread was recording commands meanwhile (CPU records inside the gap):
none = the GPU waited for work that wasn't being recorded (game / render thread, xrWaitFrame, present).

  python gpu_gaps.py <run dir> [--window 10 --phases 2] [--min-ms 0.3] [--first-window W]"""
import argparse
import bisect
import glob
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import passmarkers as pm  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--window", type=float, default=10)
    ap.add_argument("--phases", type=int, default=1)
    ap.add_argument("--min-ms", type=float, default=0.3)
    ap.add_argument("--first-window", type=int, default=0)
    ap.add_argument("--queues", default="0", help="list types whose union is the timeline (0 direct, 2 compute, 3 copy)")
    a = ap.parse_args()
    queues = {int(x) for x in a.queues.split(",")}
    ltypes = defaultdict(int)
    other_in = defaultdict(lambda: defaultdict(float))
    detail = defaultdict(lambda: defaultdict(float))
    rec_before = defaultdict(lambda: defaultdict(float))
    rec_after = defaultdict(lambda: defaultdict(float))
    rec_after_lag = defaultdict(lambda: defaultdict(float))
    tot = defaultdict(lambda: defaultdict(float))     # phase -> position -> ms (gaps >= min)
    cnt = defaultdict(lambda: defaultdict(int))
    idle_norec = defaultdict(lambda: defaultdict(float))  # same, gaps with no CPU recording inside
    small = defaultdict(float)
    frames = defaultdict(int)
    frame_ms = defaultdict(float)
    busy = defaultdict(float)
    for path in sorted(glob.glob(os.path.join(a.run, "passmarkers", "capture-*.bin"))):
        freq, recs, presents, families, gpu = pm.load(path)
        if not gpu or len(presents) < 3:
            continue
        outs = pm.find_outputs(recs, presents)
        if len(outs) < 2:
            fam, seg = ["L"] * len(recs), pm.assign_families(recs, presents, (-1, -1, -1), (-1, -1, -1))[1]
        else:
            fam, seg = pm.assign_families(recs, presents, *outs)
        ts, gf = gpu["ts"], gpu["freq"] or 1
        to_cpu = lambda t: gpu["calib_cpu"] + (t - gpu["calib_gpu"]) * freq / gf  # GPU tick -> CPU QPC tick
        spans = []  # (cpu_start, cpu_end, family, frame) on the queues chosen by --queues
        other = []  # spans of the other queue types (async compute, copy)
        last = {}
        lstate = {}
        for i, r in enumerate(recs):
            t = ts[r.query] if r.query != pm.NOQ and r.query < len(ts) else 0
            ltypes[r.ltype] += 1
            if t:
                pl = last.get(r.list)
                if pl is not None and t >= pl[1] and (t - pl[1]) * 1000.0 / gf < 50:
                    sp = (to_cpu(pl[1]), to_cpu(t), fam[pl[0]], seg[pl[0]], pl[2], r.list, pl[0])
                    (spans if r.ltype in queues else other).append(sp)
            st = lstate.get(r.list, "?")
            if r.kind == pm.KIND_OMSET:
                st = pm.shape_name(r.rt, r.ds, r.nrt)
            if r.kind == pm.KIND_CLOSE:
                last.pop(r.list, None)
                lstate.pop(r.list, None)
                continue
            lstate[r.list] = st
            if t:
                last[r.list] = (i, t, st)
        other.sort()
        other_q = [o[0] for o in other]
        if not spans:
            continue
        spans.sort()
        rec_q = [r.q for r in recs]
        # union, remembering the family/frame at each side of every gap
        merged = []
        for s in spans:
            if merged and s[0] <= merged[-1][1]:
                if s[1] > merged[-1][1]:
                    merged[-1] = (merged[-1][0], s[1], merged[-1][2], merged[-1][3], s[2], s[3], merged[-1][6], (s[4], s[5]), merged[-1][8])
            else:
                # start, end, fam_first, frame_first, fam_last, frame_last, (shape, list) first, (shape, list) last
                merged.append((s[0], s[1], s[2], s[3], s[2], s[3], (s[4], s[5]), (s[4], s[5]), s[6]))  # [8]: first record
        for k in range(1, len(presents) - 2):  # whole frames only (the first and last are partial)
            a0, a1 = presents[k], presents[k + 1]
            sec = a0 / freq
            w = int(sec // a.window)
            if w < a.first_window:
                continue
            ph = w % a.phases if a.phases > 1 else 0
            if a.phases > 1 and sec - w * a.window < 2:
                continue
            frames[ph] += 1
            frame_ms[ph] += (a1 - a0) * 1000.0 / freq
            # busy and gaps inside [a0, a1) on the GPU timeline mapped to CPU time
            prev_end, prev = None, None
            for m in merged:
                s0, s1 = max(m[0], a0), min(m[1], a1)
                if s1 <= s0:
                    if m[0] >= a1:
                        break
                    prev_end, prev = m[1], m
                    continue
                busy[ph] += (s1 - s0) * 1000.0 / freq
                if prev_end is not None:
                    g0 = max(prev_end, a0)
                    if s0 > g0:
                        gms = (s0 - g0) * 1000.0 / freq
                        if gms >= a.min_ms:
                            pos = "frame boundary" if prev[5] != m[3] else "%s->%s" % (prev[4], m[2])
                            tot[ph][pos] += gms
                            cnt[ph][pos] += 1
                            same = "same list" if prev[7][1] == m[6][1] else "next list"
                            detail[ph][(pos, prev[7][0], m[6][0], same)] += gms
                            # the work after the gap: recorded on the CPU before the GPU ran dry (then the GPU waited on
                            # its submission or a fence) or only after (the GPU waited on the recording thread)
                            rq = recs[m[8]].q
                            if rq <= g0:
                                rec_before[ph][pos] += gms
                            else:
                                rec_after[ph][pos] += gms
                                rec_after_lag[ph][pos] += (s0 - rq) * 1000.0 / freq
                            lo = bisect.bisect_left(rec_q, g0)
                            hi = bisect.bisect_left(rec_q, s0)
                            if hi - lo == 0:
                                idle_norec[ph][pos] += gms
                            # other queues' work inside the gap (clipped to it)
                            j = max(0, bisect.bisect_left(other_q, g0) - 64)
                            cov = 0.0
                            cur = g0
                            while j < len(other) and other[j][0] < s0:
                                o0, o1 = max(other[j][0], cur), min(other[j][1], s0)
                                if o1 > o0:
                                    cov += o1 - o0
                                    cur = o1
                                j += 1
                            other_in[ph][pos] += cov * 1000.0 / freq
                        else:
                            small[ph] += gms
                prev_end, prev = m[1], m
    print("records per list type:", dict(ltypes), "; timeline = list types", sorted(queues))
    for ph in sorted(frames):
        n = frames[ph]
        print("phase %d: %d frames, frame %.2f ms, covered busy %.2f ms, gaps < %.1f ms %.2f ms/f" % (
            ph, n, frame_ms[ph] / n, busy[ph] / n, a.min_ms, small[ph] / n))
        for pos, v in sorted(tot[ph].items(), key=lambda kv: -kv[1]):
            print("   %-16s %.3f ms/f in %.2f gaps/f (mean %.2f ms); other queues busy inside %.3f ms/f; no command recording inside: %.3f ms/f" % (
                pos, v / n, cnt[ph][pos] / n, v / max(1, cnt[ph][pos]), other_in[ph][pos] / n, idle_norec[ph][pos] / n))
            print("   %-16s   next work recorded before the gap began: %.3f ms/f; recorded during the gap: %.3f ms/f (GPU started it %.2f ms after its record, mean over those gaps' ms)" % (
                "", rec_before[ph][pos] / n, rec_after[ph][pos] / n, rec_after_lag[ph][pos] / max(1, cnt[ph][pos])))
        print("   largest gaps by the target before -> after:")
        for (pos, sb, sa, same), v in sorted(detail[ph].items(), key=lambda kv: -kv[1])[:8]:
            print("     %-14s %-24s -> %-24s %-9s %.3f ms/f" % (pos, sb, sa, same, v / n))


if __name__ == "__main__":
    main()
