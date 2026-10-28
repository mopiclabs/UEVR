// Automatic resolution's game-thread actuators (VR_AutoRes, auto-resolution design v1 Appendix B): the game's screen
// percentage and its DLSS quality, stepped below the game's own values and put back when the steps are undone.
// AutoResController sets the targets from the present thread; on_pre_engine_tick carries them out here, at once when a
// target changes and once a second otherwise, to notice a game that puts its own value back.
//
// A game changing the value itself (its settings menu, a level load) makes that value the new base: the steps stay
// relative to what the game or the player chose last.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <sdk/CVar.hpp>
#include <sdk/ConsoleManager.hpp>
#include <sdk/FField.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/UClass.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/UGameEngine.hpp>
#include <sdk/UObjectArray.hpp>

#include <utility/String.hpp>

#include "AutoResPolicy.hpp"
#include "CVarManager.hpp"
#include "utility/Logging.hpp"

namespace {
using AutoResCVar = CVarManager::AutoResCVar;

// UE's SetBy priorities in the top byte of a Set's flags, from SetByScalability up to SetByConsole as
// enforce_frame_generation_cvars numbers them (UE5 inserts SetByGameOverride, which moves the upper names by one; the
// read-back below is what counts).
constexpr uint32_t set_by_lowest = 0x1000000;
constexpr uint32_t set_by_step = 0x1000000;
constexpr uint32_t set_by_console = 0x9000000;

bool resolve(AutoResCVar& c) {
    if (c.cvar != nullptr) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now < c.next_lookup) {
        return false;
    }

    c.next_lookup = now + std::chrono::seconds{5};
    ++c.lookups;

    const auto console_manager = sdk::FConsoleManager::get();
    auto object = console_manager != nullptr ? console_manager->find(c.name) : nullptr;

    if (object != nullptr && object->AsCommand() == nullptr) {
        c.cvar = (sdk::IConsoleVariable*)object;
    }

    return c.cvar != nullptr;
}

std::optional<float> read(AutoResCVar& c) {
    if (c.is_int) {
        auto value = c.cvar->TryGetInt();

        if (!value) {
            (void)c.cvar->GetInt(); // locates the vtable indices TryGetInt uses for this cvar's class
            value = c.cvar->TryGetInt();
        }

        return value ? std::optional<float>{(float)*value} : std::nullopt;
    }

    auto value = c.cvar->TryGetFloat();

    if (!value) {
        (void)c.cvar->GetFloat();
        value = c.cvar->TryGetFloat();
    }

    return value && std::isfinite(*value) ? value : std::nullopt;
}

std::wstring text_of(const AutoResCVar& c, float value) {
    return c.is_int ? std::to_wstring((int)std::lround(value)) : std::to_wstring(value);
}

// Sets the value and reads it back. UE ignores a Set below the priority the value was last set with and keeps the
// priority of a Set it takes, so the lowest priority it takes is the one the cvar already has: the priorities are tried
// from the bottom (or from ours last time), and the game's own settings can still change the cvar later, as before.
// False when the game keeps its own value even at SetByConsole.
bool write(AutoResCVar& c, float value) {
    const auto text = text_of(c, value);

    for (auto priority = c.priority != 0 ? c.priority : set_by_lowest; priority <= set_by_console; priority += set_by_step) {
        c.cvar->Set(text.c_str(), priority);

        if (const auto now = read(c); now && std::abs(*now - value) < 0.01f) {
            c.priority = priority;
            c.written = value;
            return true;
        }
    }

    return false;
}

// Puts the game's own value back at the priority ours took; the next move reads the base again. Forgotten first, so a
// Set that throws is not tried again every second. A value that is no longer ours was set by the game or the player
// since, and stays.
void restore(AutoResCVar& c) {
    const auto written = std::exchange(c.written, std::nullopt);
    const auto base = std::exchange(c.base, std::nullopt);
    const auto priority = std::exchange(c.priority, 0u);

    if (c.cvar == nullptr || !written || !base) {
        return;
    }

    if (const auto current = read(c); current && std::abs(*current - *written) > 0.01f) {
        SPDLOG_INFO("[autores] {} is {} now, not ours ({}): left as it is", utility::narrow(c.name), *current, *written);
        return;
    }

    const auto text = text_of(c, *base);
    c.cvar->Set(text.c_str(), priority);
    const auto now = read(c);
    SPDLOG_INFO("[autores] {} put back {} -> {}", utility::narrow(c.name), *written, now ? std::to_string(*now) : "?");
}

