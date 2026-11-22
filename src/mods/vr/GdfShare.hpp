#pragma once

// VR_NativeStereoFixShareGlobalDistanceField (default on; acts only where the block below is found and verified):
// under the Native Stereo Fix the right eye's render uses the left eye's Global Distance Field for that frame instead of
// its own, which the right eye's refresh (VR_NativeStereoFixSecondGlobalDistanceFieldRefreshFrames) otherwise rebuilds.
//
// Why: the Native Stereo Fix renders the right eye as a second view family with a view state of its own, and each view
// state keeps its own Global Distance Field (clipmap states, update index, camera vectors, page atlas, coverage atlas,
// page tables, mip texture and page buffers). The scene's list of primitives that changed this frame is used up by the
// first render, so the right eye's clipmaps never learn of them; the refresh rebuilds them in full every Nth frame
// (about 8 ms on a 3080 laptop per rebuild, a spike up to 5.5 ms on the 4090 laptop). Sharing gives the right eye the
// field the left eye's render just brought up to date: Black Myth: Wukong (UE 5.0, 4090 laptop) right eye GPU -0.9 ms a
// frame, +3 % fps on a still scene and on an 11-minute walk, no 15-frame rebuild spikes, eyes as bright as each other.
//
// How: when the right family's render starts on the render thread (UEVR's PreRenderViewFamily_RenderThread), the GDF
// blocks of the two view states are exchanged. They are exchanged back on the render thread in the same frame, right
// after the frame's scene renders: at the start of Slate's DrawWindow_RenderThread (UEVR's hook), which the game thread
// enqueues after the viewport's scene renders. The top of every PreRenderViewFamily_RenderThread callback (before any
// early return) exchanges back too, as a backstop. Sharing needs the Slate hook to be called: where it isn't (not
// installed, or not called for 120 frames), nothing is shared. Exchanging, not copying: the block holds
// reference-counted pointers, and whatever the right render assigns to them (a reallocated texture, an RDG extraction)
// stays owned exactly once and goes back to the left view state with the exchange. Before exchanging back, the right
// render's change to the block's update index is undone, so UE's staggered clipmap schedule (power-of-two periods
// picked from that index) steps once per frame as with one view; the right render then sees the index the left render
// just used and has almost nothing to update. In frames where the right eye shares, its refresh is skipped.
//
// Finding the block (no addresses): the field holding the pooled render target named "GlobalDistanceField.PageAtlas"
// is searched in the first 16 KB of both view states (same offset in both; until found, every 60 frames, the interval
// doubling up to 600); the eight other GDF resources are checked by debug name at their places around it, on the game
// thread (deciding for the frame), again on the render thread (just before the exchange) and before the exchange back.
// The block's extent and the update index are not resources and can't be checked by name: they come from a table of
// validated engine layouts, today UE 5.0 only, validated on one title (Black Myth: Wukong's GDF update code and memory
// dumps of both eyes). Before the first exchange a self-check confirms the update index's place: over 8 consecutive
// frames both eyes' index goes up by exactly one per render (renders that leave both unchanged count neither way; 64
// other changes: off for the session). Any other engine version: off, one log line, the refresh as before.
//
// Failures (one log line per kind): block not found or a resource not verified on the game thread -> no exchange that
// frame; on the render thread, or the render thread consuming none of 30 frames the game thread chose to share -> the
// right eye's field is rebuilt in the next frame and sharing stays off for 600 frames; a failed check before the
// exchange back (a view state gone) -> nothing is written, sharing is off for the session, and the left eye's next render
// rebuilds its field in full once (its view state may keep the right eye's block). When sharing stops, the right
// eye's own (stale) field is rebuilt in that frame. Every read of engine memory before a write is SEH-guarded.
// UE 4.x keeps a different distance field (clipmap volume textures, no page atlas): never found.
// VR_DebugShareGlobalDistanceFieldFailVerify (debug, default 0): 1 fails the game thread's check, 2 the render thread's,
// to check the failure path.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <spdlog/spdlog.h>

