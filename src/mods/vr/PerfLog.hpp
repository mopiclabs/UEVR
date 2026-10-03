#pragma once

// Always-on frame-rate log (VR_PerfLog, on by default): perf.csv and perf-frames.csv next to log.txt, read by
// tools/mopic-test/perfreport.py (which looks columns up by name; the first line of each file is its header).
//
// The VR frame rate is the rate of xrEndFrame calls that carry a projection layer for a new engine frame
// ("P" submits): one newer than every engine frame submitted before. Submits of an engine frame that was already
// submitted (R: the same one again, or an older one, as the second Present of each engine frame under 2x frame
// generation resubmits), empty recovery/transition frames (E) and failed submits (F) are counted separately, so a
// runtime-paced loop that keeps resubmitting the same image does not read as full rate. Everything else here (xrWaitFrame, Present, engine Tick, UEVR's own cost, Native Stereo Fix
// capture reuse, mode flags) is there to explain that number.
//
// Frame paths only do relaxed atomic adds and steady_clock reads, plus one record pushed per xrEndFrame under a
// try-lock (a contended push is dropped and counted, never waited for). PerfLog.cpp's writer thread turns them
// into CSV rows once per second. Nothing here allocates or logs.
//
// perf-frames.csv, one row per xrEndFrame (counts and times are since the previous row of any kind):
//   qpc_ns              when xrEndFrame returned (steady_clock = QPC time in ns)
//   kind                P an engine frame newer than all submitted before, R one already submitted (the newest
//                       again or an older one), E no projection layer, F xrEndFrame failed
//   frame               engine frame of the submitted pose (0 for E rows)
//   display_time        the XrTime submitted; perf.csv's xr_pair_time/xr_pair_qpc_ns pair maps it onto qpc_ns
//   period_ns           predictedDisplayPeriod of the submitted frame state (how the runtime paces the game)
//   end_us, wait_us     time inside this xrEndFrame / inside xrWaitFrame (any thread)
//   callsite            sync_frame_callsite_name() of the last successful xrWaitFrame
//   nsf                 Native Stereo Fix right eye: 0 none, 1 this frame's capture, 2 the last capture reused,
//                       3 the flat backbuffer while the fix is active
//   nsf_pair            where a Native Stereo Fix submit's eye pair was copied from (D3D12): 0 no eye-pair copy,
//                       1 the two live engine targets, 2 the pair frozen at an engine frame boundary
//                       (VR_NativeStereoFixPairSnapshot)
//   flags               mode bits, sync stage and rendering method, see flag::
//   presents, dup_presents, present_us          Present hook passes, passes without a new engine frame, time in
//                                               the original Present
//   ticks, ticks_skipped, tick_us               UGameEngine::Tick hook passes, passes that skipped the original
//                                               Tick (synced SKIP_TICK), time in the original Tick
//   uevr_gt_us          UEVR's own game-thread time in the Tick hook (not the original Tick, not the queued
//                       game-thread jobs, not time blocked in OpenXR or on fences)
//   uevr_rt_us          UEVR's own time in the Present hook: outside the original Present and without the time it
//                       was blocked in OpenXR calls (xrWaitFrame/xrBeginFrame/xrEndFrame, swapchain-image waits)
//                       and D3D12 fence waits, which is rt_blocked_us
//
// perf.csv, one row per second on a fixed schedule, also while no frame arrives (xr_frames 0 = a freeze).
// xr_older counts the R submits that carried an engine frame older than the newest one submitted; presents_per_tick
// is presents / (ticks + ticks_skipped) of the second (2.00 with 2x frame generation, 1.00 without, also in synced
// sequential; empty without a Tick hook pass);
// nsf_live / nsf_snapshot count the Native Stereo Fix eye-pair copies by source (nsf_pair).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>

