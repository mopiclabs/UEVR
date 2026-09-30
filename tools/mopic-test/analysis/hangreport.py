# Per-thread heuristic stacks of a hang dump, with UEVRBackend frames symbolized through its PDB.
# usage: hangreport.py <dump> <UEVRBackend.dll with .pdb next to it> [hits_per_thread=16]
import logging, os, re, struct, subprocess, sys
logging.disable(logging.ERROR)
from minidump.minidumpfile import MinidumpFile
from minidump.streams.ContextStream import CONTEXT

dump, uevr_dll = sys.argv[1], sys.argv[2]
per_thread = int(sys.argv[3]) if len(sys.argv) > 3 else 16
here = os.path.dirname(os.path.abspath(__file__))

md = MinidumpFile.parse(dump)
reader = md.get_reader()
mods = sorted((m.baseaddress, m.baseaddress + m.size, m.name.split("\\")[-1]) for m in md.modules.modules)


def sym(a):
    for lo, hi, name in mods:
        if lo <= a < hi:
            return name, a - lo
    return None


threads = []
for t in md.threads.threads:
    try:
        md.file_handle.seek(t.ThreadContext.Rva)
        ctx = CONTEXT.parse(md.file_handle)
    except Exception:
        continue
    frames = []
    s = sym(ctx.Rip)
    if s:
        frames.append(s)
    for off in range(0, 0x6000, 8):
        try:
            v = struct.unpack("<Q", reader.read(ctx.Rsp + off, 8))[0]
        except Exception:
            break
        s = sym(v)
        if s and s[1] != 0:
            frames.append(s)
        if len(frames) >= per_thread:
            break
    threads.append((t.ThreadId, frames))

rvas = sorted({f"{off:x}" for _, frames in threads for name, off in frames if name.lower() == "uevrbackend.dll"})
names = {}
if rvas:
    out = subprocess.run([sys.executable, os.path.join(here, "symbolize.py"), uevr_dll] + rvas, capture_output=True,
                         text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\+0x([0-9a-f]+)\s+(.*)", line)
        if m:
            names[int(m.group(1), 16)] = m.group(2)

print(f"{len(threads)} threads")
for tid, frames in threads:
    uevr = any(n.lower() == "uevrbackend.dll" for n, _ in frames)
    print(f"\nthread {tid}{'  <-- UEVR on stack' if uevr else ''}")
    for n, off in frames:
        label = names.get(off, "") if n.lower() == "uevrbackend.dll" else ""
        print(f"    {n}+0x{off:x}  {label}")
