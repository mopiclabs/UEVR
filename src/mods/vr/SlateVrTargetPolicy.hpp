#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

// Slate.DrawToVRRenderTarget latch (VR::update_hmd_state).
//
// UE4 and UE5.0-5.3 FSlateRHIRenderer::DrawWindow_RenderThread draws the window into the FViewportInfo render target
// texture only while Slate.DrawToVRRenderTarget is 1 (the engine default; it then copies that texture to the
// backbuffer with the stereo device's RenderTexture_RenderThread at the end). With 0 and a stereo device it calls
// RenderTexture_RenderThread first and draws the window into the swapchain backbuffer, past the texture UEVR's Slate
// hook redirects to its UI target: the game's UI never reaches the VR UI layer (Stellar Blade, UE 4.26, sets 0).
// While the redirect is on, UEVR keeps the variable at 1 and writes the game's value back when it goes off.
//
// UE5.4+ is left alone: UESDK's IConsoleVariable GetInt/Set don't read it reliably there (Sonic Racing CrossWorlds,
// UE 5.4.3: 1089616160, then 0 after every write) and UE5.5+ draws Slate through other paths UEVR handles separately.

namespace uevr::slate_vr_target {

// "4.26", "5.3.2", "5.0" -> {major, minor}
inline std::optional<std::pair<int, int>> parse_engine_version(std::string_view version) {
    int major{}, minor{};
    const auto* const end = version.data() + version.size();
    auto [p, ec] = std::from_chars(version.data(), end, major);
    if (ec != std::errc{} || p == end || *p != '.') {
        return std::nullopt;
    }
    auto [p2, ec2] = std::from_chars(p + 1, end, minor);
    if (ec2 != std::errc{}) {
        return std::nullopt;
    }
    return std::pair{major, minor};
}

inline bool engine_uses_legacy_slate_vr_target(int major, int minor) {
    return major == 4 || (major == 5 && minor >= 0 && minor <= 3);
}

enum class Action {
    None,
    SetOne,          // write 1
    RestoreOriginal, // write State::original (0) back
};

struct State {
    bool forced{false}; // UEVR wrote 1 and hasn't written the game's value back yet
    int original{0};    // the value the game had when UEVR first wrote 1 (always 0, see decide)
};

// wanted: the redirect is on (HMD active, no stereo emulation, a redirected DrawWindow within the last second).
// current: the variable's value now. Only 0 (the documented "draw on top of the backbuffer" setting) is replaced;
// any value other than 0 and 1 is not a value this variable takes, so the read is not trusted and nothing is written.
inline Action decide(State& state, bool wanted, int current) {
    if (current != 0 && current != 1) {
        return Action::None;
    }

    if (wanted) {
        if (current == 1) {
            return Action::None;
        }

        if (!state.forced) {
            state.original = current;
            state.forced = true;
        }

        // Also when the game wrote its own 0 again while forced.
        return Action::SetOne;
    }

    if (!state.forced) {
        return Action::None;
    }

    state.forced = false;

    // A 0 the game wrote meanwhile is its own; leave it.
    return current == 1 ? Action::RestoreOriginal : Action::None;
}

} // namespace uevr::slate_vr_target
