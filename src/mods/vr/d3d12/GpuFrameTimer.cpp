#include <cstring>

#include <spdlog/spdlog.h>

#include "../PerfLog.hpp"

#include "GpuFrameTimer.hpp"

namespace d3d12 {
namespace {
constexpr uint64_t SLOT_BYTES = 2 * sizeof(uint64_t); // begin, end
constexpr double MAX_SPAN_NS = 2'000'000'000.0;        // longer = a stall or a broken pair; not a frame
}

GpuFrameTimer& gpu_frame_timer() {
    static GpuFrameTimer timer{};
    return timer;
}

bool GpuFrameTimer::ensure(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (device == nullptr || queue == nullptr) {
        return false;
    }

    if (m_device.Get() == device && m_heap != nullptr && m_readback != nullptr && m_frequency != 0) {
        return true;
    }

    if (m_failed && m_device.Get() == device) {
        return false;
    }

    // A new device (or the first use): the old objects belong to the old one.
    m_heap.Reset();
    m_readback.Reset();
    m_device = device;
    m_failed = true;
    m_current = -1;
    m_next = 0;
    for (auto& slot : m_slots) {
        slot = Slot{};
    }

    if (FAILED(queue->GetTimestampFrequency(&m_frequency)) || m_frequency == 0) {
        spdlog::warn("[Perf][D3D12] No timestamp frequency on the game's queue; game_gpu_span stays empty");
        return false;
    }

    D3D12_QUERY_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = SLOTS * 2;
    if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&m_heap)))) {
        spdlog::warn("[Perf][D3D12] Could not create the timestamp query heap; game_gpu_span stays empty");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = SLOTS * SLOT_BYTES;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback)))) {
        m_heap.Reset();
        spdlog::warn("[Perf][D3D12] Could not create the timestamp readback buffer; game_gpu_span stays empty");
        return false;
    }

    m_heap->SetName(L"UEVR game_gpu_span timestamps");
    m_readback->SetName(L"UEVR game_gpu_span readback");
    m_failed = false;
    spdlog::info("[Perf][D3D12] game_gpu_span: timestamps on the game's queue at {} Hz, {} frames in flight", m_frequency, SLOTS);
    return true;
}

void GpuFrameTimer::poll() {
    if (m_readback == nullptr || m_frequency == 0) {
        return;
    }

    for (uint32_t index = 0; index < SLOTS; ++index) {
        auto& slot = m_slots[index];
        if (slot.state != State::Ended || slot.fence == nullptr || slot.fence->GetCompletedValue() < slot.fence_value) {
            continue;
        }

        const D3D12_RANGE read{index * SLOT_BYTES, (index + 1) * SLOT_BYTES};
        const D3D12_RANGE written{0, 0};
        void* data{};
        uint64_t ticks[2]{};
        if (SUCCEEDED(m_readback->Map(0, &read, &data)) && data != nullptr) {
            std::memcpy(ticks, static_cast<const uint8_t*>(data) + read.Begin, sizeof(ticks));
            m_readback->Unmap(0, &written);
        }

        const auto lag = slot.presents;
        slot = Slot{};

        if (ticks[0] == 0 || ticks[1] <= ticks[0]) {
            continue; // invalid pair: no sample
        }

        const auto span_ns = static_cast<double>(ticks[1] - ticks[0]) * 1'000'000'000.0 / static_cast<double>(m_frequency);
        if (span_ns < MAX_SPAN_NS) {
            uevr::perf::note_game_gpu_span(static_cast<int64_t>(span_ns), lag);
        }
    }
}

void GpuFrameTimer::on_post_present() {
    if (!uevr::perf::enabled()) {
        return;
    }

    std::scoped_lock _{m_mtx};

    for (auto& slot : m_slots) {
        if (slot.state == State::Ended) {
            ++slot.presents;
        }
    }

    poll();

    // The frame that just presented: without a submitted end it gives no sample.
    if (m_current >= 0) {
        auto& slot = m_slots[m_current];
        if (slot.state == State::Begun) {
            slot = Slot{};
        }
        slot.end_pending = false;
        m_current = -1;
    }

    m_armed = true;
}

bool GpuFrameTimer::record_begin(ID3D12GraphicsCommandList* list, ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (list == nullptr || !uevr::perf::enabled()) {
        return false;
    }

    std::scoped_lock _{m_mtx};

    if (!m_armed) {
        return false;
    }

    m_armed = false;

    if (!ensure(device, queue)) {
        return false;
    }

    const auto index = m_next;
    m_next = (m_next + 1) % SLOTS;

    // Still not read back SLOTS frames later (its fence never completed in time): that frame is lost.
    m_slots[index] = Slot{};
    list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2);
    m_slots[index].state = State::Begun;
    m_current = static_cast<int32_t>(index);
    return true;
}

bool GpuFrameTimer::record_end(ID3D12GraphicsCommandList* list) {
    if (list == nullptr || !uevr::perf::enabled()) {
        return false;
    }

    std::scoped_lock _{m_mtx};

    if (m_current < 0 || m_heap == nullptr || m_readback == nullptr) {
        return false;
    }

    const auto index = static_cast<uint32_t>(m_current);
    list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2 + 1);
    list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2, 2, m_readback.Get(), index * SLOT_BYTES);
    m_slots[index].end_pending = true;
    return true;
}

void GpuFrameTimer::commit_end(ID3D12Fence* fence, uint64_t fence_value) {
    std::scoped_lock _{m_mtx};

    if (m_current < 0 || fence == nullptr) {
        return;
    }

    auto& slot = m_slots[m_current];
    if (!slot.end_pending) {
        return;
    }

    slot.end_pending = false;
    slot.fence = fence;
    slot.fence_value = fence_value;
    slot.presents = 0;
    slot.state = State::Ended;
}

void GpuFrameTimer::reset() {
    std::scoped_lock _{m_mtx};

    for (auto& slot : m_slots) {
        slot = Slot{};
    }

    m_current = -1;
    m_armed = false;
}
}
