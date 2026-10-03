#include <algorithm>
#include <thread>

#include <spdlog/spdlog.h>
#include <utility/Logging.hpp>
#include <utility/String.hpp>

#include "Framework.hpp"
#include "utility/D3DDeviceIdentity.hpp"

#include "CommandContext.hpp"
#include "NativePairSnapshot.hpp"

namespace d3d12 {
namespace {
// Slots rest here between a snapshot (COPY_DEST while written) and the Present passes that copy out of them.
constexpr auto SLOT_STATE = D3D12_RESOURCE_STATE_COPY_SOURCE;
// The engine leaves both eye targets as render targets; the live copy makes the same assumption.
constexpr auto SOURCE_STATE = D3D12_RESOURCE_STATE_RENDER_TARGET;
constexpr DWORD RETIRE_GPU_WAIT_MS = 2000;
// How many engine frames (pose callbacks) the lent pair may trail the newest one: 1 is the next frame armed and not
// yet copied, 2 allows one frame whose snapshot was skipped.
constexpr int32_t MAX_FRAMES_BEHIND = 2;
constexpr auto RETIRE_PRODUCER_WAIT = std::chrono::milliseconds(250);

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_TEXTURE_COPY_LOCATION subresource0(ID3D12Resource* resource) {
    D3D12_TEXTURE_COPY_LOCATION location{};
    location.pResource = resource;
    location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    location.SubresourceIndex = 0;
    return location;
}

// True once `fence` reached `value` (a removed device reports UINT64_MAX, which counts as done).
bool reached(ID3D12Fence* fence, uint64_t value) {
    return value == 0 || (fence != nullptr && fence->GetCompletedValue() >= value);
}

bool wait_fence(ID3D12Fence* fence, uint64_t value, HANDLE event, DWORD timeout_ms) {
    if (reached(fence, value)) {
        return true;
    }

    if (fence == nullptr || event == nullptr || FAILED(fence->SetEventOnCompletion(value, event))) {
        return false;
    }

    return WaitForSingleObject(event, timeout_ms) == WAIT_OBJECT_0 || reached(fence, value);
}
}

struct NativePairSnapshot::Ring {
    struct Slot {
        ComPtr<ID3D12Resource> texture{};
        ComPtr<ID3D12CommandAllocator> allocator{};
        ComPtr<ID3D12GraphicsCommandList> list{};
        // The sources of the last write: kept until the GPU is done with it (the next write or the ring's release).
        ComPtr<ID3D12Resource> left{};
        ComPtr<ID3D12Resource> right{};
        uint64_t written{};              // fence value of the last write, 0 = never written
        uint64_t read{};                 // reader-fence value after the last Present-pass copy out of it
        uint64_t capture_generation{};
        ComPtr<ID3D12CommandQueue> queue{};   // the engine queue of the last write
        uint32_t frame{};
        uint32_t uses{};
        std::chrono::steady_clock::time_point time{};
    };

    ~Ring() {
        if (event != nullptr) {
            CloseHandle(event);
        }
    }

    ComPtr<ID3D12Device> device{};
    D3D12_RESOURCE_DESC desc{};
    std::array<Slot, SLOT_COUNT> slots{};
    ComPtr<ID3D12Fence> fence{};         // slot writes, signaled on the engine queue
    ComPtr<ID3D12Fence> reader_fence{};  // copies out of a slot, signaled on the Present pass's queue
    uint64_t fence_value{};
    uint64_t reader_value{};
    HANDLE event{};

    bool slot_free(uint32_t index) const {
        const auto& slot = slots[index];
        return reached(fence.Get(), slot.written) && reached(reader_fence.Get(), slot.read);
    }

