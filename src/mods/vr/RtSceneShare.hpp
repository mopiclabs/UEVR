#pragma once

// EXPERIMENT (VR_ExperimentRightEyeRTShare, default 0 = off; Hogwarts Legacy defaults to 2 with the cull option on, see
// VR::on_config_load): the Native Stereo Fix renders the right eye as a second view family in the same engine frame,
// and a UE 4.26/4.27 renderer builds a ray tracing scene for every view it renders
// (FDeferredShadingSceneRenderer::DispatchRayTracingWorldUpdates: RHICreateRayTracingScene, a TLAS build and the
// hit-group shader binding tables per view). Hogwarts Legacy (4.27, RT reflections + RT AO on) spends ~8 ms of RHI
// thread and ~6 ms of render thread per frame on it, half of it for the right eye. Here the right eye's renderer borrows
// the scene the left eye's renderer built in the same frame: its RHICreateRayTracingScene returns the left scene, its
// TLAS build and its hit-group bindings into that scene are skipped (they would be the same TLAS, and the right's own
// bindings carry instance indices of the right's own instance list), the left's ClearRayTracingBindings is held back
// until the right is done, and everything per view (ray generation, miss/light records, dispatches) stays the right's.
//
// Nothing here uses addresses of one game build: the context's RHIBuildAccelerationStructure(Scene) is the only vtable
// slot holding the thunk "mov rax,rdx; mov r8d,4 (PREFER_FAST_TRACE); mov rdx,rcx; mov rcx,rax; jmp", the other context
// slots are its interface neighbours (validated by what they call and by the arguments they see), and
// RHICreateRayTracingScene is the FDynamicRHI slot that constructs objects of the scene class seen in those builds.
// Mode 1 only observes and checks the per-frame invariants; mode 2 shares, after mode 1's checks passed for
// kValidationFrames frame pairs in the same session, and goes back to observing on any violation.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include <utility/Module.hpp>
#include <utility/PointerHook.hpp>
#include <utility/Scan.hpp>
#include <utility/String.hpp>

#include <sdk/CVar.hpp>
#include <sdk/ConsoleManager.hpp>
#include <sdk/DynamicRHI.hpp>

namespace uevr::rt_share {
// 3 = in-run A/B: shares in even 10-second windows of QPC time and observes in odd ones (abcpu.py <cell> 10).
enum class Mode : int32_t { Off = 0, Probe = 1, Share = 2, ShareAlternating = 3 };

namespace detail {
constexpr uint32_t kValidationFrames = 240;
constexpr int32_t kClearSlotDelta = 3;         // RHIClearRayTracingBindings(Scene)
constexpr int32_t kSetHitGroupsSlotDelta = 7;  // RHISetRayTracingHitGroups(Scene, Pipeline, Num, Bindings)
constexpr int32_t kSetHitGroupSlotDelta = 8;   // RHISetRayTracingHitGroup(Scene, Instance, Segment, Slot, Pipeline, ...)
constexpr int32_t kSetMissSlotDelta = 10;      // RHISetRayTracingMissShader(Scene, Slot, Pipeline, ...), only checked
constexpr int32_t kDispatchSlotDelta = 6;      // RHIRayTraceDispatch(Pipeline, RayGenShader, Scene, GlobalBindings, W, H)

enum class Role { None, Left, Right };

struct SceneState {
    uint32_t frame{};
    bool left{};                 // created by the left eye's renderer
    bool right_probe{};          // created by the right eye's renderer while observing
    bool shared{};               // the right eye's renderer received this scene
    bool share_armed{};          // left scene of a frame whose right eye will borrow it
    bool right_phase{};          // the left's clear was held back: calls on this scene now come from the right eye
    uint32_t builds{};
    std::vector<uintptr_t> left_pipelines{};
    std::vector<std::pair<uintptr_t, uintptr_t>> left_dispatches{};  // (pipeline, ray generation shader) the left dispatched
};

struct State {
    std::mutex install_mtx{};
    std::atomic<int32_t> mode{0};
    std::atomic<bool> context_hooked{false};
    std::atomic<bool> create_hooked{false};
    std::atomic<bool> failed{false};

    std::vector<std::unique_ptr<PointerHook>> hooks{};
    uintptr_t build_thunk{};
    void* orig_build{};
    void* orig_clear{};
    void* orig_set_hit_groups{};
    void* orig_set_hit_group{};
    void* orig_dispatch{};
    void* orig_create{};
    std::atomic<uintptr_t> scene_vtable{};

    // game thread -> render thread. The family's frame number is not set yet when the game thread submits it (the
    // renderer stamps its own copy), so submissions are counted instead: left_seq counts left submissions whose right
    // eye follows, right_seq the right submissions done; the render thread renders the left families in submission
    // order and counts them in consumed_seq. Both families are recognized by their render targets, which each
    // submission records in its ring slot: a render target recreated between submission and render (a resolution
    // change) must not make the render thread miss a left family and fall a submission behind for good.
    std::atomic<uint64_t> left_seq{0};
    std::atomic<uint64_t> right_seq{0};
    uint64_t consumed_seq{0};    // render thread
    bool right_queued{};         // render thread: the current pair's right family was already submitted

