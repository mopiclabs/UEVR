#pragma once

// EXPERIMENT (exp/right-eye-cost; ported from exp/cpu-sweep, cpu-bottleneck-20261105): VR_ExperimentCvarDumpSeconds = N
// > 0 writes every console object the game registered to cvardump\<qpc ns>.tsv in the game's UEVR folder every N
// seconds (game thread): name, kind (var/cmd), value (GetFloat), flags and the help's first line. Flags and help are
// read from FConsoleVariableBase / FConsoleCommandBase (vtable, FString Help, Flags at +0x18 on UE4.27-5.x), so they
// are only as right as that layout. Two dumps (flat with UEVR loaded and no VR session, and VR) diffed list what
// UEVR or the game changes for VR.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <spdlog/spdlog.h>
#include <sdk/CVar.hpp>
#include <sdk/ConsoleManager.hpp>
#include <utility/String.hpp>

#include "Framework.hpp"

namespace uevr::cvar_dump {
inline void process(int32_t seconds) {
    static auto next = std::chrono::steady_clock::now();
    static int32_t last_seconds{0};
    if (seconds <= 0) {
        last_seconds = 0;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (seconds != last_seconds) {
        last_seconds = seconds;
        next = now + std::chrono::seconds{seconds};
        return;
    }
    if (now < next) {
        return;
    }
    next = now + std::chrono::seconds{seconds};

    const auto console_manager = sdk::FConsoleManager::get();
    if (console_manager == nullptr) {
        SPDLOG_WARN_ONCE("[CvarDump] No console manager to walk");
        return;
    }

    LARGE_INTEGER qpc{}, frequency{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&frequency);
    const auto dir = Framework::get_persistent_dir("cvardump");
    std::error_code ec{};
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (std::to_string((int64_t)((double)qpc.QuadPart * 1e9 / (double)frequency.QuadPart)) + ".tsv");
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        SPDLOG_WARN("[CvarDump] Cannot write {}", path.string());
        return;
    }

    out << "name\tkind\tvalue\tflags\thelp\n";
    size_t vars{}, commands{}, unreadable{};

    for (const auto& element : console_manager->get_console_objects()) {
        try {
            if (element.key == nullptr || element.value == nullptr || IsBadReadPtr(element.key, sizeof(wchar_t)) ||
                IsBadReadPtr(element.value, 0x20) || IsBadReadPtr(*(void**)element.value, 0x40))
            {
                ++unreadable;
                continue;
            }

            const auto name = utility::narrow(std::wstring{element.key, wcsnlen(element.key, 256)});
            const auto base = (uintptr_t)element.value;
            const auto flags = *(uint32_t*)(base + 0x18);

            std::string help{};
            const auto help_data = *(const wchar_t**)(base + 8);
            const auto help_count = *(int32_t*)(base + 0x10);
            if (help_data != nullptr && help_count > 0 && help_count < 8192 &&
                !IsBadReadPtr(help_data, (size_t)help_count * sizeof(wchar_t)))
            {
                std::wstring text{help_data, wcsnlen(help_data, (size_t)help_count)};
                text = text.substr(0, (std::min)(text.find_first_of(L"\r\n"), (size_t)200));
                std::replace(text.begin(), text.end(), L'\t', L' ');
                help = utility::narrow(text);
            }

            const bool is_command = element.value->AsCommand() != nullptr;
            std::string value{};
            if (!is_command) {
                const auto f = static_cast<sdk::IConsoleVariable*>(element.value)->GetFloat();
                char buf[64]{};
                if (f == std::floor(f) && std::abs(f) < 1e15f) {
                    snprintf(buf, sizeof(buf), "%lld", (long long)f);
                } else {
                    snprintf(buf, sizeof(buf), "%g", f);
                }
                value = buf;
                ++vars;
            } else {
                ++commands;
            }

            char flag_text[16]{};
            snprintf(flag_text, sizeof(flag_text), "%x", flags);
            out << name << '\t' << (is_command ? "cmd" : "var") << '\t' << value << '\t' << flag_text << '\t' << help << '\n';
        } catch (...) {
            ++unreadable;
        }
    }

    SPDLOG_INFO("[CvarDump] {} variables, {} commands, {} unreadable -> {}", vars, commands, unreadable, path.string());
}
} // namespace uevr::cvar_dump
