#pragma once

// EXPERIMENT (exp/right-eye-cost): VR_ExperimentFrameCvars = "N,name=value+name=value,group2,..." holds console
// variables for whole frames (both eyes, or the flat render) in N-second QPC windows: window w is phase
// w % (groups + 1); phase 0 leaves every listed cvar at the value it had before the first switch, phase k sets group
// k's values. Called once per engine tick on the game thread (VR::on_pre_engine_tick); a switch happens on the first
// tick of a new phase. Values are set with console priority so a scalability set-by cannot hold them back.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <sdk/CVar.hpp>
#include <sdk/ConsoleManager.hpp>
#include <utility/String.hpp>

#include "ExperimentPhase.hpp"

namespace uevr::frame_cvars {
struct Item {
    std::wstring name;
    sdk::IConsoleVariable* cvar{};
    std::wstring original{};
    bool have_original{false};
};

struct State {
    std::string spec{};
    int64_t window_seconds{};
    std::vector<Item> items{};
    std::vector<std::vector<std::pair<size_t, std::wstring>>> groups{};
    std::optional<int64_t> last_phase{};
    uint32_t switches{};
};

inline State& state() {
    static State s{};
    return s;
}

inline void set_value(sdk::IConsoleVariable* cvar, const std::wstring& value) {
    constexpr uint32_t set_by_console = 0x9000000;
    try {
        cvar->Set(value.c_str(), set_by_console);
    } catch (...) {
    }
}

inline void process(const std::string& spec) {
    auto& s = state();
    if (spec != s.spec) {
        // restore whatever the previous spec changed, then parse the new one
        for (auto& item : s.items) {
            if (item.cvar != nullptr && item.have_original) {
                set_value(item.cvar, item.original);
            }
        }
        s = State{spec};
        const auto& parsed = exp_phase::parse(spec);
        s.window_seconds = parsed.window_seconds;
        const auto console_manager = sdk::FConsoleManager::get();
        for (const auto& tokens : parsed.groups) {
            std::vector<std::pair<size_t, std::wstring>> group{};
            for (const auto& token : tokens) {
                const auto eq = token.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const auto name = utility::widen(token.substr(0, eq));
                const auto value = utility::widen(token.substr(eq + 1));
                size_t index = s.items.size();
                for (size_t i = 0; i < s.items.size(); ++i) {
                    if (s.items[i].name == name) {
                        index = i;
                    }
                }
                if (index == s.items.size()) {
                    Item item{name};
                    if (console_manager != nullptr) {
                        auto* object = console_manager->find(name.c_str());
                        item.cvar = object != nullptr && object->AsCommand() == nullptr ? static_cast<sdk::IConsoleVariable*>(object) : nullptr;
                    }
                    SPDLOG_INFO("[FrameCvars] {}: {}", utility::narrow(name), item.cvar != nullptr ? "found" : "not found");
                    s.items.push_back(std::move(item));
                }
                group.emplace_back(index, value);
            }
            s.groups.push_back(std::move(group));
        }
        SPDLOG_INFO("[FrameCvars] {} s windows, {} groups + a plain phase, {} cvars", s.window_seconds, s.groups.size(), s.items.size());
    }
    if (s.groups.empty() || s.window_seconds <= 0) {
        return;
    }
    const auto phase = exp_phase::phase_of(s.window_seconds, s.groups.size());
    if (s.last_phase && *s.last_phase == phase) {
        return;
    }
    s.last_phase = phase;
    for (auto& item : s.items) {
        if (item.cvar != nullptr && !item.have_original) {
            try {
                if (const auto f = item.cvar->TryGetFloat(); f) {
                    // ints parse "1.000000" as 1 (Atoi); floats keep the value
                    item.original = std::to_wstring(*f);
                    item.have_original = true;
                }
            } catch (...) {
            }
        }
    }
    // phase 0: originals; phase k: group k over the originals
    for (auto& item : s.items) {
        if (item.cvar != nullptr && item.have_original) {
            set_value(item.cvar, item.original);
        }
    }
    if (phase > 0) {
        for (const auto& [index, value] : s.groups[(size_t)phase - 1]) {
            if (s.items[index].cvar != nullptr) {
                set_value(s.items[index].cvar, value);
            }
        }
    }
    if (++s.switches <= 6 || s.switches % 50 == 0) {
        std::string text{};
        for (const auto& item : s.items) {
            if (item.cvar != nullptr) {
                text += " " + utility::narrow(item.name) + "=" + std::to_string(item.cvar->GetFloat());
            }
        }
        SPDLOG_INFO("[FrameCvars] switch {}: phase {}{}", s.switches, phase, text);
    }
}
} // namespace uevr::frame_cvars
