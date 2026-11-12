#pragma once

// Automatic resolution, the app side of XR_EXT_performance_settings. A runtime that implements the extension tells the
// app when its GPU or CPU state changes (NORMAL / WARNING / IMPAIRED); the app answers each edge with one step of its
// own actuators. Only the standard extension is used: no runtime-specific files or settings, and no list of games;
// what a game supports is found out while it runs. The steps follow Mopic's auto-resolution design v1 (2026-10-26, §3
// and Appendix B). This header is the decision part without UE or OpenXR, so tests/AutoResPolicyTests.cpp can drive
// it; VR and CVarManager carry the steps out.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace uevr::autores {
// The eye size ladder. On Mopic's 3D display it is design v1's table of sizes, whatever the runtime recommends: Min
// 1280x1440, Saver 1600x1800, Base 1920x2160, Sharp 2880x2160, Max 3840x2160 (up-steps only widen the eye: the weave
// samples the width, so the extra width is sharper text for fewer pixels than a uniform step, and a taller eye than 2160
// crashed Hogwarts Legacy there). Sizes relative to the recommended one took a session that started at Min to
// 1920x1440, 2560x1440 and 3200x1800 (2026-11-04). Elsewhere the ladder is relative to the runtime's recommended eye size
// (Base): down-steps uniform, up-steps uniform with about the same pixel counts (1.5x and 2x).
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

// Mopic's ladder (design v1 §1-3), Min to Max.
inline constexpr std::array<Size, 5> MOPIC_SIZES{{{1280, 1440}, {1600, 1800}, {1920, 2160}, {2880, 2160}, {3840, 2160}}};

// A level's eye size, in even pixels. mopic: Mopic's display, the fixed table; otherwise relative to a Base size.
constexpr Size level_size(int level, Size base, bool mopic = true) {
    const auto& l = LEVELS[level];

    if (mopic) {
        return MOPIC_SIZES[level];
    }

    return Size{(uint32_t)((uint64_t)base.width * l.uniform_num / l.uniform_den) & ~1u,
        (uint32_t)((uint64_t)base.height * l.uniform_num / l.uniform_den) & ~1u};
}

// The highest level that fits the runtime's maxImageRect (at least Min).
constexpr int highest_level_within(Size base, Size max, bool mopic = true) {
    int highest = MIN_LEVEL;

    for (int i = MIN_LEVEL; i <= MAX_LEVEL; ++i) {
        const auto size = level_size(i, base, mopic);

        if (size.width <= max.width && size.height <= max.height) {
            highest = i;
        }
    }

    return highest;
}

