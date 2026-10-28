#pragma once

// Automatic resolution, the app side of XR_EXT_performance_settings. A runtime that implements the extension tells the
// app when its GPU or CPU state changes (NORMAL / WARNING / IMPAIRED); the app answers each edge with one step of its
// own actuators. Only the standard extension is used: no runtime-specific files or settings, and no list of games;
// what a game supports is found out while it runs. The steps follow Mopic's auto-resolution design v1 (2026-10-26, §3
// and Appendix B). This header is the decision part without UE or OpenXR, so tests/AutoResPolicyTests.cpp can drive
// it; VR and CVarManager carry the steps out.

#include <array>
#include <cstdint>
#include <optional>

namespace uevr::autores {
// The eye size ladder, relative to the runtime's recommended eye size (Base). Down-steps are uniform. On Mopic's 3D
// display up-steps only widen the eye: the weave samples the width, so the extra width is sharper text for fewer pixels
// than a uniform step, and a taller eye than 2160 crashed Hogwarts Legacy there; with its 1920x2160 eye these are design
// v1's levels, Min 1280x1440, Saver 1600x1800, Base 1920x2160, Sharp 2880x2160, Max 3840x2160. On other displays the
// up-steps are uniform with about the same pixel counts (1.5x and 2x).
struct Level {
    const char* name;
    uint32_t width_num, width_den;
    uint32_t height_num, height_den;
    uint32_t uniform_num, uniform_den; // the up-steps elsewhere
};

inline constexpr std::array<Level, 5> LEVELS{{
    {"Min", 2, 3, 2, 3, 2, 3},
    {"Saver", 5, 6, 5, 6, 5, 6},
    {"Base", 1, 1, 1, 1, 1, 1},
    {"Sharp", 3, 2, 1, 1, 11, 9},
    {"Max", 2, 1, 1, 1, 17, 12},
}};
inline constexpr int MIN_LEVEL = 0;
inline constexpr int BASE_LEVEL = 2;
inline constexpr int MAX_LEVEL = 4;

struct Size {
    uint32_t width{};
    uint32_t height{};

