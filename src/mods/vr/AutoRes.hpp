#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include <dxgi1_4.h>
#include <wrl.h>

#include "AutoResGpu.hpp"
#include "AutoResPolicy.hpp"

class CVarManager;

namespace runtimes {
struct OpenXR;
}

namespace uevr::autores {
// The engine's frame rate settings (UEngine's reflected properties), read once on the game thread. A property the
// engine doesn't have stays empty.
struct EngineFrameRate {
    std::optional<bool> fixed{};      // bUseFixedFrameRate
    std::optional<float> fixed_fps{}; // FixedFrameRate
    std::optional<bool> smooth{};     // bSmoothFrameRate
    std::optional<float> smooth_min{}, smooth_max{}; // SmoothedFrameRateRange's bounds, when bounded
};

// VR_AutoResFrameBoundDetect's clock (AutoResPolicy.hpp FrameBoundJudge): the game thread adds each engine tick's
// DeltaSeconds and the wall time since the tick before; the present thread reads the sums once a second. A pair after a
// gap over 250 ms (a stall or a load) is left out of both.
class TickClock {
public:
    void on_tick(float delta) noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto gap = now - m_last;
        const auto first = m_last.time_since_epoch().count() == 0;
        m_last = now;

        if (first || gap > std::chrono::milliseconds{250} || !(delta >= 0.0f) || delta > 0.25f) {
            return;
        }

        m_delta_us.fetch_add((uint64_t)(delta * 1e6f), std::memory_order_relaxed);
        m_real_us.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(gap).count(), std::memory_order_relaxed);
    }

    uint64_t delta_us() const noexcept { return m_delta_us.load(std::memory_order_relaxed); }
    uint64_t real_us() const noexcept { return m_real_us.load(std::memory_order_relaxed); }

private:
    std::chrono::steady_clock::time_point m_last{}; // the game thread's only
    std::atomic<uint64_t> m_delta_us{0};
    std::atomic<uint64_t> m_real_us{0};
};
}

// VR_AutoRes: automatic resolution on the present thread. After each consume_events it takes the runtime's
// XR_EXT_performance_settings edges, keeps the app's own guards and moves one actuator per edge (AutoResPolicy.hpp):
// CVarManager's SP and upscaler steps on the game thread, the eye size through OpenXR's live rebuild. What the game
// supports is found out as it runs: SP is left out while DLSS sets the input resolution and when the game keeps
// putting its own value back; the upscaler step needs a DLSS plugin with a mode that is on (found any time in the
// session); an SP or upscaler step that leaves the GPU time per frame about the same, or makes the game slower, is put
// back and its actuator left out for the session; the level needs a live rebuild.
class AutoResController {
public:
    struct Inputs {
        bool enabled{false};            // VR_AutoRes
        int toggle_seconds{0};          // VR_AutoResToggleSeconds: synthetic GPU edges, for measurements
        int toggle_delay_seconds{60};   // VR_AutoResToggleDelaySeconds: from this long after the start
        int level_interval_seconds{60}; // VR_AutoResLevelIntervalSeconds
        bool loading{false};            // the engine or the Presents stopped in the last seconds
        bool frame_generation{false};   // the game presents more often than its engine ticks
        bool level_possible{true};      // the eye size follows OpenXR (not the 2D screen or extreme compatibility mode)
        bool frame_bound_detect{false}; // VR_AutoResFrameBoundDetect
        uint64_t tick_delta_us{0};      // TickClock's sums
        uint64_t tick_real_us{0};
        const uevr::autores::EngineFrameRate* engine_frame_rate{nullptr}; // once the game thread has read it
        int upscaler_floor{0};          // VR_AutoResUpscalerFloor (uevr::autores::UpscalerFloor)
        bool output_anchor{false};      // VR_AutoResOutputAnchor
        bool internal_recover{false};   // VR_AutoResInternalRecover
        int test_level{-1};             // VR_AutoResTestLevel: a LEVELS index, or -1
        int test_internal{-1};          // VR_AutoResTestInternal: 0 the game's own, 1 the floor, 2 Ultra Performance, or -1
    };

    void on_present(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs);

    bool is_active() const { return m_active; }

    // "Auto Base  SP 85%  DLSS -1" for the resolution label; empty while inactive.
    std::string describe() const;

private:
    void activate(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs);
    void deactivate(runtimes::OpenXR& openxr, CVarManager& cvars);
    void take_edge(uint32_t domain, uint32_t sub_domain, uint32_t from, uint32_t to, const char* source);
    bool carry_out(runtimes::OpenXR& openxr, CVarManager& cvars, const uevr::autores::Step& step, const char* why);
    void step_down_now(runtimes::OpenXR& openxr, CVarManager& cvars, int count, const char* why);
    void check_guards(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs);
    void sync_available(CVarManager& cvars);
    void check_step(CVarManager& cvars, const Inputs& inputs);
    void check_anchor(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs);
    void check_recovery(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs);
    void apply_test_lock(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs);
    void set_internal(CVarManager& cvars, int upscaler, int sp);
    void check_frame_bound(const Inputs& inputs);
    void send_hints(runtimes::OpenXR& openxr, const Inputs& inputs);
    std::optional<float> query_vram_percent();
    void publish();