    // The share decision is made once per left submission on the game thread (mode, mode 3's window, validation) and
    // both sides follow it: the right eye's cull hold (game thread) and the arming of the left scene (render thread).
    // Deciding twice let them disagree for a frame or two after a violation, and then the right eye rendered its culled
    // (nearly empty) instance list without the left's scene.
    static constexpr size_t kIntentRing = 16;
    struct Submission {
        std::atomic<uint8_t> intent{};
        std::atomic<uintptr_t> left_target{};
        std::atomic<uintptr_t> right_target{};
    };
    Submission ring[kIntentRing]{};              // by left_seq; written before left_seq is published
    std::atomic<bool> current_intent{false};     // game thread: decision of the latest left submission
    bool left_intent{};                          // render thread: decision of the pair being rendered
    std::atomic<uint64_t> pair_resyncs{0};       // left families found further ahead than the next submission

    // RHI thread: the pipelines the left eye bound into its scene this frame (mode 1 compares the right's against them)
    std::vector<uintptr_t> last_left_pipelines{};
    uintptr_t last_left_scene{};

    // render thread only
    Role role{Role::None};
    uint32_t role_frame{};
    uint32_t creates_in_family{};
    uintptr_t left_scene{};      // the left eye's scene of the current pair (never dereferenced unless left_scene_ref)
    bool left_scene_ref{};       // we hold a reference on it (armed frames only)
    uint32_t left_scene_frame{};
    uint32_t left_instances{};
    uint32_t last_pair_frame{UINT32_MAX};
    uint32_t pair_left_creates{}, pair_right_creates{};

    // render thread + RHI thread
    std::mutex scenes_mtx{};
    std::unordered_map<uintptr_t, SceneState> scenes{};

    std::atomic<uint32_t> valid_pairs{0};
    std::atomic<bool> validated{false};