#include <sdk/Utility.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>

namespace uevr::gdf_share {
namespace detail {
inline bool seh_read(uintptr_t address, uintptr_t* out) {
    __try {
        *out = *(const volatile uintptr_t*)address;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool read(uintptr_t a, uintptr_t& v) {
    return a >= 0x10000 && a <= 0x7fffffffffffull && seh_read(a, &v);
}

inline bool read_i32(uintptr_t a, int32_t& v) {
    uintptr_t q{};
    if (!read(a, q)) {
        return false;
    }
    v = (int32_t)(uint32_t)q;
    return true;
}

// The object at p carries a const TCHAR* debug name in qword name_index equal to the ASCII string name.
inline bool named(uintptr_t p, size_t name_index, const char* name) {
    uintptr_t s{};
    if ((p & 7) != 0 || !read(p + name_index * 8, s) || (s & 1) != 0) {
        return false;
    }
    for (size_t i = 0;; i += 4) {
        uintptr_t w4{};
        if (!read(s + i * 2, w4)) {
            return false;
        }
        for (size_t k = 0; k < 4; ++k) {
            const auto ch = (uint16_t)(w4 >> (16 * k));
            const auto want = (uint8_t)name[i + k];
            if (ch != want) {
                return false;
            }
            if (want == 0) {
                return true;
            }
        }
    }
}

struct Resource {
    ptrdiff_t from_anchor;
    size_t name_index;  // 10: FPooledRenderTarget desc debug name; 16: FRDGPooledBuffer name
    const char* name;
};
inline constexpr Resource kResources[] = {
    {-0x10, 16, "GlobalDistanceField.PageFreeListAllocator"},
    {-0x08, 16, "GlobalDistanceField.PageFreeList"},
    {0x00, 10, "GlobalDistanceField.PageAtlas"},
    {0x08, 10, "GlobalDistanceField.CoverageAtlas"},
    {0x10, 16, "GlobalDistanceField.PageObjectGridBuffer"},
    {0x18, 10, "GlobalDistanceField.PageTableCombined"},
    {0x20, 10, "GlobalDistanceField.PageTableStationaryLayer"},
    {0x28, 10, "GlobalDistanceField.PageTableMovableLayer"},
    {0x30, 10, "GlobalDistanceField.MipTexture"},
};

// Validated engine layouts of the view state's GDF block, relative to the PageAtlas field.
struct Layout {
    const char* engine;            // "major.minor"
    ptrdiff_t block_from_anchor;   // flags, 6 clipmap states, update index, camera vectors, then the nine resources
    size_t block_size;
    ptrdiff_t update_index_from_anchor;  // int32, ++ per render's GDF update
};
// UE 5.0, validated on Black Myth: Wukong only: flags at -0x268, 6 clipmap states of 0x58 from -0x260, update index at -0x50.
inline constexpr Layout kLayouts[] = {
    {"5.0", -0x268, 0x2a0, -0x50},
};
inline constexpr size_t kAnchorSearchEnd = 0x4000;
inline constexpr uint32_t kAnchorRetryFirst = 60;
inline constexpr uint32_t kAnchorRetryMax = 600;
inline constexpr uint32_t kBackoffFrames = 600;       // after a render-thread failure: frames without sharing
inline constexpr uint32_t kStallFrames = 30;          // shared frames without render-thread progress = a failure
inline constexpr uint32_t kPostRenderPointFrames = 120;  // no Slate DrawWindow_RenderThread call for this long: no sharing
inline constexpr uint32_t kSelfCheckFrames = 8;
inline constexpr uint32_t kSelfCheckMaxMisses = 64;
inline constexpr uint32_t kRing = 8;

enum Failure : uint32_t {
    NoLayout, NoPostRenderPoint, SameTarget, AnchorNotFound, AnchorMismatch, GameThreadCheck, RenderThreadCheck,
    RenderThreadStalled, LayoutSelfCheck, ExchangeBackCheck, ForcedGameThread, ForcedRenderThread, FailureCount
};
inline const char* kFailureText[FailureCount] = {
    "no validated Global Distance Field layout for this engine version",
    "no render-thread point after the frame's scene renders (Slate DrawWindow_RenderThread hook not called)",
    "the right eye's family has the left eye's render target",
    "Global Distance Field block not found in the view states (yet)",
    "the two view states hold the Global Distance Field block at different offsets",
    "a Global Distance Field resource did not verify on the game thread",
    "a Global Distance Field resource did not verify on the render thread; off for 600 frames",
    "the render thread took none of 30 frames chosen for sharing; off for 600 frames",
    "the update index did not go up by one per render; off for this session",
    "a view state failed its check before the exchange back (nothing written); off for this session",
    "game thread check failed on purpose (VR_DebugShareGlobalDistanceFieldFailVerify=1)",
    "render thread check failed on purpose (VR_DebugShareGlobalDistanceFieldFailVerify=2); off for 600 frames",
};

struct Entry {
    std::atomic<uint64_t> seq{0};
    std::atomic<uintptr_t> right_target{0};
    std::atomic<uintptr_t> left_state{0};
    std::atomic<uintptr_t> right_state{0};
    std::atomic<bool> share{false};
};

struct State {
    std::atomic<size_t> anchor{0};               // offset of the PageAtlas field, 0 = not found yet
    std::atomic<const Layout*> layout{nullptr};  // set once by the game thread, read by the render thread
    std::atomic<bool> layout_confirmed{false};   // the render thread's update-index self-check passed
    std::atomic<bool> disabled{false};           // off for the session
    std::atomic<bool> rt_failed{false};          // render thread -> game thread: an exchange it chose was declined
    std::atomic<uint64_t> rt_progress{0};        // render thread: ring entries taken
    std::atomic<uint64_t> post_render_points{0}; // render thread: Slate DrawWindow_RenderThread calls
    std::atomic<uint32_t> logged{0};             // bit per Failure kind
    Entry ring[kRing]{};
    std::atomic<uint64_t> submitted{0};
    // game thread only
    bool layout_checked{false};
    uint32_t anchor_wait{0};
    uint32_t anchor_interval{kAnchorRetryFirst};
    bool last_shared{false};
    uint32_t backoff{0};
    uint64_t last_progress{0};
    uint32_t frames_without_progress{0};
    uint64_t last_post_points{0};
    uint32_t frames_since_post_point{0xffff};
    // render thread only
    uint64_t consumed{0};
    uintptr_t swapped_left{0};
    uintptr_t swapped_right{0};
    size_t swapped_anchor{0};
    ptrdiff_t swapped_block{0};
    size_t swapped_size{0};
    ptrdiff_t swapped_index_off{0};
    int32_t swapped_index{0};
    uint64_t exchanges{0};
    bool sc_have{false};
    int32_t sc_left{0}, sc_right{0};
    uint32_t sc_streak{0}, sc_misses{0}, sc_neutral{0};
    std::atomic<bool> left_rebuild{false};  // render thread -> game thread: rebuild the left eye's field once
};

inline State& state() {
    static State s{};
    return s;
}

inline void log_once(Failure f) {
    const auto bit = 1u << f;
    if ((state().logged.fetch_or(bit) & bit) == 0) {
        SPDLOG_INFO("[GdfShare] Not shared: {}; the right eye's own distance field and its refresh are used", kFailureText[f]);
    }
}

inline std::string engine_version() {
    const auto s = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
    if (s != "0.00") {
        return s;
    }
    const auto v = sdk::get_file_version_info();
    return std::to_string(HIWORD(v.dwFileVersionMS)) + "." + std::to_string(LOWORD(v.dwFileVersionMS));
}

inline const Layout* layout_for_engine() {
    const auto v = engine_version();
    for (const auto& l : kLayouts) {
        const auto n = std::strlen(l.engine);
        if (v.compare(0, n, l.engine) == 0 && (v.size() == n || v[n] == '.')) {
            SPDLOG_INFO("[GdfShare] UE {}: validated Global Distance Field layout (one title), sharing the left eye's field "
                "with the right eye once the block is found, verified and its update index checked", v);
            return &l;
        }
    }
    SPDLOG_INFO("[GdfShare] UE {}: no validated Global Distance Field layout; sharing is off, the right eye's refresh is used", v);
    return nullptr;
}

inline bool resources_ok(uintptr_t view_state, size_t anchor) {
    for (const auto& r : kResources) {
        uintptr_t p{};
        if (!read(view_state + anchor + r.from_anchor, p) || p == 0 || !named(p, r.name_index, r.name)) {
            return false;
        }
    }
    return true;
}

inline std::optional<size_t> find_anchor(uintptr_t view_state) {
    for (size_t off = 0x40; off < kAnchorSearchEnd; off += 8) {
        uintptr_t p{};
        if (!read(view_state + off, p)) {
            return std::nullopt;
        }
        if (p != 0 && (p & 7) == 0 && named(p, 10, "GlobalDistanceField.PageAtlas")) {
            return off;
        }
    }
    return std::nullopt;
}

// Game thread: both eyes' blocks found and verified.
inline bool verified(const Layout& layout, uintptr_t left_state, uintptr_t right_state) {
    auto& s = state();
    auto anchor = s.anchor.load();
    if (anchor == 0) {
        // until found: every 60 frames, the interval doubling up to 600 (a UE 5.0 game without a distance field)
        if (s.anchor_wait > 0) {
            --s.anchor_wait;
            return false;
        }
        s.anchor_wait = s.anchor_interval;
        s.anchor_interval = (std::min)(s.anchor_interval * 2, kAnchorRetryMax);
        const auto l = find_anchor(left_state);
        const auto r = find_anchor(right_state);
        if (!l || !r) {
            log_once(AnchorNotFound);
            return false;
        }
        if (*l != *r || (ptrdiff_t)*l + layout.block_from_anchor < 0) {
            log_once(AnchorMismatch);
            return false;
        }
        anchor = *l;
        s.anchor = anchor;
        SPDLOG_INFO("[GdfShare] Global Distance Field block found: PageAtlas field +0x{:x}, block +0x{:x}..+0x{:x}", anchor,
            anchor + layout.block_from_anchor, anchor + layout.block_from_anchor + layout.block_size);
    }
    if (!resources_ok(left_state, anchor) || !resources_ok(right_state, anchor)) {
        log_once(GameThreadCheck);
        return false;
    }
    return true;
}

inline void exchange(uintptr_t a, uintptr_t b, size_t anchor, ptrdiff_t block, size_t size) {
    auto* pa = (uint64_t*)(a + anchor + block);
    auto* pb = (uint64_t*)(b + anchor + block);
    for (size_t i = 0; i < size / 8; ++i) {
        const auto t = pa[i];
        pa[i] = pb[i];
        pb[i] = t;
    }
}

// Render thread, at the right family's pre-render before the first exchange: both eyes' update index must go up by
// exactly one per render (the left rendered this frame, the right in the previous one) over 8 consecutive frames.
inline void self_check(uintptr_t l, uintptr_t r, size_t anchor, const Layout& layout) {
    auto& s = state();
    int32_t li{}, ri{};
    if (!read_i32(l + anchor + layout.update_index_from_anchor, li) || !read_i32(r + anchor + layout.update_index_from_anchor, ri)) {
        return;
    }
    // 0 = unchanged (a render without a GDF update: menu, pause, loading), 1 = +1 or the wrap, -1 = anything else
    const auto step = [](int32_t prev, int32_t cur) { return cur == prev ? 0 : (cur == prev + 1 || (cur < prev && cur <= 1)) ? 1 : -1; };
    if (s.sc_have) {
        const auto sl = step(s.sc_left, li), sr = step(s.sc_right, ri);
        if (sl < 0 || sr < 0) {
            s.sc_streak = 0;
            if (++s.sc_misses >= kSelfCheckMaxMisses) {
                log_once(LayoutSelfCheck);
                s.disabled = true;
            }
        } else if (sl == 1 && sr == 1) {
            if (++s.sc_streak >= kSelfCheckFrames && !s.layout_confirmed.load()) {
                s.layout_confirmed = true;
                SPDLOG_INFO("[GdfShare] Update index self-check passed: both eyes +1 per render over {} frames ({} misses, {} "
                    "frames without an update before)", kSelfCheckFrames, s.sc_misses, s.sc_neutral);
            }
        } else {
            ++s.sc_neutral;  // an index that didn't move is neither a pass nor a miss
        }
    }
    s.sc_left = li;
    s.sc_right = ri;
    s.sc_have = true;
}
} // namespace detail

// Render thread: undo the right family's exchange (after the frame's scene renders, and as a backstop at the top of
// every PreRenderViewFamily_RenderThread callback). Both view states are checked first; if one fails (a view state
// destroyed or reused), nothing is written and sharing is off for the session.
inline void exchange_back() {
    using namespace detail;
    auto& s = state();
    if (s.swapped_left == 0) {
        return;
    }
    const auto l = s.swapped_left, r = s.swapped_right;
    s.swapped_left = s.swapped_right = 0;
    if (!resources_ok(l, s.swapped_anchor) || !resources_ok(r, s.swapped_anchor)) {
        // The left view state may now keep the right eye's stale block for good: its next render rebuilds it in full
        // (take_left_rebuild); the right eye rebuilds its own as sharing stops.
        log_once(ExchangeBackCheck);
        s.disabled = true;
        s.left_rebuild = true;
        return;
    }
    *(int32_t*)(r + s.swapped_anchor + s.swapped_index_off) = s.swapped_index;
    exchange(l, r, s.swapped_anchor, s.swapped_block, s.swapped_size);
}

// Game thread, around the first (left-eye) submission: true once after a failed exchange back (hold the full update).
inline bool take_left_rebuild() {
    if (!state().left_rebuild.exchange(false)) {
        return false;
    }
    SPDLOG_INFO("[GdfShare] Left eye's distance field rebuilt in full once (after the failed exchange back)");
    return true;
}

// Render thread, Slate DrawWindow_RenderThread (after the frame's scene renders): exchange back, and count the call
// (sharing needs this point to be alive).
inline void on_post_render_point() {
    exchange_back();
    state().post_render_points.fetch_add(1, std::memory_order_relaxed);
}

// Game thread, before the second (right-eye) submission of a frame. Returns whether the right eye shares the left's
// field in this frame (the caller then skips the right eye's refresh). *refresh_now is set when the right eye stops
// sharing or the render thread failed (its own field is stale): the caller rebuilds it in this frame.
inline bool on_right_submission(bool enabled, int32_t debug_fail, uintptr_t left_target, uintptr_t right_target,
    uintptr_t left_state, uintptr_t right_state, bool* refresh_now) {
    using namespace detail;
    auto& s = state();
    bool share = false;
    bool failed = s.rt_failed.exchange(false);
    // The render thread takes every entry (shared or not); if it took none while this thread kept choosing to share,
    // the right eye has had neither share nor refresh: treat it as a render-thread failure.
    const auto progress = s.rt_progress.load();
    if (progress != s.last_progress) {
        s.last_progress = progress;
        s.frames_without_progress = 0;
    } else if (s.last_shared && ++s.frames_without_progress >= kStallFrames) {
        log_once(RenderThreadStalled);
        s.frames_without_progress = 0;
        failed = true;
    }
    const auto post = s.post_render_points.load(std::memory_order_relaxed);
    if (post != s.last_post_points) {
        s.last_post_points = post;
        s.frames_since_post_point = 0;
    } else if (s.frames_since_post_point < 0xffff) {
        ++s.frames_since_post_point;
    }
    if (failed) {
        s.backoff = kBackoffFrames;
    }
    if (s.disabled.load() || !enabled) {
    } else if (s.backoff > 0) {
        --s.backoff;
    } else if (right_target != 0 && left_state != 0 && right_state != 0 && left_state != right_state) {
        if (!s.layout_checked) {
            s.layout = layout_for_engine();
            s.layout_checked = true;
        }
        const auto* layout = s.layout.load();
        if (layout == nullptr) {
            log_once(NoLayout);
        } else if (s.frames_since_post_point >= kPostRenderPointFrames) {
            log_once(NoPostRenderPoint);
        } else if (left_target == right_target) {
            log_once(SameTarget);
        } else if (debug_fail == 1) {
            log_once(ForcedGameThread);
        } else {
            // until the render thread's update-index self-check has passed, the block is only verified (no exchange)
            share = verified(*layout, left_state, right_state) && s.layout_confirmed.load();
        }
    }
    *refresh_now = (s.last_shared && !share) || failed;
    s.last_shared = share;
    const auto seq = s.submitted.load() + 1;
    auto& e = s.ring[seq % kRing];
    e.right_target = right_target;
    e.left_state = left_state;
    e.right_state = right_state;
    e.share = share;
    e.seq = seq;
    s.submitted = seq;
    return share;
}

// Render thread, PreRenderViewFamily_RenderThread for a family that is rendered for VR (after the hook's early returns):
// the update-index self-check and the exchange for the right eye's family, when the game thread chose it. The right
// family is told apart by its render target (UEVR's own right-eye target; the left family keeps the game's).
inline void on_family_render(int32_t debug_fail, uintptr_t target) {
    using namespace detail;
    auto& s = state();
    exchange_back();
    const auto* layout = s.layout.load();
    if (target == 0 || layout == nullptr) {
        return;
    }
    const auto newest = s.submitted.load();
    const uint64_t first = (std::max)(s.consumed + 1, newest >= kRing ? newest - kRing + 1 : 1);
    for (uint64_t seq = first; seq <= newest; ++seq) {
        auto& e = s.ring[seq % kRing];
        if (e.seq.load() != seq || e.right_target.load() != target) {
            continue;
        }
        s.consumed = seq;
        s.rt_progress.fetch_add(1);
        const auto l = e.left_state.load();
        const auto r = e.right_state.load();
        const auto anchor = s.anchor.load();
        if (anchor != 0 && !s.layout_confirmed.load() && !s.disabled.load() && resources_ok(l, anchor) && resources_ok(r, anchor)) {
            self_check(l, r, anchor, *layout);
        }
        if (!e.share.load()) {
            return;
        }
        if (debug_fail == 2) {
            log_once(ForcedRenderThread);
            s.rt_failed = true;
            return;
        }
        if (s.disabled.load()) {
            s.rt_failed = true;  // turned off since the game thread chose it: refresh in the next frame
            return;
        }
        if (anchor == 0 || !resources_ok(l, anchor) || !resources_ok(r, anchor)) {
            log_once(RenderThreadCheck);
            s.rt_failed = true;
            return;
        }
        exchange(l, r, anchor, layout->block_from_anchor, layout->block_size);
        s.swapped_left = l;
        s.swapped_right = r;
        s.swapped_anchor = anchor;
        s.swapped_block = layout->block_from_anchor;
        s.swapped_size = layout->block_size;
        s.swapped_index_off = layout->update_index_from_anchor;
        s.swapped_index = *(const int32_t*)(r + anchor + layout->update_index_from_anchor);
        if (++s.exchanges == 1 || s.exchanges % 36000 == 0) {
            SPDLOG_INFO("[GdfShare] Right eye renders with the left eye's Global Distance Field (found and verified): {} frames",
                s.exchanges);
        }
        return;
    }
}
} // namespace uevr::gdf_share
