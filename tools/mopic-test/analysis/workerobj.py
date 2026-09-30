# Which object is a stuck worker loop waiting on? Finds threads whose stack returns to <exe>+<rva>, takes
# candidate `this` pointers (registers, stack slots), reads their vtable and resolves the MSVC RTTI class name
# from the exe image.   usage: workerobj.py <dump> <exe> <return-rva-hex>
import logging, os, struct, sys
logging.disable(logging.ERROR)
import pefile
from minidump.minidumpfile import MinidumpFile
from minidump.streams.ContextStream import CONTEXT

dump, exe_path, ret_rva = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
md = MinidumpFile.parse(dump)
reader = md.get_reader()
exe_name = os.path.basename(exe_path).lower()
mods = [m for m in md.modules.modules if os.path.basename(m.name).lower() == exe_name]
if not mods:
    sys.exit(f"{exe_name} is not loaded in {dump}")
exe_mod = mods[0]
exe_base, exe_end = exe_mod.baseaddress, exe_mod.baseaddress + exe_mod.size
pe = pefile.PE(exe_path, fast_load=True)
image = pe.get_memory_mapped_image()
# absolute pointers in the file assume the preferred base; the process has the image elsewhere (ASLR)
delta = exe_base - pe.OPTIONAL_HEADER.ImageBase


def q(addr):
    try:
        return struct.unpack("<Q", reader.read(addr, 8))[0]
    except Exception:
        return None


def img_q(rva):
    return struct.unpack_from("<Q", image, rva)[0] if 0 <= rva < len(image) - 8 else None


def rtti_name(vtable):
    col = img_q(vtable - exe_base - 8)
    if col is None:
        return None
    col += delta
    if not (exe_base <= col < exe_end):
        return None
    sig, off, cdoff, td_rva = struct.unpack_from("<IIII", image, col - exe_base)
    if sig != 1 or not (0 < td_rva < len(image)):
        return None
    name = image[td_rva + 16: td_rva + 16 + 200].split(b"\0")[0]
    return name.decode(errors="replace")


for t in md.threads.threads:
    md.file_handle.seek(t.ThreadContext.Rva)
    ctx = CONTEXT.parse(md.file_handle)
    slots = []
    hit = False
    for off in range(0, 0x3000, 8):
        v = q(ctx.Rsp + off)
        if v is None:
            break
        slots.append(v)
        if v == exe_base + ret_rva:
            hit = True
    if not hit:
        continue
    print(f"thread {t.ThreadId}: r14={ctx.R14:#x} rbx={ctx.Rbx:#x} rsi={ctx.Rsi:#x} rdi={ctx.Rdi:#x}")
    cands = [ctx.R14, ctx.Rbx, ctx.Rsi, ctx.Rdi, ctx.R12, ctx.R13, ctx.R15] + slots
    seen = set()
    for c in cands:
        if c in seen or c is None or c < 0x10000:
            continue
        seen.add(c)
        vt = q(c)
        if vt is None or not (exe_base <= vt < exe_end):
            continue
        name = rtti_name(vt)
        print(f"  object {c:#x}: vtable {exe_mod.name.split(chr(92))[-1]}+0x{vt - exe_base:x}  rtti={name}"
              f"  vfunc+0x10=+0x{(img_q(vt - exe_base + 0x10) or pe.OPTIONAL_HEADER.ImageBase) - pe.OPTIONAL_HEADER.ImageBase:x}")
