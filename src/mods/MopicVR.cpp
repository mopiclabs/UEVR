// Mopic: Present-path code of class VR that UEVR upstream does not have, moved out of VR::on_present and
// VR::on_post_present so a joeyhodge release that rewrites those meets one call line in each (MOPIC.md, "Updating to
// a new joeyhodge release").

#include <chrono>

#include <windows.h>

#include <spdlog/spdlog.h>

#include "Framework.hpp"
#include "utility/Logging.hpp"
#include "VR.hpp"

bool VR::handle_session_ended_for_exit(VRRuntime* runtime) {
    // The session was destroyed because the game's window was gone and it had stopped presenting
    // (end_openxr_session_for_exit). The frame work below would touch the destroyed swapchains, so it is skipped.
    // A Present to a live window is a recreated window, not an exit: take the wants_reinitialize path right away.
    // One to the destroyed window is the game still shutting down: stay ended rather than create a new instance and
    // session (and bring the runtime's window back) in a process that is exiting.
    if (runtime->is_openxr() && m_openxr->session_ended_for_exit) {
        // Recorded by Framework::on_frame_d3d11/d3d12 for this Present; m_wnd when the swap chain has no window.
        const auto last_present_wnd = g_framework->get_last_present_window();
        const auto present_wnd = last_present_wnd != nullptr ? last_present_wnd : g_framework->get_window();

        if (present_wnd != nullptr && !IsWindow(present_wnd)) {
            SPDLOG_INFO_EVERY_N_SEC(5, "[VR] The game presents to its destroyed window {:x} after its OpenXR session was ended for exit; keeping the session ended",
                (uintptr_t)present_wnd);
            return true;
        }

        std::scoped_lock __{m_reinitialize_mtx};

        spdlog::info("[VR] The game presents again (window {:x}) after its OpenXR session was ended for exit; reinitializing OpenXR",
            (uintptr_t)present_wnd);
        m_openxr->wants_reinitialize = false;
        reinitialize_openxr();
        return true;
    }

    return false;
}

// VR_SubmitOncePerEngineFrame: a new Present pass has skipped nothing yet, and when the engine frame counter last
// moved (see should_skip_duplicate_present).
void VR::begin_duplicate_present_check() {
    m_skipped_duplicate_present = false;

    if (m_frame_count != m_last_advanced_frame_count) {
        m_last_advanced_frame_count = m_frame_count;
        m_last_engine_frame_advance = std::chrono::steady_clock::now();
    }
}