// The game's value right now; a value that is no longer ours becomes the new base.
std::optional<float> refresh_base(AutoResCVar& c) {
    const auto current = read(c);

    if (!current) {
        return std::nullopt;
    }

    if (!c.written) {
        c.base = *current;
    } else if (std::abs(*current - *c.written) > 0.01f) {
        SPDLOG_INFO("[autores] the game set {} to {} (ours was {}); stepping from it now", utility::narrow(c.name), *current,
            *c.written);
        const auto now = std::chrono::steady_clock::now();
        c.taken_back = now - c.last_taken_back < std::chrono::seconds{30} ? c.taken_back + 1 : 1;
        c.last_taken_back = now;
        c.base = *current;
        c.written.reset();
        c.priority = 0; // the game's write may have raised it: found again from the bottom
    }

    return current;
}
}

void CVarManager::process_autores(sdk::UGameEngine* engine) {
    const auto sp_target = m_autores_sp_target.load();
    const auto upscaler_target = m_autores_upscaler_target.load();
    const auto now = std::chrono::steady_clock::now();
    const auto changed = sp_target != m_autores_sp_processed || upscaler_target != m_autores_upscaler_processed;
    // Moved away from the game's values, or asked to: checked once a second, which also retries a cvar that isn't there
    // yet until it is found or given up on.
    const auto active = sp_target < 100 || upscaler_target > 0 || m_autores_sp.written.has_value() ||
        m_autores_dlss_quality.written.has_value() || m_autores_dlss_mode.written.has_value();

    try {
        if (m_autores_active) {
            probe_autores_upscaler(engine);
        }
    } catch (...) {
        SPDLOG_WARN("[autores] looking for the game's upscaler threw; no upscaler steps");
        m_autores_upscaler_kind = (int)uevr::autores::Upscaler::none;
        m_autores_upscaler_unavailable = true;
    }

    if (!changed && (!active || now < m_autores_next_check)) {
        return;
    }

    m_autores_next_check = now + std::chrono::seconds{1};
    m_autores_sp_processed = sp_target;
    m_autores_upscaler_processed = upscaler_target;

    try {
        process_autores_sp();
    } catch (...) {
        SPDLOG_WARN("[autores] reading or setting r.ScreenPercentage threw; no SP steps");
        m_autores_sp_unavailable = true;
    }

    // Steps asked for before the probe has an answer wait for it.
    const auto kind = (uevr::autores::Upscaler)m_autores_upscaler_kind.load();

    try {
        if (kind == uevr::autores::Upscaler::dlss_quality) {
            process_autores_dlss_quality();
        } else if (kind == uevr::autores::Upscaler::dlss_mode) {
            process_autores_dlss_mode(engine);
        }
    } catch (...) {
        SPDLOG_WARN("[autores] the DLSS actuator threw; no upscaler steps");
        m_autores_upscaler_unavailable = true;
    }
}