namespace uevr::perf {
// steady_clock is QueryPerformanceCounter on MSVC, scaled to ns the same way as Python's time.perf_counter_ns(),
// so the qpc_ns columns line up with the harness and with QPC-stamped PresentMon/monado captures.
inline int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// QueryPerformanceCounter ticks -> the now_ns() time base.
int64_t qpc_ticks_to_ns(int64_t ticks) noexcept;

enum class Submit : uint8_t { New = 'P', Repeat = 'R', Empty = 'E', Failed = 'F' };

// How a Native Stereo Fix submit got its right eye: this frame's capture, the last one again (D3D12 reuses it for
// up to 500 ms when the packet is missing), or the flat backbuffer while the fix is active.
enum class NativeStereo : uint8_t { None = 0, Fresh = 1, Reused = 2, Fallback = 3 };

// Where a Native Stereo Fix submit's two eyes came from (D3D12 double-wide copy): the two live engine targets, read
// whenever the Present pass runs, or one pair frozen at an engine frame boundary (D3D12Component's pair snapshot).
enum class NativePair : uint8_t { None = 0, Live = 1, Snapshot = 2 };

// An engine frame up to this many frames older than the newest one submitted is a resubmit of an older frame (R);
// further back, the counter restarted (a new OpenXR session) and the frame counts as new.
constexpr uint32_t OLDER_FRAME_WINDOW = 120;

enum class FrameOrder : uint8_t { Unknown, Newer, Same, Older, Restart };

// `frame` against the newest engine frame submitted so far (0 = none yet), wrap-safe. Frame 0 is unknown.
constexpr FrameOrder order_frame(uint32_t frame, uint32_t newest) noexcept {
    if (frame == 0) {
        return FrameOrder::Unknown;
    }

    if (newest == 0) {
        return FrameOrder::Newer;
    }

    const auto ahead = static_cast<int32_t>(frame - newest);
    if (ahead > 0) {
        return FrameOrder::Newer;
    }

    if (ahead == 0) {
        return FrameOrder::Same;
    }

    return static_cast<uint32_t>(-static_cast<int64_t>(ahead)) <= OLDER_FRAME_WINDOW ? FrameOrder::Older : FrameOrder::Restart;
}

static_assert(order_frame(10, 0) == FrameOrder::Newer && order_frame(11, 10) == FrameOrder::Newer);
static_assert(order_frame(10, 10) == FrameOrder::Same && order_frame(9, 10) == FrameOrder::Older);
static_assert(order_frame(10, 10 + OLDER_FRAME_WINDOW) == FrameOrder::Older);
static_assert(order_frame(9, 10 + OLDER_FRAME_WINDOW) == FrameOrder::Restart && order_frame(0, 10) == FrameOrder::Unknown);
static_assert(order_frame(2, 0xFFFFFFFEu) == FrameOrder::Newer && order_frame(0xFFFFFFFEu, 2) == FrameOrder::Older);

// The "flags" columns, refreshed by VR::update_perf_state() on every Present: mode bits 0-14, the sync stage in
// bits 16-17 and the rendering method in bits 20-23. tools/mopic-test/perfreport.py decodes them with the same
// numbers (FLAG_BITS, SYNC_STAGES, METHODS), so change both together.
namespace flag {
constexpr uint32_t AFR = 1u << 0;                // alternating or synchronized sequential AFR
constexpr uint32_t SYNCED = 1u << 1;             // synchronized sequential AFR
constexpr uint32_t SKIP_DRAW = 1u << 2;          // synced sequential method SKIP_DRAW (SYNCED without it = SKIP_TICK)
constexpr uint32_t NSF = 1u << 3;                // Native Stereo Fix enabled
constexpr uint32_t NSF_ACTIVE = 1u << 4;         // Native Stereo Fix currently operating
constexpr uint32_t NSF_ARRAY = 1u << 5;          // Native Stereo Fix texture-array submit
constexpr uint32_t ASYNC_WAIT = 1u << 6;         // xrWaitFrame runs on the "UEVR Native OpenXR Wait" thread
constexpr uint32_t D3D12 = 1u << 7;
constexpr uint32_t FRAMEGEN_SWAPCHAIN = 1u << 8; // UEVR hooked Streamline's frame-generation swapchain
constexpr uint32_t DLSSG = 1u << 9;              // nvngx_dlssg.dll is loaded (DLSS Frame Generation available, not
                                                 // necessarily generating frames; left as the game set it)
constexpr uint32_t MENU = 1u << 10;              // UEVR menu open
constexpr uint32_t HMD = 1u << 11;               // headset active
constexpr uint32_t FOCUSED = 1u << 12;           // OpenXR session FOCUSED
constexpr uint32_t FOREGROUND = 1u << 13;        // the game owns the Windows foreground window
constexpr uint32_t MONO = 1u << 14;              // Mono rendering method

// The mode bits by name, bit 0 first: perf.csv's decoded columns after "flags" (PerfLog.cpp SUMMARY_HEADER).
constexpr const char* NAMES =
    "afr,synced,skip_draw,nsf,nsf_active,nsf_array,async_wait,d3d12,framegen_sc,dlssg,menu,hmd,focused,foreground,mono";

constexpr uint32_t SYNC_STAGE_SHIFT = 16;        // VR::SynchronizeStage, 2 bits
constexpr uint32_t SYNC_STAGE_MASK = 0x3;
constexpr uint32_t METHOD_SHIFT = 20;            // VR::RenderingMethod, 4 bits
constexpr uint32_t METHOD_MASK = 0xF;

constexpr uint32_t pack(uint32_t bits, uint32_t sync_stage, uint32_t method) noexcept {
    return bits | ((sync_stage & SYNC_STAGE_MASK) << SYNC_STAGE_SHIFT) | ((method & METHOD_MASK) << METHOD_SHIFT);
}

constexpr uint32_t sync_stage(uint32_t flags) noexcept {
    return (flags >> SYNC_STAGE_SHIFT) & SYNC_STAGE_MASK;
}

constexpr uint32_t method(uint32_t flags) noexcept {
    return (flags >> METHOD_SHIFT) & METHOD_MASK;
}
}

// Running totals. The writer thread reports differences between snapshots, so producers never reset anything
// except the *_max fields and callsite_mask, which the writer exchanges to zero once per second.
struct Counters {
    // xrEndFrame, by kind. xr_older = the R submits whose engine frame is older than newest_projection_frame.
    std::atomic<uint64_t> xr_new{}, xr_repeat{}, xr_empty{}, xr_fail{}, xr_older{};
    std::atomic<uint64_t> end_calls{}, end_ns{};
    std::atomic<int64_t> end_max_ns{};
    std::atomic<uint32_t> newest_projection_frame{};