    using clock = std::chrono::steady_clock;

    bool m_active{false};
    const void* m_openxr{nullptr}; // the OpenXR object and session the state belongs to
    uint64_t m_session{0};
    uevr::autores::Size m_base{};  // the runtime's recommended eye size: the ladder's Base
    bool m_widen{true};            // Mopic's 3D display: up-steps widen the eye only
    uevr::autores::State m_state{};
    uevr::autores::Available m_available{};
    uevr::autores::Request m_pending{uevr::autores::Request::none};
    const char* m_pending_why{""};
    bool m_waiting_logged{false};
    bool m_no_extension_logged{false}; // VR_AutoRes on in a session without XR_EXT_performance_settings: said once
    // The last edge's request, asked again while its state lasts: the extension only reports changes, so a GPU that
    // stays IMPAIRED after a step sends nothing more.
    uevr::autores::Request m_last_request{uevr::autores::Request::none};
    bool m_on_ladder{false}; // the eye size is a ladder level (not the slider's size)
    uint32_t m_last_event{0};
    uint32_t m_slider_applies{0};
    int m_sp_percent{100};
    int m_upscaler_steps{0};

    clock::time_point m_activated_at{};
    clock::time_point m_last_action{}; // the last edge or step
    clock::time_point m_last_level_change{};
    clock::time_point m_last_guard_step{};
    clock::time_point m_next_guard_check{};
    clock::time_point m_next_toggle{};
    clock::time_point m_next_hint{};
    bool m_toggle_down{true};

    // The guards' inputs: VR frames per second over a 3 s window, GPU memory over 3 s.
    clock::time_point m_fps_window_start{};
    uint64_t m_fps_window_frames{0};
    std::optional<float> m_fps{};
    int m_vram_over_seconds{0};
    std::optional<float> m_vram_percent{};
    Microsoft::WRL::ComPtr<IDXGIAdapter3> m_adapter{};
    clock::time_point m_next_adapter_lookup{};

    // An SP or upscaler step down is checked against the 3 s before it (check_step): the GPU time per frame 2-5 s
    // after it (AutoResPolicy.hpp judge_step), and the frame rate (10 % slower leaves the actuator out).
    struct StepCheck {
        uevr::autores::Actuator actuator{};
        int to{};          // the step it measures; any other step cancels it
        clock::time_point at{};
        float fps_before{};
        std::optional<float> gpu_ms_before{};
        clock::time_point window_start{};
        uint64_t window_frames{};
        bool loading{false};
    };
    std::optional<StepCheck> m_step_check{};
    // Left out by the step check, for the session; CVarManager's report can take an actuator out (and back) any time.
    bool m_sp_left_out{false};
    bool m_upscaler_left_out{false};
    // A step past the first that moved nothing: its actuator stops at the step before it, which worked, for the session
    // (-1: no cap).
    int m_sp_cap{-1};
    int m_upscaler_cap{-1};
    // The verdicts against each in a row (AutoResPolicy.hpp StepStrikes), and when a step put back may be taken again.
    uevr::autores::StepStrikes m_sp_strikes{};
    uevr::autores::StepStrikes m_upscaler_strikes{};
    clock::time_point m_retake_after{};
    uevr::autores::GpuBusySampler m_gpu{};

    // VR_AutoResOutputAnchor: decided once a session; raised from m_anchor_from to Base until the VRAM guard trips.
    bool m_anchor_decided{false};
    bool m_anchor_raised{false};
    int m_anchor_from{uevr::autores::MIN_LEVEL};
    // When it is decided: after the loading, on the GPU memory of the 30 s after it.
    uevr::autores::AnchorGate m_anchor_gate{};
    bool m_anchor_due{false};
    // VR_AutoResInternalRecover, and the step up it took (undone when IMPAIRED follows).
    uevr::autores::InternalRecovery m_recovery{};
    std::optional<uevr::autores::Step> m_recovery_step{};
    // VR_AutoResTestLevel / VR_AutoResTestInternal: nothing else moves.
    bool m_test{false};
    bool m_test_level_done{false};
    clock::time_point m_next_test{};
    clock::time_point m_next_gpu_log{};

    bool m_frame_generation_hinted{false};

    // VR_AutoResFrameBoundDetect: a second's sums, the last 4 for a 3 s window, and the judgement.
    struct TickSample {
        clock::time_point at{};
        uint64_t delta_us{}, real_us{}, frames{};
        bool loading{false};
    };
    std::deque<TickSample> m_tick_samples{};
    uevr::autores::FrameBoundJudge m_frame_bound{};
    std::optional<float> m_time_ratio{};
    float m_time_ratio_fps{0.0f};
    bool m_engine_frame_rate_seen{false};
    bool m_gpu_hinted{false};
    clock::time_point m_next_frame_bound_check{};
    clock::time_point m_next_frame_bound_log{};
};
