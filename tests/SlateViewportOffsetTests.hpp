#pragma once

#include <chrono>
#include <optional>

#include "mods/vr/SlateViewportOffset.hpp"

// Mopic: the UE 5.5 Slate viewport-offset search is retried until the game has a viewport (joeyhodge's abe50bd0
// initializes the framework before Dead as Disco Demo has one).
namespace slate_viewport_offset_tests {

template <typename Expect>
void run(const Expect& expect) {
    using namespace std::chrono_literals;
    namespace s = uevr::slate_viewport_offset;

    const auto t0 = s::Clock::time_point{} + 1000s;
    s::Discovery d{};
    int calls = 0;
    std::optional<size_t> answer{};
    const auto find = [&]() { ++calls; return answer; };

    expect(!s::resolve(d, t0, find) && calls == 1, "the first DrawWindow call searches for the viewport offset");
    expect(!s::resolve(d, t0 + 500ms, find) && calls == 1, "a failed search is not repeated within the retry interval");
    expect(!s::resolve(d, t0 + 1s, find) && calls == 2, "a failed search is tried again after the retry interval");

    answer = size_t{0x3a0};
    expect(s::resolve(d, t0 + 2s, find) == size_t{0x3a0} && calls == 3,
        "the viewport offset is found once the game has its viewport (Dead as Disco 0x3a0)");
    expect(s::resolve(d, t0 + 10s, find) == size_t{0x3a0} && calls == 3, "a found offset is kept and never searched again");

    s::Discovery never{};
    int never_calls = 0;
    auto t = t0;
    for (uint32_t i = 0; i < s::max_attempts + 10; ++i, t += s::retry_interval) {
        s::resolve(never, t, [&]() -> std::optional<size_t> { ++never_calls; return std::nullopt; });
    }
    expect(never_calls == static_cast<int>(s::max_attempts) && s::gave_up(never),
        "a search that never finds the offset stops after max_attempts tries");
}

} // namespace slate_viewport_offset_tests
