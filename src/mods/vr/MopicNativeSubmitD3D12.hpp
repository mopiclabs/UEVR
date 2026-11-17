#pragma once

// Mopic: the Native Stereo Fix double-wide submits UEVR upstream doesn't have, moved out of D3D12Component::on_frame
// so a joeyhodge release that rewrites that function only meets a call there (MOPIC.md, "Updating to a new joeyhodge
// release"). on_frame calls submit_mopic_double_wide before its own submit chain and skips that chain when it returns
// submitted; the PerfLog notes and m_last_native_capture_submit of upstream's own copies stay next to them. Also
// brings PerfLog.hpp and PerfLogXr.hpp, which D3D12Component.cpp uses.

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include <d3d12.h>
#include <wrl.h>

#include "D3D12Component.hpp"
#include "FFakeStereoRenderingHook.hpp"
#include "PerfLogXr.hpp"

namespace vrmod {

// What submit_mopic_double_wide reads of on_frame's frame state.
struct D3D12Component::MopicDoubleWideSubmit {
    VR* vr{};
    const ComPtr<ID3D12Resource>& backbuffer;
    D3D12_RESOURCE_STATES scene_source_state{};
    const std::shared_ptr<const FFakeStereoRenderingHook::NativeStereoFramePacket>& native_stereo_packet;
    FFakeStereoRenderingHook* native_stereo_hook{};
    uevr::native_frame::Ticket native_frame_ticket{};
    bool use_native_array_submit{};
    bool stalker_exact{};
    bool shf_using_mono_expansion{};
    bool dune_using_hmd_mono_expansion{};
    bool native_left_source_fits{};
    uint32_t native_eye_width{};
    uint32_t native_eye_height{};
    // Copies the last validated right-eye capture (or the frozen pair) with the left eye.
    std::function<void(d3d12::CommandContext&, ID3D12Resource*)> pre_render_cached{};
};

// The overlap of a double-wide eye pair and a swapchain image of another size, eye by eye (or the left eye into both
// halves), so a copy during a live resolution change neither reads nor writes outside either image. The pair goes to
// the top left, eyes side by side; returns its extent for the projection views (OpenXR::set_swapchain_content).
std::pair<UINT, UINT> copy_eye_pair_overlap(d3d12::CommandContext& commands, ID3D12Resource* src, ID3D12Resource* dst,
    D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state, bool left_eye_in_both);

} // namespace vrmod
