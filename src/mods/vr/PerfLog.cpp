#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <initializer_list>
#include <string_view>

#include "PerfLog.hpp"

// The writer for PerfLog.hpp's counters. It wakes on a fixed one-second schedule (so a frozen game still gets
// rows, with zero frames in them), turns the running totals into one perf.csv row plus one perf-frames.csv row
// per xrEndFrame, and hands them to WriteFile. WriteFile puts the data in the OS file cache straight away, so the
// rows survive a crash or a kill without any flushing.
//
// The framework is deliberately leaked at process exit (Main.cpp) and ExitProcess kills this thread wherever it
// is. So in its loop it allocates nothing, uses no CRT stdio or spdlog, and takes no lock but the frame buffer's
// SRW lock, which the frame paths only ever try-acquire: a killed writer can never leave a lock behind that the
// exit path or a frame needs.

namespace uevr::perf {
namespace {
constexpr size_t FRAME_CAPACITY = 2048;            // ~8 s of xrEndFrame at 240/s; the writer drains it every second
constexpr int64_t WRITE_INTERVAL_NS = 1'000'000'000;
constexpr int64_t TIME_PAIR_INTERVAL_NS = 1'000'000'000;
constexpr DWORD STOP_WAIT_MS = 250;
constexpr size_t TEXT_CAPACITY = 256 * 1024;
constexpr size_t ROW_RESERVE = 2048;               // longer than any single row
// perf-frames.csv is ~130 bytes per xrEndFrame (~28 MB an hour at 60 Hz). Past this it stops growing for the rest
// of the session (perf.csv counts the rows left out); perf.csv itself is ~2 MB an hour.
constexpr uint64_t FRAMES_FILE_MAX_BYTES = 64ull * 1024 * 1024;

constexpr std::string_view SUMMARY_FILE = "perf.csv";
constexpr std::string_view FRAMES_FILE = "perf-frames.csv";

// The first line of each file is its header: tools/mopic-test/perfreport.py looks the columns up by these names
// (and selftest/selftest_perf.py writes synthetic files with them).
constexpr std::string_view SUMMARY_HEADER =
    "qpc_ns,unix_ms,window_ms,"
    "xr_frames,xr_new,xr_repeat,xr_empty,xr_fail,xr_older,vr_fps,"
    "new_int_avg_ms,new_int_p50_ms,new_int_p95_ms,new_int_max_ms,"
    "wait_calls,wait_ok,wait_skip,no_render,discarded,wait_avg_ms,wait_max_ms,end_avg_ms,end_max_ms,"
    "period_ms,period_max_ms,"
    "presents,dup_presents,present_fps,present_avg_ms,present_max_ms,"
    "ticks,ticks_skipped,tick_fps,presents_per_tick,tick_avg_ms,tick_max_ms,tick_gap_max_ms,since_tick_ms,engine_delta_ms,"
    "uevr_gt_avg_ms,gt_jobs_avg_ms,uevr_rt_avg_ms,uevr_rt_max_ms,rt_blocked_avg_ms,"
    "fence_waits,fence_wait_ms,"
    "nsf_fresh,nsf_reused,nsf_fallback,nsf_live,nsf_snapshot,"
    "callsite,callsite_mask,"
    "flags,afr,synced,skip_draw,nsf,nsf_active,nsf_array,async_wait,d3d12,framegen_sc,dlssg,menu,hmd,focused,foreground,mono,"
    "sync_stage,method,"
    "xr_pair_qpc_ns,xr_pair_time,frames_dropped,frame_rows_unwritten\n";

constexpr std::string_view FRAMES_HEADER =
    "qpc_ns,kind,frame,display_time,period_ns,end_us,wait_us,callsite,nsf,nsf_pair,flags,"
    "presents,dup_presents,present_us,ticks,ticks_skipped,tick_us,uevr_gt_us,uevr_rt_us,rt_blocked_us\n";

// Shared with the frame paths, under g_frames_lock.
SRWLOCK g_frames_lock = SRWLOCK_INIT;
FrameRecord g_frames[FRAME_CAPACITY]{};
size_t g_frame_count{};
int64_t g_pair_qpc_ns{};
int64_t g_pair_xr_time{};
std::atomic<uint64_t> g_frames_dropped{};
std::atomic<int64_t> g_next_time_pair_ns{};

// Control state: start()/stop() on the caller's thread, the rest on the writer thread.
SRWLOCK g_control_lock = SRWLOCK_INIT;
std::atomic_bool g_running{};
bool g_abandoned{}; // stop() gave up waiting for the thread; never start a second one next to it
HANDLE g_thread{};
HANDLE g_stop_event{};
HANDLE g_summary_file{INVALID_HANDLE_VALUE};
HANDLE g_frames_file{INVALID_HANDLE_VALUE};

// Running totals, so each row reports the difference to the previous one.
struct Totals {
    uint64_t xr_new{}, xr_repeat{}, xr_empty{}, xr_fail{}, xr_older{}, end_calls{}, end_ns{};
    uint64_t wait_calls{}, wait_ok{}, wait_skip{}, wait_ns{}, no_render{}, discarded{};
    uint64_t presents{}, present_ns{}, dup_presents{}, uevr_rt_ns{}, rt_blocked_ns{};
    uint64_t ticks{}, ticks_skipped{}, tick_ns{}, uevr_gt_ns{}, gt_jobs_ns{}, engine_delta_us{};
    uint64_t fence_waits{}, fence_wait_ns{}, nsf_fresh{}, nsf_reused{}, nsf_fallback{}, nsf_live{}, nsf_snapshot{};
    uint64_t frames_dropped{};
};

struct WriterState {
    int64_t window_start_ns{};
    Totals totals{};
    FrameTotals frame_totals{};
    bool was_enabled{};
    int64_t last_new_qpc_ns{};   // the previous P row, so intervals chain across rows
    int64_t pair_qpc_ns{};       // the latest QPC/XrTime pair, 0 until the first one
    int64_t pair_xr_time{};
    uint64_t frames_bytes{};     // written to perf-frames.csv so far
    bool frames_full{};          // perf-frames.csv reached its cap; it stays as it is for the rest of the session
    uint64_t rows_unwritten{};   // this window's frame rows left out once perf-frames.csv reached its cap
    size_t interval_count{};
    int64_t intervals[FRAME_CAPACITY]{};
    FrameRecord drained[FRAME_CAPACITY]{};
    char text[TEXT_CAPACITY]{};
};

WriterState g_writer{};

Totals snapshot_totals() noexcept {
    const auto& c = g_counters;
    constexpr auto relaxed = std::memory_order_relaxed;
    Totals t{};
    t.xr_new = c.xr_new.load(relaxed);
    t.xr_repeat = c.xr_repeat.load(relaxed);
    t.xr_empty = c.xr_empty.load(relaxed);
    t.xr_fail = c.xr_fail.load(relaxed);
    t.xr_older = c.xr_older.load(relaxed);
    t.end_calls = c.end_calls.load(relaxed);
    t.end_ns = c.end_ns.load(relaxed);
    t.wait_calls = c.wait_calls.load(relaxed);
    t.wait_ok = c.wait_ok.load(relaxed);
    t.wait_skip = c.wait_skip.load(relaxed);
    t.wait_ns = c.wait_ns.load(relaxed);
    t.no_render = c.no_render.load(relaxed);
    t.discarded = c.discarded.load(relaxed);
    t.presents = c.presents.load(relaxed);
    t.present_ns = c.present_ns.load(relaxed);
    t.dup_presents = c.dup_presents.load(relaxed);
    t.uevr_rt_ns = c.uevr_rt_ns.load(relaxed);
    t.rt_blocked_ns = c.rt_blocked_ns.load(relaxed);
    t.ticks = c.ticks.load(relaxed);
    t.ticks_skipped = c.ticks_skipped.load(relaxed);
    t.tick_ns = c.tick_ns.load(relaxed);
    t.uevr_gt_ns = c.uevr_gt_ns.load(relaxed);
    t.gt_jobs_ns = c.gt_jobs_ns.load(relaxed);
    t.engine_delta_us = c.engine_delta_us.load(relaxed);
    t.fence_waits = c.fence_waits.load(relaxed);
    t.fence_wait_ns = c.fence_wait_ns.load(relaxed);
    t.nsf_fresh = c.nsf_fresh.load(relaxed);
    t.nsf_reused = c.nsf_reused.load(relaxed);
    t.nsf_fallback = c.nsf_fallback.load(relaxed);
    t.nsf_live = c.nsf_live.load(relaxed);
    t.nsf_snapshot = c.nsf_snapshot.load(relaxed);
    t.frames_dropped = g_frames_dropped.load(relaxed);
    return t;
}

// Appends CSV fields to a fixed buffer without allocating or touching the CRT locale.
class Row {
public:
    Row(char* begin, char* end) noexcept
        : m_pos{begin}, m_end{end} {
    }