    constexpr bool operator==(const Size&) const = default;
};

// A level's eye size for a Base size, in even pixels. widen: Mopic's display (up-steps widen only).
constexpr Size level_size(int level, Size base, bool widen = true) {
    const auto& l = LEVELS[level];

    if (!widen) {
        return Size{(uint32_t)((uint64_t)base.width * l.uniform_num / l.uniform_den) & ~1u,
            (uint32_t)((uint64_t)base.height * l.uniform_num / l.uniform_den) & ~1u};
    }

    return Size{(uint32_t)((uint64_t)base.width * l.width_num / l.width_den) & ~1u,
        (uint32_t)((uint64_t)base.height * l.height_num / l.height_den) & ~1u};
}

// The highest level that fits the runtime's maxImageRect (at least Min).
constexpr int highest_level_within(Size base, Size max, bool widen = true) {
    int highest = MIN_LEVEL;

    for (int i = MIN_LEVEL; i <= MAX_LEVEL; ++i) {
        const auto size = level_size(i, base, widen);

        if (size.width <= max.width && size.height <= max.height) {
            highest = i;
        }
    }

    return highest;
}

// The level with the pixel count closest to an eye size (by ratio, so 1.5x above and 1.5x below are as far).
constexpr int nearest_level(Size size, Size base, bool widen = true) {
    const auto pixels = (uint64_t)size.width * size.height;
    int best = BASE_LEVEL;
    uint64_t best_num = 0, best_den = 1; // the best ratio max(a, b) / min(a, b) so far, as a fraction

    for (int i = MIN_LEVEL; i <= MAX_LEVEL; ++i) {
        const auto level = level_size(i, base, widen);
        const auto level_pixels = (uint64_t)level.width * level.height;
        const auto num = pixels > level_pixels ? pixels : level_pixels;
        const auto den = pixels > level_pixels ? level_pixels : pixels;

        if (den == 0) {
            continue;
        }

        if (best_num == 0 || num * best_den < best_num * den) {
            best = i;
            best_num = num;
            best_den = den;
        }
    }

    return best;
}

// XrPerfSettingsDomainEXT / SubDomainEXT / NotificationLevelEXT, with the enum values of the extension.
enum class Domain : uint32_t { cpu = 1, gpu = 2 };
enum class SubDomain : uint32_t { compositing = 1, rendering = 2, thermal = 3 };
enum class Notification : uint32_t { normal = 0, warning = 25, impaired = 75 };

// The actuators, in the order a GPU IMPAIRED edge uses them (design v1, Appendix B): the game's screen percentage,
// its upscaler's quality mode, then the eye size.
enum class Actuator : uint8_t { sp, upscaler, level };

// The upscaler that sets the game's input resolution, as CVarManager finds it: the UE5 DLSS plugin's
// UDLSSLibrary::SetDLSSMode, or the UE4 plugin's r.NGX.DLSS.Quality.
enum class Upscaler : int8_t { unknown = -1, none = 0, dlss_mode = 1, dlss_quality = 2 };

// SP steps, in percent of the game's own value.
inline constexpr std::array<int, 3> SP_PERCENT{100, 85, 70};
inline constexpr int SP_STEPS = 2;
// One, then two quality modes below the game's own.
inline constexpr int UPSCALER_STEPS = 2;

// Where the actuators stand. sp and upscaler count steps below the game's own value; level is a LEVELS index.
struct State {
    int sp{0};
    int upscaler{0};
    int level{BASE_LEVEL};
    int start_level{BASE_LEVEL};
    int level_cap{MAX_LEVEL}; // min(Max, the runtime's maxImageRect, the VRAM guard)
    Notification gpu{Notification::normal};
    Notification cpu{Notification::normal};
};

// What can move in this game: everything at first, narrowed to what it turns out to support (SP ignored under DLSS
// or put back by the game, no DLSS plugin, no live rebuild).
struct Available {
    bool sp{true};
    int sp_steps{SP_STEPS};
    bool upscaler{true};
    int upscaler_steps{UPSCALER_STEPS}; // fewer when the game's own mode is already near the lowest
    bool level{true};
};

struct Step {
    Actuator actuator;
    int from;
    int to;
};

// up: the GPU has room. cpu_up: CPU-bound with the GPU NORMAL; the same step up, wanted for as long as both hold.
enum class Request : uint8_t { none, down, up, cpu_up };

// Records an edge and says what it asks for (design v1 §3-2). GPU RENDERING IMPAIRED: one step down; WARNING: hold;
// NORMAL: one step up. CPU RENDERING IMPAIRED: CPU-bound, so SP and the upscaler go back to the game's values and the
// spare GPU goes into pixels, but only when the GPU's last state is NORMAL. THERMAL and COMPOSITING are for display.
inline Request on_edge(State& state, Domain domain, SubDomain sub_domain, Notification to) {
    if (sub_domain != SubDomain::rendering) {
        return Request::none;
    }

    if (domain == Domain::gpu) {
        state.gpu = to;
        return to == Notification::impaired ? Request::down : to == Notification::normal ? Request::up : Request::none;
    }

    state.cpu = to;
    return to == Notification::impaired && state.gpu == Notification::normal ? Request::cpu_up : Request::none;
}

// Whether a request held back (loading, the level interval) still stands.
inline bool still_wanted(const State& state, Request request) {
    switch (request) {
    case Request::down:
        return state.gpu == Notification::impaired;
    case Request::up:
        return state.gpu == Notification::normal;
    case Request::cpu_up:
        return state.cpu == Notification::impaired && state.gpu == Notification::normal;
    default:
        return false;
    }
}

// One step down. Pixels added above the start level go first, then SP, then the upscaler, then the level below the
// start, so the steps up retrace them in reverse.
inline std::optional<Step> next_down(const State& state, const Available& available) {
    if (available.level && state.level > state.start_level) {
        return Step{Actuator::level, state.level, state.level - 1};
    }

    if (available.sp && state.sp < available.sp_steps) {
        return Step{Actuator::sp, state.sp, state.sp + 1};
    }

    if (available.upscaler && state.upscaler < available.upscaler_steps) {
        return Step{Actuator::upscaler, state.upscaler, state.upscaler + 1};
    }

    if (available.level && state.level > MIN_LEVEL) {
        return Step{Actuator::level, state.level, state.level - 1};
    }

    return std::nullopt;
}

// One step up: back to the start level, the upscaler and SP restored, then pixels above the start up to the cap.
// SP and the upscaler are restored even when they are no longer available.
inline std::optional<Step> next_up(const State& state, const Available& available) {
    if (available.level && state.level < state.start_level && state.level < state.level_cap) {
        return Step{Actuator::level, state.level, state.level + 1};
    }

    if (state.upscaler > 0) {
        return Step{Actuator::upscaler, state.upscaler, state.upscaler - 1};
    }

    if (state.sp > 0) {
        return Step{Actuator::sp, state.sp, state.sp - 1};
    }

    if (available.level && state.level < state.level_cap) {
        return Step{Actuator::level, state.level, state.level + 1};
    }

    return std::nullopt;
}

inline void apply(State& state, const Step& step) {
    switch (step.actuator) {
    case Actuator::sp:
        state.sp = step.to;
        break;
    case Actuator::upscaler:
        state.upscaler = step.to;
        break;
    case Actuator::level:
        state.level = step.to;
        break;
    }
}
}
