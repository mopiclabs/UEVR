"""GDF block layout of a game against Wukong UE5.0's, from the [Experiment][GdfAnchor] lines of a probe run's log.txt
(build 438c72f1+: probe 5 dumps the qwords around the PageAtlas field, object pointers named). Wukong: the nine
resources at anchor -0x10 .. +0x30 (FreeListAllocator, FreeList, PageAtlas, CoverageAtlas, PageObjectGridBuffer,
PageTableCombined, PageTableStationaryLayer, PageTableMovableLayer, MipTexture), the int32 update index at -0x50
(0..0x80), the block from -0x268. Prints, per line, every named object with its offset from the anchor and whether
each Wukong position holds the same name.
  python anchor_layout.py <run dir>"""
import os, re, sys

WUKONG = {-0x10: "PageFreeListAllocator", -0x08: "PageFreeList", 0x00: "PageAtlas", 0x08: "CoverageAtlas",
          0x10: "PageObjectGridBuffer", 0x18: "PageTableCombined", 0x20: "PageTableStationaryLayer",
          0x28: "PageTableMovableLayer", 0x30: "MipTexture"}
log = open(os.path.join(sys.argv[1], "log.txt"), encoding="utf-8", errors="replace").read()
lines = re.findall(r"\[GdfAnchor\] #(\d+) ([LR]) state ([0-9a-f]+): PageAtlas field \+0x([0-9a-f]+); qwords \+0x[0-9a-f]+\.\.\+0x[0-9a-f]+:(.*)", log)
finds = re.findall(r"\[GdfFind\] #(\d+) ([LR]) state [0-9a-f]+: stopped at \+0x[0-9a-f]+, (\d+) named objects; distance-field ones:(.*)", log)
print(f"[GdfFind] lines {len(finds)}; [GdfAnchor] lines {len(lines)}")
for n, eye, cnt, df in finds:
    print(f"   find #{n} {eye}: {cnt} named; distance-field: {df.strip()[:300]}")
for n, eye, st, anc, rest in lines:
    a = int(anc, 16)
    q = {}
    for off, val, name in re.findall(r"\+([0-9a-f]+):([0-9a-f?]+)(?:=(\S+))?", rest):
        q[int(off, 16) - a] = (val, name)
    named = [(o, v[1]) for o, v in sorted(q.items()) if v[1]]
    match = sum(1 for o, nm in WUKONG.items() if q.get(o, ("", ""))[1].endswith(nm))
    idx = q.get(-0x50, ("?", ""))[0]
    try:
        iv = int(idx, 16) & 0xffffffff
    except ValueError:
        iv = None
    print(f"#{n} {eye}: anchor +0x{a:x}; Wukong resource positions matching {match}/9; qword at -0x50 = {idx}"
          f"{f' (low int32 {iv})' if iv is not None else ''}")
    print("   named: " + ", ".join(f"{o:+#x} {nm}" for o, nm in named))
