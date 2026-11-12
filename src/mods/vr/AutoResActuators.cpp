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
// enforce_native_stereo_fix_cvars numbers them (UE5 inserts SetByGameOverride, which moves the upper names by one; the
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
// since, and stays, unless ours_anyway: a call of ours changed it (SetDLSSMode sets r.ScreenPercentage).
void restore(AutoResCVar& c, bool ours_anyway = false) {
    const auto written = std::exchange(c.written, std::nullopt);
    const auto base = std::exchange(c.base, std::nullopt);
    const auto priority = std::exchange(c.priority, 0u);

    if (c.cvar == nullptr || !written || !base) {
        return;
    }

    if (const auto current = read(c); current && std::abs(*current - *written) > 0.01f && !ours_anyway) {
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

    // A new activation looks again from the start (the 2 s phase, nothing assumed from the last session).
    const auto active_now = m_autores_active.load();

    if (active_now && !m_autores_was_active) {
        m_autores_probe = uevr::autores::UpscalerProbe{};
        m_autores_probe_started = {};
        m_autores_probe_next = {};
        m_autores_upscaler_kind = (int)uevr::autores::Upscaler::unknown;
    }

    m_autores_was_active = active_now;

    // A probe that threw is not tried again this run: the same faulting call would come back every few seconds.
    try {
        if (active_now && !m_autores_probe_failed) {
            probe_autores_upscaler(engine);
        }
    } catch (...) {
        SPDLOG_WARN("[autores] looking for the game's upscaler threw; no upscaler steps, SP steps, not looked for again");
        m_autores_probe_failed = true;
        m_autores_probe.kind = uevr::autores::Upscaler::none;
        m_autores_upscaler_kind = (int)uevr::autores::Upscaler::none;
        m_autores_upscaler_unavailable = true;
    }

    const auto kind_now = m_autores_upscaler_kind.load();
    const auto floor_now = m_autores_upscaler_floor.load();
    const auto changed = sp_target != m_autores_sp_processed || upscaler_target != m_autores_upscaler_processed ||
        kind_now != m_autores_kind_processed || floor_now != m_autores_floor_processed;
    // Moved away from the game's values, or asked to: checked once a second, which also retries a cvar that isn't there
    // yet until it is found or given up on.
    const auto active = sp_target < 100 || upscaler_target > 0 || m_autores_sp.written.has_value() ||
        m_autores_dlss_quality.written.has_value() || m_autores_dlss_mode.written.has_value();

    if (!changed && (!active || now < m_autores_next_check)) {
        return;
    }

    m_autores_next_check = now + std::chrono::seconds{1};
    m_autores_sp_processed = sp_target;
    m_autores_upscaler_processed = upscaler_target;
    m_autores_kind_processed = kind_now;
    m_autores_floor_processed = floor_now;

    try {
        process_autores_sp();
    } catch (...) {
        SPDLOG_WARN("[autores] reading or setting r.ScreenPercentage threw; no SP steps");
        m_autores_sp_unavailable = true;
    }

    // The upscaler that sets the input resolution now; a mode or quality ours from before it changed is put back.
    const auto kind = (uevr::autores::Upscaler)kind_now;

    try {
        // A switch from one DLSS kind to the other (a game with both, read as one before a world answered the other)
        // puts the other kind's step back first, so the two never stack.
        if (kind == uevr::autores::Upscaler::dlss_quality) {
            if (m_autores_dlss_mode.written) {
                put_back_dlss_mode(engine);
            }

            process_autores_dlss_quality();
        } else if (kind == uevr::autores::Upscaler::dlss_mode) {
            if (m_autores_dlss_quality.written) {
                restore(m_autores_dlss_quality);
            }

            process_autores_dlss_mode(engine);
        } else {
            restore_autores_upscalers(engine);
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
    const auto kind = (uevr::autores::Upscaler)m_autores_upscaler_kind.load();
    // Under DLSS r.ScreenPercentage does nothing (Black Myth: Wukong, Hogwarts Legacy, 2026-10-24).
    const auto under_dlss = kind == uevr::autores::Upscaler::dlss_mode || kind == uevr::autores::Upscaler::dlss_quality;

    if (target >= 100 || m_autores_sp_unavailable || under_dlss) {
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

    // Turned off since the probe found it: the probe makes it none after two reads, and SP takes over then. The next
    // read comes sooner, but still 2 s apart, so a DLSS off for a moment (a settings menu applying) is not none.
    if (const auto enabled = read(m_autores_dlss_enable); !enabled || *enabled < 0.5f) {
        restore(quality);
        m_autores_upscaler_applied = 0;
        probe_autores_upscaler_soon();
        return;
    }

    const auto current = refresh_base(quality);

    if (!current || !quality.base) {
        unavailable("r.NGX.DLSS.Quality has no readable value");
        return;
    }

    const auto floor = (float)uevr::autores::dlss_quality_floor((uevr::autores::UpscalerFloor)m_autores_upscaler_floor.load());
    m_autores_upscaler_max_steps = (std::max)(0, (int)std::lround(std::round(*quality.base) - floor));
    const auto value = (std::max)(floor, std::round(*quality.base) - (float)steps);
    const auto applied = (std::max)(0, (int)std::lround(*quality.base - value));

    // The game's own quality is at the floor already (or below it): nothing to step, nothing of ours to keep.
    if (applied == 0) {
        restore(quality);
        m_autores_upscaler_applied = 0;
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

// True when the library and its four functions are there. A miss walks every object on the game thread (a name that
// isn't there is not cached), so it is looked for three times a minute apart and then no more: the plugin's native
// classes are registered at startup, long before the OpenXR session, and the retries only cover a lookup made too early.
bool find_dlss_library(AutoResDlssMode& dlss) {
    if (dlss.library != nullptr) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();

    if (dlss.incomplete || dlss.lookups >= 3 || now < dlss.next_lookup) {
        return false;
    }

    ++dlss.lookups;
    dlss.next_lookup = now + std::chrono::minutes{1};

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

// GetDLSSModeInformation for a mode at 3840x2160: whether the call was made, bIsSupported and OptimalScreenPercentage.
struct DlssModeInfo {
    bool called{false};
    bool supported{false};
    float optimal{};
};

DlssModeInfo dlss_info(const AutoResDlssMode& dlss, uint8_t mode, sdk::UGameEngine* engine);

// A mode's optimal screen percentage at 3840x2160; none when the mode is off or unsupported.
std::optional<float> dlss_optimal(const AutoResDlssMode& dlss, uint8_t mode, sdk::UGameEngine* engine) {
    const auto info = dlss_info(dlss, mode, engine);
    return info.called && info.supported && std::isfinite(info.optimal) && info.optimal > 0.0f ? std::optional{info.optimal}
                                                                                              : std::nullopt;
}

DlssModeInfo dlss_info(const AutoResDlssMode& dlss, uint8_t mode, sdk::UGameEngine* engine) {
    const auto function = (sdk::UFunction*)dlss.get_info;
    const auto mode_offset = param_offset(function, L"DLSSMode");
    const auto resolution_offset = param_offset(function, L"ScreenResolution");
    const auto supported_offset = param_offset(function, L"bIsSupported");
    const auto optimal_offset = param_offset(function, L"OptimalScreenPercentage");

    if (!fits(function, mode_offset, 1) || !fits(function, resolution_offset, 8) || !fits(function, supported_offset, 1) ||
        !fits(function, optimal_offset, 4)) {
        return {};
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
        return {};
    }

    DlssModeInfo info{.called = true, .supported = parms[*supported_offset] != 0};
    std::memcpy(&info.optimal, parms.data() + *optimal_offset, sizeof(info.optimal));
    return info;
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

// What sets the game's input resolution right now: the UE5 DLSS plugin with a mode that is on, else the UE4 plugin's
// cvars with DLSS on. off: one of them answered and DLSS is off (or neither plugin is in the game); no_answer: nothing
// could be read (a call needs the world, a cvar is not registered yet). seen says what was read, for the log.
uevr::autores::ProbeReading CVarManager::read_autores_upscaler(sdk::UGameEngine* engine, std::string& seen) {
    using uevr::autores::ProbeReading;
    auto& dlss = m_autores_dlss_mode;
    bool answered = false;

    if (find_dlss_library(dlss)) {
        if (const auto mode = dlss_get_mode(dlss, engine)) {
            const auto info = dlss_info(dlss, *mode, engine);
            seen = fmt::format("UDLSSLibrary: GetDLSSMode {}, GetDLSSModeInformation {}", *mode,
                info.called ? fmt::format("supported={} optimal={:.3f}", info.supported, info.optimal) : std::string{"no answer"});

            if (info.called && info.supported && std::isfinite(info.optimal) && info.optimal > 0.0f) {
                return ProbeReading::dlss_mode;
            }

            answered = info.called;
        } else {
            seen = "UDLSSLibrary: GetDLSSMode no answer";
        }
    } else {
        seen = dlss.incomplete ? "UDLSSLibrary lacks a function" : "no UDLSSLibrary";
    }

    const auto quality = resolve(m_autores_dlss_quality) ? read(m_autores_dlss_quality) : std::nullopt;
    const auto enable = resolve(m_autores_dlss_enable) ? read(m_autores_dlss_enable) : std::nullopt;
    seen += fmt::format("; r.NGX.DLSS.Enable {}, r.NGX.DLSS.Quality {}",
        m_autores_dlss_enable.cvar == nullptr ? std::string{"not found"} : enable ? fmt::format("{}", *enable) : std::string{"?"},
        m_autores_dlss_quality.cvar == nullptr ? std::string{"not found"} : quality ? fmt::format("{}", *quality) : std::string{"?"});

    if (quality && enable) {
        if (*enable >= 0.5f) {
            return ProbeReading::dlss_quality;
        }

        answered = true;
    }

    // Neither plugin in the game (each looked for at least once): a plain answer.
    if (dlss.library == nullptr && dlss.lookups > 0 && m_autores_dlss_enable.cvar == nullptr && m_autores_dlss_enable.lookups > 0) {
        answered = true;
    }

    return answered ? ProbeReading::off : ProbeReading::no_answer;
}

// Which upscaler sets the game's input resolution, looked for the whole session (AutoResPolicy.hpp UpscalerProbe): every
// 2 s for 2 minutes, then every 10 s. Nothing in 30 s is none for now (SP steps); DLSS found later takes over, and a
// DLSS turned off goes back to SP. Under DLSS r.ScreenPercentage does nothing (Black Myth: Wukong, Hogwarts Legacy,
// 2026-10-24), so SP is left out while it sets the input resolution.
void CVarManager::probe_autores_upscaler(sdk::UGameEngine* engine) {
    using uevr::autores::Upscaler;
    using uevr::autores::UpscalerProbe;

    const auto now = std::chrono::steady_clock::now();

    if (now < m_autores_probe_next) {
        return;
    }

    m_autores_probe_last = now;

    if (m_autores_probe_started.time_since_epoch().count() == 0) {
        m_autores_probe_started = now;
    }

    const auto seconds = (int)std::chrono::duration_cast<std::chrono::seconds>(now - m_autores_probe_started).count();
    m_autores_probe_next = now + std::chrono::seconds{UpscalerProbe::interval_seconds(seconds)};

    check_autores_temporal_upscale();

    std::string seen{};
    const auto reading = read_autores_upscaler(engine, seen);
    const auto before = m_autores_probe.kind;
    const auto changed = m_autores_probe.update(reading);

    if (m_autores_probe.tries >= uevr::autores::UpscalerProbe::PROVISIONAL_TRIES && !m_autores_dynamic_resolution_logged) {
        m_autores_dynamic_resolution_logged = true;
        log_autores_dynamic_resolution();
    }

    if (!changed) {
        return;
    }

    const auto kind = m_autores_probe.kind;

    // How many steps lie above the floor, known before the first step (VR_AutoResOutputAnchor and the step order need it).
    if (kind == Upscaler::dlss_mode) {
        if (const auto mode = dlss_get_mode(m_autores_dlss_mode, engine); mode && !m_autores_dlss_mode.written) {
            rank_autores_dlss_modes(engine, *mode);
        }
    } else if (kind == Upscaler::dlss_quality) {
        if (const auto base = read(m_autores_dlss_quality)) {
            const auto floor = uevr::autores::dlss_quality_floor((uevr::autores::UpscalerFloor)m_autores_upscaler_floor.load());
            m_autores_upscaler_max_steps = (std::max)(0, (int)std::lround(*base) - floor);
        }
    }

    m_autores_upscaler_kind = (int)kind;

    if (kind == Upscaler::none && before == Upscaler::unknown) {
        SPDLOG_INFO("[autores] no DLSS sets the input resolution after {} tries in {} s ({}): SP steps for now, still looking",
            m_autores_probe.tries, seconds, seen);
    } else if (kind == Upscaler::none) {
        SPDLOG_INFO("[autores] DLSS is off now at +{} s ({}): upscaler steps put back, SP steps", seconds, seen);
        m_autores_upscaler_unavailable = false; // a DLSS turned on again starts over
    } else {
        SPDLOG_INFO("[autores] {} sets the input resolution, found at +{} s on try {} ({}): upscaler steps, no SP steps{}",
            kind == Upscaler::dlss_mode ? "UDLSSLibrary's DLSS mode" : "r.NGX.DLSS.Quality", seconds, m_autores_probe.tries, seen,
            before == Upscaler::none ? " (SP put back)" : "");
    }
}

// The other DLSS kind's next read: sooner after an off read, but not sooner than 2 s after the last one (the probe's two
// off reads in a row must be two separate moments).
void CVarManager::probe_autores_upscaler_soon() {
    m_autores_probe_next = (std::min)(m_autores_probe_next, m_autores_probe_last + std::chrono::seconds{2});
}

// A DLSS mode or quality of ours put back, when DLSS no longer sets the input resolution.
void CVarManager::restore_autores_upscalers(sdk::UGameEngine* engine) {
    if (m_autores_dlss_quality.written) {
        restore(m_autores_dlss_quality);
    }

    put_back_dlss_mode(engine);
    m_autores_upscaler_applied = 0;
    m_autores_upscaler_max_steps = -1;
}

// The game's DLSS mode and r.ScreenPercentage back, forgotten first (a call that throws is not tried again); a mode
// that is no longer ours stays. SetDLSSMode sets r.ScreenPercentage to the mode's optimal one (putting TEKKEN 8 Demo
// and E33 back to mode 4 left them at 66.7 %, the games had run 58 %, 2026-11-13), so after the mode the game's
// percentage goes back too.
void CVarManager::put_back_dlss_mode(sdk::UGameEngine* engine) {
    auto& dlss = m_autores_dlss_mode;
    const auto written = std::exchange(dlss.written, std::nullopt);
    const auto base = std::exchange(dlss.base, std::nullopt);
    bool mode_back = false;

    if (written && base && dlss.library != nullptr) {
        if (const auto current = dlss_get_mode(dlss, engine); current != written) {
            SPDLOG_INFO("[autores] the DLSS mode is {} now, not ours ({}): left as it is", current ? (int)*current : -1, *written);
        } else {
            mode_back = dlss_set_mode(dlss, *base, engine);
            SPDLOG_INFO("[autores] DLSS mode put back {} -> {} (ok={})", *written, *base, mode_back);
        }
    }

    restore(m_autores_dlss_sp, mode_back);
}

// The game's DLSS mode and the supported modes that render fewer pixels than it, down to the floor
// (AutoResPolicy.hpp dlss_modes_below). False when the game's mode has no optimal percentage (DLSS off).
bool CVarManager::rank_autores_dlss_modes(sdk::UGameEngine* engine, uint8_t current) {
    auto& dlss = m_autores_dlss_mode;
    const auto floor = (uevr::autores::UpscalerFloor)m_autores_upscaler_floor.load();
    std::string ranked{};
    std::vector<std::pair<float, uint8_t>> by_pixels{};

    for (const auto mode : dlss_supported(dlss, engine)) {
        if (const auto optimal = dlss_optimal(dlss, mode, engine)) {
            by_pixels.emplace_back(*optimal, mode);
            ranked += fmt::format(" {}:{:.0f}%", mode, *optimal * (*optimal <= 1.0f ? 100.0f : 1.0f));
        }
    }

    const auto base_optimal = dlss_optimal(dlss, current, engine);
    dlss.base = current;
    dlss.ranked_floor = (int)floor;

    // The UE5 plugin renders DLSS's input at r.ScreenPercentage; the mode picks its preset (TEKKEN 8 Demo and E33 ran
    // mode 4, Quality, at the game's 58 %, and a mode alone moved nothing, 2026-11-13). The steps go below what the
    // input is now, each setting a mode and its optimal percentage together, as the plugin asks of games.
    // An SP step of ours still in place (DLSS found after the SP steps began) is not the game's input: its base is.
    const auto sp_ours = m_autores_sp.written && m_autores_sp.base;
    const auto game_sp = sp_ours ? m_autores_sp.base : resolve(m_autores_dlss_sp) ? refresh_base(m_autores_dlss_sp) : std::nullopt;
    const auto input = sp_ours ? m_autores_sp.base
        : m_autores_dlss_sp.base && *m_autores_dlss_sp.base > 0.0f ? m_autores_dlss_sp.base
        : base_optimal ? std::optional{*base_optimal <= 1.0f ? *base_optimal * 100.0f : *base_optimal} : std::nullopt;
    dlss.lower = input ? uevr::autores::dlss_modes_below(by_pixels, *input, floor) : std::vector<uint8_t>{};
    dlss.lower_percent.clear();

    for (const auto mode : dlss.lower) {
        for (const auto& [optimal, m] : by_pixels) {
            if (m == mode) {
                dlss.lower_percent.push_back(optimal <= 1.0f ? optimal * 100.0f : optimal);
                break;
            }
        }
    }

    m_autores_upscaler_max_steps = (int)dlss.lower.size();

    std::string lower{};

    for (size_t i = 0; i < dlss.lower.size(); ++i) {
        lower += fmt::format(" {} ({:.1f} %)", dlss.lower[i], dlss.lower_percent[i]);
    }

    SPDLOG_INFO("[autores] DLSS mode {} (optimal {}), input r.ScreenPercentage {}; supported{}; down to the floor ({}):{}",
        current, base_optimal ? fmt::format("{:.2f}", *base_optimal) : "none",
        game_sp ? fmt::format("{}", *game_sp) : std::string{"-"}, ranked,
        floor == uevr::autores::UpscalerFloor::ultra_performance ? "Ultra Performance" : "Performance",
        lower.empty() ? std::string{" none"} : lower);

    if (!base_optimal) {
        dlss.base.reset();
    }

    return base_optimal.has_value();
}

// SP's floor: without a temporal upscaler the engine stretches the smaller image, so two steps (85 / 70 %); with one
// (UE5 r.AntiAliasingMethod 4, TSR; 2, TAA, with r.TemporalAA.Upsampling; UE4 r.TemporalAA.Upsampling), four (to 50 %).
void CVarManager::check_autores_temporal_upscale() {
    const auto method = resolve(m_autores_aa_method) ? read(m_autores_aa_method) : std::nullopt;
    const auto upsampling = resolve(m_autores_taa_upsampling) ? read(m_autores_taa_upsampling) : std::nullopt;
    int temporal = -1;

    if (method) {
        temporal = std::lround(*method) == 4 || (std::lround(*method) == 2 && upsampling && *upsampling >= 0.5f) ? 1 : 0;
    } else if (upsampling && m_autores_aa_method.lookups >= 2) {
        temporal = *upsampling >= 0.5f ? 1 : 0;
    } else if (m_autores_aa_method.lookups >= 2 && m_autores_taa_upsampling.lookups >= 2) {
        temporal = 0;
    }

    if (temporal < 0 || temporal == m_autores_temporal_seen) {
        return;
    }

    m_autores_temporal_seen = temporal;
    m_autores_sp_max_steps = temporal == 1 ? uevr::autores::SP_STEPS_TEMPORAL : uevr::autores::SP_STEPS;
    SPDLOG_INFO("[autores] {} (r.AntiAliasingMethod {}, r.TemporalAA.Upsampling {}): SP steps down to {} %",
        temporal == 1 ? "temporal upscaling" : "no temporal upscaling",
        method ? fmt::format("{}", *method) : std::string{"-"}, upsampling ? fmt::format("{}", *upsampling) : std::string{"-"},
        uevr::autores::SP_PERCENT[temporal == 1 ? uevr::autores::SP_STEPS_TEMPORAL : uevr::autores::SP_STEPS]);
}

// Read only (design v1 §7-57, a later option): the engine's own dynamic resolution and the screen percentages.
void CVarManager::log_autores_dynamic_resolution() {
    std::string text{};

    for (const auto name : {L"r.DynamicRes.OperationMode", L"r.DynamicRes.MinScreenPercentage", L"r.DynamicRes.MaxScreenPercentage",
             L"r.DynamicRes.FrameTimeBudget", L"r.ScreenPercentage", L"r.SecondaryScreenPercentage.GameViewport"}) {
        AutoResCVar c{name};
        const auto value = resolve(c) ? read(c) : std::nullopt;
        text += fmt::format(" {}={}", utility::narrow(name), c.cvar == nullptr ? std::string{"none"} : value ? fmt::format("{}", *value) : std::string{"?"});
    }

    SPDLOG_INFO("[autores] engine resolution cvars:{}", text);
}

// The UE5 DLSS plugin (Black Myth: Wukong): its input resolution follows the DLSS mode. The modes are ranked by
// GetDLSSModeInformation's optimal screen percentage instead of assuming the plugin's enum values, and each step takes
// the next mode that renders fewer pixels.
void CVarManager::process_autores_dlss_mode(sdk::UGameEngine* engine) {
    const auto steps = m_autores_upscaler_target.load();
    auto& dlss = m_autores_dlss_mode;

    if (steps <= 0 || m_autores_upscaler_unavailable) {
        // A mode that is no longer ours stays.
        m_autores_upscaler_applied = 0;
        put_back_dlss_mode(engine);
        return;
    }

    const auto unavailable = [&](const char* why) {
        SPDLOG_WARN("[autores] no upscaler steps: {}", why);
        m_autores_upscaler_unavailable = true;
        process_autores_dlss_mode(engine); // puts the game's mode and r.ScreenPercentage back
    };

    if (dlss.library == nullptr) {
        unavailable("UDLSSLibrary is gone");
        return;
    }

    const auto current = dlss_get_mode(dlss, engine);

    // No answer for now (a load, no world yet): tried again in a second; a step that never lands is caught by the
    // controller's step check.
    if (!current) {
        return;
    }

    if (dlss.written && *current != *dlss.written) {
        SPDLOG_INFO("[autores] the game set the DLSS mode to {} (ours was {}); stepping from it now", *current, *dlss.written);
        dlss.written.reset();
    }

    // The game's mode, and the supported modes below it down to the floor; ranked again when the game's mode or the floor
    // changed.
    const auto floor = m_autores_upscaler_floor.load();

    if ((!dlss.written && dlss.base != current) || dlss.ranked_floor != floor) {
        // Turned off since the probe found it: the probe makes it none after two reads, and SP takes over then.
        if (!rank_autores_dlss_modes(engine, dlss.written && dlss.base ? *dlss.base : *current)) {
            m_autores_upscaler_applied = 0;
            probe_autores_upscaler_soon();
            return;
        }
    }

    // The game's own mode is at the floor already: nothing to step.
    if (dlss.lower.empty()) {
        m_autores_upscaler_applied = 0;

        if (dlss.written) {
            put_back_dlss_mode(engine);
        }

        return;
    }

    const auto applied = (std::min)(steps, (int)dlss.lower.size());
    const auto target = dlss.lower[applied - 1];
    const auto target_sp = std::round(dlss.lower_percent[applied - 1] * 10.0f) / 10.0f;
    auto& sp = m_autores_dlss_sp;

    if (sp.taken_back >= 3) {
        unavailable("the game keeps putting its own r.ScreenPercentage back under DLSS");
        return;
    }

    const auto sp_before = resolve(sp) ? refresh_base(sp) : std::nullopt;

    if (*current != target) {
        if (!dlss_set_mode(dlss, target, engine)) {
            unavailable("SetDLSSMode did not take");
            return;
        }

        dlss.written = target;
    }

    // The input follows r.ScreenPercentage: the mode's optimal percentage with it. A game that keeps its own value
    // leaves the step without effect, which the controller's step check finds.
    if (sp_before && std::abs(*sp_before - target_sp) >= 0.05f) {
        if (!write(sp, target_sp)) {
            SPDLOG_INFO("[autores] r.ScreenPercentage stays {} under DLSS mode {}", *sp_before, target);
        }

        const auto sp_after = read(sp);
        SPDLOG_INFO("[autores] DLSS mode {} -> {} ({} below the input), r.ScreenPercentage {} -> {} (game {})", *current, target,
            applied, *sp_before, sp_after ? fmt::format("{}", *sp_after) : std::string{"?"},
            sp.base ? fmt::format("{}", *sp.base) : std::string{"-"});
    }

    m_autores_upscaler_applied = applied;
}