    // xrWaitFrame. wait_skip = synchronize_frame() found the frame already synchronized.
    std::atomic<uint64_t> wait_calls{}, wait_ok{}, wait_skip{}, wait_ns{}, no_render{}, discarded{};
    std::atomic<int64_t> wait_max_ns{}, period_ns{}, period_max_ns{};
    std::atomic<const char*> last_callsite_name{nullptr}; // a string literal (sync_frame_callsite_name)
    std::atomic<uint32_t> callsite_mask{}; // bit n = VRRuntime::SyncFrameCallsite n waited successfully

    // Present hook. uevr_rt = UEVR's own time in the hook: outside the original Present and without rt_blocked,
    // the time it spent blocked in OpenXR calls and D3D12 fence waits.
    std::atomic<uint64_t> presents{}, present_ns{}, dup_presents{}, uevr_rt_ns{}, rt_blocked_ns{};
    std::atomic<int64_t> present_max_ns{}, uevr_rt_max_ns{};

    // UGameEngine::Tick hook. uevr_gt = UEVR's own time in the hook outside the original Tick, the queued
    // game-thread jobs (gt_jobs, which include the synced-sequential second-eye redraw) and blocked time.
    std::atomic<uint64_t> ticks{}, ticks_skipped{}, tick_ns{}, uevr_gt_ns{}, gt_jobs_ns{}, engine_delta_us{};
    std::atomic<int64_t> tick_max_ns{}, tick_gap_max_ns{}, last_tick_ns{};

    std::atomic<uint64_t> fence_waits{}, fence_wait_ns{};
    std::atomic<uint64_t> nsf_fresh{}, nsf_reused{}, nsf_fallback{};
    std::atomic<uint64_t> nsf_live{}, nsf_snapshot{};
    std::atomic<uint8_t> pending_nsf{}, pending_pair{};
    std::atomic<uint32_t> flags{}; // flag::pack()
};

// Running totals copied into each per-frame record, so every perf-frames.csv row can say what happened since
// the previous row (any kind).
struct FrameTotals {
    uint64_t presents{}, dup_presents{}, present_ns{}, ticks{}, ticks_skipped{}, tick_ns{};
    uint64_t uevr_gt_ns{}, uevr_rt_ns{}, rt_blocked_ns{}, wait_ns{};
};

struct FrameRecord {
    int64_t qpc_ns{};       // xrEndFrame returned
    int64_t display_time{}; // XrTime submitted
    int64_t period_ns{};    // predictedDisplayPeriod of the submitted frame state
    int64_t end_ns{};       // time inside xrEndFrame
    const char* callsite{}; // last successful xrWaitFrame callsite (a string literal), null before the first
    uint32_t frame{};       // engine frame of the submitted pose (0 for empty frames)
    uint32_t flags{};
    Submit kind{Submit::Empty};
    NativeStereo nsf{NativeStereo::None};
    NativePair pair{NativePair::None};
    FrameTotals totals{};
};

inline std::atomic_bool g_enabled{false};
inline Counters g_counters{};

// Time this thread spent blocked in the runtime or on the GPU. The Present and Tick timers subtract it, so
// "UEVR's own cost" is CPU work rather than waiting.
inline thread_local int64_t t_blocked_ns{};

inline bool enabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}