    // CPU wait until the GPU is done with every slot. False on timeout.
    bool wait_idle() const {
        return wait_fence(fence.Get(), fence_value, event, RETIRE_GPU_WAIT_MS) &&
            wait_fence(reader_fence.Get(), reader_value, event, RETIRE_GPU_WAIT_MS);
    }
};

namespace {
// A ring the GPU might still use after a bounded wait gave up: kept for the rest of the session rather than released
// under the GPU.
std::mutex g_leaked_mutex{};
std::vector<std::shared_ptr<NativePairSnapshot::Ring>> g_leaked_rings{};

void leak(std::shared_ptr<NativePairSnapshot::Ring> ring, const char* why) {
    spdlog::error("[NativeStereoFix][D3D12] Keeping the pair snapshot ring alive for the session: {}", why);
    std::scoped_lock _{g_leaked_mutex};
    g_leaked_rings.push_back(std::move(ring));
}
}

NativePairSnapshot::~NativePairSnapshot() {
    if (g_framework != nullptr) {
        if (auto& hook = g_framework->get_d3d12_hook(); hook != nullptr) {
            hook->clear_execute_observer(this);
        }
    }

    retire();
}

std::string NativePairSnapshot::debug_name(ID3D12Object* object) {
    if (object == nullptr) {
        return "<null>";
    }

    UINT size = 0;
    if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, nullptr)) || size <= sizeof(wchar_t)) {
        return "<unnamed>";
    }

    std::wstring name(size / sizeof(wchar_t), L'\0');
    if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name.data()))) {
        return "<unnamed>";
    }

    while (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }

    return name.empty() ? "<unnamed>" : utility::narrow(name);
}

bool NativePairSnapshot::configure(ID3D12Device* device, const D3D12_RESOURCE_DESC& target_desc, const Sources& sources,
    const char** reason)
{
    const auto fail = [&](const char* why) {
        if (reason != nullptr) {
            *reason = why;
        }
        return false;
    };

    if (device == nullptr || sources.left == nullptr || sources.right == nullptr || sources.left.Get() == sources.right.Get()) {
        return fail("the eye targets are not set up");
    }

    if (target_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || target_desc.DepthOrArraySize != 1 ||
        target_desc.SampleDesc.Count != 1 || sources.eye_width == 0 || sources.eye_height == 0 ||
        target_desc.Width < static_cast<uint64_t>(sources.eye_width) + sources.right_width ||
        target_desc.Height < (std::max)(sources.eye_height, sources.right_height))
    {
        return fail("the double-wide swapchain image does not hold the eye pair");
    }

    std::shared_ptr<Ring> current{};
    {
        std::scoped_lock _{m_mutex};
        if (m_failed) {
            return fail("pair snapshots failed earlier this session");
        }
        current = m_ring;

        // No ring (and no VRAM) before the pose callback's thread was seen submitting: arm() runs without one, and
        // the first armed direct-queue submission marks the engine's submission thread (take_snapshot()).
        if (current == nullptr && !m_submitter_seen) {
            m_sources = sources;
            m_suspended = false;
            return true;
        }
    }

    const auto same_shape = current != nullptr && current->device.Get() == device &&
        current->desc.Width == target_desc.Width && current->desc.Height == target_desc.Height &&
        current->desc.Format == target_desc.Format;

    if (!same_shape) {
        if (current != nullptr) {
            spdlog::info("[NativeStereoFix][D3D12] Recreating the pair snapshot ring for {}x{} format {}",
                target_desc.Width, target_desc.Height, static_cast<uint32_t>(target_desc.Format));
            retire();
        }

        auto ring = std::make_shared<Ring>();
        ring->device = device;
        ring->desc = target_desc;
        ring->desc.Alignment = 0;
        ring->desc.MipLevels = 1;
        ring->desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ring->desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        ring->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        bool created = ring->event != nullptr &&
            SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ring->fence))) &&
            SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ring->reader_fence)));

        for (uint32_t i = 0; created && i < SLOT_COUNT; ++i) {
            auto& slot = ring->slots[i];
            created =
                SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &ring->desc, SLOT_STATE, nullptr,
                    IID_PPV_ARGS(&slot.texture))) &&
                SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) &&
                SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                    IID_PPV_ARGS(&slot.list))) &&
                SUCCEEDED(slot.list->Close());

            if (created) {
                const auto name = L"Native Stereo Fix Pair Snapshot " + std::to_wstring(i);
                slot.texture->SetName(name.c_str());
                slot.list->SetName(name.c_str());
            }
        }

        if (!created) {
            spdlog::error("[NativeStereoFix][D3D12] Failed to create the pair snapshot ring ({}x{} format {})",
                target_desc.Width, target_desc.Height, static_cast<uint32_t>(target_desc.Format));
            std::scoped_lock _{m_mutex};
            m_failed = true;
            return fail("the pair snapshot ring could not be created");
        }

        spdlog::info("[NativeStereoFix][D3D12] Created the pair snapshot ring: {} slots of {}x{} format {}",
            SLOT_COUNT, target_desc.Width, target_desc.Height, static_cast<uint32_t>(target_desc.Format));

        std::scoped_lock _{m_mutex};
        m_ring = std::move(ring);
        m_published = -1;
        m_cursor = 0;
        m_checked_queue = nullptr;
    }

    std::scoped_lock _{m_mutex};
    if (m_ring == nullptr) {
        return fail("the pair snapshot ring was released");
    }

    // A published slot taken from other sources is not lent from here on (acquire() compares them).
    m_sources = sources;
    m_suspended = false;
    return true;
}

