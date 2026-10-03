#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <d3d12.h>

#include "hooks/D3D12Hook.hpp"
#include "ComPtr.hpp"

namespace d3d12 {
struct CommandContext;

// VR_NativeStereoFixPairSnapshot (D3D12): Native Stereo Fix eye pairs frozen on the engine's own timeline.
//
// The fix builds every submitted double-wide image from two live engine targets: the engine's double-wide target
// (left eye) and the scene-capture target the right eye is rendered into, one scene render later. A Present pass
// that ran between frame F+1's left render and its right render copied left F+1 next to right F. With 2x frame
// generation that happens all the time: the hooked Present runs twice per engine frame, off the frame boundaries.
//
// Instead, once per engine frame the pair is copied into an owned ring slot shaped like the DOUBLE_WIDE swapchain
// image, on the engine's own direct queue, right behind the engine's submission of that frame. The frame's pose
// callback on the render-submission thread (the hijacked RHI command that enqueues its poses, which runs once both
// eye families are recorded) arms the snapshot; the next ExecuteCommandLists that thread makes on a direct queue
// (D3D12Hook's execute observer) is followed by the snapshot on that queue. Everything recorded before the callback
// is on the queue by then, and nothing of the next frame: the engine flushes its work before it presents. Each
// Present pass then copies the newest published slot, after a GPU-side wait on its fence when it runs on another
// queue, and copies the live targets as before while no snapshot is available (startup, Native Fix transitions).
//
// The ring (SLOT_COUNT images, VRAM) is only created once the thread that runs the pose callback was seen making a
// direct-queue submission after it. An engine that submits from another thread (UE 5.1+'s submission thread) never
// gets one, and keeps the live copy without the VRAM.
//
// Which submission fires an armed snapshot (on_post_execute_command_lists()):
// - Only one to the engine queue: the direct queue that carries most of the game's command lists by a clear margin,
//   most of them from the thread that runs the pose callback. A submission that thread makes to any other direct queue
//   (a frame generation library working inside the engine's Present) is unordered with the engine's rendering, so the
//   arm waits for the engine queue's next one. UEVR's own submissions, and those made on a Present pass's thread when
//   that is not the engine's, are not counted.
// - Within ARM_LIFETIME of the arm: an older arm belongs to an earlier frame and would fire in the middle of a later
//   one (the left eye of that later frame next to the right eye of the earlier one, the split this class removes).
// - Only while some Present pass ran off the engine's thread within FOREIGN_PRESENT_WINDOW. Without frame generation
//   the engine calls Present itself, on the pose callback's thread and between its frames: those passes copy the live
//   targets, which then hold one frame's pair (acquire() refuses them a lease), and no snapshot is taken at all.
//
// Threads: configure(), suspend(), retire(), acquire() and release() on the Present thread; arm() on the
// render-submission thread; on_post_execute_command_lists() inside the game's ExecuteCommandLists calls. One mutex
// guards the state and is never held across a D3D12 submission, signal or wait (the engine's thread takes it from
// inside its ExecuteCommandLists). Only retire() and a release() on a retired ring wait on the GPU: the engine's thread
// never does (a slot still in use skips that frame's snapshot, and the passes keep the previous pair).
class NativePairSnapshot final : public D3D12ExecuteObserver {
public:
    static constexpr uint32_t SLOT_COUNT = 3;
    static constexpr auto ARM_LIFETIME = std::chrono::milliseconds(100);
    static constexpr auto FOREIGN_PRESENT_WINDOW = std::chrono::seconds(2);

    // What a snapshot copies, with the live copy's boxes: the left eye (0,0)-(eye_width,eye_height) of `left`, and
    // the right eye (0,0)-(right_width,right_height) of `right`, placed at x = eye_width. Both sources rest in the
    // RENDER_TARGET state, as the live copy assumes.
    struct Sources {
        ComPtr<ID3D12Resource> left{};
        ComPtr<ID3D12Resource> right{};
        uint64_t right_generation{};
        uint32_t eye_width{};
        uint32_t eye_height{};
        uint32_t right_width{};
        uint32_t right_height{};

        bool operator==(const Sources& other) const {
            return left.Get() == other.left.Get() && right.Get() == other.right.Get() &&
                right_generation == other.right_generation && eye_width == other.eye_width &&
                eye_height == other.eye_height && right_width == other.right_width && right_height == other.right_height;
        }
    };

    struct Ring;

    // The newest published pair, lent to one Present pass until release().
    struct Lease {
        std::shared_ptr<Ring> ring{};
        ID3D12Resource* texture{};       // owned by `ring`
        ID3D12Fence* fence{};            // owned by `ring`; the slot is complete at fence_value
        uint64_t fence_value{};
        ID3D12CommandQueue* queue{};     // the engine queue the snapshot ran on (held by `ring`)
        uint32_t slot{};
        uint32_t frame{};                // the pose frame of the callback that armed it
        uint32_t width{};
        uint32_t height{};
        uint32_t uses{};                 // Present passes that took this snapshot before this one
    };

