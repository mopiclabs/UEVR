// Mopic: the Native Stereo Fix double-wide submits of D3D12Component::on_frame (see MopicNativeSubmitD3D12.hpp).

#include <algorithm>
#include <chrono>
#include <optional>

#include <spdlog/spdlog.h>
#include <utility/Logging.hpp>

#include "../VR.hpp"
#include "PerfLog.hpp"
#include "MopicNativeSubmitD3D12.hpp"

namespace vrmod {

// The overlap of a double-wide eye pair and a swapchain image of another size, eye by eye (or the left eye into both
// halves), so a copy during a live resolution change neither reads nor writes outside either image. The pair goes to
// the top left, eyes side by side; returns its extent for the projection views (OpenXR::set_swapchain_content).
std::pair<UINT, UINT> copy_eye_pair_overlap(d3d12::CommandContext& commands, ID3D12Resource* src, ID3D12Resource* dst,
    D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state, bool left_eye_in_both)
{
    const auto s = src->GetDesc();
    const auto d = dst->GetDesc();
    const auto w = static_cast<UINT>(std::min<UINT64>(s.Width / 2, d.Width / 2));
    const auto h = std::min<UINT>(s.Height, d.Height);
    const auto src_right = left_eye_in_both ? 0u : static_cast<UINT>(s.Width / 2);

    D3D12_BOX left{0, 0, 0, w, h, 1};
    D3D12_BOX right{src_right, 0, 0, src_right + w, h, 1};
    commands.copy_region(src, dst, &left, 0, 0, 0, src_state, dst_state);
    commands.copy_region(src, dst, &right, w, 0, 0, src_state, dst_state);
    return {2 * w, h};
}

void D3D12Component::set_native_pair_content(VR* vr, uint32_t eye_width, uint32_t eye_height) {
    vr->m_openxr->set_swapchain_content((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE,
        (int32_t)(2 * eye_width), (int32_t)eye_height);
}

D3D12Component::MopicDoubleWideResult D3D12Component::submit_mopic_double_wide(MopicDoubleWideSubmit&& submit) {
    namespace frame_diag = uevr::native_frame;
    auto* const vr = submit.vr;
    const auto& backbuffer = submit.backbuffer;
    const auto scene_source_state = submit.scene_source_state;
    const auto& native_stereo_packet = submit.native_stereo_packet;
    auto* const native_stereo_hook = submit.native_stereo_hook;
    const auto native_frame_ticket = submit.native_frame_ticket;
    const auto use_native_array_submit = submit.use_native_array_submit;
    const auto shf_using_mono_expansion = submit.shf_using_mono_expansion;
    const auto dune_using_hmd_mono_expansion = submit.dune_using_hmd_mono_expansion;
    const auto native_left_source_fits = submit.native_left_source_fits;
    const auto native_eye_width = submit.native_eye_width;
    const auto native_eye_height = submit.native_eye_height;

    const bool reuse_last_native_capture =
        native_stereo_packet == nullptr &&
        vr->is_native_stereo_fix_enabled() &&
        // only while the fix is operating and just this frame's packet is missing/refused; its
        // fallback frames (non-Active states) render the right half natively
        native_stereo_hook != nullptr && native_stereo_hook->is_native_stereo_fix_active() &&
        !uevr::nascar::is_target() &&
        m_scene_capture_tex.texture.Get() != nullptr &&
        m_game_tex.texture.Get() != nullptr &&
        native_left_source_fits &&
        m_last_native_capture_submit.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - m_last_native_capture_submit < std::chrono::milliseconds(500);
    // VR_PerfLog: whether a Native Stereo Fix submit carries this frame's right eye.
    const bool native_stereo_fix_live =
        vr->is_native_stereo_fix_enabled() &&
        native_stereo_hook != nullptr && native_stereo_hook->is_native_stereo_fix_active();
    // VR_AutoResNsfHoldMs: after a live resolution change the fix's new right-eye capture comes out
    // dark for up to ~0.6 s after it is accepted (exposure and history start over), and in the frames
    // before that the right half is a stale capture or unrendered (live-resolution research 10-24).
    // Show the left eye in both halves from the change until the hold has passed since the accept.
    // The texture-array submit (VR_NativeStereoFixTextureArraySubmit) keeps its own path.
    const auto nsf_hold = std::chrono::milliseconds(vr->m_openxr->autores.nsf_hold_ms);
    const auto live_resize_at = vr->m_openxr->autores.live_resize_at;
    const auto copy_now = std::chrono::steady_clock::now();
    // The hold's timing (uevr::autores::nsf_resize_hold): a capture accepted late still gets its hold.
    const auto ms = [](std::chrono::steady_clock::time_point t) {
        return t.time_since_epoch().count() == 0 ? int64_t{0}
            : (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    };
    const bool native_stereo_resize_hold =
        vr->is_native_stereo_fix_enabled() &&
        !use_native_array_submit &&
        !shf_using_mono_expansion && !dune_using_hmd_mono_expansion &&
        uevr::autores::nsf_resize_hold(ms(copy_now), ms(live_resize_at), ms(m_scene_capture_accepted_at), nsf_hold.count());

    // VR_AutoResSourceHoldMs, without the fix: the game's first frames in its render target of the new
    // size have a black right eye (Hozy: 1-3 frames from ~40 ms after the switch, live-resolution
    // research 10-24), so the left eye goes in both halves for a moment after the size changes.
    if (const auto source_desc = backbuffer != nullptr ? backbuffer->GetDesc() : D3D12_RESOURCE_DESC{};
        backbuffer != nullptr &&
        (source_desc.Width != m_hold_source_width || source_desc.Height != m_hold_source_height))
    {
        m_hold_source_width = source_desc.Width;
        m_hold_source_height = source_desc.Height;
        m_hold_source_changed_at = copy_now;
    }
    const auto source_hold = std::chrono::milliseconds(vr->m_openxr->autores.source_hold_ms);
    const bool source_resize_hold =
        source_hold.count() > 0 &&
        !vr->is_native_stereo_fix_enabled() &&
        !use_native_array_submit &&
        !shf_using_mono_expansion && !dune_using_hmd_mono_expansion &&
        vr->m_openxr->in_live_resize_transition() &&
        m_hold_source_changed_at >= live_resize_at &&
        copy_now - m_hold_source_changed_at < source_hold;

    if (native_stereo_resize_hold != m_native_stereo_resize_holding) {
        m_native_stereo_resize_holding = native_stereo_resize_hold;
        spdlog::info("[NativeStereoFix][D3D12] Resize hold {} {} ms after the change, capture accepted {} ms after it",
            native_stereo_resize_hold ? "start" : "end",
            std::chrono::duration_cast<std::chrono::milliseconds>(copy_now - live_resize_at).count(),
            m_scene_capture_accepted_at < live_resize_at ? -1LL
                : static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(m_scene_capture_accepted_at - live_resize_at).count()));
    }

    MopicDoubleWideResult result{false, native_stereo_fix_live};

    if (native_stereo_resize_hold || source_resize_hold) {
        if (source_resize_hold) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[autores] Render target took the new size; left eye in both halves for {} ms",
                source_hold.count());
        } else {
            uevr::perf::note_native_stereo(uevr::perf::NativeStereo::Fallback);
        }
        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr,
            [source = backbuffer, scene_source_state, native_stereo_packet, native_stereo_hook, native_frame_ticket, vr](
                d3d12::CommandContext& commands, ID3D12Resource* dst) {
                const auto [content_width, content_height] = copy_eye_pair_overlap(commands, source.Get(),
                    dst, scene_source_state, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
                vr->m_openxr->set_swapchain_content((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE,
                    (int32_t)content_width, (int32_t)content_height);
                // Consumed as a real submit would be, which keeps the fix Active; the right half came from the backbuffer.
                if (native_stereo_packet != nullptr && native_stereo_hook != nullptr) {
                    if (native_frame_ticket) {
                        native_stereo_hook->record_native_frame_stage(*native_stereo_packet, native_frame_ticket,
                            frame_diag::Backend::d3d12, frame_diag::Runtime::openxr, frame_diag::Stage::copy_recorded,
                            0, 2, 0, source.Get(), dst);
                    }
                    native_stereo_hook->note_native_stereo_frame_packet_consumed(native_stereo_packet->serial);
                }
            }, std::nullopt, scene_source_state, nullptr);
        result.submitted = true;
        return result;
    }

    // on_frame's texture-array submit and Stalker 2 exact pair come next, then the right-eye reuse below.
    if (use_native_array_submit || submit.stalker_exact) {
        return result;
    }

    if (reuse_last_native_capture && !shf_using_mono_expansion && !dune_using_hmd_mono_expansion) {
        SPDLOG_INFO_EVERY_N_SEC(5, "[NativeStereoFix][D3D12] No right-eye packet this frame, reusing the last right-eye capture");
        uevr::perf::note_native_stereo(uevr::perf::NativeStereo::Reused);
        set_native_pair_content(vr, native_eye_width, native_eye_height);
        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, std::move(submit.pre_render_cached), std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
        result.submitted = true;
        return result;
    }

    // on_frame's branch for a frame without a right-eye capture.
    if (native_stereo_packet == nullptr ||
        m_scene_capture_tex.texture.Get() == nullptr ||
        shf_using_mono_expansion ||
        dune_using_hmd_mono_expansion) {
        // Just after a live resolution change the Native Stereo Fix has no capture for the new eye
        // size yet, and while it is active the game does not draw its right half: this copy showed
        // a black right eye for up to 0.8 s (live-resolution research 10-24). Put the left eye in both
        // halves until the fix captures again - a moment of mono instead of one black eye.
        if (native_stereo_fix_live && vr->m_openxr->in_live_resize_transition() &&
            !shf_using_mono_expansion && !dune_using_hmd_mono_expansion) {
            uevr::perf::note_native_stereo(uevr::perf::NativeStereo::Fallback); // on_frame notes the branch's other frames
            SPDLOG_INFO_EVERY_N_SEC(1, "[NativeStereoFix][D3D12] No right-eye capture after a live resolution change; left eye in both halves");
            m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr,
                [source = backbuffer, scene_source_state, vr](d3d12::CommandContext& commands, ID3D12Resource* dst) {
                    const auto [content_width, content_height] = copy_eye_pair_overlap(commands, source.Get(),
                        dst, scene_source_state, D3D12_RESOURCE_STATE_RENDER_TARGET, true);
                    vr->m_openxr->set_swapchain_content((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE,
                        (int32_t)content_width, (int32_t)content_height);
                }, std::nullopt, scene_source_state, nullptr);
            result.submitted = true;
        }
    }

    return result;
}
} // namespace vrmod
