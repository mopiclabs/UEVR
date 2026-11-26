#pragma once

// VR_NativeStereoFixShareShadowCascades (default on; acts only where everything below is found and verified): under the
// Native Stereo Fix the right eye's render uses the left eye's directional-light shadow cascades for the frame, and
// skips rendering its own copy of the cascaded shadow map (CSM) atlas; its lighting samples the atlas the left eye's
// render just drew, which is what it would have drawn itself.
//
// Why: the Native Stereo Fix renders the right eye as a second view family, and UE builds each family's whole-scene
// directional cascades around that family's view origin. UE 5.0 FDirectionalLightSceneProxy::GetShadowSplitBounds
// passes View.ViewMatrices.GetViewOrigin() to the frustum-slice sphere fit; only the direction and projection come from
// ShadowViewMatrices. Everything else in a cascade (PreShadowTranslation = -centre, the texel snap, ShadowBounds, the
// atlas placement, the translated-space matrices) follows from that sphere, the light and the resolution. With the
// left's spheres the right family's cascades are the left's, so its atlas would be drawn again, identically. UE's own
// single-family stereo does the same: AddViewDependentWholeSceneShadowsForView builds cascades only for the primary
// (left) view and projects them for both eyes. Black Myth: Wukong (UE 5.0, 4090 laptop, shipping defaults): right eye
// CSM GPU 0.57 -> 0.00 ms a frame, +2.8 % fps on a still scene and +3.0 % on an 11-minute walk, both eyes' pictures as
// without it.
//
// Cascades (one inline hook, no addresses): GetShadowSplitBounds is found by name. It is the only renderer function that
// reads the cvar r.SkyAtmosphere.SampleLightShadowmap by its name (a function-local FindTConsoleVariableDataInt), so it
// is the function holding a code reference to that UTF-16 string (as a whole string), which some vtable holds at a slot
// k where slot k+1 is a function it calls directly (GetShadowSplitBoundsDepthRange) and slot k-2 a function that calls
// it directly (the view-dependent whole-scene shadow initializer, whose call is qualified and so not virtual). Both of
// its callers (the initializer, and FProjectedShadowInfo::SetupWholeSceneProjection's ShadowBounds) reach the hook. After
// the original: a left-family call records the sphere per (light proxy, cascade index); a right-family call gets the
// left's sphere for the same proxy and cascade instead of its own, when the two agree on the radius (relative 1e-3) and
// the centres are within a quarter radius (any real eye offset is far smaller: Wukong 14.7 units, 0.8 % of cascade 0's
// radius; this only rejects a different cascade or garbage). A caster the right eye alone would see within that offset
// of a cascade's far edge can drop out of the fade band, as with UE's own instanced stereo.
//
// Families: the game thread names each pair's left and right render targets (as GdfShare does); the render thread sees
// each family begin (UEVR's PreRenderViewFamily_RenderThread) and the hook checks that its view's Family pointer
// (FSceneView +0x0) is that family. Calls from any other family (scene captures) pass through.
//
// Atlas skip: FSceneRenderer::RenderShadowDepthMapAtlases is found by name (the only function using the RDG event name
// "Atlas%u %ux%u", all of whose direct callers are in the function using "ShadowDepths", RenderShadowDepthMaps) and
// hooked. In a right family whose every cascade was adopted (as many initializer and setup calls as the left family
// made, no refusal), the trailing CSM atlas entry (pooled target named "WholeSceneShadowmap*"; UE appends the CSM atlases
// last) is left out of the renderer's ShadowMapAtlases for that one call (Num lowered, restored in an SEH __finally on
// every exit of the original), so the right family adds no clear, no depth draws and no Nanite shadow raster for it. Its
// projected shadow infos keep pointing at the pooled atlas, which must be the very element the left family just rendered
// with the same shadow count (checked by pointer; otherwise nothing is left out). The array and its entry layout are
// derived at run time, not taken from a table: the renderer is searched for a TArray whose entries hold a named pooled
// target and a Shadows TArray of shadow infos that each hold that target; the target and Shadows places within an entry
// and the stride between entries are whatever validates every entry (Wukong: +0x30, +0x38, 0x48 at renderer +0x7c8; the
// 5.0 source alone would suggest +0x10, +0x18, 0x28). The skip is used only after the layout held 30 times over both
// families and is checked again on every call; 600 misses before that confirmation turn the skip off for the session.
// Known side effect: Scene->PrevAtlasHZBs is sized to the shorter list by the right call, so the left family's next
// Nanite CSM raster runs without the previous frame's HZB (Wukong: no measurable change in the left eye's CSM cost).
//
// Checks: engine 5.0 only (FSphere is four doubles; validated on one title, Black Myth: Wukong); any other version is off
// with one log line. A self-check over the first 64 hooked calls inside the two families (the hook returns the hidden
// result pointer, the view's Family is the family whose render is running, the cascade index is -1..16, the sphere is
// finite with a positive radius) comes before the first adoption; a failure turns it off for the session and the hook
// passes through. Every read of engine memory is SEH-guarded. Any failure: one log line, the right family keeps its own
// cascades and atlas (today's path).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <sdk/Utility.hpp>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <utility/String.hpp>