    struct Stats {
        uint64_t snapshots{};            // published
        uint64_t skipped_busy{};         // no free slot (the GPU still writing or reading all of them)
        uint64_t skipped_sources{};      // the armed frame's capture no longer matched the configured sources
        uint64_t missed_arms{};          // a frame armed again before its thread made a direct-queue submission
        uint64_t failures{};
        // Present passes that took the previous pair while the next frame was armed but not yet copied: its pose
        // callback had run, so such a pass may submit that newer frame's pose with the older pair.
        uint64_t pending_at_acquire{};
        uint64_t stale{};                // passes refused the pair: newer engine frames were armed and not copied
        uint64_t expired_arms{};         // armed longer than ARM_LIFETIME before an engine-queue submission
        uint64_t other_queue_skips{};    // armed thread's submissions to a direct queue that is not the engine queue
        uint64_t engine_thread_arms{};   // frames not armed: every recent Present ran on the engine's thread
        uint64_t previous_pair{};        // passes that took the pair before the newest: the newest was still on the GPU
        uint64_t unfinished{};           // passes with no finished pair (the live targets were copied)
    };

    NativePairSnapshot() = default;
    NativePairSnapshot(const NativePairSnapshot&) = delete;
    NativePairSnapshot& operator=(const NativePairSnapshot&) = delete;
    ~NativePairSnapshot() override;

    // Present thread: points the next snapshots at `sources` and (re)creates the ring for images shaped like
    // `target_desc` once the engine's submission thread was seen (until then acquire() has no pair). False, with the
    // reason, when no snapshot can be taken (the caller copies the live targets).
    bool configure(ID3D12Device* device, const D3D12_RESOURCE_DESC& target_desc, const Sources& sources, const char** reason);
    // Present thread: no more snapshots, and none published, until the next configure() (Native Fix not active).
    void suspend();
    // Present thread: waits (bounded) until the GPU is done with the ring, then releases it.
    void retire();

    // Render-submission thread, once per engine frame after both eye families were recorded.
    void arm(uint32_t frame, uint64_t capture_generation);
    // Render-submission thread: this frame takes no snapshot (the Native Fix did not publish it). Drops an older arm
    // that would otherwise fire inside this frame.
    void disarm();

    void on_post_execute_command_lists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) override;

    // Present thread: the newest published pair if it was taken from the configured sources within max_age, at most
    // two armed engine frames before the newest one.
    std::optional<Lease> acquire(std::chrono::milliseconds max_age, const char** reason);
    // Inside the copy's pre-commands: makes `consumer_queue` wait for the snapshot (not on the snapshot's own queue,
    // nor once it completed) and records the copy of the slot into `target` (RENDER_TARGET before and after).
    static bool record_copy(const Lease& lease, CommandContext& commands, ID3D12Resource* target, ID3D12CommandQueue* consumer_queue);
    // Present thread, after the copy was executed on `consumer_queue` (copied) or not recorded at all.
    void release(const Lease& lease, ID3D12CommandQueue* consumer_queue, bool copied);

    Stats stats() const;

    static std::string debug_name(ID3D12Object* object);

private:
    struct Submission {
        bool direct{};                   // to a direct queue
        bool engine_queue{};             // to the engine queue (see the class comment)
    };

    // Counts a game submission (any thread but UEVR's) and says whether it went to the engine queue.
    Submission tally(ID3D12CommandQueue* queue, UINT count, bool from_engine_thread);
    void take_snapshot(ID3D12CommandQueue* queue);

    mutable std::mutex m_mutex{};
    std::shared_ptr<Ring> m_ring{};
    Sources m_sources{};
    int32_t m_published{-1};
    int32_t m_previous_published{-1}; // published before m_published; lent while that is still being written
    int32_t m_pinned{-1};
    uint32_t m_cursor{};
    bool m_suspended{true};
    bool m_producing{};
    bool m_failed{};
    bool m_submitter_seen{};                // an armed thread made a direct-queue submission (the ring may exist)
    ID3D12CommandQueue* m_checked_queue{};  // the last queue whose device was compared with the ring's
    bool m_checked_queue_ok{};
    Stats m_stats{};

    std::atomic<bool> m_armed{};
    std::atomic<uint32_t> m_armed_thread{};
    uint32_t m_armed_frame{};
    uint32_t m_last_arm_frame{};            // the newest frame armed (acquire() refuses pairs trailing it too far)
    uint64_t m_armed_generation{};
    std::chrono::steady_clock::time_point m_armed_time{};
    std::chrono::steady_clock::time_point m_last_foreign_present{}; // a Present pass off the engine's thread
    std::atomic<uint32_t> m_present_thread{};  // the thread of the last Present pass

    // Recent game submissions per queue, decayed (tally()).
    struct QueueTally {
        ID3D12CommandQueue* queue{};
        D3D12_COMMAND_LIST_TYPE type{};
        uint64_t lists{};
        uint64_t engine_thread_lists{};  // from the thread that runs the pose callback
    };

    std::mutex m_tally_mutex{};
    std::array<QueueTally, 8> m_tally{};
    ID3D12CommandQueue* m_engine_queue{};
    std::chrono::steady_clock::time_point m_engine_queue_logged{};
};
}
