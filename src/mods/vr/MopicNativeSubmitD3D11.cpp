// Mopic: the live resolution change holds of D3D11Component::on_frame's double-wide submit (see MopicNativeSubmitD3D11.hpp).

#include <algorithm>
#include <chrono>

#include <spdlog/spdlog.h>
#include <utility/Logging.hpp>

#include "../VR.hpp"
#include "MopicNativeSubmitD3D11.hpp"

namespace vrmod {

// The D3D11 copy_eye_pair_overlap of D3D12Component: the overlap of a double-wide eye pair and an image of another
// size, eye by eye (or the left eye into both halves), packed at the top left; returns the pair's extent.
std::pair<UINT, UINT> copy_eye_pair_overlap(ID3D11DeviceContext* context, ID3D11Texture2D* src, ID3D11Texture2D* dst,
    bool left_eye_in_both)
{
    D3D11_TEXTURE2D_DESC s{}, d{};
    src->GetDesc(&s);
    dst->GetDesc(&d);
    const auto w = std::min(s.Width / 2, d.Width / 2);
    const auto h = std::min(s.Height, d.Height);
    const auto src_right = left_eye_in_both ? 0u : s.Width / 2;

    const D3D11_BOX left{0, 0, 0, w, h, 1};
    const D3D11_BOX right{src_right, 0, 0, src_right + w, h, 1};
    context->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &left);
    context->CopySubresourceRegion(dst, 0, w, 0, 0, src, 0, &right);
    return {2 * w, h};
}

bool D3D11Component::submit_mopic_hold(const MopicHoldSubmit& submit) {
    namespace frame_diag = uevr::native_frame;
    auto* const vr = submit.vr;
    auto* const context = submit.context;
    const auto& backbuffer = submit.backbuffer;
    auto* const ffsr = submit.ffsr;
    const auto& native_stereo_packet = submit.native_stereo_packet;
    const auto& native_frame_ticket = submit.native_frame_ticket;

    // Live resolution change holds, as in D3D12Component: left eye in both halves while the right eye
    // would show the first, black frames of a new Native Stereo Fix capture (VR_AutoResNsfHoldMs: from
    // the change until the hold has passed since the accept, and until the fix has been active again for
    // 200 ms - Stray, 3080L, 10-25: black ended up to 0.74 s after the accept when the fix came back late)
    // or of the game's render target at the new size (VR_AutoResSourceHoldMs, without the fix).
    const auto live_resize_at = vr->m_openxr->autores.live_resize_at;
    const bool resized_live = live_resize_at.time_since_epoch().count() != 0;
    const auto copy_now = std::chrono::steady_clock::now();
    const bool nsf_active = ffsr != nullptr && ffsr->is_native_stereo_fix_active();
    if (nsf_active != m_nsf_was_active) {
        m_nsf_was_active = nsf_active;
        if (nsf_active) {
            m_nsf_active_since = copy_now;
        }
    }
    const auto nsf_hold = std::chrono::milliseconds(vr->m_openxr->autores.nsf_hold_ms);
    // The hold's timing (uevr::autores::nsf_resize_hold): a capture accepted late still gets its hold. D3D11 also holds
    // while the fix is not active yet or became active in the last 200 ms, within the wait after the change.
    const auto ms = [](std::chrono::steady_clock::time_point t) {
        return t.time_since_epoch().count() == 0 ? int64_t{0}
            : (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    };
    const bool native_stereo_resize_hold =
        nsf_hold.count() > 0 && vr->is_native_stereo_fix_enabled() && resized_live &&
        (uevr::autores::nsf_resize_hold(ms(copy_now), ms(live_resize_at), ms(m_scene_capture_accepted_at), nsf_hold.count()) ||
         (copy_now - live_resize_at < nsf_hold + std::chrono::milliseconds(uevr::autores::NSF_HOLD_WAIT_MS) &&
          (!nsf_active || copy_now - m_nsf_active_since < std::chrono::milliseconds(200))));

    D3D11_TEXTURE2D_DESC source_desc{};
    backbuffer->GetDesc(&source_desc);
    if (source_desc.Width != m_hold_source_width || source_desc.Height != m_hold_source_height) {
        m_hold_source_width = source_desc.Width;
        m_hold_source_height = source_desc.Height;
        m_hold_source_changed_at = copy_now;
    }
    const auto source_hold = std::chrono::milliseconds(vr->m_openxr->autores.source_hold_ms);
    const bool source_resize_hold =
        source_hold.count() > 0 && !vr->is_native_stereo_fix_enabled() &&
        vr->m_openxr->in_live_resize_transition() && m_hold_source_changed_at >= live_resize_at &&
        copy_now - m_hold_source_changed_at < source_hold;

    if (native_stereo_resize_hold != m_native_stereo_resize_holding) {
        m_native_stereo_resize_holding = native_stereo_resize_hold;
        spdlog::info("[NativeStereoFix][D3D11] Resize hold {} {} ms after the change, capture accepted {} ms after it",
            native_stereo_resize_hold ? "start" : "end",
            std::chrono::duration_cast<std::chrono::milliseconds>(copy_now - live_resize_at).count(),
            m_scene_capture_accepted_at < live_resize_at ? -1LL
                : static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(m_scene_capture_accepted_at - live_resize_at).count()));
    }

    if (native_stereo_resize_hold || source_resize_hold) {
        if (source_resize_hold) {
            SPDLOG_INFO_EVERY_N_SEC(1, "[autores] Render target took the new size; left eye in both halves for {} ms",
                source_hold.count());
        }
        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, nullptr, [&](ID3D11Texture2D* render_target) {
            const auto [content_width, content_height] =
                copy_eye_pair_overlap(context, backbuffer.Get(), render_target, true);
            vr->m_openxr->set_swapchain_content((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE,
                (int32_t)content_width, (int32_t)content_height);
            // Consumed as a real submit would be, which keeps the fix Active; the right half came from the backbuffer.
            if (native_stereo_packet != nullptr && ffsr != nullptr) {
                if (native_frame_ticket) {
                    ffsr->record_native_frame_stage(*native_stereo_packet, native_frame_ticket,
                        frame_diag::Backend::d3d11, frame_diag::Runtime::openxr, frame_diag::Stage::copy_recorded,
                        0, 2, 0, backbuffer.Get(), render_target);
                }
                ffsr->note_native_stereo_frame_packet_consumed(native_stereo_packet->serial);
            }
        });
        return true;
    }

    return false;
}

} // namespace vrmod