void NativePairSnapshot::suspend() {
    std::scoped_lock _{m_mutex};
    m_suspended = true;
    m_published = -1;
    m_armed.store(false, std::memory_order_release);
}

void NativePairSnapshot::retire() {
    std::shared_ptr<Ring> ring{};
    bool producing = false;

    {
        std::unique_lock lock{m_mutex};
        m_suspended = true;
        m_armed.store(false, std::memory_order_release);

        // A snapshot being recorded on the engine's thread finishes its bookkeeping under the lock.
        const auto deadline = std::chrono::steady_clock::now() + RETIRE_PRODUCER_WAIT;
        while (m_producing && std::chrono::steady_clock::now() < deadline) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
        }

        producing = m_producing;
        ring = std::move(m_ring);
        m_ring.reset();
        m_published = -1;
        // A lease still out on the old ring is tracked by that ring (release() waits for its copy), not here.
        m_pinned = -1;
        m_sources = {};
        m_checked_queue = nullptr;
    }

    if (ring == nullptr) {
        return;
    }

    if (producing) {
        leak(std::move(ring), "a snapshot was still being recorded");
        return;
    }

    // A pass still holding a lease keeps the ring alive (its shared_ptr) and waits for its own copy in release().
    if (!ring->wait_idle()) {
        leak(std::move(ring), "the GPU did not finish with it in time");
    }
}

void NativePairSnapshot::arm(uint32_t frame, uint64_t capture_generation) {
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock _{m_mutex};

    // The engine's thread, also for a frame that is not armed: acquire() tells the engine's own Present from frame
    // generation's by it, and tally() the engine's submissions.
    m_armed_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    m_last_arm_frame = frame;

    // Also without a ring: the first armed submission is what lets configure() create it.
    if (m_suspended || m_failed) {
        m_armed.store(false, std::memory_order_release);
        return;
    }

    // No frame generation: every pass copies the live targets (acquire()), so no snapshot nor its GPU copy.
    if (m_last_foreign_present.time_since_epoch().count() == 0 || now - m_last_foreign_present > FOREIGN_PRESENT_WINDOW) {
        m_armed.store(false, std::memory_order_release);
        ++m_stats.engine_thread_arms;
        return;
    }

    if (m_armed.load(std::memory_order_relaxed)) {
        ++m_stats.missed_arms;
    }

    m_armed_frame = frame;
    m_armed_generation = capture_generation;
    m_armed_time = now;
    m_armed.store(true, std::memory_order_release);
}

void NativePairSnapshot::disarm() {
    m_armed.store(false, std::memory_order_release);
}