namespace uevr::cascade_share {
namespace detail {
inline bool rd8(uintptr_t a, uint64_t& v) {
    if (a < 0x10000 || a > 0x7ffffffffff0ull) {
        return false;
    }
    __try {
        v = *(const volatile uint64_t*)a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool rd_sphere(uintptr_t a, double (&s)[4]) {
    if (a < 0x10000 || a > 0x7ffffffffff0ull) {
        return false;
    }
    __try {
        std::memcpy(s, (const void*)a, sizeof(s));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool wr_sphere(uintptr_t a, const double (&s)[4]) {
    __try {
        std::memcpy((void*)a, s, sizeof(s));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class Role : uint8_t { None, Left, Right };

struct PairSlot {
    std::atomic<uintptr_t> left_target{0}, right_target{0};
};

constexpr size_t kRing = 8;
constexpr size_t kCascades = 18;  // index -1..16
constexpr uint32_t kSelfCheckCalls = 64;
constexpr uint32_t kLayoutConfirmations = 30;  // consistent validations (both families) before the first skip
constexpr uint32_t kLayoutMissesOff = 600;     // misses before confirmation that turn the skip off
constexpr uint64_t kProgressPairs = 3600;      // pairs between progress lines

struct Rec {
    uintptr_t proxy{};
    uint64_t seq{};
    double s[4]{};
    bool valid{};
};

struct AtlasLayout {
    size_t off{};       // TArray in the renderer
    size_t depth{};     // DepthTarget within an entry
    size_t shadows{};   // Shadows TArray within an entry
    size_t stride{};    // 0 = not known yet (only one entry seen)
};

struct Totals {
    std::atomic<uint64_t> right_families{0}, adopted_families{0}, adopted{0}, refused_radius{0}, refused_centre{0},
        refused_missing{0}, refused_write{0}, closed_again{0}, foreign{0}, skipped{0}, no_layout{0}, not_gated{0},
        other_rt{0}, count{0};
    std::atomic<uint64_t> max_dc_ppm{0};  // max |centre L-R| / radius, ppm
};

struct State {
    // discovery / hook
    std::atomic<int32_t> install{0};  // 0 not started, 1 running, 2 hooked, -1 failed
    safetyhook::InlineHook hook{};
    std::atomic<bool> disabled{false};
    std::atomic<uint32_t> checked_calls{0};

    // game thread
    PairSlot ring[kRing]{};
    std::atomic<uint64_t> left_seq{0};
    uint64_t pairs{0};

    // render thread (guarded by mtx: the hook may run on task threads)
    std::mutex mtx{};
    uint64_t consumed_seq{0};
    Role role{Role::None};
    uintptr_t family{0};
    bool adopt{false};
    uint64_t pair_seq{0};
    Rec rec[kCascades]{};
    uint32_t left_init{0}, left_setup{0};
    uint32_t right_init{0}, right_setup{0}, right_refused{0};
    bool gate_open{false};
    bool first_adopted_logged{false}, first_skip_logged{false};

    // engine-side atlas skip (RenderShadowDepthMapAtlases)
    std::atomic<int32_t> atlas_install{0};  // 2 hooked, -1 not available
    safetyhook::InlineHook atlas_hook{};
    uint32_t atlas_find_tries{0};
    AtlasLayout layout{};
    bool layout_found{false}, layout_failed{false};
    uint32_t layout_confirmed{0}, layout_misses{0};
    uintptr_t left_atlas_rt{0};
    uint32_t left_atlas_shadows{0};
    uint64_t left_atlas_seq{0};

    Totals total{}, since{};  // whole run, since the last progress line
};

inline State& state() {
    static State s{};
    return s;
}

inline void add(std::atomic<uint64_t> Totals::*field, uint64_t n = 1) {
    auto& s = state();
    (s.total.*field).fetch_add(n, std::memory_order_relaxed);
    (s.since.*field).fetch_add(n, std::memory_order_relaxed);
}

// Code bytes of [begin, end): targets of E8 rel32 calls (raw scan; false matches are filtered by the callers' checks).
inline std::vector<uintptr_t> direct_calls(uintptr_t begin, uintptr_t end) {
    std::vector<uintptr_t> out{};
    for (uintptr_t p = begin; p + 5 <= end; ++p) {
        uint64_t q{};
        if (!rd8(p, q)) {
            break;
        }
        if ((uint8_t)q == 0xE8) {
            const auto rel = (int32_t)(uint32_t)(q >> 8);
            out.push_back(p + 5 + (intptr_t)rel);
        }
    }
    return out;
}

inline bool function_range(uintptr_t addr, uintptr_t& begin, uintptr_t& end) {
    DWORD64 image{};
    const auto* rf = RtlLookupFunctionEntry((DWORD64)addr, &image, nullptr);
    if (rf == nullptr) {
        return false;
    }
    begin = (uintptr_t)image + rf->BeginAddress;
    end = (uintptr_t)image + rf->EndAddress;
    return begin == addr || (begin <= addr && addr < end);
}

inline bool calls(uintptr_t fn, uintptr_t target) {
    uintptr_t b{}, e{};
    if (!function_range(fn, b, e) || b != fn) {
        return false;
    }
    for (const auto t : direct_calls(b, e)) {
        if (t == target) {
            return true;
        }
    }
    return false;
}

// The pooled render target at p carries the debug name (const TCHAR*) in qword 10 (FPooledRenderTarget desc, UE 5.0;
// as GdfShare): does it start with the ASCII prefix?
inline bool pooled_named_prefix(uintptr_t p, const char* prefix) {
    uint64_t s{};
    if (p == 0 || (p & 7) != 0 || !rd8(p + 10 * 8, s) || (s & 1) != 0) {
        return false;
    }
    for (size_t i = 0;; i += 4) {
        uint64_t w4{};
        if (!rd8(s + i * 2, w4)) {
            return false;
        }
        for (size_t k = 0; k < 4; ++k) {
            const auto ch = (uint16_t)(w4 >> (16 * k));
            const auto want = (uint8_t)prefix[i + k];
            if (want == 0) {
                return true;
            }
            if (ch != want) {
                return false;
            }
        }
    }
}

// A pooled target with a readable debug name: at least three printable UTF-16 characters.
inline bool pooled_name_ok(uintptr_t p) {
    uint64_t s{}, w{};
    if (p == 0 || (p & 7) != 0 || !rd8(p + 10 * 8, s) || (s & 1) != 0 || !rd8(s, w)) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        const auto ch = (uint16_t)(w >> (16 * k));
        if (ch < 0x20 || ch > 0x7e) {
            return false;
        }
    }
    return true;
}

// FSortedShadowMapAtlas entry at e under (depth, shadows): its pooled target has a readable debug name and each of its
// 1..16 shadow infos holds that same target pointer in its first 0x200 bytes. Returns the target and shadow count.
inline bool atlas_entry(uintptr_t e, size_t depth, size_t shadows, uintptr_t& rt_out, uint32_t& n_out) {
    uint64_t rt{}, sd{}, sn{};
    if (!rd8(e + depth, rt) || !pooled_name_ok((uintptr_t)rt) || !rd8(e + shadows, sd) || !rd8(e + shadows + 8, sn)) {
        return false;
    }
    const auto n = (int32_t)(uint32_t)sn;
    if (n < 1 || n > 16 || sd == 0 || (sd & 7) != 0) {
        return false;
    }
    for (int32_t i = 0; i < n; ++i) {
        uint64_t info{};
        if (!rd8(sd + 8 * (uint64_t)i, info) || info == 0) {
            return false;
        }
        bool holds = false;
        for (size_t k = 0; k < 64 && !holds; ++k) {
            uint64_t v{};
            holds = rd8(info + 8 * k, v) && v == rt;
        }
        if (!holds) {
            return false;
        }
    }
    rt_out = (uintptr_t)rt;
    n_out = (uint32_t)n;
    return true;
}

struct ArrayView {
    uintptr_t data{};
    int32_t num{};
};

inline bool array_at(uintptr_t a, ArrayView& out) {
    uint64_t d{}, nm{};
    if (!rd8(a, d) || !rd8(a + 8, nm)) {
        return false;
    }
    const auto num = (int32_t)(uint32_t)nm, max = (int32_t)(uint32_t)(nm >> 32);
    if (num < 1 || num > 16 || max < num || max > 64 || d == 0 || (d & 7) != 0) {
        return false;
    }
    out = ArrayView{(uintptr_t)d, num};
    return true;
}

// Does the array at renderer + L.off validate under L (every entry when the stride is known, else the only entry)?
// Fills the last entry's target / shadow count and whether it is a CSM atlas.
inline bool layout_holds(uintptr_t renderer, const AtlasLayout& L, uintptr_t& last_rt, uint32_t& last_n, bool& last_csm) {
    ArrayView a{};
    if (!array_at(renderer + L.off, a)) {
        return false;
    }
    if (a.num > 1 && L.stride == 0) {
        return false;
    }
    for (int32_t i = 0; i < a.num; ++i) {
        uintptr_t rt{};
        uint32_t n{};
        if (!atlas_entry(a.data + (size_t)i * L.stride, L.depth, L.shadows, rt, n)) {
            return false;
        }
        if (i == a.num - 1) {
            last_rt = rt;
            last_n = n;
        }
    }
    last_csm = pooled_named_prefix(last_rt, "WholeSceneShadowmap");
    return true;
}

// Search the renderer for the atlas array and its layout; with two or more entries the stride is derived (the
// smallest that validates every entry), with one entry it stays unknown until a frame with two.
inline bool find_layout(uintptr_t renderer, AtlasLayout& out) {
    for (size_t off = 8; off < 0x8000; off += 8) {
        ArrayView a{};
        if (!array_at(renderer + off, a)) {
            continue;
        }
        for (size_t depth = 0; depth < 0x80; depth += 8) {
            for (size_t shadows = 0; shadows + 16 <= 0x90; shadows += 8) {
                if (shadows < depth + 8 && depth < shadows + 16) {
                    continue;  // overlapping fields
                }
                uintptr_t rt{};
                uint32_t n{};
                if (!atlas_entry(a.data, depth, shadows, rt, n)) {
                    continue;
                }
                if (a.num == 1) {
                    out = AtlasLayout{off, depth, shadows, 0};
                    return true;
                }
                const size_t min_stride = std::max(depth + 8, shadows + 16);
                for (size_t stride = (min_stride + 7) & ~size_t{7}; stride <= 0x100; stride += 8) {
                    const AtlasLayout L{off, depth, shadows, stride};
                    uintptr_t lr{};
                    uint32_t ln{};
                    bool csm{};
                    if (layout_holds(renderer, L, lr, ln, csm)) {
                        out = L;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// All E8 call sites in the module's executable sections whose target is fn.
inline std::vector<uintptr_t> callers_of(HMODULE module, uintptr_t fn) {
    std::vector<uintptr_t> out{};
    const auto base = (uintptr_t)module;
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    const auto* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const auto b = base + sec->VirtualAddress;
        const auto n = (uintptr_t)sec->Misc.VirtualSize;
        MEMORY_BASIC_INFORMATION mbi{};
        for (uintptr_t a = b; a < b + n;) {
            if (VirtualQuery((void*)a, &mbi, sizeof(mbi)) == 0) {
                break;
            }
            const auto rb = (uintptr_t)mbi.BaseAddress;
            const auto re = std::min<uintptr_t>(rb + mbi.RegionSize, b + n);
            const auto p = mbi.Protect & 0xff;
            const bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0 &&
                (p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY || p == PAGE_READONLY ||
                 p == PAGE_READWRITE);
            if (readable) {
                for (auto q = (const uint8_t*)std::max(rb, a); (uintptr_t)(q + 5) <= re; ++q) {
                    if (*q == 0xE8 && (uintptr_t)(q + 5) + (intptr_t)*(const int32_t*)(q + 1) == fn) {
                        out.push_back((uintptr_t)q);
                    }
                }
            }
            a = re;
        }
    }
    return out;
}

// Every place the UTF-16 string `name` sits in the module as a whole string (NUL before it), and every code reference
// to each. A plain first-match search can land on the tail of a longer string (Wukong: "ShadowDepths" first matches
// inside "r.Nanite.AsyncRasterization.ShadowDepths", which has no reference of its own).
inline std::vector<uintptr_t> whole_string_refs(HMODULE module, const wchar_t* name) {
    std::vector<uintptr_t> refs{};
    for (const auto a : utility::scan_strings(module, std::wstring{name}, true)) {
        uint64_t before{};
        if (!rd8(a - 8, before) || (uint16_t)(before >> 48) != 0) {
            continue;
        }
        for (const auto r : utility::scan_displacement_references(module, a)) {
            refs.push_back(r);
        }
    }
    return refs;
}

inline void hooked_atlases(uintptr_t renderer, uintptr_t graph_builder);

// Worker thread, after GetShadowSplitBounds is hooked: find and hook RenderShadowDepthMapAtlases.
inline void discover_atlases(HMODULE module) {
    auto& s = state();
    const auto base = (uintptr_t)module;
    auto fn_of = [&](const wchar_t* name, std::vector<uintptr_t>& fns) {
        for (const auto r : whole_string_refs(module, name)) {
            if (const auto f = utility::find_function_start_unwind(r); f && std::find(fns.begin(), fns.end(), *f) == fns.end()) {
                fns.push_back(*f);
            }
        }
        return !fns.empty();
    };
    std::vector<uintptr_t> atlas_fns{}, depth_fns{};
    if (!fn_of(L"Atlas%u %ux%u", atlas_fns) || atlas_fns.size() != 1 || !fn_of(L"ShadowDepths", depth_fns)) {
        s.atlas_install = -1;
        SPDLOG_INFO("[CascadeShare] Atlas skip off: RenderShadowDepthMapAtlases not found by its event name ({} candidate "
            "functions); the right eye adopts the cascades and draws its own atlas", atlas_fns.size());
        return;
    }
    const auto a = atlas_fns[0];
    uint32_t from_depth = 0, total = 0;
    for (const auto c : callers_of(module, a)) {
        ++total;
        const auto f = utility::find_function_start_unwind(c);
        if (f && std::find(depth_fns.begin(), depth_fns.end(), *f) != depth_fns.end()) {
            ++from_depth;
        }
    }
    if (total == 0 || from_depth != total) {
        s.atlas_install = -1;
        SPDLOG_INFO("[CascadeShare] Atlas skip off: exe+0x{:x} has {} direct callers, {} in RenderShadowDepthMaps; the right "
            "eye adopts the cascades and draws its own atlas", a - base, total, from_depth);
        return;
    }
    s.atlas_hook = safetyhook::create_inline((void*)a, (void*)&hooked_atlases);
    if (!s.atlas_hook) {
        s.atlas_install = -1;
        SPDLOG_INFO("[CascadeShare] Atlas skip off: inline hook failed; the right eye adopts the cascades and draws its own atlas");
        return;
    }
    s.atlas_install = 2;
    SPDLOG_INFO("[CascadeShare] RenderShadowDepthMapAtlases at exe+0x{:x} ({} direct caller(s), all in RenderShadowDepthMaps), "
        "hooked", a - base, total);
}

// Calls the original with the array's Num lowered by `lower`, restoring it on every exit (SEH __finally: also on an
// exception unwinding through the original).
using AtlasFn = void (*)(uintptr_t, uintptr_t);
inline void call_with_num(AtlasFn fn, uintptr_t renderer, uintptr_t graph_builder, int32_t* num, int32_t lower) {
    const int32_t before = *num;
    *num = before - lower;
    __try {
        fn(renderer, graph_builder);
    } __finally {
        *num = before;
    }
}

// Render thread. Left family: remember its CSM atlas target. Right family whose every cascade was adopted: leave the
// trailing CSM atlas out of this call when it is the same pooled element (same shadow count) the left family just
// rendered and the layout has been confirmed; otherwise today's path.
inline void hooked_atlases(uintptr_t renderer, uintptr_t graph_builder) {
    auto& s = state();
    const auto original = s.atlas_hook.original<AtlasFn>();
    int32_t* num_field = nullptr;
    {
        std::scoped_lock _{s.mtx};
        if (s.role != Role::None && !s.layout_failed) {
            if (!s.layout_found && (s.atlas_find_tries++ % 60) == 0) {
                AtlasLayout L{};
                if (find_layout(renderer, L)) {
                    s.layout = L;
                    s.layout_found = true;
                    SPDLOG_INFO("[CascadeShare] Atlas array found by structure: renderer +0x{:x}, entry depth target +0x{:x}, "
                        "shadows +0x{:x}, stride {}", L.off, L.depth, L.shadows,
                        L.stride != 0 ? std::format("0x{:x}", L.stride) : std::string{"unknown (one entry)"});
                }
            }
            uintptr_t rt{};
            uint32_t n{};
            bool csm{};
            bool holds = s.layout_found && layout_holds(renderer, s.layout, rt, n, csm);
            if (s.layout_found && !holds && s.layout.stride == 0) {
                // One entry seen so far: derive the stride now that there may be more.
                AtlasLayout L{};
                if (find_layout(renderer, L) && L.off == s.layout.off && L.depth == s.layout.depth && L.shadows == s.layout.shadows) {
                    s.layout = L;
                    holds = layout_holds(renderer, s.layout, rt, n, csm);
                    if (L.stride != 0) {
                        SPDLOG_INFO("[CascadeShare] Atlas entry stride derived: 0x{:x}", L.stride);
                    }
                }
            }
            if (s.layout_found && !holds) {
                if (++s.layout_misses >= kLayoutMissesOff && s.layout_confirmed < kLayoutConfirmations) {
                    s.layout_failed = true;
                    SPDLOG_INFO("[CascadeShare] Atlas skip off: the atlas layout did not hold ({} misses before {} "
                        "confirmations); the right eye adopts the cascades and draws its own atlas", s.layout_misses,
                        kLayoutConfirmations);
                }
            } else if (holds && s.layout_confirmed < kLayoutConfirmations && ++s.layout_confirmed == kLayoutConfirmations) {
                SPDLOG_INFO("[CascadeShare] Atlas layout confirmed ({} validations over both eyes)", kLayoutConfirmations);
            }
            const bool ready = holds && csm && s.layout_confirmed >= kLayoutConfirmations;
            if (s.role == Role::Left) {
                s.left_atlas_rt = holds && csm ? rt : 0;
                s.left_atlas_shadows = n;
                s.left_atlas_seq = s.pair_seq;
            } else if (s.adopt) {
                if (!ready) {
                    add(&Totals::no_layout);
                } else if (!s.gate_open) {
                    add(&Totals::not_gated);
                } else if (s.left_atlas_seq != s.pair_seq || rt != s.left_atlas_rt || rt == 0) {
                    add(&Totals::other_rt);
                } else if (n != s.left_atlas_shadows) {
                    add(&Totals::count);
                } else {
                    num_field = (int32_t*)(renderer + s.layout.off + 8);
                    if (!s.first_skip_logged) {
                        s.first_skip_logged = true;
                        SPDLOG_INFO("[CascadeShare] Right eye skips its shadow atlas and samples the left eye's (found and "
                            "verified): first frame");
                    }
                }
            }
        }
    }
    if (num_field == nullptr) {
        original(renderer, graph_builder);
        return;
    }
    call_with_num(original, renderer, graph_builder, num_field, 1);
    add(&Totals::skipped);
}

inline std::string engine_version() {
    const auto s = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
    if (s != "0.00") {
        return s;
    }
    const auto v = sdk::get_file_version_info();
    return std::to_string(HIWORD(v.dwFileVersionMS)) + "." + std::to_string(LOWORD(v.dwFileVersionMS));
}

inline void fail(const std::string& why) {
    SPDLOG_INFO("[CascadeShare] Off: {}; the right eye keeps its own shadow cascades and atlas", why);
    state().install = -1;
}

inline uintptr_t hooked(uintptr_t self, uintptr_t ret, uintptr_t view, int32_t index, bool precomputed, uintptr_t out);

// Worker thread: find GetShadowSplitBounds by name and structure, then hook it.
inline void discover() {
    auto& s = state();
    const auto t0 = GetTickCount64();
    const auto v = engine_version();
    if (!(v == "5.0" || v.rfind("5.0.", 0) == 0)) {
        fail("UE " + v + " has no validated layout (5.0 only)");
        return;
    }
    const auto module = utility::get_executable();
    const auto refs = whole_string_refs(module, L"r.SkyAtmosphere.SampleLightShadowmap");
    if (refs.empty()) {
        fail("no code reference to the cvar name r.SkyAtmosphere.SampleLightShadowmap as a whole string");
        return;
    }
    std::vector<uintptr_t> candidates{};
    for (const auto r : refs) {
        if (const auto f = utility::find_function_start_unwind(r); f && std::find(candidates.begin(), candidates.end(), *f) == candidates.end()) {
            candidates.push_back(*f);
        }
    }
    // Readable, non-executable pages of the module: where vtables live.
    const auto base = (uintptr_t)module;
    const auto size = utility::get_module_size(module).value_or(0);
    uintptr_t found{}, found_vt_slot{}, depth_range{}, initializer{};
    uint32_t vt_hits{}, consistent{};
    for (const auto f : candidates) {
        uintptr_t fb{}, fe{};
        if (!function_range(f, fb, fe)) {
            continue;
        }
        const auto callees = direct_calls(fb, fe);
        for (uintptr_t a = base; a < base + size;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery((void*)a, &mbi, sizeof(mbi)) == 0) {
                break;
            }
            const auto rb = (uintptr_t)mbi.BaseAddress, re = rb + mbi.RegionSize;
            const auto p = mbi.Protect & 0xff;
            const bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0 &&
                (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY);
            if (readable) {
                for (auto q = (const uint64_t*)rb; (uintptr_t)(q + 1) <= re; ++q) {
                    if (*q != f || (uintptr_t)q < rb + 16 || (uintptr_t)(q + 2) > re) {
                        continue;
                    }
                    ++vt_hits;
                    const auto next = (uintptr_t)q[1], prev2 = (uintptr_t)q[-2];
                    if (std::find(callees.begin(), callees.end(), next) == callees.end() || !calls(prev2, f)) {
                        continue;
                    }
                    if (found == 0) {
                        found = f;
                        found_vt_slot = (uintptr_t)q;
                        depth_range = next;
                        initializer = prev2;
                    }
                    ++consistent;
                }
            }
            a = re;
        }
    }
    if (found == 0) {
        fail(std::format("GetShadowSplitBounds not found by structure ({} string references, {} candidate functions, {} vtable "
            "entries, none with the depth-range callee next and the initializer two before)", refs.size(), candidates.size(),
            vt_hits));
        return;
    }
    s.hook = safetyhook::create_inline((void*)found, (void*)&hooked);
    if (!s.hook) {
        fail("inline hook on GetShadowSplitBounds failed");
        return;
    }
    s.install = 2;
    discover_atlases(module);
    SPDLOG_INFO("[CascadeShare] UE {}: GetShadowSplitBounds at exe+0x{:x} (vtable entry exe+0x{:x}, depth range exe+0x{:x}, "
        "initializer exe+0x{:x}; {} matching vtable entries), hooked in {} ms; self-check over the first {} calls",
        v, found - base, found_vt_slot - base, depth_range - base, initializer - base, consistent, GetTickCount64() - t0,
        kSelfCheckCalls);
}

inline uintptr_t hooked(uintptr_t self, uintptr_t ret, uintptr_t view, int32_t index, bool precomputed, uintptr_t out) {
    auto& s = state();
    const auto r = s.hook.call<uintptr_t>(self, ret, view, index, precomputed, out);
    if (s.disabled.load(std::memory_order_relaxed)) {
        return r;
    }
    uint64_t fam{};
    double sp[4]{};
    const bool fam_ok = rd8(view, fam);
    const bool sp_ok = rd_sphere(ret, sp);
    const bool sp_valid = sp_ok && std::isfinite(sp[0]) && std::isfinite(sp[1]) && std::isfinite(sp[2]) &&
        std::isfinite(sp[3]) && sp[3] > 0.0;
    std::scoped_lock _{s.mtx};
    // Self-check over the first calls made inside a left or right family's render (other families' calls are not
    // counted: their views belong to families this code does not track).
    if (s.checked_calls.load(std::memory_order_relaxed) < kSelfCheckCalls && s.role != Role::None) {
        const bool own_family = fam_ok && (uintptr_t)fam == s.family;
        if (r != ret || !own_family || index < -1 || index > 16 || !sp_valid) {
            s.disabled = true;
            SPDLOG_INFO("[CascadeShare] Off: self-check failed at call {} (returns the result pointer {}, view's family is the "
                "rendering family {}, cascade {}, sphere finite with a positive radius {}); the hook passes through, the right "
                "eye keeps its own shadow cascades and atlas", s.checked_calls.load(), r == ret, own_family, index, sp_valid);
            return r;
        }
        if (s.checked_calls.fetch_add(1) + 1 == kSelfCheckCalls) {
            SPDLOG_INFO("[CascadeShare] Self-check passed ({} calls)", kSelfCheckCalls);
        }
    }
    if (s.role == Role::None || !fam_ok || (uintptr_t)fam != s.family || !sp_ok) {
        if (s.role != Role::None) {
            add(&Totals::foreign);
        }
        return r;
    }
    const auto slot = (size_t)(index + 1);
    if (slot >= kCascades) {
        return r;
    }
    if (s.role == Role::Left) {
        auto& e = s.rec[slot];
        e.proxy = self;
        e.seq = s.pair_seq;
        std::memcpy(e.s, sp, sizeof(sp));
        e.valid = true;
        ++(out != 0 ? s.left_init : s.left_setup);
        return r;
    }
    // Right family
    if (!s.adopt) {
        return r;
    }
    // Any refusal below closes the atlas skip's gate again for this family (a cascade the left did not have, say, after
    // the others matched): cascade setup comes before the family's shadow depth rendering, so the close is in time.
    struct CloseOnRefusal {
        State& s;
        uint32_t before;
        ~CloseOnRefusal() {
            if (s.right_refused != before && s.gate_open) {
                s.gate_open = false;
                add(&Totals::closed_again);
            }
        }
    } close_on_refusal{s, s.right_refused};
    const auto& e = s.rec[slot];
    if (!e.valid || e.seq != s.pair_seq || e.proxy != self) {
        ++s.right_refused;
        add(&Totals::refused_missing);
        return r;
    }
    const double dw = std::abs(sp[3] - e.s[3]);
    const double dc = std::sqrt((sp[0] - e.s[0]) * (sp[0] - e.s[0]) + (sp[1] - e.s[1]) * (sp[1] - e.s[1]) +
        (sp[2] - e.s[2]) * (sp[2] - e.s[2]));
    if (dw > 1e-3 * e.s[3]) {
        ++s.right_refused;
        add(&Totals::refused_radius);
        return r;
    }
    if (dc > 0.25 * e.s[3]) {
        ++s.right_refused;
        add(&Totals::refused_centre);
        return r;
    }
    if (!wr_sphere(ret, e.s)) {
        ++s.right_refused;
        add(&Totals::refused_write);
        return r;
    }
    add(&Totals::adopted);
    const auto ppm = (uint64_t)(dc / e.s[3] * 1e6);
    for (auto* m : {&s.total.max_dc_ppm, &s.since.max_dc_ppm}) {
        if (ppm > m->load(std::memory_order_relaxed)) {
            m->store(ppm, std::memory_order_relaxed);
        }
    }
    ++(out != 0 ? s.right_init : s.right_setup);
    if (!s.gate_open && s.right_refused == 0 && s.right_init == s.left_init && s.right_setup == s.left_setup && s.left_init > 0) {
        s.gate_open = true;
        add(&Totals::adopted_families);
        if (!s.first_adopted_logged) {
            s.first_adopted_logged = true;
            SPDLOG_INFO("[CascadeShare] Right eye renders with the left eye's shadow cascades (found and verified): first "
                "frame, {} cascade(s)", s.left_init);
        }
    }
    return r;
}

inline void log_progress() {
    auto& s = state();
    auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    auto& d = s.since;
    const auto right = take(d.right_families), fams = take(d.adopted_families), adopted = take(d.adopted),
               skipped = take(d.skipped), r_radius = take(d.refused_radius), r_centre = take(d.refused_centre),
               r_missing = take(d.refused_missing), r_write = take(d.refused_write), closed = take(d.closed_again),
               foreign = take(d.foreign), no_layout = take(d.no_layout), not_gated = take(d.not_gated),
               other_rt = take(d.other_rt), count = take(d.count), ppm = take(d.max_dc_ppm);
    SPDLOG_INFO("[CascadeShare] Last {} pairs: right eye frames {}, with all of the left eye's cascades {} ({} spheres; "
        "refused: radius {}, centre {}, no left record {}, write {}; closed again {}; other families' views {}; max centre "
        "offset {:.3f} % of the radius); atlas skipped {} (not skipped: layout not ready {}, cascades not all adopted {}, "
        "another atlas target {}, shadow count {}). Whole run: {} of {} right eye frames skipped the atlas",
        kProgressPairs, right, fams, adopted, r_radius, r_centre, r_missing, r_write, closed, foreign, ppm / 1e4, skipped,
        no_layout, not_gated, other_rt, count, s.total.skipped.load(), s.total.right_families.load());
}
} // namespace detail

// Game thread, before the left family of a pair is submitted (both render targets known).
inline void on_left_submission(bool enabled, uintptr_t left_target, uintptr_t right_target) {
    using namespace detail;
    auto& s = state();
    if (!enabled || left_target == 0 || right_target == 0) {
        return;
    }
    if (s.install.load() == 0) {
        int32_t expected = 0;
        if (s.install.compare_exchange_strong(expected, 1)) {
            std::thread{[] { discover(); }}.detach();
        }
    }
    if (s.install.load() != 2 || s.disabled.load()) {
        return;
    }
    auto& slot = s.ring[(s.left_seq.load() + 1) % kRing];
    slot.left_target = left_target;
    slot.right_target = right_target;
    ++s.left_seq;
    if (++s.pairs % kProgressPairs == 0) {
        log_progress();
    }
}

// Render thread, at the start of every view family's render (UEVR's PreRenderViewFamily_RenderThread).
inline void on_family_render(uintptr_t family, uintptr_t target) {
    using namespace detail;
    auto& s = state();
    if (s.install.load() != 2) {
        return;
    }
    std::scoped_lock _{s.mtx};
    Role role = Role::None;
    if (target != 0 && s.consumed_seq > 0 && target == s.ring[s.consumed_seq % kRing].right_target.load()) {
        role = Role::Right;
    } else if (target != 0) {
        const auto newest = s.left_seq.load();
        for (uint64_t seq = s.consumed_seq + 1; seq <= newest && seq < s.consumed_seq + kRing; ++seq) {
            if (target == s.ring[seq % kRing].left_target.load()) {
                role = Role::Left;
                s.consumed_seq = seq;
                break;
            }
        }
    }
    s.role = role;
    s.family = family;
    if (role == Role::Left) {
        s.pair_seq = s.consumed_seq;
        s.left_init = s.left_setup = 0;
    } else if (role == Role::Right) {
        s.adopt = !s.disabled.load() && s.checked_calls.load() >= kSelfCheckCalls;
        s.right_init = s.right_setup = s.right_refused = 0;
        s.gate_open = false;
        add(&Totals::right_families);
    }
}
} // namespace uevr::cascade_share
