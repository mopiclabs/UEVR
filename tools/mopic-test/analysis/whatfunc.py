# For RVAs in an unsymbolized PE: the containing function (from .pdata) and the strings it references.
# usage: whatfunc.py <exe> <rva> [<rva> ...]
import bisect, re, struct, sys
import pefile, capstone

pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"], pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
iat = {imp.address - pe.OPTIONAL_HEADER.ImageBase: (d.dll.decode() + "!" + (imp.name.decode() if imp.name else str(imp.ordinal))) for d in pe.DIRECTORY_ENTRY_IMPORT for imp in d.imports}
data = pe.get_memory_mapped_image()
base = pe.OPTIONAL_HEADER.ImageBase
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def string_at(rva):
    chunk = data[rva:rva + 200]
    m = re.match(rb"([\x20-\x7e]{4,})\x00", chunk)
    if m:
        return m.group(1).decode()
    m = re.match(rb"((?:[\x20-\x7e]\x00){4,})\x00\x00", chunk)
    if m:
        return "L" + m.group(1).decode("utf-16-le")
    return None


for arg in sys.argv[2:]:
    rva = int(arg, 16)
    i = bisect.bisect_right(starts, rva) - 1
    if i < 0 or not (funcs[i][0] <= rva < funcs[i][1]):
        print(f"+0x{rva:x}: no .pdata entry"); continue
    b, e = funcs[i]
    strings, calls, imports = [], 0, []
    for ins in md.disasm(data[b:e], base + b):
        if ins.mnemonic == "call":
            calls += 1
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                target = ins.address + ins.size + op.mem.disp - base
                if target in iat and iat[target] not in imports:
                    imports.append(iat[target])
                s = string_at(target)
                if s and s not in strings:
                    strings.append(s)
    print(f"+0x{rva:x}: in function +0x{b:x}..+0x{e:x} ({e - b} bytes, {calls} calls); strings: {strings[:12]}; imports: {imports}")
