# Nearest exported function below an RVA in a system DLL.  usage: sysexports.py <dll> <rva> [<rva> ...]
import bisect, sys
import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
exps = sorted((s.address, (s.name or b"#%d" % s.ordinal).decode()) for s in pe.DIRECTORY_ENTRY_EXPORT.symbols)
addrs = [a for a, _ in exps]
for arg in sys.argv[2:]:
    rva = int(arg, 16)
    i = bisect.bisect_right(addrs, rva) - 1
    print(f"+0x{rva:x}: {exps[i][1]}+0x{rva - exps[i][0]:x}" if i >= 0 else f"+0x{rva:x}: ?")
