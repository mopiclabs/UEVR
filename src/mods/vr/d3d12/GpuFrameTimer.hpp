#pragma once

// EXPERIMENT (VR_PerfLog game_gpu_span): the game's GPU frame span, measured on the game's own direct queue (the one
// the swap chain presents on, which UEVR's CommandContexts submit to).
//
//   begin  EndQuery(TIMESTAMP) at the end of the first command list UEVR submits to that queue after the previous
//          Present returned: D3D12Component::on_post_present's backbuffer clear. Everything the game queues for the
//          next frame comes after it.
//   end    EndQuery(TIMESTAMP) at the end of the command list that copies the eyes into the OpenXR scene swapchain
//          (D3D12Component::OpenXR::copy of DOUBLE_WIDE / NATIVE_STEREO_ARRAY / AFR eye): the Native Stereo Fix pair
//          copy while it operates, the ordinary copy otherwise. The same list resolves the pair into a readback
//          buffer. A second scene copy in the same Present moves the end later (the last one wins).
//
// span = end - begin, at ID3D12CommandQueue::GetTimestampFrequency. A span, not GPU busy: idle gaps on the queue
// between the two count. SLOTS frames are in flight; a frame is read back once the fence of its last end list has
// completed (checked without waiting at each post-present, normally ~3 Presents later), and dropped when its slot
// comes round again before that. Frames without a begin (no backbuffer clear: HMD inactive, the clear's context still
// busy, Mono transitions) or without an end (no scene copy in that Present) give no sample.
//
// Present-thread only work: two EndQuery + one ResolveQueryData per frame, a few fence reads, one Map of 16 bytes.
// Nothing runs while VR_PerfLog is off.

#include <array>
#include <cstdint>
#include <mutex>

#include <d3d12.h>

#include "ComPtr.hpp"

namespace d3d12 {
class GpuFrameTimer {
public:
    static constexpr uint32_t SLOTS = 8;

    // Start of D3D12Component::on_post_present (not the Mono-transition early outs): reads back finished frames,
    // closes the frame that just presented and arms the next begin.
    void on_post_present();

    // The begin timestamp, into `list` (still open, about to be closed and executed on `queue`). Only once per
    // on_post_present. False when nothing was recorded.
    bool record_begin(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12CommandQueue* queue);

    // The end timestamp and the resolve of the current frame, into `list` (open, with commands, about to be executed
    // on the game's queue). Call commit_end with that context's fence once it was submitted.
    bool record_end(ID3D12GraphicsCommandList* list);
    void commit_end(ID3D12Fence* fence, uint64_t fence_value);

    // Forget the frames in flight (D3D12Component::on_reset). The heap and readback buffer stay for the same device.
    void reset();

private:
    enum class State : uint8_t { Free, Begun, Ended };

    struct Slot {
        State state{State::Free};
        bool end_pending{};       // record_end ran, commit_end not yet
        ComPtr<ID3D12Fence> fence{};
        uint64_t fence_value{};
        uint32_t presents{};      // post-present points since the frame's end was committed
    };

    bool ensure(ID3D12Device* device, ID3D12CommandQueue* queue);
    void poll();

    std::mutex m_mtx{};
    ComPtr<ID3D12Device> m_device{};
    ComPtr<ID3D12QueryHeap> m_heap{};
    ComPtr<ID3D12Resource> m_readback{};
    uint64_t m_frequency{};
    bool m_failed{};
    bool m_armed{};
    int32_t m_current{-1};
    uint32_t m_next{};
    std::array<Slot, SLOTS> m_slots{};
};

GpuFrameTimer& gpu_frame_timer();
}
