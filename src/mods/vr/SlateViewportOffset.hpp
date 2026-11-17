#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

// Mopic: SlateRHIRenderer::DrawWindow_RenderThread (UE 5.5+ inputs) finds the SWindow's viewport member by emulating
// the function. It ran once per process: before the game has a viewport the member reads 0, the search fails and the
// failure was kept for the session, so no Slate resource was ever adopted (black screen). Since joeyhodge's abe50bd0
// the framework initializes 1-8 s after the hooks, early enough for that to happen: Dead as Disco Demo injected at
// launch stayed black 3 of 3 times (2026-11-17). A search that failed because a window member it read was still null
// (the game isn't ready) is tried again once a second, at most max_attempts times; a search that failed for any other
// reason isn't (each try toggles the DrawWindow hook and sets up an emulator over the whole module).
namespace uevr::slate_viewport_offset {

using Clock = std::chrono::steady_clock;

inline constexpr auto retry_interval = std::chrono::seconds{1};
inline constexpr uint32_t max_attempts = 60;

struct Attempt {
    std::optional<size_t> offset{};
    bool not_ready{}; // the search read a window member that was still null
};

struct Discovery {
    std::optional<size_t> offset{};
    uint32_t attempts{};
    Clock::time_point next_attempt{};
    bool stopped{}; // a failure the game being early doesn't explain: no more tries
};

inline bool gave_up(const Discovery& discovery) {
    return !discovery.offset && (discovery.stopped || discovery.attempts >= max_attempts);
}

// The offset once found. Calls find() at the first call, then at most once per retry_interval while it finds
// nothing because the game isn't ready, and no more once it found the offset, after max_attempts tries, or after a
// failure that wasn't "not ready".
template <typename Find>
std::optional<size_t> resolve(Discovery& discovery, Clock::time_point now, Find&& find) {
    if (discovery.offset || gave_up(discovery) || now < discovery.next_attempt) {
        return discovery.offset;
    }

    ++discovery.attempts;
    const Attempt attempt = find();
    discovery.offset = attempt.offset;
    discovery.next_attempt = now + retry_interval;
    discovery.stopped = !attempt.offset && !attempt.not_ready;
    return discovery.offset;
}

} // namespace uevr::slate_viewport_offset
