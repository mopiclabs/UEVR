#pragma once

// EXPERIMENT (exp/right-eye-cost): the window phase of VR_ExperimentSecondFamilyCvars ("N,group,group,..."), shared by
// the game thread (the view constructor asks whether this frame's right eye is SECONDARY) and the right family's hold.
// Windows of N seconds of QPC time take turns: window w is phase w % (groups + 1); phase 0 is plain. A group is a
// "+"-joined list of tokens (name=value cvars, skip, nogdf, secondary).

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uevr::exp_phase {
struct Spec {
    std::string text{};
    int64_t window_seconds{};
    std::vector<std::vector<std::string>> groups{};
};

inline std::vector<std::string> split(const std::string& text, const char* separators) {
    std::vector<std::string> parts{};
    for (size_t start = 0; start <= text.size();) {
        const auto end = (std::min)(text.find_first_of(separators, start), text.size());
        if (end > start) {
            parts.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return parts;
}

// Cached per thread (the spec string rarely changes).
inline const Spec& parse(const std::string& text) {
    thread_local Spec spec{};
    thread_local bool parsed{false};
    if (parsed && spec.text == text) {
        return spec;
    }
    parsed = true;
    spec = Spec{text};
    const auto tokens = split(text, ",;");
    if (tokens.empty()) {
        return spec;
    }
    try {
        spec.window_seconds = std::stoll(tokens[0]);
    } catch (...) {
        spec.window_seconds = 0;
        return spec;
    }
    for (size_t i = 1; i < tokens.size(); ++i) {
        spec.groups.push_back(split(tokens[i], "+"));
    }
    return spec;
}

inline int64_t phase_of(int64_t window_seconds, size_t groups) {
    if (groups == 0 || window_seconds <= 0) {
        return 0;
    }
    LARGE_INTEGER qpc{}, frequency{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&frequency);
    return (qpc.QuadPart / frequency.QuadPart / window_seconds) % (int64_t)(groups + 1);
}

// Whether the current window's group holds `token` (false in the plain phase or without a spec).
inline bool current_group_has(const std::string& text, std::string_view token) {
    if (text.empty()) {
        return false;
    }
    const auto& spec = parse(text);
    const auto phase = phase_of(spec.window_seconds, spec.groups.size());
    if (phase == 0) {
        return false;
    }
    for (const auto& t : spec.groups[(size_t)phase - 1]) {
        if (t == token) {
            return true;
        }
    }
    return false;
}
} // namespace uevr::exp_phase
