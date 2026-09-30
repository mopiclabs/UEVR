# Resolve module RVAs to function+offset (and source line) with dbghelp and the module's PDB.
# usage: symbolize.py <module.dll> <rva> [<rva> ...]      (the .pdb next to the dll is used; DBGHELP=<path> picks a dbghelp.dll)
import ctypes, ctypes.wintypes as wt, os, sys

dbghelp = ctypes.WinDLL(os.environ.get("DBGHELP", r"C:\Windows\System32\dbghelp.dll"), use_last_error=True)
dll = os.path.abspath(sys.argv[1])
rvas = [int(a, 16) for a in sys.argv[2:]]

MAX_NAME = 1024


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", wt.WCHAR * MAX_NAME)]


class IMAGEHLP_LINEW64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD),
                ("FileName", ctypes.c_wchar_p), ("Address", ctypes.c_uint64)]


h = wt.HANDLE(0x1234)
dbghelp.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES (no DEFERRED_LOADS: SymFromAddr then fails with 126)
if not dbghelp.SymInitializeW(h, ctypes.c_wchar_p(os.path.dirname(dll)), False):
    raise SystemExit(f"SymInitialize failed {ctypes.get_last_error()}")
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint64,
                                     wt.DWORD, ctypes.c_void_p, wt.DWORD]
base = 0x180000000
b = dbghelp.SymLoadModuleExW(h, None, dll, None, base, 0, None, 0)
if not b:
    raise SystemExit(f"SymLoadModuleEx failed {ctypes.get_last_error()}")
# (SymLoadModuleEx returns the base it used)
if b and b != base:
    base = b
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
dbghelp.SymGetLineFromAddrW64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINEW64)]
for rva in rvas:
    si = SYMBOL_INFOW(); si.SizeOfStruct = 88; si.MaxNameLen = MAX_NAME
    disp = ctypes.c_uint64()
    name = "?"
    if dbghelp.SymFromAddrW(h, base + rva, ctypes.byref(disp), ctypes.byref(si)):
        name = f"{si.Name}+0x{disp.value:x}"
    else:
        name = f"? (error {ctypes.get_last_error()})"
    line = IMAGEHLP_LINEW64(); line.SizeOfStruct = ctypes.sizeof(line)
    d32 = wt.DWORD()
    src = ""
    if dbghelp.SymGetLineFromAddrW64(h, base + rva, ctypes.byref(d32), ctypes.byref(line)):
        src = f"  ({os.path.basename(line.FileName)}:{line.LineNumber})"
    print(f"+0x{rva:x}  {name}{src}")
