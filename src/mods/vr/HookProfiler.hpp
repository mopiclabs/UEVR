#pragma once

// EXPERIMENT (exp/right-eye-cost, R21/R22): where the presenting thread's time goes inside UEVR's D3D12 Present hook, on
// top of the frame profilers VR_EnableHitchDiagnostics already turns on ([D3D12][frame-profiler] for
// D3D12Component::on_frame and its UI copy / swapchain copy / OpenXR submit / spectator mirror / post-present parts,
// [OpenXR][frame-profiler] for xrWaitFrame / xrBeginFrame / xrEndFrame). Same switch, off by default: nothing is
// timed or logged unless VR_EnableHitchDiagnostics is on, and nothing is changed either way.
// One "[HookProfiler]" line every 5 s (or, while VR_ExperimentPresentHookAB runs, one per window with its phase), from
// the presenting thread: per frame (sum / presents of the window) the whole hook, the original Present, the Framework
// parts around the mods, the eye and UI copies split into xrAcquireSwapchainImage / xrWaitSwapchainImage / command
// context wait / recording / ExecuteCommandLists, each mod's on_present above 0.01 ms, and the game window's swap
// chain (size, format, buffers, swap effect, flags) with the sync interval and Present flags of the last Present.
//
// R22, VR_ExperimentPresentHookAB = "N,present" or "N,ui" (default "" = off): in even N-second windows of QPC time,
//   present : the game window's original Present is not called (the hook returns S_OK; the window keeps its last
//             image). A watchdog turns the skip off for the rest of the run when a skip window's Present hook comes
//             more than 250 ms after the previous one (the engine waiting for a Present that didn't come).
//   ui      : the UI layer copy is skipped (the existing OpenXR debug UI-copy path; the layer keeps its last image).
// Odd windows run as usual. For sizing each part's cost only.

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>

#include <spdlog/spdlog.h>