    // statistics, logged every 10 s
    std::atomic<uint64_t> pairs{0}, bad_pairs{0}, shared_frames{0}, builds_skipped{0}, clears_held{0},
        hit_groups_skipped{0}, hit_group_skipped{0}, pipeline_mismatch{0}, instance_mismatch{0}, stale_refs{0},
        right_without_stash{0}, set_hit_group_calls{0},
        pipeline_mismatch_shared{0},  // shared frames whose right eye used a pipeline the left did not (its hit groups stay empty)
        intended_not_shared{0},       // pairs decided "share" whose right eye still got its own scene
        dispatch_redirected{0},       // right-eye dispatches moved to the left's pipeline (see hook_dispatch)
        dispatch_unmatched{0};        // right-eye dispatches with a pipeline unknown to the left and no left match
    std::chrono::steady_clock::time_point last_log{};
};

inline State& state() {
    static State s{};
    return s;
}

inline bool readable(const void* p, size_t n) {
    return p != nullptr && !IsBadReadPtr(p, n);
}

// First direct call target within the first max_insns instructions of fn.
inline std::optional<uintptr_t> first_call(uintptr_t fn, size_t max_insns = 40) {
    uintptr_t ip = fn;
    for (size_t i = 0; i < max_insns; ++i) {
        if (!readable((void*)ip, 16)) {
            return std::nullopt;
        }
        const auto* b = (const uint8_t*)ip;
        if (b[0] == 0xE8) {
            return utility::calculate_absolute(ip + 1);
        }
        if (b[0] == 0xC3 || b[0] == 0xCC) {
            return std::nullopt;
        }
        const auto len = utility::get_insn_size(ip);
        if (len == 0) {
            return std::nullopt;
        }
        ip += len;
    }
    return std::nullopt;
}

// Direct call targets within the first max_insns instructions of fn (stops at ret/int3).
inline std::vector<uintptr_t> calls(uintptr_t fn, size_t max_insns) {
    std::vector<uintptr_t> out{};
    uintptr_t ip = fn;
    for (size_t i = 0; i < max_insns && readable((void*)ip, 16); ++i) {
        const auto* b = (const uint8_t*)ip;
        if (b[0] == 0xC3 || b[0] == 0xCC) {
            break;
        }
        if (b[0] == 0xE8) {
            out.push_back(utility::calculate_absolute(ip + 1));
        }
        const auto len = utility::get_insn_size(ip);
        if (len == 0) {
            break;
        }
        ip += len;
    }
    return out;
}

// Whether fn (or one call level below it) loads the address 'target' RIP-relatively within max_insns instructions.
inline bool references(uintptr_t fn, uintptr_t target, size_t max_insns, bool follow_calls) {
    uintptr_t ip = fn;
    for (size_t i = 0; i < max_insns && readable((void*)ip, 16); ++i) {
        const auto insn = utility::decode_one((uint8_t*)ip);
        if (!insn) {
            return false;
        }
        if (const auto disp = utility::resolve_displacement(ip, &*insn); disp && *disp == target) {
            return true;
        }
        if (follow_calls && ((const uint8_t*)ip)[0] == 0xE8 &&
            references(utility::calculate_absolute(ip + 1), target, max_insns, false))
        {
            return true;
        }
        if (((const uint8_t*)ip)[0] == 0xC3) {
            return false;
        }
        ip += insn->Length;
    }
    return false;
}

inline void add_ref(uintptr_t resource) {
    // FRHIResource: vtable, then the reference count (FThreadSafeCounter NumRefs) at +8
    InterlockedIncrement((volatile LONG*)(resource + 8));
}

inline int32_t ref_count(uintptr_t resource) {
    return *(volatile LONG*)(resource + 8);
}

// Drops our reference without ever taking a resource to zero behind the engine's back (the engine's own Release
// queues it for deferred deletion, which we cannot reach): a reference that would be the last one is kept and
// counted (a leak of one scene, logged).
inline void drop_ref(uintptr_t resource) {
    auto* refs = (volatile LONG*)(resource + 8);
    for (;;) {
        const LONG v = *refs;
        if (v <= 1) {
            ++state().stale_refs;
            return;
        }
        if (InterlockedCompareExchange(refs, v - 1, v) == v) {
            return;
        }
    }
}

void install_create_hook();
void maybe_log();

inline void note_violation(const char* why) {
    auto& s = state();
    ++s.bad_pairs;
    s.valid_pairs = 0;
    if (s.validated.exchange(false)) {
        SPDLOG_WARN("[RtShare] Sharing stopped (back to observing): {}", why);
    } else {
        SPDLOG_INFO_EVERY_N_SEC(10, "[RtShare] Frame pair rejected: {}", why);
    }
}

// --- RHI thread (command context vtable) ---

inline void hook_build(void* ctx, uintptr_t scene, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    auto& s = state();
    if (scene != 0 && s.scene_vtable.load() == 0 && readable((void*)scene, 8)) {
        s.scene_vtable = *(uintptr_t*)scene;
        SPDLOG_INFO("[RtShare] Ray tracing scene class vtable {:x} (from the first TLAS build)", s.scene_vtable.load());
    }
    {
        std::scoped_lock _{s.scenes_mtx};
        if (auto it = s.scenes.find(scene); it != s.scenes.end()) {
            if (it->second.right_phase && it->second.builds >= 1) {
                ++s.builds_skipped;
                return;
            }
            ++it->second.builds;
        }
    }
    ((void (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))s.orig_build)(ctx, scene, a3, a4, a5, a6);
}

inline void hook_clear(void* ctx, uintptr_t scene, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    auto& s = state();
    {
        std::scoped_lock _{s.scenes_mtx};
        if (auto it = s.scenes.find(scene); it != s.scenes.end()) {
            if (it->second.share_armed && !it->second.right_phase) {
                // The left eye is done with it; the right eye's dispatches still need the shader tables.
                it->second.right_phase = true;
                ++s.clears_held;
                return;
            }
            s.scenes.erase(it);
        }
    }
    ((void (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))s.orig_clear)(ctx, scene, a3, a4, a5, a6);
}

inline void hook_set_hit_groups(void* ctx, uintptr_t scene, uintptr_t pipeline, uintptr_t num, uintptr_t bindings, uintptr_t a6) {
    auto& s = state();
    {
        std::scoped_lock _{s.scenes_mtx};
        if (auto it = s.scenes.find(scene); it != s.scenes.end()) {
            auto& st = it->second;
            if (st.right_phase) {
                const bool known = std::find(st.left_pipelines.begin(), st.left_pipelines.end(), pipeline) != st.left_pipelines.end();
                if (!known) {
                    // The left never bound this pipeline into the scene: the right's records would carry indices of
                    // the right's own instance list. Leave its hit groups empty; hook_dispatch sends the right eye's
                    // dispatches with the left's pipeline instead.
                    ++s.pipeline_mismatch;
                    ++s.pipeline_mismatch_shared;
                }
                ++s.hit_groups_skipped;
                return;
            }
            if (st.left) {
                if (s.last_left_scene != scene) {
                    s.last_left_scene = scene;
                    s.last_left_pipelines.clear();
                }
                if (std::find(s.last_left_pipelines.begin(), s.last_left_pipelines.end(), pipeline) == s.last_left_pipelines.end()) {
                    s.last_left_pipelines.push_back(pipeline);
                }
            } else if (st.right_probe &&
                std::find(s.last_left_pipelines.begin(), s.last_left_pipelines.end(), pipeline) == s.last_left_pipelines.end())
            {
                // Observing: sharing would leave this right-eye pipeline without hit groups.
                ++s.pipeline_mismatch;
            }
            if (std::find(st.left_pipelines.begin(), st.left_pipelines.end(), pipeline) == st.left_pipelines.end()) {
                st.left_pipelines.push_back(pipeline);
            }
        }
    }
    ((void (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))s.orig_set_hit_groups)(ctx, scene, pipeline, num, bindings, a6);
}

using SetHitGroupFn = void (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
inline void hook_set_hit_group(void* ctx, uintptr_t scene, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7,
    uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12, uintptr_t a13, uintptr_t a14)
{
    auto& s = state();
    ++s.set_hit_group_calls;
    {
        std::scoped_lock _{s.scenes_mtx};
        if (auto it = s.scenes.find(scene); it != s.scenes.end() && it->second.right_phase) {
            ++s.hit_group_skipped;
            return;
        }
    }
    ((SetHitGroupFn)s.orig_set_hit_group)(ctx, scene, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

// RHIRayTraceDispatch. With non-blocking pipeline creation (r.RayTracing.NonBlockingPipelineCreation) each view renders
// with its view state's previous pipeline until the new one has compiled (BindRayTracingMaterialPipeline). When the
// compile finishes between the two eyes' renders, the right eye gets the new pipeline while the shared scene only has
// the left's hit groups (for the previous pipeline), and its dispatch would find an empty table: no reflections or AO in
// the right eye for that frame. Then the right eye's dispatch is sent with the left's pipeline instead, but only as a
// replay of a dispatch the left made in this frame on this scene with the same ray generation shader (the fallback
// pipeline is signature-compatible, so the ray generation shaders are the same set): the right eye then shows the same
// fallback materials the left eye shows in that frame.
inline void hook_dispatch(void* ctx, uintptr_t pipeline, uintptr_t ray_gen, uintptr_t scene, uintptr_t bindings,
    uintptr_t width, uintptr_t height)
{
    auto& s = state();
    {
        std::scoped_lock _{s.scenes_mtx};
        if (auto it = s.scenes.find(scene); it != s.scenes.end()) {
            auto& st = it->second;
            if (st.right_phase) {
                // Pipelines without hit groups (no SetHitGroups call) count as known when the left dispatched them.
                const bool known = std::find(st.left_pipelines.begin(), st.left_pipelines.end(), pipeline) != st.left_pipelines.end() ||
                    std::any_of(st.left_dispatches.begin(), st.left_dispatches.end(), [&](const auto& d) { return d.first == pipeline; });
                if (!known) {
                    const auto match = std::find_if(st.left_dispatches.begin(), st.left_dispatches.end(),
                        [&](const auto& d) { return d.second == ray_gen; });
                    if (match != st.left_dispatches.end()) {
                        pipeline = match->first;
                        ++s.dispatch_redirected;
                    } else {
                        ++s.dispatch_unmatched;
                    }
                }
            } else if (st.left) {
                const std::pair<uintptr_t, uintptr_t> d{pipeline, ray_gen};
                if (std::find(st.left_dispatches.begin(), st.left_dispatches.end(), d) == st.left_dispatches.end()) {
                    st.left_dispatches.push_back(d);
                }
            }
        }
    }
    ((void (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))s.orig_dispatch)(
        ctx, pipeline, ray_gen, scene, bindings, width, height);
}

// --- render thread (FDynamicRHI vtable) ---

inline void** hook_create(void* rhi, void** ret, const void* initializer) {
    auto& s = state();
    const auto mode = (Mode)s.mode.load();
    const auto instances = readable(initializer, 16) ? *(const uint32_t*)((uintptr_t)initializer + 8) : 0u;
    ++s.creates_in_family;

    if (s.role == Role::Right && s.left_scene != 0 && s.left_scene_frame == s.role_frame && s.creates_in_family == 1) {
        ++s.pair_right_creates;
        if (instances != s.left_instances) {
            ++s.instance_mismatch;
        }
        bool armed = false;
        {
            std::scoped_lock _{s.scenes_mtx};
            if (auto it = s.scenes.find(s.left_scene); it != s.scenes.end() && it->second.share_armed) {
                it->second.shared = true;
                armed = true;
            }
        }
        if (armed && s.left_scene_ref) {
            // Hand our reference to the right eye's view (its FRayTracingSceneRHIRef owns it from here).
            *ret = (void*)s.left_scene;
            s.left_scene = 0;
            s.left_scene_ref = false;
            ++s.shared_frames;
            return ret;
        }
        if (s.left_intent) {
            ++s.intended_not_shared;
        }
        if (s.left_scene_ref) {
            drop_ref(s.left_scene);
        }
        s.left_scene = 0;
        s.left_scene_ref = false;
    } else if (s.role == Role::Right) {
        ++s.pair_right_creates;
        ++s.right_without_stash;
        if (s.left_intent && s.creates_in_family == 1) {
            ++s.intended_not_shared;
        }
    }

    auto* result = ((void** (*)(void*, void**, const void*))s.orig_create)(rhi, ret, initializer);
    const auto scene = (uintptr_t)*result;

    if (s.role == Role::Right && scene != 0 && s.creates_in_family == 1) {
        std::scoped_lock _{s.scenes_mtx};
        auto& st = s.scenes[scene];
        st = SceneState{};
        st.frame = s.role_frame;
        st.right_probe = true;
    }

    if (s.role == Role::Left && scene != 0) {
        ++s.pair_left_creates;
        if (s.left_scene != 0) {
            // Last pair's right eye never took it.
            if (s.left_scene_ref) {
                drop_ref(s.left_scene);
            }
            std::scoped_lock _{s.scenes_mtx};
            s.scenes.erase(s.left_scene);
            s.left_scene = 0;
            s.left_scene_ref = false;
        }
        if (s.creates_in_family == 1) {
            // The game thread's decision for this pair (see State::intent), not a fresh look at s.validated: the right
            // family was already submitted with or without the cull hold according to it.
            const bool armed = s.left_intent && s.right_queued && (mode == Mode::Share || mode == Mode::ShareAlternating);
            if (armed) {
                // Our reference keeps it alive across the right family's RHI flush (FDeferredShadingSceneRenderer::
                // Render flushes pending deletes between its instance gathering and its scene creation).
                add_ref(scene);
            }
            s.left_scene = scene;
            s.left_scene_ref = armed;
            s.left_scene_frame = s.role_frame;
            s.left_instances = instances;
            std::scoped_lock _{s.scenes_mtx};
            auto& st = s.scenes[scene];
            st = SceneState{};
            st.frame = s.role_frame;
            st.left = true;
            st.share_armed = armed;
        }
    }
    return result;
}

// Ends a frame pair (left + right family of one engine frame) for the invariants mode 1 checks.
inline void close_pair() {
    auto& s = state();
    if (s.last_pair_frame == UINT32_MAX) {
        return;
    }
    ++s.pairs;
    static uint64_t last_pipeline_mismatch{0};
    const auto pipeline_mismatch = s.pipeline_mismatch.load();
    if (s.pair_left_creates != 1 || s.pair_right_creates != 1) {
        note_violation(s.pair_left_creates != 1 ? "the left family did not create exactly one ray tracing scene" :
            "the right family did not create exactly one ray tracing scene");
    } else if (pipeline_mismatch != last_pipeline_mismatch) {
        note_violation("the right eye bound hit groups for a pipeline the left eye did not");
    } else if (!s.validated && ++s.valid_pairs >= kValidationFrames) {
        s.validated = true;
        SPDLOG_INFO("[RtShare] {} consecutive frame pairs with one ray tracing scene per eye: sharing is {}",
            kValidationFrames, (Mode)s.mode.load() == Mode::Share ? "on" :
                (Mode)s.mode.load() == Mode::ShareAlternating ? "on in even 10 s windows" : "allowed (mode 1 only observes)");
    }
    s.pair_left_creates = s.pair_right_creates = 0;
    last_pipeline_mismatch = pipeline_mismatch;
}
}  // namespace detail

// Game thread, just before the left family's BeginRenderingViewFamily when the right eye will follow in this frame.
inline void on_left_submission(int32_t mode, uintptr_t left_target, uintptr_t right_target) {
    using namespace detail;
    auto& s = state();
    s.mode = mode;
    s.current_intent = false;
    if (mode == 0 || s.failed) {
        return;
    }
    bool intent = (Mode)mode == Mode::Share && s.validated;
    if ((Mode)mode == Mode::ShareAlternating && s.validated) {
        LARGE_INTEGER qpc{}, frequency{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&frequency);
        intent = (qpc.QuadPart / frequency.QuadPart / 10) % 2 == 0;
    }
    auto& slot = s.ring[(s.left_seq.load() + 1) % State::kIntentRing];
    slot.intent = intent ? 1 : 0;
    slot.left_target = left_target;
    slot.right_target = right_target;
    s.current_intent = intent;
    ++s.left_seq;

    if (!s.context_hooked) {
        std::scoped_lock _{s.install_mtx};
        if (!s.context_hooked && !s.failed) {
            const auto module = utility::get_executable();
            const auto thunk = utility::scan(module, "48 8B C2 41 B8 04 00 00 00 48 8B D1 48 8B C8 E9");
            if (!thunk || utility::scan(*thunk + 1, utility::get_module_size(module).value_or(0) - (*thunk + 1 - (uintptr_t)module),
                "48 8B C2 41 B8 04 00 00 00 48 8B D1 48 8B C8 E9"))
            {
                SPDLOG_WARN("[RtShare] RHIBuildAccelerationStructure(Scene) thunk not found or not unique; off");
                s.failed = true;
                return;
            }
            s.build_thunk = *thunk;
            std::vector<uintptr_t> slots{};
            for (auto cursor = (uintptr_t)module;;) {
                const auto end = (uintptr_t)module + utility::get_module_size(module).value_or(0);
                const auto slot = utility::scan_ptr(cursor, end - cursor, *thunk);
                if (!slot) {
                    break;
                }
                slots.push_back(*slot);
                cursor = *slot + 8;
            }
            if (slots.empty()) {
                SPDLOG_WARN("[RtShare] No context vtable slot holds the TLAS build thunk {:x}; off", *thunk);
                s.failed = true;
                return;
            }

            // Interface neighbours: SetRayTracingHitGroups and SetRayTracingMissShader both start by looking up the
            // scene's shader table for the pipeline (the same callee).
            const auto slot0 = slots.front();
            const auto fn = [](uintptr_t slot) { return *(uintptr_t*)slot; };
            const auto hit_groups_call = first_call(fn(slot0 + 8 * kSetHitGroupsSlotDelta));
            const auto miss_call = first_call(fn(slot0 + 8 * kSetMissSlotDelta));
            const auto hit_group_call = first_call(fn(slot0 + 8 * kSetHitGroupSlotDelta));
            if (!hit_groups_call || !miss_call || *hit_groups_call != *miss_call) {
                SPDLOG_WARN("[RtShare] Context slots +{} / +{} do not share the shader table lookup ({:x} / {:x}); off",
                    kSetHitGroupsSlotDelta, kSetMissSlotDelta, hit_groups_call.value_or(0), miss_call.value_or(0));
                s.failed = true;
                return;
            }
            const bool hook_single = hit_group_call && *hit_group_call == *miss_call;
            // RHIRayTraceDispatch also starts with the shader table lookup (scene, pipeline). Optional: without it the
            // right eye's frame with a pipeline the left did not use keeps empty hit groups.
            const auto dispatch_call = first_call(fn(slot0 + 8 * kDispatchSlotDelta));
            bool hook_dispatch_slot = dispatch_call && *dispatch_call == *miss_call;
            for (const auto slot : slots) {
                if (fn(slot + 8 * kDispatchSlotDelta) != fn(slot0 + 8 * kDispatchSlotDelta)) {
                    hook_dispatch_slot = false;   // optional: share without the redirect
                }
            }

            for (const auto slot : slots) {
                for (const auto delta : {0, kClearSlotDelta, kSetHitGroupsSlotDelta, kSetHitGroupSlotDelta}) {
                    if (fn(slot + 8 * delta) != fn(slot0 + 8 * delta)) {
                        SPDLOG_WARN("[RtShare] Context vtables {:x} and {:x} differ at +{}; off", slot0, slot, delta);
                        s.failed = true;
                        return;
                    }
                }
            }

            for (const auto slot : slots) {
                SPDLOG_INFO("[RtShare] Context vtable slot {:x}: build {:x}, clear {:x}, hit groups {:x}, hit group {:x}{}, dispatch {:x}{}",
                    slot, fn(slot), fn(slot + 8 * kClearSlotDelta), fn(slot + 8 * kSetHitGroupsSlotDelta),
                    fn(slot + 8 * kSetHitGroupSlotDelta), hook_single ? "" : " (not hooked: no shader table lookup)",
                    fn(slot + 8 * kDispatchSlotDelta), hook_dispatch_slot ? "" : " (not hooked: no shader table lookup)");
                if (hook_dispatch_slot) {
                    s.orig_dispatch = (void*)fn(slot + 8 * kDispatchSlotDelta);
                    s.hooks.push_back(std::make_unique<PointerHook>((void**)(slot + 8 * kDispatchSlotDelta), (void*)&hook_dispatch));
                }
                s.orig_build = (void*)fn(slot);
                s.orig_clear = (void*)fn(slot + 8 * kClearSlotDelta);
                s.orig_set_hit_groups = (void*)fn(slot + 8 * kSetHitGroupsSlotDelta);
                s.hooks.push_back(std::make_unique<PointerHook>((void**)slot, (void*)&hook_build));
                s.hooks.push_back(std::make_unique<PointerHook>((void**)(slot + 8 * kClearSlotDelta), (void*)&hook_clear));
                s.hooks.push_back(std::make_unique<PointerHook>((void**)(slot + 8 * kSetHitGroupsSlotDelta), (void*)&hook_set_hit_groups));
                if (hook_single) {
                    s.orig_set_hit_group = (void*)fn(slot + 8 * kSetHitGroupSlotDelta);
                    s.hooks.push_back(std::make_unique<PointerHook>((void**)(slot + 8 * kSetHitGroupSlotDelta), (void*)&hook_set_hit_group));
                }
            }
            s.context_hooked = true;
        }
    }

    if (!s.create_hooked && s.scene_vtable.load() != 0) {
        install_create_hook();
    }
    maybe_log();
}

// Game thread, after the right family of the latest left submission was submitted.
inline void on_right_submitted() {
    auto& s = detail::state();
    if (s.mode.load() != 0) {
        s.right_seq = s.left_seq.load();
    }
    s.current_intent = false;  // a frame without on_left_submission must not reuse this decision
}

// Render thread, at the start of every view family's render (UEVR's PreRenderViewFamily_RenderThread).
inline void on_family_render(uint32_t frame, uintptr_t target) {
    using namespace detail;
    auto& s = state();
    if (s.mode.load() == 0 || !s.create_hooked) {
        // Turned off at run time: let go of a left scene still waiting for its right eye (its held clear is then
        // done by the scene's destructor).
        if (s.left_scene_ref) {
            drop_ref(s.left_scene);
        }
        s.left_scene = 0;
        s.left_scene_ref = false;
        s.role = Role::None;
        return;
    }
    Role role = Role::None;
    const uint64_t newest = s.left_seq.load();
    if (target == 0) {
    } else if (s.consumed_seq > 0 && target == s.ring[s.consumed_seq % State::kIntentRing].right_target.load()) {
        role = Role::Right;
    } else {
        // The next left submission in order (or, if a left family was missed, a later one within the ring); its right
        // eye is already queued when right_seq has reached it.
        for (uint64_t seq = s.consumed_seq + 1; seq <= newest && seq < s.consumed_seq + State::kIntentRing; ++seq) {
            const auto& slot = s.ring[seq % State::kIntentRing];
            if (target == slot.left_target.load()) {
                if (seq != s.consumed_seq + 1) {
                    ++s.pair_resyncs;
                }
                role = Role::Left;
                s.consumed_seq = seq;
                s.right_queued = s.right_seq.load() >= seq;
                s.left_intent = slot.intent.load() != 0;
                break;
            }
        }
    }
    if (role == Role::Left && s.last_pair_frame != frame) {
        close_pair();
        s.last_pair_frame = frame;
    }
    s.role = role;
    s.role_frame = frame;
    s.creates_in_family = 0;
}

namespace detail {
inline void install_create_hook() {
    auto& s = state();
    std::scoped_lock _{s.install_mtx};
    if (s.create_hooked || s.failed) {
        return;
    }
    auto* rhi = sdk::FDynamicRHI::get();
    if (rhi == nullptr || !readable(rhi, 8)) {
        return;
    }
    const auto vtable = *(uintptr_t*)rhi;
    const auto scene_vtable = s.scene_vtable.load();
    // RHICreateRayTracingScene: allocate, construct (the constructor installs the scene class vtable), store into
    // the returned reference, AddRef ("lock inc dword ptr [rax+8]").
    std::vector<uint32_t> found{};
    for (uint32_t i = 1; i < 400; ++i) {
        const auto entry = vtable + 8 * i;
        if (!readable((void*)entry, 8)) {
            break;
        }
        const auto f = *(uintptr_t*)entry;
        if (!readable((void*)f, 0x60)) {
            continue;
        }
        bool add_ref_shape = false;
        for (size_t k = 0; k + 4 <= 0x60; ++k) {
            const auto* b = (const uint8_t*)f + k;
            if (b[0] == 0xF0 && b[1] == 0xFF && b[2] == 0x40 && b[3] == 0x08) {
                add_ref_shape = true;
                break;
            }
        }
        if (!add_ref_shape) {
            continue;
        }
        const auto c = calls(f, 24);
        if (c.size() >= 2 && references(c[1], scene_vtable, 120, true)) {
            found.push_back(i);
        }
    }
    if (found.size() != 1) {
        SPDLOG_WARN("[RtShare] RHICreateRayTracingScene: {} FDynamicRHI slots construct the scene class {:x}; off",
            found.size(), scene_vtable);
        s.failed = true;
        return;
    }
    const auto entry = vtable + 8 * found[0];
    s.orig_create = (void*)*(uintptr_t*)entry;
    s.hooks.push_back(std::make_unique<PointerHook>((void**)entry, (void*)&hook_create));
    s.create_hooked = true;
    SPDLOG_INFO("[RtShare] RHICreateRayTracingScene = FDynamicRHI slot {} ({:x}); observing frame pairs", found[0], (uintptr_t)s.orig_create);
}

inline void maybe_log() {
    auto& s = state();
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last_log < std::chrono::seconds{10}) {
        return;
    }
    s.last_log = now;
    if (!s.create_hooked) {
        return;
    }
    SPDLOG_INFO("[RtShare] mode {} validated {} pairs {} bad {} shared {} | builds skipped {} clears held {} hit-group batches skipped {} "
        "single skipped {} (calls {}) | pipeline mismatch {} (in shared frames {}) instance mismatch {} right without stash {} "
        "stale refs {} intended but not shared {} pair resyncs {} | right dispatches moved to the left's pipeline {} unmatched {}",
        s.mode.load(), s.validated.load(), s.pairs.load(), s.bad_pairs.load(), s.shared_frames.load(), s.builds_skipped.load(),
        s.clears_held.load(), s.hit_groups_skipped.load(), s.hit_group_skipped.load(), s.set_hit_group_calls.load(),
        s.pipeline_mismatch.load(), s.pipeline_mismatch_shared.load(), s.instance_mismatch.load(), s.right_without_stash.load(),
        s.stale_refs.load(), s.intended_not_shared.load(), s.pair_resyncs.load(), s.dispatch_redirected.load(),
        s.dispatch_unmatched.load());
}
}  // namespace detail

// Game thread, between the left and the right submission: whether this frame's right eye is to borrow the left eye's
// scene (the decision on_left_submission made; the render thread arms the left scene by the same decision).
inline bool right_eye_shares_this_frame() {
    return detail::state().current_intent.load();
}

// VR_ExperimentRightEyeRTShareCullGather: while the right eye borrows the left eye's scene, its own instance list goes
// unused (its scene creation returns the left's, its TLAS build and hit-group bindings are skipped), so most of its
// GatherRayTracingWorldInstances and the binding work after it is wasted. Holding the ray tracing culling cvars for
// the right eye's submission only (render-thread safe, like the other second-submission holds) leaves that list nearly
// empty: Hogwarts Legacy's r.RayTracing.Culling.MinDistance (an engine extension) culls everything; on a stock
// 4.26/4.27 renderer, r.RayTracing.Culling 1 with radius 0 and angle 180 culls everything behind the camera.
// Hogwarts Legacy 4.27, blade18 2026-11-06: render thread -1.5 ms, RHI thread -1.2 ms per frame on top of the share,
// eyes unchanged.
class RightGatherCullHold {
public:
    explicit RightGatherCullHold(bool hold) {
        if (!hold) {
            return;
        }
        if (auto* min_distance = find(L"r.RayTracing.Culling.MinDistance"); min_distance != nullptr) {
            set(min_distance, L"1000000000");
        } else {
            set(find(L"r.RayTracing.Culling"), L"1");
            set(find(L"r.RayTracing.Culling.Radius"), L"0");
            set(find(L"r.RayTracing.Culling.Angle"), L"180");
        }
        if (!m_held.empty()) {
            SPDLOG_INFO_ONCE("[RtShare] Culling the right eye's own ray tracing instance gathering ({} cvars held)", m_held.size());
        }
    }

    ~RightGatherCullHold() {
        for (auto it = m_held.rbegin(); it != m_held.rend(); ++it) {
            try {
                it->first->Set(it->second.c_str());
            } catch (...) {
            }
        }
    }

    RightGatherCullHold(const RightGatherCullHold&) = delete;
    RightGatherCullHold& operator=(const RightGatherCullHold&) = delete;

private:
    static sdk::IConsoleVariable* find(const wchar_t* name) {
        static std::unordered_map<std::wstring, sdk::IConsoleVariable*> cache{};
        if (auto it = cache.find(name); it != cache.end()) {
            return it->second;
        }
        const auto console_manager = sdk::FConsoleManager::get();
        if (console_manager == nullptr) {
            return nullptr;   // not cached: look again next time
        }
        sdk::IConsoleVariable* cvar{};
        auto* object = console_manager->find(name);
        if (object != nullptr && object->AsCommand() == nullptr) {
            cvar = static_cast<sdk::IConsoleVariable*>(object);
            // Held on the game thread around the right submission: only a render-thread-safe cvar (the render thread
            // gets the value in order with that family's render) applies to that render alone. Otherwise the render
            // thread reads the live value whenever it runs, which could be during the left eye's render (the scene
            // the right eye borrows). FConsoleVariableBase: vtable, FString Help, EConsoleVariableFlags at +0x18
            // (UE 4.26-5.x); ECVF_RenderThreadSafe = 0x20.
            const auto flags_at = (const uint32_t*)((uintptr_t)cvar + 0x18);
            const bool flags_ok = detail::readable(flags_at, 4);
            const uint32_t flags = flags_ok ? *flags_at : 0u;
            if (!flags_ok || (flags & 0x20) == 0) {
                SPDLOG_WARN("[RtShare] {} is not render-thread safe (flags {:x}); the right eye's gathering is not culled with it",
                    utility::narrow(std::wstring{name}), flags);
                cvar = nullptr;
            }
        }
        cache[name] = cvar;
        return cvar;
    }

    void set(sdk::IConsoleVariable* cvar, const wchar_t* value) {
        if (cvar == nullptr) {
            return;
        }
        try {
            auto before = cvar->TryGetFloat();
            if (!before) {
                (void)cvar->GetFloat(); // locates the getter for this vtable; its 0-on-failure result is not used
                before = cvar->TryGetFloat();
            }
            if (!before) {
                return;
            }
            wchar_t text[64]{};
            if (*before == std::floor(*before) && std::fabs(*before) < 1e15) {
                swprintf_s(text, L"%lld", (long long)*before);
            } else {
                swprintf_s(text, L"%.9g", (double)*before);   // a float round-trips in 9 significant digits
            }
            if (cvar->Set(value)) {
                m_held.emplace_back(cvar, text);
            }
        } catch (...) {
        }
    }

    std::vector<std::pair<sdk::IConsoleVariable*, std::wstring>> m_held{};
};
}  // namespace uevr::rt_share