    char* pos() const noexcept { return m_pos; }

    Row& text(std::string_view value) noexcept {
        const auto n = std::min<size_t>(value.size(), static_cast<size_t>(m_end - m_pos));
        std::memcpy(m_pos, value.data(), n);
        m_pos += n;
        return *this;
    }

    Row& comma() noexcept { return text(","); }
    Row& end_line() noexcept { return text("\n"); }

    Row& i(int64_t value) noexcept {
        if (const auto r = std::to_chars(m_pos, m_end, value); r.ec == std::errc{}) {
            m_pos = r.ptr;
        }
        return *this;
    }

    Row& u(uint64_t value) noexcept {
        if (const auto r = std::to_chars(m_pos, m_end, value); r.ec == std::errc{}) {
            m_pos = r.ptr;
        }
        return *this;
    }

    Row& f(double value, int precision = 3) noexcept {
        if (const auto r = std::to_chars(m_pos, m_end, value, std::chars_format::fixed, precision); r.ec == std::errc{}) {
            m_pos = r.ptr;
        }
        return *this;
    }

    // An integer, or an empty field when it isn't known (0).
    Row& known(int64_t value) noexcept { return value != 0 ? i(value) : *this; }

    Row& bit(uint32_t flags, uint32_t mask) noexcept { return text((flags & mask) != 0 ? "1" : "0"); }

private:
    char* m_pos{};
    char* m_end{};
};

// Totals only grow, but a frame record's snapshot can be a little older than the writer's previous one (two
// threads ending frames, or a record pushed while start() reset the window), so never wrap below zero.
uint64_t delta(uint64_t current, uint64_t previous) noexcept {
    return current > previous ? current - previous : 0;
}

double ns_to_ms(uint64_t ns) noexcept {
    return static_cast<double>(ns) / 1'000'000.0;
}

double avg_ms(uint64_t total_ns, uint64_t count) noexcept {
    return count == 0 ? 0.0 : ns_to_ms(total_ns) / static_cast<double>(count);
}

double per_second(uint64_t count, int64_t window_ns) noexcept {
    return window_ns <= 0 ? 0.0 : static_cast<double>(count) * 1'000'000'000.0 / static_cast<double>(window_ns);
}

// Nearest-rank percentile of values[0, count), which it reorders. 0 when empty.
int64_t percentile(int64_t* values, size_t count, double q) noexcept {
    if (count == 0) {
        return 0;
    }

    const auto rank = static_cast<size_t>(q * static_cast<double>(count) + 0.999999);
    const auto index = std::min<size_t>(count - 1, rank > 0 ? rank - 1 : 0);
    std::nth_element(values, values + index, values + count);
    return values[index];
}

int64_t unix_ms_now() noexcept {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    const auto ticks = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (ticks - 116'444'736'000'000'000LL) / 10'000;
}

bool write_all(HANDLE file, const char* data, size_t size) noexcept {
    while (size > 0) {
        DWORD written{};
        const auto chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
        if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }

    return true;
}

// Not noexcept: the path operations allocate, and start() turns an exception into an error code.
HANDLE create_log_file(const std::filesystem::path& dir, std::string_view name, std::string_view header, DWORD& error) {
    std::error_code ec{};
    const auto path = dir / name;
    auto previous = path;
    previous.replace_extension(".prev.csv");

    // Keep the previous session's file (one generation), like a log rotation.
    if (std::filesystem::exists(path, ec)) {
        MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return INVALID_HANDLE_VALUE;
    }

    if (!write_all(file, header.data(), header.size())) {
        error = GetLastError();
        CloseHandle(file);
        return INVALID_HANDLE_VALUE;
    }

    return file;
}

void close_files() noexcept {
    for (auto* file : {&g_summary_file, &g_frames_file}) {
        if (*file != INVALID_HANDLE_VALUE) {
            CloseHandle(*file);
            *file = INVALID_HANDLE_VALUE;
        }
    }
}

size_t drain_frames(FrameRecord* out, int64_t& pair_qpc_ns, int64_t& pair_xr_time) noexcept {
    AcquireSRWLockExclusive(&g_frames_lock);
    const auto count = g_frame_count;
    std::copy_n(g_frames, count, out);
    g_frame_count = 0;
    pair_qpc_ns = g_pair_qpc_ns;
    pair_xr_time = g_pair_xr_time;
    ReleaseSRWLockExclusive(&g_frames_lock);
    return count;
}

void flush_frame_text(WriterState& w, const char* begin, const char* end, size_t rows) noexcept {
    const auto size = static_cast<size_t>(end - begin);

    if (size == 0) {
        return;
    }

    if (w.frames_full || w.frames_bytes + size > FRAMES_FILE_MAX_BYTES) {
        w.frames_full = true;
        w.rows_unwritten += rows;
        return;
    }

    write_all(g_frames_file, begin, size);
    w.frames_bytes += size;
}

void write_frame_rows(WriterState& w, size_t count) noexcept {
    auto* const begin = w.text;
    auto* const end = w.text + TEXT_CAPACITY;
    auto* pos = begin;
    size_t pending_rows{};
    w.interval_count = 0;

    for (size_t n = 0; n < count; ++n) {
        if (static_cast<size_t>(end - pos) < ROW_RESERVE) {
            flush_frame_text(w, begin, pos, pending_rows);
            pos = begin;
            pending_rows = 0;
        }

        const auto& r = w.drained[n];
        const auto& t = r.totals;
        const auto& p = w.frame_totals;
        const char kind[2]{static_cast<char>(r.kind), '\0'};

        if (r.kind == Submit::New) {
            if (w.last_new_qpc_ns != 0 && r.qpc_ns > w.last_new_qpc_ns && w.interval_count < FRAME_CAPACITY) {
                w.intervals[w.interval_count++] = r.qpc_ns - w.last_new_qpc_ns;
            }
            w.last_new_qpc_ns = r.qpc_ns;
        }

        Row row{pos, end};
        row.i(r.qpc_ns).comma()
            .text(kind).comma()
            .u(r.frame).comma()
            .i(r.display_time).comma()
            .i(r.period_ns).comma()
            .i(r.end_ns / 1000).comma()
            .u(delta(t.wait_ns, p.wait_ns) / 1000).comma()
            .text(r.callsite != nullptr ? r.callsite : "").comma()
            .u(static_cast<uint32_t>(r.nsf)).comma()
            .u(static_cast<uint32_t>(r.pair)).comma()
            .u(r.flags).comma()
            .u(delta(t.presents, p.presents)).comma()
            .u(delta(t.dup_presents, p.dup_presents)).comma()
            .u(delta(t.present_ns, p.present_ns) / 1000).comma()
            .u(delta(t.ticks, p.ticks)).comma()
            .u(delta(t.ticks_skipped, p.ticks_skipped)).comma()
            .u(delta(t.tick_ns, p.tick_ns) / 1000).comma()
            .u(delta(t.uevr_gt_ns, p.uevr_gt_ns) / 1000).comma()
            .u(delta(t.uevr_rt_ns, p.uevr_rt_ns) / 1000).comma()
            .u(delta(t.rt_blocked_ns, p.rt_blocked_ns) / 1000)
            .end_line();
        pos = row.pos();
        ++pending_rows;
        w.frame_totals = t;
    }

    flush_frame_text(w, begin, pos, pending_rows);
}

void write_summary_row(WriterState& w, int64_t now) noexcept {
    auto& c = g_counters;
    constexpr auto relaxed = std::memory_order_relaxed;
    const auto t = snapshot_totals();
    const auto& p = w.totals;
    const auto window_ns = now - w.window_start_ns;
    const auto flags = c.flags.load(relaxed);
    const auto last_tick = c.last_tick_ns.load(relaxed);
    const auto* callsite = c.last_callsite_name.load(relaxed);

    const auto xr_new = delta(t.xr_new, p.xr_new);
    const auto presents = delta(t.presents, p.presents);
    const auto ticks = delta(t.ticks, p.ticks);
    const auto ticks_skipped = delta(t.ticks_skipped, p.ticks_skipped);
    const auto uevr_rt = delta(t.uevr_rt_ns, p.uevr_rt_ns);
    const auto rt_blocked = delta(t.rt_blocked_ns, p.rt_blocked_ns);

    // Intervals between this window's new frames (the first one chains to the previous window's last).
    int64_t interval_sum{};
    int64_t interval_max{};
    for (size_t n = 0; n < w.interval_count; ++n) {
        interval_sum += w.intervals[n];
        interval_max = std::max<int64_t>(interval_max, w.intervals[n]);
    }
    const auto interval_avg = w.interval_count == 0 ? 0.0 : static_cast<double>(interval_sum) / static_cast<double>(w.interval_count);
    const auto interval_p50 = percentile(w.intervals, w.interval_count, 0.50);
    const auto interval_p95 = percentile(w.intervals, w.interval_count, 0.95);

    Row row{w.text, w.text + TEXT_CAPACITY};
    row.i(now).comma()
        .i(unix_ms_now()).comma()
        .f(static_cast<double>(window_ns) / 1'000'000.0).comma()
        .u(delta(t.end_calls, p.end_calls)).comma()
        .u(xr_new).comma()
        .u(delta(t.xr_repeat, p.xr_repeat)).comma()
        .u(delta(t.xr_empty, p.xr_empty)).comma()
        .u(delta(t.xr_fail, p.xr_fail)).comma()
        .u(delta(t.xr_older, p.xr_older)).comma()
        .f(per_second(xr_new, window_ns), 2).comma()
        .f(interval_avg / 1'000'000.0).comma()
        .f(ns_to_ms(non_negative(interval_p50))).comma()
        .f(ns_to_ms(non_negative(interval_p95))).comma()
        .f(ns_to_ms(non_negative(interval_max))).comma()
        .u(delta(t.wait_calls, p.wait_calls)).comma()
        .u(delta(t.wait_ok, p.wait_ok)).comma()
        .u(delta(t.wait_skip, p.wait_skip)).comma()
        .u(delta(t.no_render, p.no_render)).comma()
        .u(delta(t.discarded, p.discarded)).comma()
        .f(avg_ms(delta(t.wait_ns, p.wait_ns), delta(t.wait_calls, p.wait_calls))).comma()
        .f(ns_to_ms(non_negative(c.wait_max_ns.exchange(0, relaxed)))).comma()
        .f(avg_ms(delta(t.end_ns, p.end_ns), delta(t.end_calls, p.end_calls))).comma()
        .f(ns_to_ms(non_negative(c.end_max_ns.exchange(0, relaxed)))).comma()
        .f(ns_to_ms(non_negative(c.period_ns.load(relaxed)))).comma()
        .f(ns_to_ms(non_negative(c.period_max_ns.exchange(0, relaxed)))).comma()
        .u(presents).comma()
        .u(delta(t.dup_presents, p.dup_presents)).comma()
        .f(per_second(presents, window_ns), 2).comma()
        .f(avg_ms(delta(t.present_ns, p.present_ns), presents)).comma()
        .f(ns_to_ms(non_negative(c.present_max_ns.exchange(0, relaxed)))).comma()
        .u(ticks).comma()
        .u(ticks_skipped).comma()
        .f(per_second(ticks, window_ns), 2).comma();
    // Per Tick hook pass, skipped ones included (perfreport.py's presents_per_tick: synced sequential's skipped Tick
    // still renders a frame). Empty without one (a game whose Tick hook never counts, or a stall).
    if (ticks + ticks_skipped != 0) {
        row.f(static_cast<double>(presents) / static_cast<double>(ticks + ticks_skipped), 2);
    }
    row.comma()
        .f(avg_ms(delta(t.tick_ns, p.tick_ns), ticks)).comma()
        .f(ns_to_ms(non_negative(c.tick_max_ns.exchange(0, relaxed)))).comma()
        .f(ns_to_ms(non_negative(c.tick_gap_max_ns.exchange(0, relaxed)))).comma()
        .f(last_tick != 0 ? ns_to_ms(non_negative(now - last_tick)) : -1.0).comma()
        .f(static_cast<double>(delta(t.engine_delta_us, p.engine_delta_us)) / 1000.0).comma()
        .f(avg_ms(delta(t.uevr_gt_ns, p.uevr_gt_ns), ticks)).comma()
        .f(avg_ms(delta(t.gt_jobs_ns, p.gt_jobs_ns), ticks + ticks_skipped)).comma()
        .f(avg_ms(uevr_rt, presents)).comma()
        .f(ns_to_ms(non_negative(c.uevr_rt_max_ns.exchange(0, relaxed)))).comma()
        .f(avg_ms(rt_blocked, presents)).comma()
        .u(delta(t.fence_waits, p.fence_waits)).comma()
        .f(ns_to_ms(delta(t.fence_wait_ns, p.fence_wait_ns))).comma()
        .u(delta(t.nsf_fresh, p.nsf_fresh)).comma()
        .u(delta(t.nsf_reused, p.nsf_reused)).comma()
        .u(delta(t.nsf_fallback, p.nsf_fallback)).comma()
        .u(delta(t.nsf_live, p.nsf_live)).comma()
        .u(delta(t.nsf_snapshot, p.nsf_snapshot)).comma()
        .text(callsite != nullptr ? callsite : "").comma()
        .u(c.callsite_mask.exchange(0, relaxed)).comma()
        .u(flags).comma()
        .bit(flags, flag::AFR).comma()
        .bit(flags, flag::SYNCED).comma()
        .bit(flags, flag::SKIP_DRAW).comma()
        .bit(flags, flag::NSF).comma()
        .bit(flags, flag::NSF_ACTIVE).comma()
        .bit(flags, flag::NSF_ARRAY).comma()
        .bit(flags, flag::ASYNC_WAIT).comma()
        .bit(flags, flag::D3D12).comma()
        .bit(flags, flag::FRAMEGEN_SWAPCHAIN).comma()
        .bit(flags, flag::DLSSG).comma()
        .bit(flags, flag::MENU).comma()
        .bit(flags, flag::HMD).comma()
        .bit(flags, flag::FOCUSED).comma()
        .bit(flags, flag::FOREGROUND).comma()
        .bit(flags, flag::MONO).comma()
        .u(flag::sync_stage(flags)).comma()
        .u(flag::method(flags)).comma()
        .known(w.pair_qpc_ns).comma()
        .known(w.pair_xr_time).comma()
        .u(delta(t.frames_dropped, p.frames_dropped)).comma()
        .u(w.rows_unwritten)
        .end_line();

    write_all(g_summary_file, w.text, static_cast<size_t>(row.pos() - w.text));
    w.totals = t;
    w.window_start_ns = now;
    w.rows_unwritten = 0;
}

// Starts a fresh window, e.g. after VR_PerfLog was off, so the next row doesn't cover the gap.
void reset_window(WriterState& w, int64_t now) noexcept {
    auto& c = g_counters;
    constexpr auto relaxed = std::memory_order_relaxed;
    w.totals = snapshot_totals();
    w.frame_totals = snapshot_frame_totals();
    w.window_start_ns = now;
    w.last_new_qpc_ns = 0;
    w.interval_count = 0;
    w.rows_unwritten = 0;

    for (auto* slot : {&c.wait_max_ns, &c.end_max_ns, &c.period_max_ns, &c.present_max_ns, &c.uevr_rt_max_ns, &c.tick_max_ns, &c.tick_gap_max_ns}) {
        slot->store(0, relaxed);
    }
    c.callsite_mask.store(0, relaxed);
}

void write_once(WriterState& w, int64_t now) noexcept {
    int64_t pair_qpc_ns{};
    int64_t pair_xr_time{};
    const auto count = drain_frames(w.drained, pair_qpc_ns, pair_xr_time);
    const auto active = enabled();

    if (pair_qpc_ns != 0) {
        w.pair_qpc_ns = pair_qpc_ns;
        w.pair_xr_time = pair_xr_time;
    }

    if (!active) {
        w.was_enabled = false;
        return;
    }

    if (!w.was_enabled) {
        // Records drained here were pushed before the reset point and would report negative deltas.
        reset_window(w, now);
        w.was_enabled = true;
        return;
    }

    write_frame_rows(w, count);
    write_summary_row(w, now);
}

// The stop event is the thread's parameter, so an abandoned writer (see stop()) still owns a valid one.
DWORD WINAPI writer_main(void* stop_event) {
    SetThreadDescription(GetCurrentThread(), L"UEVR Perf Log");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    auto& w = g_writer;
    auto next = now_ns() + WRITE_INTERVAL_NS;

    while (true) {
        const auto wait_ms = static_cast<DWORD>(std::max<int64_t>(0, (next - now_ns() + 999'999) / 1'000'000));

        if (WaitForSingleObject(static_cast<HANDLE>(stop_event), wait_ms) != WAIT_TIMEOUT) {
            break;
        }

        const auto now = now_ns();
        write_once(w, now);

        // Fixed schedule, but after a long stall start over rather than writing a burst of catch-up rows.
        next += WRITE_INTERVAL_NS;
        if (next <= now) {
            next = now + WRITE_INTERVAL_NS;
        }
    }

    // The last partial second.
    write_once(w, now_ns());
    return 0;
}
}

int64_t qpc_ticks_to_ns(int64_t ticks) noexcept {
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return 0;
    }

    // Same scaling as MSVC's steady_clock (and Python's perf_counter_ns), so the result is on now_ns()'s clock.
    const auto f = frequency.QuadPart;
    return (ticks / f) * 1'000'000'000LL + (ticks % f) * 1'000'000'000LL / f;
}

void push_frame(const FrameRecord& record) noexcept {
    if (!TryAcquireSRWLockExclusive(&g_frames_lock)) {
        g_frames_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (g_frame_count < FRAME_CAPACITY) {
        g_frames[g_frame_count++] = record;
    } else {
        g_frames_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    ReleaseSRWLockExclusive(&g_frames_lock);
}

bool time_pair_due(int64_t now) noexcept {
    return enabled() && g_running.load(std::memory_order_relaxed) && now >= g_next_time_pair_ns.load(std::memory_order_relaxed);
}

void note_time_pair(int64_t qpc_ticks, int64_t xr_time) noexcept {
    const auto qpc_ns = qpc_ticks_to_ns(qpc_ticks);

    if (!TryAcquireSRWLockExclusive(&g_frames_lock)) {
        return; // the writer is draining; the next frame tries again
    }

    g_pair_qpc_ns = qpc_ns;
    g_pair_xr_time = xr_time;
    ReleaseSRWLockExclusive(&g_frames_lock);
    g_next_time_pair_ns.store(qpc_ns + TIME_PAIR_INTERVAL_NS, std::memory_order_relaxed);
}

uint32_t start(const std::filesystem::path& dir) noexcept try {
    AcquireSRWLockExclusive(&g_control_lock);
    struct Unlock {
        ~Unlock() { ReleaseSRWLockExclusive(&g_control_lock); }
    } unlock{};

    if (g_running.load()) {
        return ERROR_SUCCESS;
    }

    if (g_abandoned) {
        return ERROR_BUSY;
    }

    DWORD error{ERROR_SUCCESS};
    g_summary_file = create_log_file(dir, SUMMARY_FILE, SUMMARY_HEADER, error);
    if (g_summary_file != INVALID_HANDLE_VALUE) {
        g_frames_file = create_log_file(dir, FRAMES_FILE, FRAMES_HEADER, error);
    }

    if (g_summary_file == INVALID_HANDLE_VALUE || g_frames_file == INVALID_HANDLE_VALUE) {
        close_files();
        return error != ERROR_SUCCESS ? error : ERROR_OPEN_FAILED;
    }

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        error = GetLastError();
        close_files();
        return error;
    }

    // Rows start from here: drop anything pushed before the files existed.
    {
        int64_t unused_qpc{};
        int64_t unused_xr{};
        drain_frames(g_writer.drained, unused_qpc, unused_xr);
    }
    reset_window(g_writer, now_ns());
    g_writer.was_enabled = true;
    g_writer.frames_bytes = 0;
    g_writer.frames_full = false;
    g_next_time_pair_ns.store(0, std::memory_order_relaxed);

    g_thread = CreateThread(nullptr, 0, writer_main, g_stop_event, 0, nullptr);
    if (g_thread == nullptr) {
        error = GetLastError();
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        close_files();
        return error;
    }

    g_running.store(true);
    return ERROR_SUCCESS;
} catch (...) {
    return ERROR_INVALID_FUNCTION;
}

void stop() noexcept {
    AcquireSRWLockExclusive(&g_control_lock);

    if (g_running.exchange(false)) {
        SetEvent(g_stop_event);

        // Bounded: under the loader lock (FreeLibrary) the thread can't finish exiting even once it has left this
        // module's code, so give up instead of deadlocking, and leave its stop event and files to it.
        if (WaitForSingleObject(g_thread, STOP_WAIT_MS) == WAIT_OBJECT_0) {
            CloseHandle(g_stop_event);
            close_files();
        } else {
            g_abandoned = true;
        }

        CloseHandle(g_thread);
        g_thread = nullptr;
        g_stop_event = nullptr;
    }

    ReleaseSRWLockExclusive(&g_control_lock);
}

bool is_running() noexcept {
    return g_running.load(std::memory_order_relaxed);
}
}
