#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <dxgi1_4.h>
#include <wrl.h>

#include "AutoResPolicy.hpp"

class CVarManager;

namespace runtimes {
struct OpenXR;
}

// VR_AutoRes: automatic resolution on the present thread. After each consume_events it takes the runtime's
// XR_EXT_performance_settings edges, keeps the app's own guards and moves one actuator per edge (AutoResPolicy.hpp):
// CVarManager's SP and upscaler steps on the game thread, the eye size through OpenXR's live rebuild. What the game
// supports is found out as it runs: SP is left out under DLSS, when the game keeps putting its own value back or when
// a step makes it slower; the upscaler step needs a DLSS plugin; the level needs a live rebuild.
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
    void check_sp_step(CVarManager& cvars, const Inputs& inputs);
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

    // An SP step down is checked against the frame rate before it: a game that gets slower leaves SP out.
    struct SpCheck {
        clock::time_point at{};
        int sp{};          // the step it measures; any other step cancels it
        float fps_before{};
        clock::time_point window_start{};
        uint64_t window_frames{};
        bool loading{false};
    };
    std::optional<SpCheck> m_sp_check{};

    bool m_frame_generation_hinted{false};
};