NativePairSnapshot::Submission NativePairSnapshot::tally(ID3D12CommandQueue* queue, UINT count, bool from_engine_thread) {
    // Recent enough to follow a queue change, long enough that one frame's submissions don't swing it.
    constexpr uint64_t DECAY_AT = 4096;
    constexpr uint64_t MIN_LISTS = 16;

    std::unique_lock lock{m_tally_mutex};

    QueueTally* entry = nullptr;
    QueueTally* weakest = &m_tally[0];
    for (auto& candidate : m_tally) {
        if (candidate.queue == queue) {
            entry = &candidate;
            break;
        }

        if (candidate.lists < weakest->lists) {
            weakest = &candidate;
        }
    }

    if (entry == nullptr) {
        *weakest = QueueTally{queue, queue->GetDesc().Type};
        entry = weakest;
    }

    const uint64_t lists = (std::max)(count, 1u);
    entry->lists += lists;
    if (from_engine_thread) {
        entry->engine_thread_lists += lists;
    }

    if (entry->lists >= DECAY_AT) {
        for (auto& decayed : m_tally) {
            decayed.lists /= 2;
            decayed.engine_thread_lists /= 2;
        }
    }

    const QueueTally* first = nullptr;
    const QueueTally* second = nullptr;
    for (const auto& candidate : m_tally) {
        if (candidate.queue == nullptr || candidate.type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            continue;
        }

        if (first == nullptr || candidate.lists > first->lists) {
            second = first;
            first = &candidate;
        } else if (second == nullptr || candidate.lists > second->lists) {
            second = &candidate;
        }
    }

    const auto runner_up = second != nullptr ? second->lists : 0;
    ID3D12CommandQueue* engine = nullptr;
    if (first != nullptr && first->lists >= MIN_LISTS && first->lists >= 2 * runner_up &&
        2 * first->engine_thread_lists >= first->lists)
    {
        engine = first->queue;
    }

    const Submission result{entry->type == D3D12_COMMAND_LIST_TYPE_DIRECT, engine != nullptr && engine == queue};

    if (engine == m_engine_queue) {
        return result;
    }

    m_engine_queue = engine;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_engine_queue_logged < std::chrono::seconds(5)) {
        return result;
    }

    m_engine_queue_logged = now;
    const auto first_queue = first != nullptr ? first->queue : nullptr;
    const auto first_lists = first != nullptr ? first->lists : 0;
    const auto first_engine_lists = first != nullptr ? first->engine_thread_lists : 0;
    lock.unlock();

    if (engine != nullptr) {
        spdlog::info("[NativeStereoFix][D3D12] Pair snapshots follow engine queue {:x} '{}' ({} recent command lists, {} "
            "from the pose callback's thread; next direct queue {})",
            (uintptr_t)engine, debug_name(engine), first_lists, first_engine_lists, runner_up);
    } else {
        spdlog::info("[NativeStereoFix][D3D12] No engine queue for pair snapshots: busiest direct queue {:x} has {} recent "
            "command lists, {} from the pose callback's thread; next direct queue {}",
            (uintptr_t)first_queue, first_lists, first_engine_lists, runner_up);
    }

    return result;
}

void NativePairSnapshot::on_post_execute_command_lists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const*) {
    if (queue == nullptr) {
        return;
    }

    const auto self = GetCurrentThreadId();
    const bool engine_thread = m_armed_thread.load(std::memory_order_relaxed) == self;

    // A Present pass's own thread, when it is not the engine's, submits that pass's work (frame generation's, the
    // OpenXR runtime's), not the engine's frames.
    if (!engine_thread && m_present_thread.load(std::memory_order_relaxed) == self) {
        return;
    }

    const auto submission = tally(queue, count, engine_thread);

    // The engine's graphics queue only: its async compute and copy queues run beside it, not in its order.
    if (!engine_thread || !submission.direct || !m_armed.load(std::memory_order_acquire)) {
        return;
    }

    if (!submission.engine_queue) {
        // Unordered with the engine's rendering: the arm waits for its queue's next submission (or expires).
        std::scoped_lock _{m_mutex};
        ++m_stats.other_queue_skips;
        return;
    }

    take_snapshot(queue);
}

