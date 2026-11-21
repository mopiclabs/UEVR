"""Names inside DXIL shader containers (exp/right-eye-cost pass markers: passmarkers/shaders/<hash>.bin).

Reads the DXBC container, the DXIL part's LLVM 3.7 bitcode, and collects the strings that survive stripping: the
module's value symbol table (function names: the entry point, e.g. "MainCS") and metadata strings (dx.entryPoints'
name, resource names in dx.resources when not stripped). Also the ILDN debug name part when present.

  python dxilnames.py <passmarkers dir> [--hash H]       -> names.tsv in that dir: hash, entry, resources, debug name
"""
import argparse
import glob
import os
import struct
import sys


class Bits:
    def __init__(self, data):
        self.d = data
        self.pos = 0  # bit position
        self.n = len(data) * 8

    def read(self, width):
        v = 0
        shift = 0
        while width:
            byte = self.d[self.pos >> 3]
            off = self.pos & 7
            take = min(8 - off, width)
            v |= ((byte >> off) & ((1 << take) - 1)) << shift
            shift += take
            width -= take
            self.pos += take
        return v

    def vbr(self, width):
        hi = 1 << (width - 1)
        v = 0
        shift = 0
        while True:
            piece = self.read(width)
            v |= (piece & (hi - 1)) << shift
            if not piece & hi:
                return v
            shift += width - 1

    def align32(self):
        self.pos = (self.pos + 31) & ~31

    def eof(self):
        return self.pos + 32 > self.n


CHAR6 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._"


def read_abbrev_op(b, op):
    kind, val = op
    if kind == "lit":
        return val
    if kind == "fixed":
        return b.read(val) if val else 0
    if kind == "vbr":
        return b.vbr(val) if val else 0
    if kind == "char6":
        return ord(CHAR6[b.read(6)])
    raise ValueError(kind)


def parse_bitcode(data):
    """Yields (block_id, code, ops) for every record."""
    b = Bits(data)
    if b.read(32) != 0xDEC04342:  # 'BC' 0xC0DE
        return
    blockinfo = {}  # block id -> [abbrevs]
    stack = [(2, [], -1)]  # (abbrev width, abbrevs, block id)
    cur_bid_for_info = None
    while not b.eof():
        width, abbrevs, bid = stack[-1]
        aid = b.read(width)
        if aid == 0:  # END_BLOCK
            b.align32()
            stack.pop()
            if not stack:
                return
            continue
        if aid == 1:  # ENTER_SUBBLOCK
            nbid = b.vbr(8)
            nwidth = b.vbr(4)
            b.align32()
            nwords = b.read(32)
            stack.append((nwidth, list(blockinfo.get(nbid, [])), nbid))
            continue
        if aid == 2:  # DEFINE_ABBREV
            numops = b.vbr(5)
            ops = []
            i = 0
            while i < numops:
                if b.read(1):
                    ops.append(("lit", b.vbr(8)))
                else:
                    enc = b.read(3)
                    if enc == 1:
                        ops.append(("fixed", b.vbr(5)))
                    elif enc == 2:
                        ops.append(("vbr", b.vbr(5)))
                    elif enc == 3:
                        ops.append(("array", None))
                    elif enc == 4:
                        ops.append(("char6", None))
                    elif enc == 5:
                        ops.append(("blob", None))
                    else:
                        raise ValueError(f"encoding {enc}")
                i += 1
            if bid == 0:
                blockinfo.setdefault(cur_bid_for_info, []).append(ops)
            else:
                abbrevs.append(ops)
            continue
        if aid == 3:  # UNABBREV_RECORD
            code = b.vbr(6)
            n = b.vbr(6)
            ops = [b.vbr(6) for _ in range(n)]
        else:
            ab = abbrevs[aid - 4]
            vals = []
            i = 0
            while i < len(ab):
                kind, val = ab[i]
                if kind == "array":
                    n = b.vbr(6)
                    elt = ab[i + 1]
                    vals.extend(read_abbrev_op(b, elt) for _ in range(n))
                    i += 2
                    continue
                if kind == "blob":
                    n = b.vbr(6)
                    b.align32()
                    start = b.pos >> 3
                    vals.extend(data[start:start + n])
                    b.pos += n * 8
                    b.align32()
                    i += 1
                    continue
                vals.append(read_abbrev_op(b, ab[i]))
                i += 1
            code, ops = vals[0], vals[1:]
        if bid == 0 and code == 1:  # BLOCKINFO SETBID
            cur_bid_for_info = ops[0]
            continue
        yield bid, code, ops