// The level with the pixel count closest to an eye size (by ratio, so 1.5x above and 1.5x below are as far).
constexpr int nearest_level(Size size, Size base, bool mopic = true) {
    const auto pixels = (uint64_t)size.width * size.height;
    int best = BASE_LEVEL;
    uint64_t best_num = 0, best_den = 1; // the best ratio max(a, b) / min(a, b) so far, as a fraction

    for (int i = MIN_LEVEL; i <= MAX_LEVEL; ++i) {
        const auto level = level_size(i, base, mopic);
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

// The actuators. A GPU IMPAIRED edge takes the game's internal resolution down first (its DLSS mode or quality where a
// DLSS plugin sets the input resolution, else its screen percentage), to the floor, and only then the eye size (design
// v1 §7-57, 2026-11-12: a lower DLSS mode looks better than a smaller output, the more so as DLSS gets better).
enum class Actuator : uint8_t { sp, upscaler, level };

// The upscaler that sets the game's input resolution, as CVarManager finds it: the UE5 DLSS plugin's
// UDLSSLibrary::SetDLSSMode, or the UE4 plugin's r.NGX.DLSS.Quality.
enum class Upscaler : int8_t { unknown = -1, none = 0, dlss_mode = 1, dlss_quality = 2 };

// SP steps, in percent of the game's own value. Without a temporal upscaler (TSR, or TAA with upsampling) the engine
// stretches the smaller image, so only the first two; with one, down to 50 %.
inline constexpr std::array<int, 5> SP_PERCENT{100, 85, 70, 58, 50};
inline constexpr int SP_STEPS = 2;
inline constexpr int SP_STEPS_TEMPORAL = 4;
// Upscaler steps before the game's modes are ranked (CVarManager reports how many lie above the floor).
inline constexpr int UPSCALER_STEPS = 2;

// The internal resolution's floor under DLSS (VR_AutoResUpscalerFloor): Performance, or one more to Ultra Performance
// where the game supports it. UE5's modes are ranked by GetDLSSModeInformation's optimal screen percentage (Performance
// 50 %, Ultra Performance 33 %), so the floor is a percentage; UE4's r.NGX.DLSS.Quality is -1 / -2.
enum class UpscalerFloor : int { performance = 0, ultra_performance = 1 };
inline constexpr float DLSS_PERFORMANCE_FLOOR_PERCENT = 45.0f;

inline constexpr int dlss_quality_floor(UpscalerFloor floor) {
    return floor == UpscalerFloor::ultra_performance ? -2 : -1;
}

// The modes below the game's, highest first, down to the floor; one per distinct optimal percentage. by_pixels: the
// supported modes as (optimal screen percentage, mode); base_percent: the game's mode's. A percentage of 1 or less is
// a fraction.
inline std::vector<uint8_t> dlss_modes_below(std::vector<std::pair<float, uint8_t>> by_pixels, float base_percent,
    UpscalerFloor floor) {
    const auto percent = [](float value) { return value <= 1.0f ? value * 100.0f : value; };
    // Fewer pixels last; Auto reports one of the fixed modes' percentages, so a tie keeps one mode, the higher value
    // (Auto is 1, below the fixed modes, in every plugin version).
    std::sort(by_pixels.begin(), by_pixels.end(),
        [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
    std::vector<uint8_t> lower{};
    float last = -1.0f;
    const auto base = percent(base_percent);

    for (const auto& [optimal, mode] : by_pixels) {
        const auto p = percent(optimal);

        if (p < base * 0.99f && std::abs(p - last) > 0.01f * p &&
            (floor == UpscalerFloor::ultra_performance || p >= DLSS_PERFORMANCE_FLOOR_PERCENT)) {
            lower.push_back(mode);
            last = p;
        }
    }

    return lower;
}

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

// One step down: the internal resolution to its floor first (the DLSS mode where DLSS sets the input resolution, else
// SP; one of them at a time), then the eye size, from whatever level it is at.
inline std::optional<Step> next_down(const State& state, const Available& available) {
    if (available.upscaler && state.upscaler < available.upscaler_steps) {
        return Step{Actuator::upscaler, state.upscaler, state.upscaler + 1};
    }

    if (!available.upscaler && available.sp && state.sp < available.sp_steps) {
        return Step{Actuator::sp, state.sp, state.sp + 1};
    }

    if (available.level && state.level > MIN_LEVEL) {
        return Step{Actuator::level, state.level, state.level - 1};
    }

    return std::nullopt;
}

// One step up: the eye size back to the anchor (start_level: where the session started, or Base after
// VR_AutoResOutputAnchor), then the internal resolution back to the game's own, then pixels above the anchor up to the
// cap. SP and the upscaler are restored even when they are no longer available.
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

// Frame-bound games (design v1 §3-2, Appendix B): a game whose time runs with its frames plays in slow motion below its
// frame rate (TEKKEN 8 at 30 fps took 183 s for a 137 s fight, 2026-11-05), so it needs 60 whatever the objective. The
// judgement, once a second, takes r = the engine ticks' DeltaSeconds / the wall time between them over the last 3 s and
// that window's VR frame rate. A game on real time has r about 1 at any frame rate. It judges only at 22-55 VR fps
// outside loads: above 55 a frame-bound game runs at about real time too, and below 22 UE's smoothing floor could hold
// the game time back. r < 0.8 for 2 judged seconds in a row is bound, r >= 0.95 for 2 is not bound; bound stays for the
// session (design 2026-11-06: a 3 s window, 2 in a row).
//
// Not bound needs r >= 0.95, above what a game whose time is its frames reads anywhere the judge looks: at 55 fps of 60
// its r is 0.917 (with 0.9 a fixed 60 game at 54-55 fps read as not bound). Games on real time read 0.99-1.00 (E5).
// The hint does not change with it: GPU BOOST is bound, anything else is SUSTAINED_HIGH.
enum class FrameBound : uint8_t { unknown, not_bound, bound };

inline constexpr float FRAME_BOUND_RATIO = 0.8f;
inline constexpr float NOT_BOUND_RATIO = 0.95f;
inline constexpr int FRAME_BOUND_SECONDS = 2;
inline constexpr int FRAME_BOUND_WINDOW_SECONDS = 3;
inline constexpr float FRAME_BOUND_MIN_FPS = 22.0f;
inline constexpr float FRAME_BOUND_MAX_FPS = 55.0f;
static_assert(FRAME_BOUND_MAX_FPS / 60.0f < NOT_BOUND_RATIO, "a 60 fps frame-bound game must never read as not bound");

struct FrameBoundJudge {
    FrameBound state{FrameBound::unknown};
    int low_seconds{0};  // judged seconds in a row under FRAME_BOUND_RATIO
    int high_seconds{0}; // and at NOT_BOUND_RATIO or more

    // One second's window; true when the state changed.
    bool update(std::optional<float> ratio, float fps, bool loading) {
        if (state == FrameBound::bound) {
            return false;
        }

        if (!ratio || loading || fps < FRAME_BOUND_MIN_FPS || fps > FRAME_BOUND_MAX_FPS) {
            low_seconds = high_seconds = 0;
            return false;
        }

        low_seconds = *ratio < FRAME_BOUND_RATIO ? low_seconds + 1 : 0;
        high_seconds = *ratio >= NOT_BOUND_RATIO ? high_seconds + 1 : 0;

        if (low_seconds >= FRAME_BOUND_SECONDS) {
            state = FrameBound::bound;
            return true;
        }

        if (high_seconds >= FRAME_BOUND_SECONDS && state != FrameBound::not_bound) {
            state = FrameBound::not_bound;
            return true;
        }

        return false;
    }
};

// Which upscaler sets the game's input resolution, found while the session runs (CVarManager's probe). The probe tries
// every 2 s for the first 2 minutes, then every 10 s, for the whole session. A game may make its DLSS mode readable only
// at its title or main menu (TEKKEN 8 Demo, about 30 s after the start, 2026-11-12), so finding nothing in the first 15
// tries (30 s) is "none for now": SP steps meanwhile, and DLSS found later takes over (SP put back). A DLSS that was found
// and then reads as off twice in a row (the player turned it off) is none again. A try that gets no answer (the game is
// loading, no world yet) changes nothing.
enum class ProbeReading : uint8_t { no_answer, off, dlss_mode, dlss_quality };

struct UpscalerProbe {
    static constexpr int FAST_SECONDS = 120;
    static constexpr int FAST_INTERVAL_SECONDS = 2;
    static constexpr int SLOW_INTERVAL_SECONDS = 10;
    static constexpr int PROVISIONAL_TRIES = 15;
    static constexpr int OFF_READS = 2;

    Upscaler kind{Upscaler::unknown};
    int tries{0};
    int off_reads{0};

    static constexpr int interval_seconds(int seconds_since_start) {
        return seconds_since_start < FAST_SECONDS ? FAST_INTERVAL_SECONDS : SLOW_INTERVAL_SECONDS;
    }

    // One try; true when kind changed.
    bool update(ProbeReading reading) {
        ++tries;
        const auto before = kind;

        if (reading == ProbeReading::dlss_mode || reading == ProbeReading::dlss_quality) {
            kind = reading == ProbeReading::dlss_mode ? Upscaler::dlss_mode : Upscaler::dlss_quality;
            off_reads = 0;
        } else if (kind == Upscaler::unknown) {
            if (tries >= PROVISIONAL_TRIES) {
                kind = Upscaler::none;
            }
        } else if (kind == Upscaler::dlss_mode || kind == Upscaler::dlss_quality) {
            off_reads = reading == ProbeReading::off ? off_reads + 1 : 0;

            if (off_reads >= OFF_READS) {
                kind = Upscaler::none;
                off_reads = 0;
            }
        }

        return kind != before;
    }

    bool dlss() const { return kind == Upscaler::dlss_mode || kind == Upscaler::dlss_quality; }
};

// The effect of an SP or upscaler step down (design v1 Appendix B, self-calibration): the game's GPU time per frame (its
// GPU busy share times the frame interval) 2-5 s after the step against the 3 s before it. A step that takes 8 % or
// more off works; under 5 % it does nothing worth keeping, so it is put back and that actuator is left out for the
// session (a request still wanted goes to the next one); in between it stays. A 0.7 step measured 0.87 / 0.77 (SP,
// TEKKEN, 4090L / 3080L), 0.90 (E33's DLSS mode), 0.84 / 0.71 (Hogwarts Legacy's DLSS quality) and 0.96 (Black Myth:
// Wukong's DLSS mode, which barely changes its input resolution) of the GPU time (design v1 Appendix A-3): under 5 %
// leaves Wukong's mode out and keeps the others with room for noise.
inline constexpr float STEP_WORKS_DROP = 0.08f;
inline constexpr float STEP_NOTHING_DROP = 0.05f;

enum class StepEffect : uint8_t { works, unclear, nothing };

inline StepEffect judge_step(float gpu_ms_before, float gpu_ms_after) {
    if (!(gpu_ms_before > 0.0f) || !(gpu_ms_after >= 0.0f)) {
        return StepEffect::unclear;
    }

    const auto drop = 1.0f - gpu_ms_after / gpu_ms_before;
    return drop >= STEP_WORKS_DROP ? StepEffect::works : drop < STEP_NOTHING_DROP ? StepEffect::nothing : StepEffect::unclear;
}

// VR_AutoResInternalRecover: the runtime's GPU NORMAL edge comes only when the next eye size fits, so on a device where
// a level costs much (8 GB laptops) the internal resolution could stay at its floor with GPU room left. Once a second:
// 60 s in a row not IMPAIRED with the game's GPU busy under 70 % takes the internal resolution one step up; an IMPAIRED
// within 30 s of that puts it back and blocks the next one for 5 minutes, twice as long each time (up to an hour).
struct InternalRecovery {
    static constexpr int CALM_SECONDS = 60;
    static constexpr float BUSY_LIMIT_PERCENT = 70.0f;
    static constexpr int WATCH_SECONDS = 30;
    static constexpr int FIRST_BLOCK_SECONDS = 300;
    static constexpr int LONGEST_BLOCK_SECONDS = 3600;

    enum class Act : uint8_t { none, step_up, undo };

    int calm{0};
    int watch{0};   // seconds left to watch a step up of ours
    int blocked{0}; // seconds left before the next one
    int block{FIRST_BLOCK_SECONDS};
    int ups{0};
    int undos{0};

    Act tick(bool impaired, bool loading, std::optional<float> busy_percent, bool can_step_up) {
        if (watch > 0) {
            if (impaired) {
                watch = 0;
                calm = 0;
                blocked = block;
                block = (std::min)(block * 2, LONGEST_BLOCK_SECONDS);
                ++undos;
                return Act::undo;
            }

            --watch;
        }

        if (blocked > 0) {
            --blocked;
        }

        if (impaired || loading || !busy_percent || *busy_percent >= BUSY_LIMIT_PERCENT) {
            calm = 0;
            return Act::none;
        }

        if (++calm >= CALM_SECONDS && blocked == 0 && watch == 0 && can_step_up) {
            calm = 0;
            watch = WATCH_SECONDS;
            ++ups;
            return Act::step_up;
        }

        return Act::none;
    }
};

// VR_AutoResOutputAnchor decides after the loading (design v1 §7-57), not in the menus the session starts in: a menu's
// GPU memory says nothing about the level's (Hogwarts and Wukong on 8 GB are 103-110 % in game). Once a second: a load
// (an engine stall) seen after the first 15 s, then 30 s without one; GPU memory is the most it reached in those 30 s.
// A session with no load by 3 minutes decides then, on the most since its last load.
struct AnchorGate {
    static constexpr int IGNORE_LOADS_SECONDS = 15; // the session's own start-up stalls
    static constexpr int MIN_SECONDS = 40;
    static constexpr int CALM_SECONDS = 30;
    static constexpr int NO_LOAD_SECONDS = 180;

    int seconds{0};
    bool load_seen{false};
    int calm{0};
    std::optional<float> vram_max{};

    // True when the decision is due (and stays true until the caller decides).
    bool tick(bool loading, std::optional<float> vram_percent) {
        ++seconds;

        if (loading) {
            load_seen = load_seen || seconds > IGNORE_LOADS_SECONDS;
            calm = 0;
            vram_max.reset();
            return false;
        }

        ++calm;

        if (vram_percent) {
            vram_max = vram_max ? (std::max)(*vram_max, *vram_percent) : *vram_percent;
        }

        if (seconds < MIN_SECONDS || !vram_max) {
            return false;
        }

        // 3 minutes only when no load came at all: a load near then still gets its 30 s.
        return load_seen ? calm >= CALM_SECONDS : seconds >= NO_LOAD_SECONDS;
    }
};

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