inline void set_enabled(bool value) noexcept {
    g_enabled.store(value, std::memory_order_relaxed);
}

// `flags` = flag::pack(mode bits, sync stage, rendering method).
inline void set_state(uint32_t flags) noexcept {
    g_counters.flags.store(flags, std::memory_order_relaxed);
}

inline void add(std::atomic<uint64_t>& counter, uint64_t value = 1) noexcept {
    counter.fetch_add(value, std::memory_order_relaxed);
}

inline void update_max(std::atomic<int64_t>& slot, int64_t value) noexcept {
    auto current = slot.load(std::memory_order_relaxed);
    while (value > current && !slot.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

inline uint64_t non_negative(int64_t value) noexcept {
    return value > 0 ? static_cast<uint64_t>(value) : 0;
}

inline void add_blocked_ns(int64_t ns) noexcept {
    t_blocked_ns += ns > 0 ? ns : 0;
}

inline FrameTotals snapshot_frame_totals() noexcept {
    const auto& c = g_counters;
    constexpr auto relaxed = std::memory_order_relaxed;
    return FrameTotals{
        c.presents.load(relaxed), c.dup_presents.load(relaxed), c.present_ns.load(relaxed),
        c.ticks.load(relaxed), c.ticks_skipped.load(relaxed), c.tick_ns.load(relaxed),
        c.uevr_gt_ns.load(relaxed), c.uevr_rt_ns.load(relaxed), c.rt_blocked_ns.load(relaxed), c.wait_ns.load(relaxed)};
}

// Pushes one perf-frames.csv record. Never blocks: a push that meets the writer draining the buffer is dropped
// and counted in perf.csv's frames_dropped.
void push_frame(const FrameRecord& record) noexcept;

// Called from a successful xrEndFrame when XR_KHR_win32_convert_performance_counter_time is available: once a
// second, a (QPC, XrTime) pair read at the same moment goes into perf.csv, so XrTime display times can be
// placed on the qpc_ns clock (display_qpc_ns = display_time - xr_pair_time + xr_pair_qpc_ns).
bool time_pair_due(int64_t now) noexcept;
void note_time_pair(int64_t qpc_ticks, int64_t xr_time) noexcept;

inline void note_xr_wait(int64_t duration_ns, bool ok, bool should_render, int64_t period_ns, uint8_t callsite, const char* callsite_name) noexcept {
    if (!enabled()) {
        return;
    }

    auto& c = g_counters;
    add_blocked_ns(duration_ns);
    add(c.wait_calls);
    add(c.wait_ns, non_negative(duration_ns));
    update_max(c.wait_max_ns, duration_ns);

    if (!ok) {
        return;
    }

    add(c.wait_ok);
    if (!should_render) {
        add(c.no_render);
    }

    c.period_ns.store(period_ns, std::memory_order_relaxed);
    update_max(c.period_max_ns, period_ns);
    c.last_callsite_name.store(callsite_name, std::memory_order_relaxed);
    if (callsite < 32) {
        c.callsite_mask.fetch_or(1u << callsite, std::memory_order_relaxed);
    }
}

inline void note_xr_wait_skipped() noexcept {
    if (enabled()) {
        add(g_counters.wait_skip);
    }
}

inline void note_xr_begin(int64_t duration_ns, bool discarded) noexcept {
    if (!enabled()) {
        return;
    }

    add_blocked_ns(duration_ns);
    if (discarded) {
        add(g_counters.discarded);
    }
}

// Every xrEndFrame. `projection` = a real projection layer was submitted (not only the Virtual Desktop dummy).
inline void note_xr_end(bool ok, bool projection, uint32_t frame, int64_t end_time_ns, int64_t duration_ns,
    int64_t display_time, int64_t period_ns) noexcept {
    if (!enabled()) {
        return;
    }

    auto& c = g_counters;
    add_blocked_ns(duration_ns);
    add(c.end_calls);
    add(c.end_ns, non_negative(duration_ns));
    update_max(c.end_max_ns, duration_ns);

    FrameRecord record{};
    record.qpc_ns = end_time_ns;
    record.display_time = display_time;
    record.period_ns = period_ns;
    record.end_ns = duration_ns;
    record.frame = projection ? frame : 0;

    if (!ok) {
        record.kind = Submit::Failed;
        add(c.xr_fail);
    } else if (!projection) {
        record.kind = Submit::Empty;
        add(c.xr_empty);
    } else {
        // New only past the newest engine frame submitted so far: frame generation's second Present of an engine
        // frame resubmits the previous one, which alternates with the new one and must not count as a new frame.
        // Frame 0 means the engine frame is unknown; count it as new rather than hide it as a repeat.
        auto newest = c.newest_projection_frame.load(std::memory_order_relaxed);
        auto order = order_frame(frame, newest);
        while ((order == FrameOrder::Newer || order == FrameOrder::Restart) &&
               !c.newest_projection_frame.compare_exchange_weak(newest, frame, std::memory_order_relaxed)) {
            order = order_frame(frame, newest);
        }

        const bool is_new = order == FrameOrder::Unknown || order == FrameOrder::Newer || order == FrameOrder::Restart;
        record.kind = is_new ? Submit::New : Submit::Repeat;
        add(is_new ? c.xr_new : c.xr_repeat);
        if (order == FrameOrder::Older) {
            add(c.xr_older);
        }
    }

    if (projection) {
        // A failed submit leaves the right-eye state to its retry (D3D12 resubmits on XR_ERROR_LAYER_INVALID).
        record.nsf = static_cast<NativeStereo>(ok
            ? c.pending_nsf.exchange(0, std::memory_order_relaxed)
            : c.pending_nsf.load(std::memory_order_relaxed));
        record.pair = static_cast<NativePair>(ok
            ? c.pending_pair.exchange(0, std::memory_order_relaxed)
            : c.pending_pair.load(std::memory_order_relaxed));
    }

    record.flags = c.flags.load(std::memory_order_relaxed);
    record.callsite = c.last_callsite_name.load(std::memory_order_relaxed);
    record.totals = snapshot_frame_totals();
    push_frame(record);
}

inline void note_native_stereo(NativeStereo kind) noexcept {
    if (!enabled()) {
        return;
    }

    auto& c = g_counters;
    switch (kind) {
    case NativeStereo::Fresh:
        add(c.nsf_fresh);
        break;
    case NativeStereo::Reused:
        add(c.nsf_reused);
        break;
    case NativeStereo::Fallback:
        add(c.nsf_fallback);
        break;
    default:
        return;
    }

    c.pending_nsf.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
}

// The D3D12 Native Stereo Fix copied this submit's eye pair from the live engine targets or from a frozen pair.
inline void note_native_pair(NativePair kind) noexcept {
    if (!enabled() || kind == NativePair::None) {
        return;
    }

    auto& c = g_counters;
    add(kind == NativePair::Snapshot ? c.nsf_snapshot : c.nsf_live);
    c.pending_pair.store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
}

inline void note_duplicate_present() noexcept {
    if (enabled()) {
        add(g_counters.dup_presents);
    }
}

// A D3D12 fence wait that actually blocked.
inline void note_fence_wait(int64_t duration_ns) noexcept {
    if (!enabled()) {
        return;
    }

    add_blocked_ns(duration_ns);
    add(g_counters.fence_waits);
    add(g_counters.fence_wait_ns, non_negative(duration_ns));
}

// Swapchain-image waits and other runtime calls that are blocked time, not UEVR work.
inline void note_blocked(int64_t duration_ns) noexcept {
    if (enabled()) {
        add_blocked_ns(duration_ns);
    }
}

// Times one pass through a D3D11/D3D12 Present hook: the original Present call, and UEVR's own work around it.
class PresentTimer {
public:
    PresentTimer() noexcept
        : m_active{enabled()} {
        if (m_active) {
            m_start = now_ns();
            m_blocked_start = t_blocked_ns;
        }
    }

    void begin_present() noexcept {
        if (m_active) {
            m_present_start = now_ns();
            m_present_blocked_start = t_blocked_ns;
        }
    }

    // Blocked time noted inside the original Present (a hook it reaches) belongs to present_us, not to UEVR.
    void end_present() noexcept {
        if (m_active) {
            m_present_ns += now_ns() - m_present_start;
            m_present_blocked += t_blocked_ns - m_present_blocked_start;
            m_presented = true;
        }
    }

    void finish() noexcept {
        if (!m_active || !m_presented) {
            return;
        }

        auto& c = g_counters;
        const auto blocked = t_blocked_ns - m_blocked_start - m_present_blocked;
        const auto own = now_ns() - m_start - m_present_ns - blocked;
        add(c.presents);
        add(c.present_ns, non_negative(m_present_ns));
        update_max(c.present_max_ns, m_present_ns);
        add(c.uevr_rt_ns, non_negative(own));
        add(c.rt_blocked_ns, non_negative(blocked));
        update_max(c.uevr_rt_max_ns, own);
    }

private:
    bool m_active{};
    bool m_presented{};
    int64_t m_start{}, m_blocked_start{}, m_present_start{}, m_present_blocked_start{}, m_present_ns{}, m_present_blocked{};
};

// Times one pass through the UGameEngine::Tick hook.
class TickTimer {
public:
    TickTimer() noexcept
        : m_active{enabled()} {
        if (m_active) {
            m_start = now_ns();
            m_blocked_start = t_blocked_ns;
        }
    }

    void begin_jobs() noexcept { begin(m_jobs); }
    void end_jobs() noexcept { end(m_jobs); }
    void begin_tick() noexcept { begin(m_tick); }
    void end_tick() noexcept { end(m_tick); }

    // The hook skipped the original Tick (synced sequential SKIP_TICK). The jobs it ran still count.
    void finish_skipped() noexcept {
        if (m_active) {
            add(g_counters.ticks_skipped);
            add(g_counters.gt_jobs_ns, non_negative(m_jobs.ns));
        }
    }

    void finish(float delta) noexcept {
        if (!m_active) {
            return;
        }

        auto& c = g_counters;
        const auto end = now_ns();
        const auto blocked = t_blocked_ns - m_blocked_start - m_jobs.blocked - m_tick.blocked;
        const auto own = end - m_start - m_jobs.ns - m_tick.ns - blocked;
        add(c.ticks);
        add(c.tick_ns, non_negative(m_tick.ns));
        update_max(c.tick_max_ns, m_tick.ns);
        add(c.gt_jobs_ns, non_negative(m_jobs.ns));
        add(c.uevr_gt_ns, non_negative(own));
        add(c.engine_delta_us, delta > 0.0f ? static_cast<uint64_t>(static_cast<double>(delta) * 1'000'000.0) : 0);

        if (const auto previous = c.last_tick_ns.exchange(m_start, std::memory_order_relaxed); previous != 0) {
            update_max(c.tick_gap_max_ns, m_start - previous);
        }
    }

private:
    struct Span {
        int64_t start{}, blocked_start{}, ns{}, blocked{};
    };

    void begin(Span& span) noexcept {
        if (m_active) {
            span.start = now_ns();
            span.blocked_start = t_blocked_ns;
        }
    }

    void end(Span& span) noexcept {
        if (m_active) {
            span.ns += now_ns() - span.start;
            span.blocked += t_blocked_ns - span.blocked_start;
        }
    }

    bool m_active{};
    int64_t m_start{}, m_blocked_start{};
    Span m_jobs{}, m_tick{};
};

// Starts the once-per-second writer: rotates perf.csv/perf-frames.csv in `dir` to *.prev.csv, creates new ones
// with their header and returns 0, or a Win32 error code (nothing is written then). Safe to call again; a running
// writer is left alone.
uint32_t start(const std::filesystem::path& dir) noexcept;

// Writes the last partial second and stops the writer. Bounded: it waits at most a quarter second for the thread,
// so it is safe from ~VR even if that runs under the loader lock. Not called when the game exits (the framework
// is leaked then and ExitProcess ends the thread wherever it is).
void stop() noexcept;

bool is_running() noexcept;
}
