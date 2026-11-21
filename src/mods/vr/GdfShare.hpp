#pragma once

// EXPERIMENT (exp/right-eye-cost, VR_ExperimentRightEyeGdfShare, default 0 = off; 1 = share whenever the block is
// validated; 2 = in-run A/B: share in even 10-second windows of QPC time, today's refresh in odd ones).
//
// The Native Stereo Fix renders the right eye as a second view family with a view state of its own, and each view
// state keeps its own Global Distance Field (clipmap states, update index, camera vectors, page atlas, coverage atlas,
// page tables, mip texture and page buffers). The scene's list of primitives that changed this frame is used up by the
// first render, so the right eye's clipmaps never learn of them, and UEVR rebuilds the right eye's field every Nth frame
// (VR_NativeStereoFixSecondGlobalDistanceFieldRefreshFrames). Here the right eye's render uses the left eye's field
// for that frame instead: when the right family's render starts on the render thread, the GDF blocks of the two view
// states are exchanged, and at the start of the next family's render (the render thread runs families one after
// another) they are exchanged back. Exchanging, not copying: the block holds reference-counted pointers, and whatever
// the right render assigns to them (a reallocated texture, an RDG extraction) stays owned exactly once and goes back to
// the left view state with the exchange. In frames where it shares, the right eye's refresh is skipped.
//
// Finding the block (this prototype): the field holding the pooled render target named
// "GlobalDistanceField.PageAtlas" is searched in both view states (same offset in both); the eight other GDF
// resources are checked by name at their places around it; the block extent relative to that anchor is the layout of
// Black Myth: Wukong's UE 5.0 view state (from its GDF update code: flags at -0x268, 6 clipmap states of 0x58 from
// -0x260, update index at -0x50, camera vectors, then the nine resources). Not validated -> no exchange, and today's
// refresh runs. Every read of engine memory is SEH-guarded. Local experiment, not for push.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

#include <spdlog/spdlog.h>

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
inline constexpr ptrdiff_t kBlockFromAnchor = -0x268;
inline constexpr size_t kBlockSize = 0x2a0;
inline constexpr size_t kAnchorSearchEnd = 0x4000;
inline constexpr uint32_t kRing = 8;

struct Entry {
    std::atomic<uint64_t> seq{0};
    std::atomic<uintptr_t> right_target{0};
    std::atomic<uintptr_t> left_state{0};
    std::atomic<uintptr_t> right_state{0};
    std::atomic<bool> share{false};
};

struct State {
    std::atomic<size_t> anchor{0};  // offset of the PageAtlas field, 0 = not found yet
    Entry ring[kRing]{};
    std::atomic<uint64_t> submitted{0};
    // render thread only
    uint64_t consumed{0};
    uintptr_t swapped_left{0};
    uintptr_t swapped_right{0};
    size_t swapped_anchor{0};
    uint64_t exchanges{0};
    uint64_t declined{0};
    uint64_t not_validated{0};
    bool logged_first{false};
    // per 10-second window (QPC), logged when the window changes: right families seen, exchanged, skipped and why
    std::atomic<uint32_t> gt_wanted{0};         // game thread: share wanted in this frame (mode 1, or a share window)
    std::atomic<uint32_t> gt_not_validated{0};  // game thread: wanted, but the block did not validate
    int64_t window{-1};
    uint32_t w_right{0}, w_exchanged{0}, w_declined{0}, w_rt_not_validated{0};
};