// SP: 100 / 85 / 70 % of the game's own r.ScreenPercentage. The swapchain and render targets keep their size; the
// game renders fewer pixels into them and upscales (TAAU / TSR), so a step costs no hitch.
void CVarManager::process_autores_sp() {
    const auto target = m_autores_sp_target.load();
    auto& sp = m_autores_sp;

    if (target >= 100 || m_autores_sp_unavailable) {
        if (sp.written) {
            restore(sp);
            restore(m_autores_resize_method);
        }

        m_autores_sp_applied = 100;
        return;
    }

    const auto unavailable = [&](const char* why) {
        SPDLOG_WARN("[autores] no SP steps: {}", why);
        restore(sp);
        restore(m_autores_resize_method);
        m_autores_sp_unavailable = true;
        m_autores_sp_applied = 100;
    };

    if (!resolve(sp)) {
        if (sp.lookups >= 12) {
            unavailable("r.ScreenPercentage was not found in a minute");
        }

        return;
    }

    const auto current = refresh_base(sp);

    if (!current || !sp.base || *sp.base <= 0.0f) {
        unavailable("r.ScreenPercentage has no readable value");
        return;
    }

    if (sp.taken_back >= 3) {
        unavailable("the game keeps putting its own r.ScreenPercentage back");
        return;
    }

    const auto value = std::round(*sp.base * (float)target / 100.0f);

    if (std::abs(*current - value) < 0.01f) {
        m_autores_sp_applied = target;
        return;
    }

    // Keep the scene textures at their largest size, so a lower percentage renders into part of them instead of
    // reallocating them (design v1 Appendix B).
    if (!m_autores_resize_method.written && resolve(m_autores_resize_method)) {
        if (const auto method = refresh_base(m_autores_resize_method); method && *method != 2.0f) {
            if (!write(m_autores_resize_method, 2.0f)) {
                SPDLOG_INFO("[autores] r.SceneRenderTargetResizeMethod stays {}", *method);
            }
        }
    }

    if (!write(sp, value)) {
        unavailable("the game keeps r.ScreenPercentage at its own value");
        return;
    }

    m_autores_sp_applied = target;
    SPDLOG_INFO("[autores] r.ScreenPercentage {} -> {} ({} % of the game's {}, set at priority {:x})", *current, value, target,
        *sp.base, sp.priority);
}

// The UE4 DLSS plugin (Hogwarts Legacy): r.NGX.DLSS.Quality -2 (Ultra Performance) .. 2 (Ultra Quality) sets the
// input resolution and r.ScreenPercentage is ignored. One quality lower per step.
void CVarManager::process_autores_dlss_quality() {
    const auto steps = m_autores_upscaler_target.load();
    auto& quality = m_autores_dlss_quality;

    if (steps <= 0 || m_autores_upscaler_unavailable) {
        restore(quality);
        m_autores_upscaler_applied = 0;
        return;
    }

    const auto unavailable = [&](const char* why) {
        SPDLOG_WARN("[autores] no upscaler steps: {}", why);
        restore(quality);
        m_autores_upscaler_unavailable = true;
        m_autores_upscaler_applied = 0;
    };

    if (!resolve(quality) || !resolve(m_autores_dlss_enable)) {
        if (quality.lookups >= 12 || m_autores_dlss_enable.lookups >= 12) {
            unavailable("r.NGX.DLSS.Quality / r.NGX.DLSS.Enable were not found in a minute");
        }

        return;
    }

    if (const auto enabled = read(m_autores_dlss_enable); !enabled || *enabled < 0.5f) {
        unavailable("DLSS is off (r.NGX.DLSS.Enable)");
        return;
    }

    const auto current = refresh_base(quality);

    if (!current || !quality.base) {
        unavailable("r.NGX.DLSS.Quality has no readable value");
        return;
    }

    m_autores_upscaler_max_steps = (std::max)(0, (int)std::lround(*quality.base) + 2);
    const auto value = (std::max)(-2.0f, std::round(*quality.base) - (float)steps);
    const auto applied = (int)std::lround(*quality.base - value);

    if (applied == 0) {
        unavailable("the game already runs DLSS Ultra Performance");
        return;
    }

    if (std::abs(*current - value) >= 0.01f) {
        if (!write(quality, value)) {
            unavailable("the game keeps r.NGX.DLSS.Quality at its own value");
            return;
        }

        SPDLOG_INFO("[autores] r.NGX.DLSS.Quality {} -> {} ({} below the game's {})", *current, value, applied, *quality.base);
    }

    m_autores_upscaler_applied = applied;
}

