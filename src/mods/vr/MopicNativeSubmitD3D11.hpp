#pragma once

// Mopic: the live resolution change holds of the D3D11 double-wide submit, moved out of D3D11Component::on_frame so a
// joeyhodge release that rewrites that function only meets a call there (MOPIC.md, "Updating to a new joeyhodge
// release"). on_frame calls submit_mopic_hold first and skips its own double-wide copies when it returns true; the
// PerfLog notes of upstream's own copies stay next to them. Also brings PerfLog.hpp, which D3D11Component.cpp uses.

#include <memory>
#include <utility>

#include <d3d11.h>
#include <wrl.h>

#include "D3D11Component.hpp"
#include "FFakeStereoRenderingHook.hpp"
#include "PerfLog.hpp"

namespace vrmod {

// What submit_mopic_hold reads of on_frame's frame state.
struct D3D11Component::MopicHoldSubmit {
    VR* vr{};
    ID3D11DeviceContext* context{};
    const ComPtr<ID3D11Texture2D>& backbuffer;
    FFakeStereoRenderingHook* ffsr{};
    const std::shared_ptr<const FFakeStereoRenderingHook::NativeStereoFramePacket>& native_stereo_packet;
    const uevr::native_frame::Ticket& native_frame_ticket;
};

// The D3D11 copy_eye_pair_overlap of D3D12Component (MopicNativeSubmitD3D12.hpp).
std::pair<UINT, UINT> copy_eye_pair_overlap(ID3D11DeviceContext* context, ID3D11Texture2D* src, ID3D11Texture2D* dst,
    bool left_eye_in_both);

} // namespace vrmod