void NativePairSnapshot::take_snapshot(ID3D12CommandQueue* queue) {
    std::shared_ptr<Ring> ring{};
    Sources sources{};
    uint32_t index{};
    uint32_t frame{};
    uint64_t generation{};
    uint64_t value{};

    // The tally caches each queue's type by pointer; a direct command list on a recycled compute queue's address
    // would remove the device.
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }

    {
        std::unique_lock lock{m_mutex};

        if (!m_armed.load(std::memory_order_relaxed) || m_armed_thread.load(std::memory_order_relaxed) != GetCurrentThreadId()) {
            return;
        }

        m_armed.store(false, std::memory_order_release);
        ring = m_ring;
        frame = m_armed_frame;
        generation = m_armed_generation;

        if (m_suspended || m_failed) {
            return;
        }

        if (std::chrono::steady_clock::now() - m_armed_time > ARM_LIFETIME) {
            ++m_stats.expired_arms;
            return;
        }

        if (ring == nullptr) {
            // The engine submits its frames from the thread that runs the pose callback: the next configure()
            // creates the ring.
            const bool first = !m_submitter_seen;
            m_submitter_seen = true;
            lock.unlock();

            if (first) {
                spdlog::info(
                    "[NativeStereoFix][D3D12] The engine submits its frames from the pose callback's thread {} (queue {:x} '{}'); "
                    "creating the pair snapshot ring",
                    GetCurrentThreadId(), (uintptr_t)queue, debug_name(queue));
            }
            return;
        }

        if (m_producing) {
            ++m_stats.skipped_busy;
            return;
        }

        if (generation != m_sources.right_generation) {
            ++m_stats.skipped_sources;
            return;
        }

        if (queue != m_checked_queue) {
            ComPtr<ID3D12Device> queue_device{};
            queue->GetDevice(IID_PPV_ARGS(&queue_device));
            m_checked_queue = queue;
            m_checked_queue_ok = utility::is_same_d3d12_device(queue_device.Get(), ring->device.Get());
        }

        if (!m_checked_queue_ok) {
            ++m_stats.skipped_sources;
            return;
        }

        // Never the slot a Present pass may copy from (published) or is copying from (pinned).
        bool found = false;
        for (uint32_t step = 1; step <= SLOT_COUNT && !found; ++step) {
            const auto candidate = (m_cursor + step) % SLOT_COUNT;
            if (static_cast<int32_t>(candidate) == m_published || static_cast<int32_t>(candidate) == m_pinned ||
                !ring->slot_free(candidate))
            {
                continue;
            }
            index = candidate;
            found = true;
        }

        if (!found) {
            ++m_stats.skipped_busy;
            return;
        }

        sources = m_sources;
        value = ++ring->fence_value;
        m_producing = true;
    }

    // Recorded without the lock; the slot is neither published nor pinned, and m_producing keeps retire() off it.
    auto& slot = ring->slots[index];
    bool executed = false;
    bool signaled = false;
    HRESULT hr = slot.allocator->Reset();
    if (SUCCEEDED(hr)) {
        hr = slot.list->Reset(slot.allocator.Get(), nullptr);
    }

    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER to_copy[3]{
            transition(sources.left.Get(), SOURCE_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(sources.right.Get(), SOURCE_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(slot.texture.Get(), SLOT_STATE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.list->ResourceBarrier(3, to_copy);

        const auto dst = subresource0(slot.texture.Get());
        const auto left = subresource0(sources.left.Get());
        const auto right = subresource0(sources.right.Get());
        const D3D12_BOX left_box{0, 0, 0, sources.eye_width, sources.eye_height, 1};
        const D3D12_BOX right_box{0, 0, 0, sources.right_width, sources.right_height, 1};
        slot.list->CopyTextureRegion(&dst, 0, 0, 0, &left, &left_box);
        slot.list->CopyTextureRegion(&dst, sources.eye_width, 0, 0, &right, &right_box);

        D3D12_RESOURCE_BARRIER back[3]{
            transition(sources.left.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, SOURCE_STATE),
            transition(sources.right.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, SOURCE_STATE),
            transition(slot.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, SLOT_STATE),
        };
        slot.list->ResourceBarrier(3, back);
        hr = slot.list->Close();
    }

    if (SUCCEEDED(hr)) {
        // Through the hooked entry; the observer is not re-entered (D3D12Hook marks calls made from inside it).
        ID3D12CommandList* const lists[]{slot.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        executed = true;
        hr = queue->Signal(ring->fence.Get(), value);
        signaled = SUCCEEDED(hr);
    }

    std::scoped_lock _{m_mutex};
    m_producing = false;

    if (!signaled) {
        ++m_stats.failures;
        if (executed) {
            // Its GPU work can't be tracked: no more snapshots, and the slot is never reused.
            m_failed = true;
            slot.written = UINT64_MAX;
        } else if (ring->fence_value == value) {
            --ring->fence_value; // never submitted: nothing will signal it
        }
        SPDLOG_ERROR_EVERY_N_SEC(5, "[NativeStereoFix][D3D12] Pair snapshot failed (hr=0x{:08x}{})",
            static_cast<uint32_t>(hr), executed ? ", fence signal; snapshots off for the session" : "");
        return;
    }

    slot.written = value;
    slot.left = sources.left;
    slot.right = sources.right;
    slot.capture_generation = generation;
    slot.queue = queue;
    slot.frame = frame;
    slot.uses = 0;
    slot.time = std::chrono::steady_clock::now();

    if (ring == m_ring && !m_suspended && sources == m_sources) {
        m_published = static_cast<int32_t>(index);
        m_cursor = index;
        ++m_stats.snapshots;
    }
}

std::optional<NativePairSnapshot::Lease> NativePairSnapshot::acquire(std::chrono::milliseconds max_age, const char** reason) {
    const auto fail = [&](const char* why) -> std::optional<Lease> {
        if (reason != nullptr) {
            *reason = why;
        }
        return std::nullopt;
    };

    std::scoped_lock _{m_mutex};

    if (m_suspended) {
        return fail("no pair snapshot ring");
    }

    const auto self = GetCurrentThreadId();
    const auto engine_thread = m_armed_thread.load(std::memory_order_relaxed);
    m_present_thread.store(self, std::memory_order_relaxed);

    // Each reason is a separate literal: the caller logs each one once.
    if (engine_thread == 0) {
        return fail("waiting for the engine's first pose callback");
    }

    if (engine_thread == self) {
        // Without frame generation the engine presents on its own thread, after it submitted the frame and before it
        // records the next one: the live targets hold one frame's pair, and copying them is exact.
        return fail("the Present runs on the engine's own thread, between its frames (no frame generation)");
    }

    m_last_foreign_present = std::chrono::steady_clock::now();

    if (m_ring == nullptr) {
        if (m_submitter_seen) {
            return fail("no pair snapshot ring");
        }

        return fail(m_stats.missed_arms + m_stats.expired_arms >= 120
            ? "the pose callback's thread never submits to the engine queue (the engine submits from another thread)"
            : "waiting to see the engine submit a frame after its pose callback");
    }

    if (m_published < 0) {
        return fail("no eye pair frozen at an engine frame boundary yet");
    }

    auto& slot = m_ring->slots[m_published];
    if (slot.left.Get() != m_sources.left.Get() || slot.right.Get() != m_sources.right.Get() ||
        slot.capture_generation != m_sources.right_generation)
    {
        return fail("the eye targets changed since the last frozen pair");
    }

    if (std::chrono::steady_clock::now() - slot.time > max_age) {
        return fail("no eye pair frozen recently (the engine's submissions are not being observed)");
    }

    // The engine keeps rendering while snapshots stop (no free slot, or its frames are submitted elsewhere): past
    // one skipped frame, the live targets are newer than this pair by more than a frozen image is worth.
    const auto frames_behind = static_cast<int32_t>(m_last_arm_frame - slot.frame);
    if (frames_behind < 0 || frames_behind > MAX_FRAMES_BEHIND) {
        ++m_stats.stale;
        return fail("the engine rendered newer frames than the last frozen pair");
    }

    m_pinned = m_published;

    if (m_armed.load(std::memory_order_relaxed)) {
        ++m_stats.pending_at_acquire;
    }

    Lease lease{};
    lease.ring = m_ring;
    lease.texture = slot.texture.Get();
    lease.fence = m_ring->fence.Get();
    lease.fence_value = slot.written;
    lease.queue = slot.queue.Get();
    lease.slot = static_cast<uint32_t>(m_published);
    lease.frame = slot.frame;
    lease.width = static_cast<uint32_t>(m_ring->desc.Width);
    lease.height = m_ring->desc.Height;
    lease.uses = slot.uses++;
    return lease;
}

bool NativePairSnapshot::record_copy(const Lease& lease, CommandContext& commands, ID3D12Resource* target,
    ID3D12CommandQueue* consumer_queue)
{
    if (lease.ring == nullptr || lease.texture == nullptr || target == nullptr || consumer_queue == nullptr) {
        return false;
    }

    std::scoped_lock _{commands.mtx};

    if (!commands.ready()) {
        return false;
    }

    // On the snapshot's own queue the copy already runs after it.
    if (consumer_queue != lease.queue && !reached(lease.fence, lease.fence_value) &&
        FAILED(consumer_queue->Wait(lease.fence, lease.fence_value)))
    {
        return false;
    }

    D3D12_RESOURCE_BARRIER to_copy = transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    commands.cmd_list->ResourceBarrier(1, &to_copy);

    const auto dst = subresource0(target);
    const auto src = subresource0(lease.texture);
    const D3D12_BOX box{0, 0, 0, lease.width, lease.height, 1};
    commands.cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    D3D12_RESOURCE_BARRIER back = transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands.cmd_list->ResourceBarrier(1, &back);
    commands.has_commands = true;
    return true;
}

void NativePairSnapshot::release(const Lease& lease, ID3D12CommandQueue* consumer_queue, bool copied) {
    if (lease.ring == nullptr) {
        return;
    }

    auto& ring = *lease.ring;
    const bool track = copied && consumer_queue != nullptr;
    uint64_t value{};
    bool signaled = false;

    if (track) {
        {
            // Only the Present thread advances the reader fence.
            std::scoped_lock _{m_mutex};
            value = ring.reader_value + 1;
        }

        // Without the lock (the engine's thread takes it inside its ExecuteCommandLists). The slot stays pinned until
        // its read value is recorded below, so no snapshot can be written into it meanwhile.
        signaled = SUCCEEDED(consumer_queue->Signal(ring.reader_fence.Get(), value));
    }

    std::unique_lock lock{m_mutex};

    if (track) {
        if (signaled) {
            ring.reader_value = value;
            ring.slots[lease.slot].read = value;
        } else {
            // The copy can't be tracked: keep its slot out of use.
            ring.slots[lease.slot].read = UINT64_MAX;
            ++m_stats.failures;
            SPDLOG_ERROR_EVERY_N_SEC(5, "[NativeStereoFix][D3D12] Could not track a copy out of a pair snapshot slot");
        }
    }

    if (m_pinned == static_cast<int32_t>(lease.slot) && lease.ring == m_ring) {
        m_pinned = -1;
    }

    if (!signaled) {
        return;
    }

    if (lease.ring != m_ring) {
        // The ring was retired while this pass held it: it is released once this lease goes, so wait for the copy.
        lock.unlock();
        if (!wait_fence(ring.reader_fence.Get(), value, ring.event, RETIRE_GPU_WAIT_MS)) {
            leak(lease.ring, "a Present-pass copy out of a retired ring did not finish in time");
        }
    }
}

NativePairSnapshot::Stats NativePairSnapshot::stats() const {
    std::scoped_lock _{m_mutex};
    return m_stats;
}
}
