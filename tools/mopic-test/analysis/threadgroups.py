# Group a hang report's threads by stack signature and print the rare ones (the interesting threads in a hang).
# Also lists thread names if the dump has a ThreadNameList stream.  usage: threadgroups.py <hangreport.txt> [<dump>]
import collections, re, struct, sys

raw = open(sys.argv[1], "rb").read()
# Windows PowerShell 5.1's '>' writes UTF-16
text = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8-sig", "replace")
blocks, cur = [], None
for line in text.splitlines():
    line = line.rstrip()
    if line.startswith("thread "):
        if cur:
            blocks.append(cur)
        cur = [line]
    elif cur is not None and line.strip():
        cur.append(line.strip())
if cur:
    blocks.append(cur)

names = {}
if len(sys.argv) > 2:
    data = open(sys.argv[2], "rb").read()
    sig, ver, nstreams, dir_rva = struct.unpack_from("<IIII", data, 0)
    for i in range(nstreams):
        stype, size, rva = struct.unpack_from("<III", data, dir_rva + i * 12)
        if stype == 24:  # ThreadNamesStream
            count = struct.unpack_from("<I", data, rva)[0]
            for j in range(count):
                tid, name_rva = struct.unpack_from("<IQ", data, rva + 4 + j * 12)
                length = struct.unpack_from("<I", data, name_rva)[0]
                names[tid] = data[name_rva + 4:name_rva + 4 + length].decode("utf-16-le", "replace")

def sig(b):
    return tuple(re.sub(r"\s+.*", "", f) for f in b[1:7])

groups = collections.defaultdict(list)
for b in blocks:
    groups[sig(b)].append(b)
print(f"{len(blocks)} threads, {len(groups)} distinct stacks, {len(names)} names")
for s, bs in sorted(groups.items(), key=lambda kv: len(kv[1])):
    tids = [re.match(r"thread (\d+)", b[0]).group(1) for b in bs]
    label = ", ".join(f"{t}{'=' + names[int(t)] if int(t) in names else ''}" for t in tids[:6])
    print(f"\n[{len(bs)}x] {label}")
    for f in bs[0][1:]:
        print("    " + f)
