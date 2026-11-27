#pragma once

#include <cstdint>

// Slate-thread pose handoff on UE5 when RenderTexture_RenderThread gets the frame's FRDGBuilder
// (FFakeStereoRenderingHook, Slate worker's last branch).
//
// The worker hands frame k's poses over either on the newest command of the immediate list (the RHI thread publishes
// them when it reaches that command) or directly on the render thread (enqueue_render_poses), as before the command
// path existed. Some titles call RenderTexture twice per frame with the same frame number (Dead as Disco).
//
// Only a frame whose poses a command already carries is skipped on a repeated call. Every other call behaves as
// before the command path: no command found (discovery pending, failed or off, empty list, validation failure), or a
// command rejected by hook_new_rhi_command (which enqueues directly itself), and the next call with the same number
// tries again.

namespace uevr::slate_pose_handoff {

enum class Action {
    Skip,          // this frame's poses already ride on a command
    HookCommand,   // hand them to the newest command
    EnqueueDirect, // as before the command path
};

struct State {
    uint32_t verified_frame{}; // frame whose poses a command carries, when has_verified
    bool has_verified{};
};

inline bool is_carried(const State& state, uint32_t frame) {
    return state.has_verified && frame == state.verified_frame;
}

inline Action decide(const State& state, uint32_t frame, bool have_command) {
    if (is_carried(state, frame)) {
        return Action::Skip;
    }

    return have_command ? Action::HookCommand : Action::EnqueueDirect;
}

// After HookCommand: carried = the command now carries the frame's poses (hook_new_rhi_command returned true).
inline void note_hook_result(State& state, uint32_t frame, bool carried) {
    if (carried) {
        state.verified_frame = frame;
        state.has_verified = true;
    }
}

} // namespace uevr::slate_pose_handoff
