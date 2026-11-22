"""R20: fixed-per-view and per-megapixel GPU cost from two eye sizes (vr10 1116x1256, vr14 1580x1776), per category,
for the first view (left family: mean of plain and skip windows, which agree) and the second view (right family + tail
in plain windows), from phase_categories.py's output files (timestamp sums, ms per frame). Then the model's prediction
for the shipping state E (two eyes of 640x720 DLSS input, Performance).

  python view_model.py <vr10 phase_categories-r20.txt> <vr14 phase_categories-r20.txt>"""
import re
import sys
from collections import defaultdict

MP1, MP2 = 1116 * 1256 / 1e6, 1580 * 1776 / 1e6
E_MP = 640 * 720 / 1e6


def parse(path):
    out = defaultdict(lambda: defaultdict(lambda: [0.0, 0.0]))  # fam -> cat -> [plain, skip]
    fam = None
    for line in open(path, encoding="utf-8-sig"):
        m = re.match(r"family (\w)", line)
        if m:
            fam = m.group(1)
            continue
        m = re.match(r"\s+(.+?)\s{2,}(-?[\d.]+)\s+(-?[\d.]+)\s*$", line)
        if fam and m and m.group(1) != "total":
            out[fam][m.group(1)] = [float(m.group(2)), float(m.group(3))]
    return out


def main():
    a, b = parse(sys.argv[1]), parse(sys.argv[2])
    cats = sorted(set(a["L"]) | set(b["L"]) | set(a["R"]) | set(b["R"]))

    def first(d, c):
        return sum(d["L"].get(c, [0, 0])) / 2

    def second(d, c):
        return d["R"].get(c, [0, 0])[0] + d["T"].get(c, [0, 0])[0]

    rows = []
    for c in cats:
        r = [c]
        for f in (first, second):
            c1, c2 = f(a, c), f(b, c)
            slope = (c2 - c1) / (MP2 - MP1)
            fixed = c1 - slope * MP1
            r += [c1, c2, fixed, slope]
        rows.append(r)
    rows.sort(key=lambda r: -(r[1] + r[5]))
    print("| category | 1st view @1.40 MP | @2.81 MP | fixed | per MP | 2nd view @1.40 MP | @2.81 MP | fixed | per MP |")
    print("|---|---|---|---|---|---|---|---|---|")
    tot = [0.0] * 8
    for r in rows:
        print("| %s | %.2f | %.2f | %+.2f | %.2f | %.2f | %.2f | %+.2f | %.2f |" % tuple(r))
        for i in range(8):
            tot[i] += r[i + 1]
    print("| total | %.2f | %.2f | %+.2f | %.2f | %.2f | %.2f | %+.2f | %.2f |" % tuple(tot))
    e1 = tot[2] + tot[3] * E_MP
    e2 = tot[6] + tot[7] * E_MP
    print("\nE (two eyes of %.2f MP input): first view %.2f + second %.2f = %.2f ms/f (timestamps); fixed part %.2f (%.0f %%)" % (
        E_MP, e1, e2, e1 + e2, tot[2] + tot[6], 100 * (tot[2] + tot[6]) / (e1 + e2)))
    print("E by category (both views):")
    for r in rows:
        v = r[3] + r[4] * E_MP + r[7] + r[8] * E_MP
        print("   %-58s %.2f (fixed %.2f)" % (r[0], v, r[3] + r[7]))


if __name__ == "__main__":
    main()
