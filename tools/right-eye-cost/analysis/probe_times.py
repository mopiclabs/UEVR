"""QPC times (ns, the clock of perf-frames.csv) of log lines matching a pattern in a run's log.txt, through the
pilot-status.json clock pair (qpc_ns, unix_ns). Default pattern: the [GdfFind] scans of GDF probe 5, which stall the
game thread for 1-6 s at pair submissions 1200 / 2400 / 4800 / 9600 / 19200 whenever pass markers are on (builds up to
28dedeef). Used by hitch_kinds.py / hitch_switch.py --exclude-probe S to leave out the S seconds after each scan."""
import json, os, re
from datetime import datetime

DEFAULT = r"\[GdfFind\]"


def probe_qpc(run, pattern=DEFAULT):
    st = json.load(open(os.path.join(run, "pilot-status.json"), encoding="utf-8-sig"))
    c = st.get("clock") or {}
    if "qpc_ns" not in c or "unix_ns" not in c:
        return []
    out = []
    rx = re.compile(pattern)
    for line in open(os.path.join(run, "log.txt"), encoding="utf-8", errors="replace"):
        if rx.search(line) and line.startswith("["):
            try:
                t = datetime.strptime(line[1:24], "%Y-%m-%d %H:%M:%S.%f")   # local time
            except ValueError:
                continue
            unix_ns = int(t.timestamp() * 1e9)
            out.append(int(c["qpc_ns"]) + (unix_ns - int(c["unix_ns"])))
    return sorted(out)


def near(q, times, after_s, before_s=6.0):
    """q within [t - before_s, t + after_s] of any time (a scan logs at its end, so its stall lies before the line)."""
    return any(t - before_s * 1e9 <= q <= t + after_s * 1e9 for t in times)