inline State& state() {
    static State s{};
    return s;
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

// Both eyes' blocks validated (finds the anchor on first use).
inline bool validated(uintptr_t left_state, uintptr_t right_state) {
    auto& s = state();
    if (left_state == 0 || right_state == 0 || left_state == right_state) {
        return false;
    }
    auto anchor = s.anchor.load();
    if (anchor == 0) {
        const auto l = find_anchor(left_state);
        const auto r = find_anchor(right_state);
        if (!l || !r || *l != *r || (ptrdiff_t)*l + kBlockFromAnchor < 0) {
            return false;
        }
        anchor = *l;
        s.anchor = anchor;
        SPDLOG_INFO("[GdfShare] Global Distance Field block found: PageAtlas field +0x{:x}, block +0x{:x}..+0x{:x}",
            anchor, anchor + kBlockFromAnchor, anchor + kBlockFromAnchor + kBlockSize);
    }
    return resources_ok(left_state, anchor) && resources_ok(right_state, anchor);
}

inline void exchange(uintptr_t a, uintptr_t b, size_t anchor) {
    auto* pa = (uint64_t*)(a + anchor + kBlockFromAnchor);
    auto* pb = (uint64_t*)(b + anchor + kBlockFromAnchor);
    for (size_t i = 0; i < kBlockSize / 8; ++i) {
        const auto t = pa[i];
        pa[i] = pb[i];
        pb[i] = t;
    }
}

inline bool window_shares() {
    LARGE_INTEGER q{}, f{};
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return ((q.QuadPart / f.QuadPart) / 10) % 2 == 0;
}
} // namespace detail

// Game thread, before the second (right-eye) submission of a frame. Returns whether the right eye shares the left's
// field in this frame (then the caller skips the right eye's refresh).
inline bool on_right_submission(int32_t mode, uintptr_t right_target, uintptr_t left_state, uintptr_t right_state) {
    using namespace detail;
    auto& s = state();
    bool share = false;
    if (mode == 1 || (mode == 2 && window_shares())) {
        ++s.gt_wanted;
        share = right_target != 0 && validated(left_state, right_state);
        if (!share) {
            ++s.gt_not_validated;
        }
    }
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

// Render thread, at the start of every view family's render (UEVR's PreRenderViewFamily_RenderThread).
inline void on_family_render(int32_t mode, uintptr_t target) {
    using namespace detail;
    auto& s = state();
    // The previous family's render is done: exchange back first.
    if (s.swapped_left != 0) {
        exchange(s.swapped_left, s.swapped_right, s.swapped_anchor);
        s.swapped_left = s.swapped_right = 0;
    }
    if (mode == 0 || target == 0) {
        return;
    }
    {
        LARGE_INTEGER q{}, f{};
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        const auto window = (int64_t)((q.QuadPart / f.QuadPart) / 10);
        if (window != s.window) {
            if (s.window >= 0 && s.w_right > 0) {
                SPDLOG_INFO("[GdfShare] window {} ({}): right families {}, exchanged {}, not shared {} (game thread: wanted {}, "
                    "block not validated {}), not validated on the render thread {}",
                    s.window, mode == 2 ? (s.window % 2 == 0 ? "share" : "refresh") : "share", s.w_right, s.w_exchanged,
                    s.w_declined, s.gt_wanted.load(), s.gt_not_validated.load(), s.w_rt_not_validated);
            }
            s.window = window;
            s.w_right = s.w_exchanged = s.w_declined = s.w_rt_not_validated = 0;
            s.gt_wanted = 0;
            s.gt_not_validated = 0;
        }
    }
    const auto newest = s.submitted.load();
    for (uint64_t seq = s.consumed + 1; seq <= newest && seq + kRing > newest; ++seq) {
        auto& e = s.ring[seq % kRing];
        if (e.seq.load() != seq || e.right_target.load() != target) {
            continue;
        }
        s.consumed = seq;
        ++s.w_right;
        if (!e.share.load()) {
            ++s.declined;
            ++s.w_declined;
            return;
        }
        const auto l = e.left_state.load();
        const auto r = e.right_state.load();
        const auto anchor = s.anchor.load();
        // validated again here: the game thread checked a frame ago
        if (anchor == 0 || !resources_ok(l, anchor) || !resources_ok(r, anchor)) {
            ++s.not_validated;
            ++s.w_rt_not_validated;
            return;
        }
        ++s.w_exchanged;
        exchange(l, r, anchor);
        s.swapped_left = l;
        s.swapped_right = r;
        s.swapped_anchor = anchor;
        if (++s.exchanges == 1 || s.exchanges % 3600 == 0) {
            SPDLOG_INFO("[GdfShare] right eye renders with the left eye's Global Distance Field: {} frames (declined {}, "
                "not validated on the render thread {})", s.exchanges, s.declined, s.not_validated);
        }
        return;
    }
}
} // namespace uevr::gdf_share