namespace {
// A UFunction's parameter by name, as the experiment found them (2026-10-23, Black Myth: Wukong): offsets only, so the
// sizes come from the next parameter's offset.
std::optional<int32_t> param_offset(sdk::UFunction* function, std::wstring_view name) {
    for (auto field = function->get_child_properties(); field != nullptr; field = field->get_next()) {
        if (field->get_field_name().to_string() == name) {
            return ((sdk::FProperty*)field)->get_offset();
        }
    }

    return std::nullopt;
}

int32_t param_size(sdk::UFunction* function, int32_t offset) {
    int32_t next = function->get_parms_size();

    for (auto field = function->get_child_properties(); field != nullptr; field = field->get_next()) {
        const auto other = ((sdk::FProperty*)field)->get_offset();

        if (other > offset && other < next) {
            next = other;
        }
    }

    return next - offset;
}

bool fits(sdk::UFunction* function, std::optional<int32_t> offset, int32_t size) {
    return offset && *offset >= 0 && *offset + size <= (int32_t)function->get_parms_size();
}

// A static Blueprint library function, called on the library's default object. WorldContextObject gets the game world:
// a plugin build that resolves the world from it must not get null.
bool call(sdk::UClass* library, sdk::UFunction* function, std::vector<uint8_t>& parms, sdk::UGameEngine* engine) {
    const auto object = library->get_class_default_object();

    if (object == nullptr) {
        return false;
    }

    if (const auto world_context = param_offset(function, L"WorldContextObject"); fits(function, world_context, sizeof(void*))) {
        const auto world = engine != nullptr ? (sdk::UObject*)engine->get_world() : nullptr;

        if (world == nullptr) {
            return false;
        }

        *(sdk::UObject**)(parms.data() + *world_context) = world;
    }

    object->process_event(function, parms.data());
    return true;
}

// The UE5 DLSS plugin's Blueprint library (UDLSSLibrary).
using AutoResDlssMode = CVarManager::AutoResDlssMode;

// One lookup per session: true when the library and its four functions are there. A miss walks every object, and the
// plugin's native classes are registered at startup, long before the OpenXR session.
bool find_dlss_library(AutoResDlssMode& dlss) {
    if (dlss.library != nullptr) {
        return true;
    }

    if (dlss.incomplete || dlss.lookups > 0) {
        return false;
    }

    ++dlss.lookups;

    const auto found = sdk::find_uobject<sdk::UClass>(L"Class /Script/DLSSBlueprint.DLSSLibrary");

    if (found == nullptr) {
        return false;
    }

    dlss.get_mode = found->find_function(L"GetDLSSMode");
    dlss.set_mode = found->find_function(L"SetDLSSMode");
    dlss.get_info = found->find_function(L"GetDLSSModeInformation");
    dlss.get_supported = found->find_function(L"GetSupportedDLSSModes");

    if (dlss.get_mode == nullptr || dlss.set_mode == nullptr || dlss.get_info == nullptr || dlss.get_supported == nullptr) {
        SPDLOG_WARN("[autores] UDLSSLibrary lacks a function: GetDLSSMode={} SetDLSSMode={} GetDLSSModeInformation={} "
            "GetSupportedDLSSModes={}", dlss.get_mode != nullptr, dlss.set_mode != nullptr, dlss.get_info != nullptr,
            dlss.get_supported != nullptr);
        dlss.incomplete = true;
        return false;
    }

    dlss.library = found;
    return true;
}

std::optional<uint8_t> dlss_get_mode(const AutoResDlssMode& dlss, sdk::UGameEngine* engine) {
    const auto function = (sdk::UFunction*)dlss.get_mode;
    std::vector<uint8_t> parms(function->get_parms_size());
    const auto ret = (int32_t)function->get_return_value_offset();

    if (ret < 0 || ret >= (int32_t)parms.size() || !call((sdk::UClass*)dlss.library, function, parms, engine)) {
        return std::nullopt;
    }

    return parms[ret];
}

bool dlss_set_mode(const AutoResDlssMode& dlss, uint8_t mode, sdk::UGameEngine* engine) {
    const auto function = (sdk::UFunction*)dlss.set_mode;
    const auto offset = param_offset(function, L"DLSSMode");

    if (!fits(function, offset, 1)) {
        return false;
    }

    std::vector<uint8_t> parms(function->get_parms_size());
    parms[*offset] = mode;
    return call((sdk::UClass*)dlss.library, function, parms, engine) && dlss_get_mode(dlss, engine) == mode;
}

// A mode's optimal screen percentage at 3840x2160; none when the mode is off or unsupported.
std::optional<float> dlss_optimal(const AutoResDlssMode& dlss, uint8_t mode, sdk::UGameEngine* engine) {
    const auto function = (sdk::UFunction*)dlss.get_info;
    const auto mode_offset = param_offset(function, L"DLSSMode");
    const auto resolution_offset = param_offset(function, L"ScreenResolution");
    const auto supported_offset = param_offset(function, L"bIsSupported");
    const auto optimal_offset = param_offset(function, L"OptimalScreenPercentage");

    if (!fits(function, mode_offset, 1) || !fits(function, resolution_offset, 8) || !fits(function, supported_offset, 1) ||
        !fits(function, optimal_offset, 4)) {
        return std::nullopt;
    }

    std::vector<uint8_t> parms(function->get_parms_size());
    parms[*mode_offset] = mode;

    // FVector2D: two floats, or two doubles from UE 5.0 on.
    if (param_size(function, *resolution_offset) >= 16) {
        const double resolution[2]{3840.0, 2160.0};
        std::memcpy(parms.data() + *resolution_offset, resolution, sizeof(resolution));
    } else {
        const float resolution[2]{3840.0f, 2160.0f};
        std::memcpy(parms.data() + *resolution_offset, resolution, sizeof(resolution));
    }

    if (!call((sdk::UClass*)dlss.library, function, parms, engine)) {
        return std::nullopt;
    }

    float optimal{};
    std::memcpy(&optimal, parms.data() + *optimal_offset, sizeof(optimal));
    return parms[*supported_offset] != 0 && std::isfinite(optimal) && optimal > 0.0f ? std::optional{optimal} : std::nullopt;
}

// GetSupportedDLSSModes: a TArray<UDLSSMode> (data, count, capacity). The engine allocated it; the few bytes are left
// to it.
std::vector<uint8_t> dlss_supported(const AutoResDlssMode& dlss, sdk::UGameEngine* engine) {
    const auto function = (sdk::UFunction*)dlss.get_supported;
    const auto ret = (int32_t)function->get_return_value_offset();
    std::vector<uint8_t> modes{};

    if (fits(function, ret, 16)) {
        std::vector<uint8_t> parms(function->get_parms_size());

        if (call((sdk::UClass*)dlss.library, function, parms, engine)) {
            const auto data = *(const uint8_t**)(parms.data() + ret);
            const auto count = *(const int32_t*)(parms.data() + ret + 8);

            for (int32_t i = 0; data != nullptr && i < count && i < 16; ++i) {
                modes.push_back(data[i]);
            }
        }
    }

    return modes;
}
}

