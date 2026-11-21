#pragma once

// EXPERIMENT (exp/right-eye-cost, VR_ExperimentRightEyeCsmShare, default 0 = off; 1 = align the right eye's shadow
// cascades to the left's only; 2 = align + the right family skips its cascaded-shadow-map depth rendering and samples
// the atlas the left family left behind; N >= 10 = A/B: mode 2 in even N-second windows of QPC time, off in odd ones).
//
// Why it can work: both view families render the CSM into one D3D12 atlas resource (CSM probe, 11-22: one resource,
// ~12 depth-only binds per pair), the left family first. If the right eye's cascades are the left's (same shadow view
// matrices, so the same projected shadow infos and the same atlas layout), the left's depth is exactly what the right
// family would render, and it is still in the atlas when the right family's lighting samples it.
//
// Align (game thread, before the right family is submitted): FSceneView holds ViewMatrices and, a few fields later,
// ShadowViewMatrices (a copy that the shadow setup uses). Both are found at run time without addresses: in a view, the
// longest run of qwords that repeats at a second offset K (Wukong UE5.0: 0x760 bytes at +0x340, K = 0x840). The later
// copy is ShadowViewMatrices; the left view's copy is written over the right view's (eyes differ there only in the view
// origin). Not found, or the views disagree on the layout -> nothing is written and mode 2's skip stays off.
// Skip (RHI / translate threads, PassMarkers): see pass_markers::csm_share.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

namespace uevr::csm_share {
// Set by the caller: returns (and resets) the RHI-side skip counters for the window log.
inline std::string (*g_rhi_counts)() = nullptr;

namespace detail {
inline bool seh_read8(uintptr_t a, uint64_t& v) {
    __try {
        v = *(const volatile uint64_t*)a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool seh_copy(uintptr_t dst, uintptr_t src, size_t size) {
    __try {
        std::memcpy((void*)dst, (const void*)src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Layout {
    size_t start{};  // ViewMatrices block start (bytes into FSceneView)
    size_t size{};   // block size
    size_t shift{};  // ShadowViewMatrices = start + shift
};

inline constexpr size_t kScan = 0x2400;
inline constexpr size_t kMinBlock = 0x600;

// Longest run of qwords v[o] == v[o + shift] (not all zero) for shift in [0x400, 0x1400).
inline bool find_layout(uintptr_t view, Layout& out) {
    static uint64_t q[kScan / 8];
    for (size_t i = 0; i < kScan / 8; ++i) {
        if (!seh_read8(view + i * 8, q[i])) {
            return false;
        }
    }
    Layout best{};
    for (size_t shift = 0x400; shift < 0x1400; shift += 8) {
        const size_t k = shift / 8;
        size_t run = 0, run_start = 0, nonzero = 0;
        for (size_t i = 0; i + k < kScan / 8; ++i) {
            if (q[i] == q[i + k]) {
                if (run == 0) {
                    run_start = i;
                    nonzero = 0;
                }
                ++run;
                nonzero += q[i] != 0;
                if (run * 8 > best.size && nonzero * 2 > run) {
                    best = Layout{run_start * 8, run * 8, shift};
                }
            } else {
                run = 0;
            }
        }
    }
    if (best.size < kMinBlock) {
        return false;
    }
    out = best;
    return true;
}

struct State {
    std::atomic<int32_t> found{0};  // 0 unknown, 1 found, -1 given up
    Layout layout{};
    uint32_t tries{0};
    // per 10-second window, logged when the window changes
    int64_t window{-1};
    uint32_t w_frames{0}, w_aligned{0}, w_layout_mismatch{0};
};

inline State& state() {
    static State s{};
    return s;
}

inline int64_t window_index(int32_t mode) {
    LARGE_INTEGER q{}, f{};
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return (q.QuadPart / f.QuadPart) / (mode >= 10 ? mode : 10);
}
} // namespace detail

// Whether the skip is wanted in this frame (mode 2, or an even window of mode N >= 10).
inline bool skip_wanted(int32_t mode) {
    if (mode == 2) {
        return true;
    }
    return mode >= 10 && detail::window_index(mode) % 2 == 0;
}

// Game thread, both eyes' FSceneViews built, before the right family is submitted. Returns whether the right view's
// shadow view matrices now equal the left's (the condition for the skip in this frame).
inline bool align(int32_t mode, uintptr_t left_view, uintptr_t right_view) {
    using namespace detail;
    auto& s = state();
    if (mode == 0 || left_view == 0 || right_view == 0) {
        return false;
    }
    const bool want = mode == 1 || skip_wanted(mode);
    {
        const auto w = window_index(mode);
        if (w != s.window) {
            if (s.window >= 0 && s.w_frames > 0) {
                SPDLOG_INFO("[CsmShare] window {} ({}): frames {}, right shadow view matrices aligned {}, layout mismatches {}; {}",
                    s.window, mode >= 10 ? (s.window % 2 == 0 ? "share" : "off") : (mode == 1 ? "align" : "share"),
                    s.w_frames, s.w_aligned, s.w_layout_mismatch, g_rhi_counts != nullptr ? g_rhi_counts() : std::string{});
            }
            s.window = w;
            s.w_frames = s.w_aligned = s.w_layout_mismatch = 0;
        }
    }
    ++s.w_frames;
    // Menus and loading screens may not show the pattern (R19 run 1 gave up there after 600 frames): keep trying every
    // 30 frames until found (one scan of both views costs ~1 ms), logging the first misses once.
    if (s.found.load() == 0 && (s.tries++ % 30) == 0) {
        Layout l{}, r{};
        const bool lf = find_layout(left_view, l), rf = find_layout(right_view, r);
        if (lf && rf && l.start == r.start && l.size == r.size && l.shift == r.shift && l.start + l.shift + l.size <= kScan) {
            s.layout = l;
            s.found = 1;
            SPDLOG_INFO("[CsmShare] FSceneView layout found after {} frames: ViewMatrices block +0x{:x} ({} bytes), "
                "ShadowViewMatrices +0x{:x}", s.tries, l.start, l.size, l.start + l.shift);
        } else if (s.tries <= 30 * 5) {
            SPDLOG_INFO("[CsmShare] FSceneView layout not found yet (frame {}): left {} +0x{:x}/{}/+0x{:x}, right {} +0x{:x}/{}/+0x{:x}",
                s.tries, lf, l.start, l.size, l.shift, rf, r.start, r.size, r.shift);
        }
    }
    if (s.found.load() != 1 || !want) {
        return false;
    }
    const auto& L = s.layout;
    // The right view's own copy must still mirror its ViewMatrices (the layout holds for this view).
    uint64_t a{}, b{};
    if (!seh_read8(right_view + L.start + 0x10, a) || !seh_read8(right_view + L.start + L.shift + 0x10, b) || a != b) {
        ++s.w_layout_mismatch;
        return false;
    }
    if (!seh_copy(right_view + L.start + L.shift, left_view + L.start + L.shift, L.size)) {
        ++s.w_layout_mismatch;
        return false;
    }
    ++s.w_aligned;
    return true;
}
} // namespace uevr::csm_share