namespace uevr::present_ab {
enum Arm : int { None = 0, Present = 1, Ui = 2 };
inline std::atomic<int> g_window_s{0};
inline std::atomic<int> g_arm{None};
inline std::atomic<bool> g_disabled{false};  // the present-skip watchdog fired
inline std::string g_last_spec{"\x01"};       // presenting thread only (a value no option has, so "" logs "off" once)

// Presenting thread (VR::on_present): parse the option when it changes.
inline void set_spec(const std::string& spec) {
    if (spec == g_last_spec) {
        return;
    }
    g_last_spec = spec;
    int n = 0;
    int arm = None;
    const auto comma = spec.find(',');
    if (comma != std::string::npos) {
        n = std::atoi(spec.substr(0, comma).c_str());
        const auto what = spec.substr(comma + 1);
        arm = what == "present" ? Present : what == "ui" ? Ui : None;
    }
    if (n < 2 || arm == None) {
        n = 0;
        arm = None;
    }
    g_window_s = n;
    g_arm = arm;
    SPDLOG_INFO("[PresentHookAB] '{}': {}", spec, arm == None ? std::string{"off"}
        : fmt::format("{} skipped in even {}-second windows", arm == Present ? "the window's original Present" : "the UI layer copy", n));
}

inline int64_t window_index() {
    const int n = g_window_s.load(std::memory_order_relaxed);
    if (n <= 0) {
        return -1;
    }
    LARGE_INTEGER q{}, f{};
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return (q.QuadPart / f.QuadPart) / n;
}

inline bool active() { return g_arm.load(std::memory_order_relaxed) != None; }

inline bool skip_window() {
    const auto w = window_index();
    return w >= 0 && w % 2 == 0;
}

inline std::atomic<bool> g_armed{false};  // 120 s in and 60 s of steady Presents seen (no hook gap over 250 ms)
inline std::atomic<int64_t> g_armed_at_ms{0};
// The skip ends 200 s after arming, so the run's exit menus (pause, quit) are drawn and presented as usual.
inline bool within_skip_span() {
    const auto at = g_armed_at_ms.load(std::memory_order_relaxed);
    const auto now = (int64_t)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return at > 0 && now - at < 200000;
}

inline bool skip_present() {
    return g_arm.load(std::memory_order_relaxed) == Present && g_armed.load(std::memory_order_relaxed) && within_skip_span() &&
        !g_disabled.load(std::memory_order_relaxed) && skip_window();
}

// Both arms start only once armed (below): skipping the UI copy in the menus froze the menu the pilot reads (R22 dpu,
// MENU_FAIL at the load list).
inline bool skip_ui() {
    return g_arm.load(std::memory_order_relaxed) == Ui && g_armed.load(std::memory_order_relaxed) && within_skip_span() && skip_window();
}

// Presenting thread, at every Present hook entry: the watchdog for the present skip (R22, second version: the first
// counted a startup stall that began in a keep window). The skip starts only after 60 s without a hook gap over 250 ms.
// Every gap over 250 ms is logged with the phase of the window it began in. Only gaps that began while Presents were
// being skipped count: two of them in the same or consecutive skip windows turn the skip off for the rest of the run.
inline void note_hook_entry() {
    static double last_ms = 0.0, steady_since = 0.0, first_ms = 0.0;
    static bool last_skipping = false;
    static int64_t last_counted_window = -10;
    const double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_arm.load(std::memory_order_relaxed) != None) {
        if (first_ms == 0.0) {
            first_ms = now;
        }
        const double gap = last_ms > 0.0 ? now - last_ms : 0.0;
        if (gap > 250.0 && g_arm.load(std::memory_order_relaxed) == Ui) {
            steady_since = now;
        } else if (gap > 250.0) {
            steady_since = now;
            const auto w = window_index();
            spdlog::info("[PresentHookAB] hook gap {:.0f} ms, began in a {} window (now window {})", gap,
                last_skipping ? "skip (Presents skipped)" : "keep / not armed", w);
            if (last_skipping) {
                if (w - last_counted_window <= 2) {
                    g_disabled = true;
                    spdlog::warn("[PresentHookAB] second hook gap begun while skipping within two windows: the present skip is off for the rest of the run");
                }
                last_counted_window = w;
            }
        } else if (steady_since == 0.0) {
            steady_since = now;
        }
        // armed: 120 s after the first Present with the option on (menus and the save load are over) and 60 s steady
        if (!g_armed.load(std::memory_order_relaxed) && steady_since > 0.0 && now - steady_since > 60000.0 && now - first_ms > 120000.0) {
            g_armed_at_ms = (int64_t)now;
            g_armed = true;
            spdlog::info("[PresentHookAB] 120 s in and 60 s of steady Presents: the {} skip runs in even windows for the next 200 s",
                g_arm.load() == Present ? "present" : "UI copy");
        }
    }
    last_ms = now;
    last_skipping = skip_present();
}
} // namespace uevr::present_ab

