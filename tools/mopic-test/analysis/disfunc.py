# Disassemble the .pdata function containing an RVA, with import names and direct call targets.
# usage: disfunc.py <exe> <rva>
import bisect, sys
import pefile, capstone

pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"],
                                       pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
base = pe.OPTIONAL_HEADER.ImageBase
iat = {imp.address - base: (d.dll.decode() + "!" + (imp.name.decode() if imp.name else str(imp.ordinal)))
       for d in pe.DIRECTORY_ENTRY_IMPORT for imp in d.imports}
data = pe.get_memory_mapped_image()
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
rva = int(sys.argv[2], 16)
i = bisect.bisect_right(starts, rva) - 1
if i < 0 or not (funcs[i][0] <= rva < funcs[i][1]):
    sys.exit(f"+0x{rva:x}: no .pdata entry (a leaf function, or not code)")
b, e = funcs[i]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
for ins in md.disasm(data[b:e], base + b):
    note = ""
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            t = ins.address + ins.size + op.mem.disp - base
            note = iat.get(t, f"[+0x{t:x}]")
        elif ins.mnemonic in ("call", "jmp") and op.type == capstone.x86.X86_OP_IMM:
            note = f"-> +0x{op.imm - base:x}"
    mark = " <==" if ins.address - base <= rva < ins.address - base + ins.size + 1 and ins.address - base + ins.size == rva else ""
    print(f"+0x{ins.address - base:x}: {ins.mnemonic} {ins.op_str}   {note}{mark}")
