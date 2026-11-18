#pragma once

// Mopic VR_PerfLog: timing of OpenXR calls that leaves upstream's call lines as they are. A probe is declared as a
// local object named after the OpenXR function it times, ahead of upstream's call, and shadows that function for the
// rest of its block: the call line stays upstream's (`auto result = xrEndFrame(this->session, &frame_end_info);`) and
// goes through the probe, which calls the real function and notes what the inline code around the call noted before
// (the columns and values of perf.csv and perf-frames.csv are unchanged). A joeyhodge release that wraps the same call
// in its own measurement then merges around one declaration line instead of meeting lines on both sides of the call.
// A destructor at the end of the scope would time everything after the call as well, so the probes are not RAII.

#include <openxr/openxr.h>

#include "PerfLog.hpp"

namespace uevr::perf {

// xrBeginFrame: counts XR_FRAME_DISCARDED and keeps the time inside the runtime out of UEVR's own render-thread cost.
struct XrBeginFrameProbe {
    XrResult operator()(XrSession session, const XrFrameBeginInfo* info) const {
        const auto start = now_ns();
        const auto result = ::xrBeginFrame(session, info);
        note_xr_begin(now_ns() - start, result == XR_FRAME_DISCARDED);
        return result;
    }
};

// xrEndFrame of the frame's layers: the projection views, engine frame and frame state are read after the call.
template <typename ProjectionViews, typename FrameCount, typename FrameState>
struct XrEndFrameProbe {
    const ProjectionViews& projection_views;
    const FrameCount& frame_count;
    const FrameState& frame_state;

    XrResult operator()(XrSession session, const XrFrameEndInfo* info) const {
        const auto start = now_ns();
        const auto result = ::xrEndFrame(session, info);
        const auto end = now_ns();
        // A projection for an engine frame that was already submitted counts as a repeat, not a new VR frame.
        note_xr_end(result == XR_SUCCESS, !projection_views.empty(), frame_count, end, end - start, info->displayTime,
            frame_state.predictedDisplayPeriod);
        return result;
    }
};

template <typename ProjectionViews, typename FrameCount, typename FrameState>
XrEndFrameProbe(const ProjectionViews&, const FrameCount&, const FrameState&) -> XrEndFrameProbe<ProjectionViews, FrameCount, FrameState>;

// xrWaitSwapchainImage: waiting for the runtime to release the image is blocked time, not UEVR's own cost.
struct XrWaitSwapchainImageProbe {
    XrResult operator()(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info) const {
        const auto start = now_ns();
        const auto result = ::xrWaitSwapchainImage(swapchain, info);
        note_blocked(now_ns() - start);
        return result;
    }
};

} // namespace uevr::perf