namespace uevr::hook_profiler {
enum Part : int {
    HookTotal, OriginalPresent, ImguiNewFrame, ModsPresent, ImguiDraw, ModsPostFrame, PostPresent,
    UiCopy, EyeAcquire, EyeWaitImage, EyeCtxWait, EyeRecord, EyeExecute, UiAcquire, UiWaitImage, UiCtxWait, UiRecord, UiExecute,
    PartCount
};

inline std::atomic<bool> g_on{false};  // set from VR::on_present each frame (VR_EnableHitchDiagnostics)

struct Stat {
    double sum_ms{}, max_ms{};
    uint64_t n{};
    void add(double ms) {
        sum_ms += ms;
        n += 1;
        if (ms > max_ms) {
            max_ms = ms;
        }
    }
    double avg() const { return n ? sum_ms / n : 0.0; }
};

struct State {
    Stat parts[PartCount]{};
    std::map<std::string, Stat> mods{};
    std::chrono::steady_clock::time_point last_log{};
    int64_t window{-2};
    uint64_t skipped_presents{}, skipped_ui{};
    UINT sync_interval{}, flags{};
};

inline State& state() {
    static State s{};
    return s;
}

inline bool on() { return g_on.load(std::memory_order_relaxed); }

inline double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A scope on the presenting thread; does nothing when the switch is off.
class Scope {
public:
    explicit Scope(Part p) : m_part{p}, m_on{on()}, m_start{m_on ? now_ms() : 0.0} {}
    ~Scope() {
        if (m_on) {
            state().parts[m_part].add(now_ms() - m_start);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Part m_part;
    bool m_on;
    double m_start;
};

// Split a copy into eye or UI parts by the swapchain kind.
inline Part copy_part(bool ui, Part eye_part) { return ui ? static_cast<Part>(eye_part + (UiAcquire - EyeAcquire)) : eye_part; }

inline void add_mod(const std::string& name, double ms) { state().mods[name].add(ms); }

inline void note_present_params(UINT sync_interval, UINT flags) {
    auto& s = state();
    s.sync_interval = sync_interval;
    s.flags = flags;
}

// Presenting thread, after the hook's work, with the swap chain of this Present call (only that one is queried): one
// line every 5 s, or at each VR_ExperimentPresentHookAB window change with the window's phase; then the counters start
// over.
inline void log_if_due(IDXGISwapChain* current) {
    if (!on()) {
        return;
    }
    auto& s = state();
    const auto now = std::chrono::steady_clock::now();
    std::string label{};
    if (present_ab::active()) {
        const auto w = present_ab::window_index();
        if (s.window == -2) {
            s.window = w;
            return;
        }
        if (w == s.window) {
            return;
        }
        label = fmt::format(" window {} ({}{}{})", s.window, s.window % 2 == 0 ? "skip" : "keep",
            present_ab::g_disabled.load() ? ", present skip off by the watchdog" : "",
            present_ab::active() && !present_ab::g_armed.load() ? ", skip not armed yet" : "");
        s.window = w;
    } else {
        if (s.last_log.time_since_epoch().count() == 0) {
            s.last_log = now;
            return;
        }
        if (now - s.last_log < std::chrono::seconds(5)) {
            return;
        }
    }
    s.last_log = now;
    static const char* names[PartCount] = {"hook", "original_present", "imgui_new_frame", "mods_on_present", "imgui_draw",
        "mods_on_post_frame", "on_post_present", "ui_copy", "eye_acquire", "eye_wait_image", "eye_ctx_wait", "eye_record",
        "eye_execute", "ui_acquire", "ui_wait_image", "ui_ctx_wait", "ui_record", "ui_execute"};
    const double frames = (double)(s.parts[HookTotal].n ? s.parts[HookTotal].n : 1);
    std::string parts{};
    for (int i = 0; i < PartCount; ++i) {
        parts += fmt::format(" {} avg={:.3f}ms max={:.2f}ms n={}", names[i], s.parts[i].sum_ms / frames, s.parts[i].max_ms, s.parts[i].n);
    }
    std::string mods{};
    for (const auto& [name, st] : s.mods) {
        if (st.sum_ms / frames >= 0.01) {
            mods += fmt::format(" {}={:.3f}", name, st.sum_ms / frames);
        }
    }
    std::string sc{" swapchain=?"};
    DXGI_SWAP_CHAIN_DESC d{};
    if (current != nullptr && SUCCEEDED(current->GetDesc(&d))) {
        sc = fmt::format(" swapchain={}x{} fmt={} buffers={} swap_effect={} windowed={} sc_flags=0x{:x}", d.BufferDesc.Width,
            d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, (int)d.SwapEffect, d.Windowed, d.Flags);
    }
    SPDLOG_INFO("[HookProfiler]{} (per frame, {} presents){}; skipped presents {}, skipped UI copies {}; mods on_present ms:{}{} last_sync_interval={} last_present_flags=0x{:x}",
        label, (uint64_t)frames, parts, s.skipped_presents, s.skipped_ui, mods.empty() ? std::string{" none above 0.01"} : mods, sc,
        s.sync_interval, s.flags);
    for (auto& p : s.parts) {
        p = Stat{};
    }
    s.mods.clear();
    s.skipped_presents = s.skipped_ui = 0;
}
} // namespace uevr::hook_profiler
