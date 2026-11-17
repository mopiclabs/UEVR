#pragma once

#include <chrono>
#include <optional>

#include "mods/vr/SlateViewportOffset.hpp"

// Mopic: the UE 5.5 Slate viewport-offset search is retried while the game has no viewport yet (joeyhodge's
// abe50bd0 initializes the framework before Dead as Disco Demo has one).
namespace slate_viewport_offset_tests {

template <typename Expect>
void run(const Expect& expect) {
    using namespace std::chrono_literals;
    namespace s = uevr::slate_viewport_offset;

    const auto t0 = s::Clock::time_point{} + 1000s;

    {
        // Dead as Disco injected at launch (2026-11-17): the window's viewport member reads 0 twice, then 0x3a0.
        s::Discovery d{};
        int calls = 0;
        s::Attempt answer{std::nullopt, true};
        const auto find = [&]() { ++calls; return answer; };

        expect(!s::resolve(d, t0, find) && calls == 1, "the first DrawWindow call searches for the viewport offset");
        expect(!s::resolve(d, t0 + 500ms, find) && calls == 1, "a failed search is not repeated within the retry interval");
        expect(!s::resolve(d, t0 + 1s, find) && calls == 2 && !s::gave_up(d),
            "a search that met a null viewport member is tried again after the retry interval");

        answer = {size_t{0x3a0}, false};
        expect(s::resolve(d, t0 + 2s, find) == size_t{0x3a0} && calls == 3,
            "the viewport offset is found once the game has its viewport (Dead as Disco 0x3a0, attempt 3)");
        expect(s::resolve(d, t0 + 10s, find) == size_t{0x3a0} && calls == 3, "a found offset is kept and never searched again");
    }

    {
        s::Discovery d{};
        int calls = 0;
        const auto find = [&]() { ++calls; return s::Attempt{std::nullopt, false}; };
        expect(!s::resolve(d, t0, find) && calls == 1 && d.stopped && s::gave_up(d),
            "a search that failed for another reason than a missing viewport stops at once, as before");
        expect(!s::resolve(d, t0 + 5s, find) && calls == 1, "and is never tried again");
    }

    {
        s::Discovery never{};
        int never_calls = 0;
        auto t = t0;
        for (uint32_t i = 0; i < s::max_attempts + 10; ++i, t += s::retry_interval) {
            s::resolve(never, t, [&]() { ++never_calls; return s::Attempt{std::nullopt, true}; });
        }
        expect(never_calls == static_cast<int>(s::max_attempts) && s::gave_up(never) && !never.stopped,
            "a viewport that never appears stops the search after max_attempts tries");
    }

    {
        s::Discovery d{};
        int calls = 0;
        const auto find = [&]() { ++calls; return s::Attempt{size_t{0x3a0}, false}; };
        expect(s::resolve(d, t0, find) == size_t{0x3a0} && calls == 1 && d.attempts == 1,
            "a game that has its viewport is found on the first try, as before");
    }
}

} // namespace slate_viewport_offset_tests