def text(ops):
    try:
        return bytes(o & 0xFF for o in ops).decode("latin-1")
    except Exception:
        return ""


def shader_names(blob):
    """(entry candidates, metadata strings, debug name) of one DXBC container."""
    if blob[:4] != b"DXBC":
        return [], [], ""
    count = struct.unpack_from("<I", blob, 28)[0]
    offsets = struct.unpack_from(f"<{count}I", blob, 32)
    debug = ""
    funcs, mds = [], []
    for off in offsets:
        fourcc = blob[off:off + 4]
        size = struct.unpack_from("<I", blob, off + 4)[0]
        part = blob[off + 8: off + 8 + size]
        if fourcc == b"ILDN" and len(part) >= 4:
            n = struct.unpack_from("<H", part, 2)[0]
            debug = part[4:4 + n].decode("latin-1", "replace")
        if fourcc in (b"DXIL", b"STAT") and len(part) >= 24 and part[8:12] == b"DXIL":
            bc_off, bc_size = struct.unpack_from("<II", part, 16)
            bc = part[8 + bc_off: 8 + bc_off + bc_size]
            try:
                for bid, code, ops in parse_bitcode(bc):
                    if bid == 14 and code in (1, 3):  # VALUE_SYMTAB entry / function entry
                        name = text(ops[2:] if code == 3 else ops[1:])
                        if name and not name.startswith(("dx.", "llvm.")):
                            funcs.append(name)
                    elif bid == 15 and code in (1, 4):  # METADATA_STRING / METADATA_NAME
                        s = text(ops)
                        if s:
                            mds.append(s)
            except Exception as e:  # a part we can't walk: keep what we have
                mds.append(f"<parse error {e}>")
    return funcs, mds, debug


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--hash", default="")
    a = ap.parse_args()
    sd = os.path.join(a.dir, "shaders")
    files = [os.path.join(sd, a.hash + ".bin")] if a.hash else sorted(glob.glob(os.path.join(sd, "*.bin")))
    rows = []
    for p in files:
        blob = open(p, "rb").read()
        funcs, mds, debug = shader_names(blob)
        h = os.path.basename(p)[:-4]
        skip = {"dx.version", "dx.valver", "dx.shaderModel", "dx.resources", "dx.typeAnnotations", "dx.entryPoints",
                "dx.viewIdState", "dx.source.contents", "dx.source.defines", "dx.source.mainFileName", "dx.source.args",
                "llvm.ident", "dx.precise", "dx.dxrPayloadAnnotations", "dx.controlflow.hints", "llvm.loop.unroll.disable"}
        res = [m for m in mds if m not in skip and not m.startswith(("dx.", "llvm.", "clang", "<"))]
        rows.append((h, ",".join(dict.fromkeys(funcs)), ",".join(dict.fromkeys(res))[:300], debug))
        if a.hash:
            print("functions:", funcs)
            print("metadata strings:", mds[:80])
            print("debug name:", debug)
    if not a.hash:
        out = os.path.join(a.dir, "names.tsv")
        with open(out, "w", encoding="utf-8") as f:
            f.write("hash\tfunctions\tstrings\tdebug\n")
            for r in rows:
                f.write("\t".join(r) + "\n")
        print(f"{len(rows)} shaders -> {out}")


if __name__ == "__main__":
    sys.exit(main())
