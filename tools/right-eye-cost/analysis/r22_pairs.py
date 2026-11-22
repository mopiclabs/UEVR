"""R22: paired A/B over the windows where the skip really ran. A skip window counts when its [HookProfiler] line shows
skipped presents (or skipped UI copies) for more than half of its presents; it is paired with the mean of its keep
neighbours (w-1, w+1). Prints, for fps, GPU idle / work, the game's Copy engine, thread ms per frame (r20_rungs.py
--json blocks) and the hook parts (the [HookProfiler] window lines), skip - keep with a 95 % interval.

  python r22_pairs.py --run <run dir> --json <r20_rungs.json> [--arm present|ui]"""
import argparse
import json
import math
import os
import re
import statistics
from collections import defaultdict

T95 = {1: 12.71, 2: 4.30, 3: 3.18, 4: 2.78, 5: 2.57, 6: 2.45, 7: 2.36, 8: 2.31, 9: 2.26, 10: 2.23, 11: 2.20, 12: 2.18,
       13: 2.16, 14: 2.14, 15: 2.13, 16: 2.12, 17: 2.11, 18: 2.10, 19: 2.09, 20: 2.09}
LINE = re.compile(r"\[HookProfiler\] window (\d+) \((skip|keep)[^)]*\) \(per frame, (\d+) presents\)(.*)skipped presents (\d+), skipped UI copies (\d+)")
FIELD = re.compile(r"(\w+) avg=([\d.]+)ms")


def ci(d):
    if len(d) < 2:
        return float("nan"), float("nan")
    m = statistics.mean(d)
    return m, T95.get(len(d) - 1, 2.0) * statistics.stdev(d) / math.sqrt(len(d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--json", required=True)
    ap.add_argument("--arm", default="present")
    a = ap.parse_args()
    blocks = {int(w): v for w, v in json.load(open(a.json))["blocks"].items()}
    hp = {}
    for line in open(os.path.join(a.run, "log.txt"), encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if not m:
            continue
        w, presents = int(m.group(1)), int(m.group(3))
        skipped = int(m.group(5)) if a.arm == "present" else int(m.group(6))
        hp[w] = {"phase": m.group(2), "presents": presents, "skipped": skipped,
                 "parts": {k: float(v) for k, v in FIELD.findall(m.group(4))}}
    skips = sorted(w for w, h in hp.items() if h["phase"] == "skip" and h["skipped"] > 0.5 * max(1, h["presents"]))
    print("%s: %d windows with the %s skip running: %s" % (os.path.basename(a.run), len(skips), a.arm, skips[:3] + ["..."] if len(skips) > 3 else skips))
    print("skip windows for gpu_gaps --skip-windows:", ",".join(str(w) for w in skips))
    keys = ["fps", "idle_ms", "game_work_ms", "nv_work_ms", "nv_busy", "copy_ms", "3d_service", "RHIThread_ms_f", "RenderThread_ms_f",
            "GameThread_ms_f", "RHIThread_busy", "wait_ms"]
    print("| measure | skip | keep (neighbours) | skip - keep [95 %] | n |")
    print("|---|---|---|---|---|")
    for k in keys:
        d, sv, kv = [], [], []
        for w in skips:
            nb = [blocks[x][k] for x in (w - 1, w + 1) if x in blocks and blocks[x].get(k) is not None and hp.get(x, {}).get("phase") == "keep"]
            if w in blocks and blocks[w].get(k) is not None and nb:
                d.append(blocks[w][k] - statistics.mean(nb))
                sv.append(blocks[w][k])
                kv.append(statistics.mean(nb))
        if len(d) >= 2:
            m, h = ci(d)
            print("| %s | %.3f | %.3f | %+.3f [%+.3f, %+.3f] | %d |" % (k, statistics.mean(sv), statistics.mean(kv), m, m - h, m + h, len(d)))
    parts = sorted({p for h in hp.values() for p in h["parts"]})
    print("| hook part (ms per frame) | skip | keep | skip - keep [95 %] | n |")
    print("|---|---|---|---|---|")
    for p in parts:
        d, sv, kv = [], [], []
        for w in skips:
            nb = [hp[x]["parts"].get(p) for x in (w - 1, w + 1) if x in hp and hp[x]["phase"] == "keep" and p in hp[x]["parts"]]
            if p in hp[w]["parts"] and nb:
                d.append(hp[w]["parts"][p] - statistics.mean(nb))
                sv.append(hp[w]["parts"][p])
                kv.append(statistics.mean(nb))
        if len(d) >= 2 and (statistics.mean(sv) >= 0.01 or statistics.mean(kv) >= 0.01):
            m, h = ci(d)
            print("| %s | %.3f | %.3f | %+.3f [%+.3f, %+.3f] | %d |" % (p, statistics.mean(sv), statistics.mean(kv), m, m - h, m + h, len(d)))


if __name__ == "__main__":
    main()