// Which upscaler sets the game's input resolution, found once per session: the UE5 DLSS plugin with a mode that is on,
// else the UE4 plugin's cvars with DLSS on, else none (after 30 s). Under DLSS r.ScreenPercentage does nothing (Black
// Myth: Wukong, Hogwarts Legacy, 2026-10-24), so SP is left out there.
void CVarManager::probe_autores_upscaler(sdk::UGameEngine* engine) {
    using uevr::autores::Upscaler;

    if (m_autores_upscaler_kind != (int)Upscaler::unknown) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now < m_autores_probe_next) {
        return;
    }

    m_autores_probe_next = now + std::chrono::seconds{2};
    ++m_autores_probe_attempts;
    auto kind = Upscaler::unknown;

    if (find_dlss_library(m_autores_dlss_mode)) {
        if (const auto mode = dlss_get_mode(m_autores_dlss_mode, engine); mode && dlss_optimal(m_autores_dlss_mode, *mode, engine)) {
            kind = Upscaler::dlss_mode;
        }
    }

    if (kind == Upscaler::unknown && resolve(m_autores_dlss_quality) && resolve(m_autores_dlss_enable)) {
        if (const auto enabled = read(m_autores_dlss_enable); enabled && *enabled >= 0.5f) {
            kind = Upscaler::dlss_quality;
        }
    }

    if (kind == Upscaler::unknown && m_autores_probe_attempts < 15) {
        return;
    }

    if (kind == Upscaler::unknown) {
        kind = Upscaler::none;
    }

    m_autores_upscaler_kind = (int)kind;

    if (kind == Upscaler::none) {
        SPDLOG_INFO("[autores] no DLSS sets the input resolution: SP steps, no upscaler steps");
        m_autores_upscaler_unavailable = true;
    } else {
        SPDLOG_INFO("[autores] {} sets the input resolution: upscaler steps, no SP steps",
            kind == Upscaler::dlss_mode ? "UDLSSLibrary's DLSS mode" : "r.NGX.DLSS.Quality");
        m_autores_sp_unavailable = true;
    }
}

