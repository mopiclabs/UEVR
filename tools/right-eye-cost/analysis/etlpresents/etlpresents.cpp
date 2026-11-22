// etlpresents: DXGI Present starts per process from an ETW trace file, fast (R20, 11-22: PresentMon 2.5.1 --etl_file
// wrote nothing for a DXGI-only logman trace, tracerpt's CSV of a 7 GB trace is too big). Prints, to stdout:
//   "E <provider-guid-short> <event id> <pid> <count>"   every (provider, id, pid) seen, at the end
//   "P <pid> <qpc_ticks>"                                every Microsoft-Windows-DXGI event 42 (IDXGISwapChain::Present
//                                                        start) and 55 (PresentMultiplaneOverlay start), with --presents
// Timestamps are raw QPC ticks (the logman session's default clock), the clock of UEVR's perf CSVs and the pilot.
//   etlpresents.exe <file.etl> [--presents]
#define INITGUID
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <cstdio>
#include <map>
#include <tuple>

static const GUID kDxgi = {0xca11c036, 0x0102, 0x4a2d, {0xa6, 0xad, 0xf0, 0x3c, 0xfe, 0xd5, 0xd3, 0xc9}};
static std::map<std::tuple<unsigned long, unsigned short, unsigned long>, unsigned long long> g_counts;
static bool g_presents = false;
static unsigned long long g_events = 0;

static void WINAPI on_event(PEVENT_RECORD e) {
    ++g_events;
    const auto& h = e->EventHeader;
    g_counts[{h.ProviderId.Data1, h.EventDescriptor.Id, h.ProcessId}]++;
    if (g_presents && IsEqualGUID(h.ProviderId, kDxgi) && (h.EventDescriptor.Id == 42 || h.EventDescriptor.Id == 55)) {
        printf("P %lu %lld %u\n", h.ProcessId, h.TimeStamp.QuadPart, h.EventDescriptor.Id);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fprintf(stderr, "usage: etlpresents <file.etl> [--presents]\n"); return 2; }
    g_presents = argc > 2 && wcscmp(argv[2], L"--presents") == 0;
    EVENT_TRACE_LOGFILEW lf{};
    lf.LogFileName = argv[1];
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = on_event;
    TRACEHANDLE th = OpenTraceW(&lf);
    if (th == INVALID_PROCESSTRACE_HANDLE) { fprintf(stderr, "OpenTrace failed %lu\n", GetLastError()); return 1; }
    fprintf(stderr, "timer resolution %lu, perf freq %lld, clock type (ReservedFlags) %lu, events lost %lu, buffers lost %lu\n",
        lf.LogfileHeader.TimerResolution, lf.LogfileHeader.PerfFreq.QuadPart, lf.LogfileHeader.ReservedFlags,
        lf.LogfileHeader.EventsLost, lf.LogfileHeader.BuffersLost);
    ULONG st = ProcessTrace(&th, 1, nullptr, nullptr);
    CloseTrace(th);
    for (const auto& [k, v] : g_counts) {
        printf("E %08lx %u %lu %llu\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), v);
    }
    fprintf(stderr, "events %llu, ProcessTrace %lu\n", g_events, st);
    return 0;
}
