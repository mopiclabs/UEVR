#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

// Mopic: SlateRHIRenderer::DrawWindow_RenderThread (UE 5.5+ inputs) finds the SWindow's viewport member by emulating
// the function. It ran once per process: before the game has a viewport the member reads 0, the search fails and the
// failure was kept for the session, so no Slate resource was ever adopted (black screen). Since joeyhodge's abe50bd0
// the framework initializes 1-8 s after the hooks, early enough for that to happen: Dead as Disco Demo injected at
// launch stayed black 3 of 3 times (2026-11-17). A failed search is tried again once a second until it finds the
// offset, at most max_attempts times.
namespace uevr::slate_viewport_offset {

using Clock = std::chrono::steady_clock;

inline constexpr auto retry_interval = std::chrono::seconds{1};
inline constexpr uint32_t max_attempts = 120;

struct Discovery {
    std::optional<size_t> offset{};
    uint32_t attempts{};
    Clock::time_point next_attempt{};
};

// The offset once found. Calls find() at the first call, then at most once per retry_interval while it finds
// nothing, and no more after max_attempts failures or once it found the offset.
template <typename Find>
std::optional<size_t> resolve(Discovery& discovery, Clock::time_point now, Find&& find) {
    if (discovery.offset || discovery.attempts >= max_attempts || now < discovery.next_attempt) {
        return discovery.offset;
    }

    ++discovery.attempts;
    discovery.offset = find();
    discovery.next_attempt = now + retry_interval;
    return discovery.offset;
}

inline bool gave_up(const Discovery& discovery) {
    return !discovery.offset && discovery.attempts >= max_attempts;
}

} // namespace uevr::slate_viewport_offset
