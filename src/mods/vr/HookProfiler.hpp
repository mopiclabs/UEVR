#pragma once

// EXPERIMENT (exp/right-eye-cost, R21): where the presenting thread's time goes inside UEVR's D3D12 Present hook, on
// top of the frame profilers VR_EnableHitchDiagnostics already turns on ([D3D12][frame-profiler] for
// D3D12Component::on_frame and its UI copy / swapchain copy / OpenXR submit / spectator mirror / post-present parts,
// [OpenXR][frame-profiler] for xrWaitFrame / xrBeginFrame / xrEndFrame). Same switch, off by default: nothing is
// timed or logged unless VR_EnableHitchDiagnostics is on, and nothing is changed either way.
// Every 5 s, from the presenting thread, one line "[HookProfiler]": the whole hook, the original Present, the
// Framework parts around the mods (ImGui new frame, the mods' on_present, the ImGui draw pass, on_post_frame,
// on_post_present), each mod's on_present that took more than 0.01 ms on average, and the game window's swap chain
// (size, format, buffers, swap effect, flags) with the sync interval and Present flags of the last original Present.

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>

#include <spdlog/spdlog.h>

namespace uevr::hook_profiler {
enum Part : int { HookTotal, OriginalPresent, ImguiNewFrame, ModsPresent, ImguiDraw, ModsPostFrame, PostPresent, PartCount };

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
    UINT sync_interval{}, flags{};
    IDXGISwapChain* swap_chain{};
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

inline void add_mod(const std::string& name, double ms) { state().mods[name].add(ms); }

inline void note_present_params(IDXGISwapChain* sc, UINT sync_interval, UINT flags) {
    auto& s = state();
    s.swap_chain = sc;
    s.sync_interval = sync_interval;
    s.flags = flags;
}

// Presenting thread, after the hook's work, with the swap chain of this Present call (only that one is queried): one
// line every 5 s, then the counters start over.
inline void log_if_due(IDXGISwapChain* current) {
    if (!on()) {
        return;
    }
    auto& s = state();
    const auto now = std::chrono::steady_clock::now();
    if (s.last_log.time_since_epoch().count() == 0) {
        s.last_log = now;
        return;
    }
    if (now - s.last_log < std::chrono::seconds(5)) {
        return;
    }
    s.last_log = now;
    static const char* names[PartCount] = {"hook", "original_present", "imgui_new_frame", "mods_on_present", "imgui_draw",
        "mods_on_post_frame", "on_post_present"};
    std::string parts{};
    for (int i = 0; i < PartCount; ++i) {
        parts += fmt::format(" {} avg={:.3f}ms max={:.2f}ms n={}", names[i], s.parts[i].avg(), s.parts[i].max_ms, s.parts[i].n);
    }
    std::string mods{};
    for (const auto& [name, st] : s.mods) {
        if (st.avg() >= 0.01) {
            mods += fmt::format(" {}={:.3f}", name, st.avg());
        }
    }
    std::string sc{" swapchain=?"};
    DXGI_SWAP_CHAIN_DESC d{};
    if (current != nullptr && SUCCEEDED(current->GetDesc(&d))) {
        sc = fmt::format(" swapchain={}x{} fmt={} buffers={} swap_effect={} windowed={} sc_flags=0x{:x}", d.BufferDesc.Width,
            d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, (int)d.SwapEffect, d.Windowed, d.Flags);
    }
    SPDLOG_INFO("[HookProfiler]{}; mods on_present ms:{}{} last_sync_interval={} last_present_flags=0x{:x}", parts,
        mods.empty() ? std::string{" none above 0.01"} : mods, sc, s.sync_interval, s.flags);
    for (auto& p : s.parts) {
        p = Stat{};
    }
    s.mods.clear();
}
} // namespace uevr::hook_profiler
