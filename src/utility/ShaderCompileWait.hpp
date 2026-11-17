#pragma once

#include <chrono>

// Mopic: Framework::should_defer_vr_start_for_shader_compile decides whether the game is compiling pipeline states at
// startup. A game only starts its compile some time into its boot (Black Myth: Wukong from cold: running with 18
// threads 24 s after UEVR's hooks, not yet 14 s after them). The decision used to come 12-23 s after the hooks, behind
// the framework's initialization retry backoff; joeyhodge's abe50bd0 initializes the framework after 60 frames (1-8 s),
// and VR then started before the compile and ran through it (Wukong from cold, 2026-11-17: 376 s against 325 s).
// So "no compile" is concluded only once the game has presented for min_observe; a compile seen before that is waited
// out as before.
namespace uevr::shader_compile {

using Clock = std::chrono::steady_clock;

inline constexpr auto min_observe = std::chrono::seconds{20};

// Whether it is still too early to conclude that the game doesn't compile at startup. first_present is the first
// Present the framework saw (a zero time point: not known, no wait).
inline bool still_observing(Clock::time_point first_present, Clock::time_point now,
    Clock::duration observe = min_observe) {
    return first_present != Clock::time_point{} && now >= first_present && now - first_present < observe;
}

} // namespace uevr::shader_compile