// The UE5 DLSS plugin (Black Myth: Wukong): its input resolution follows the DLSS mode. The modes are ranked by
// GetDLSSModeInformation's optimal screen percentage instead of assuming the plugin's enum values, and each step takes
// the next mode that renders fewer pixels.
void CVarManager::process_autores_dlss_mode(sdk::UGameEngine* engine) {
    const auto steps = m_autores_upscaler_target.load();
    auto& dlss = m_autores_dlss_mode;

    if (steps <= 0 || m_autores_upscaler_unavailable) {
        // Forgotten first (a call that throws is not tried again); a mode that is no longer ours stays.
        const auto written = std::exchange(dlss.written, std::nullopt);
        const auto base = std::exchange(dlss.base, std::nullopt);
        m_autores_upscaler_applied = 0;

        if (written && base && dlss.library != nullptr) {
            if (const auto current = dlss_get_mode(dlss, engine); current != written) {
                SPDLOG_INFO("[autores] the DLSS mode is {} now, not ours ({}): left as it is", current ? (int)*current : -1, *written);
            } else {
                const auto ok = dlss_set_mode(dlss, *base, engine);
                SPDLOG_INFO("[autores] DLSS mode put back {} -> {} (ok={})", *written, *base, ok);
            }
        }

        return;
    }

    const auto unavailable = [&](const char* why) {
        SPDLOG_WARN("[autores] no upscaler steps: {}", why);
        m_autores_upscaler_unavailable = true;
        process_autores_dlss_mode(engine); // puts the game's mode back
    };

    if (dlss.library == nullptr) {
        unavailable("UDLSSLibrary is gone");
        return;
    }

    const auto current = dlss_get_mode(dlss, engine);

    if (!current) {
        unavailable("GetDLSSMode has no return value");
        return;
    }

    if (dlss.written && *current != *dlss.written) {
        SPDLOG_INFO("[autores] the game set the DLSS mode to {} (ours was {}); stepping from it now", *current, *dlss.written);
        dlss.written.reset();
    }

    // The game's mode, and the supported modes that render fewer pixels than it.
    if (!dlss.written && dlss.base != current) {
        dlss.base = *current;
        dlss.lower.clear();

        std::string ranked{};
        std::vector<std::pair<float, uint8_t>> by_pixels{};

        for (const auto mode : dlss_supported(dlss, engine)) {
            if (const auto optimal = dlss_optimal(dlss, mode, engine)) {
                by_pixels.emplace_back(*optimal, mode);
                ranked += fmt::format(" {}:{:.0f}%", mode, *optimal * (*optimal <= 1.0f ? 100.0f : 1.0f));
            }
        }

        const auto base_optimal = dlss_optimal(dlss, *current, engine);
        // Fewer pixels first; Auto reports one of the fixed modes' percentages, so a tie keeps one mode, the higher
        // value (Auto is 1, below the fixed modes, in every plugin version).
        std::sort(by_pixels.begin(), by_pixels.end(),
            [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
        float last_optimal = -1.0f;

        for (const auto& [optimal, mode] : by_pixels) {
            if (base_optimal && optimal < *base_optimal * 0.99f && std::abs(optimal - last_optimal) > 0.01f * optimal) {
                dlss.lower.push_back(mode);
                last_optimal = optimal;
            }
        }

        m_autores_upscaler_max_steps = (int)dlss.lower.size();

        SPDLOG_INFO("[autores] DLSS mode {} (optimal {}); supported{}; {} lower", *current,
            base_optimal ? fmt::format("{:.2f}", *base_optimal) : "none", ranked, dlss.lower.size());

        if (!base_optimal) {
            dlss.base.reset();
            unavailable("the game's DLSS mode is off or has no optimal screen percentage");
            return;
        }
    }

    if (dlss.lower.empty()) {
        unavailable("no supported DLSS mode renders fewer pixels than the game's");
        return;
    }

    const auto applied = (std::min)(steps, (int)dlss.lower.size());
    const auto target = dlss.lower[applied - 1];

    if (*current != target) {
        if (!dlss_set_mode(dlss, target, engine)) {
            unavailable("SetDLSSMode did not take");
            return;
        }

        dlss.written = target;
        SPDLOG_INFO("[autores] DLSS mode {} -> {} ({} below the game's {})", *current, target, applied, *dlss.base);
    }

    m_autores_upscaler_applied = applied;
}
