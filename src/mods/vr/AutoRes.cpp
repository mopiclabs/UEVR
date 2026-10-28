// VR_AutoRes: automatic resolution on the present thread. See AutoRes.hpp.

#include <algorithm>

#include "Framework.hpp"
#include "CVarManager.hpp"
#include "PerfLog.hpp"
#include "runtimes/OpenXR.hpp"
#include "utility/Logging.hpp"

#include "AutoRes.hpp"

namespace ar = uevr::autores;

namespace {
const char* actuator_name(ar::Actuator actuator) {
    switch (actuator) {
    case ar::Actuator::sp: return "SP";
    case ar::Actuator::upscaler: return "upscaler";
    case ar::Actuator::level: return "level";
    }

    return "?";
}

const char* notification_name(uint32_t level) {
    switch (level) {
    case XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT: return "NORMAL";
    case XR_PERF_SETTINGS_NOTIF_LEVEL_WARNING_EXT: return "WARNING";
    case XR_PERF_SETTINGS_NOTIF_LEVEL_IMPAIRED_EXT: return "IMPAIRED";
    default: return "?";
    }
}

const char* domain_name(uint32_t domain) {
    return domain == XR_PERF_SETTINGS_DOMAIN_CPU_EXT ? "CPU" : domain == XR_PERF_SETTINGS_DOMAIN_GPU_EXT ? "GPU" : "?";
}

const char* sub_domain_name(uint32_t sub_domain) {
    switch (sub_domain) {
    case XR_PERF_SETTINGS_SUB_DOMAIN_COMPOSITING_EXT: return "COMPOSITING";
    case XR_PERF_SETTINGS_SUB_DOMAIN_RENDERING_EXT: return "RENDERING";
    case XR_PERF_SETTINGS_SUB_DOMAIN_THERMAL_EXT: return "THERMAL";
    default: return "?";
    }
}

ar::Size eye_size(const runtimes::OpenXR& openxr) {
    return ar::Size{openxr.get_width(), openxr.get_height()};
}

// How long a request's state has to last before it is asked again.
std::chrono::seconds settle_time(ar::Request request) {
    return std::chrono::seconds{request == ar::Request::down ? 5 : 10};
}

const char* still_why(ar::Request request) {
    switch (request) {
    case ar::Request::down: return "still IMPAIRED";
    case ar::Request::cpu_up: return "still CPU-bound";
    default: return "still NORMAL";
    }
}
}

void AutoResController::on_present(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs) {
    const auto wanted = inputs.enabled || inputs.toggle_seconds > 0;

    if (!wanted || !openxr.ready() || openxr.view_configs.empty()) {
        if (m_active && (!wanted || openxr.view_configs.empty())) {
            deactivate(openxr, cvars);
        }

        (void)openxr.take_perf_settings_events();
        return;
    }

    // A new OpenXR object or session starts at the slider's size: start over from it.
    if (m_active && (m_openxr != &openxr || m_session != (uint64_t)openxr.session)) {
        SPDLOG_INFO("[autores] new OpenXR session: starting over");
        cvars.set_autores_sp(100);
        cvars.set_autores_upscaler(0);
        m_active = false;
    }

    if (!m_active) {
        activate(openxr, cvars, inputs);
    }

    const auto now = clock::now();

    if (m_available.level && !inputs.level_possible) {
        SPDLOG_INFO("[autores] the 2D screen or extreme compatibility mode sizes the eye: no level steps");
        m_available.level = false;
    }

    // The slider applied live: its size is the new start, and the level gave way to it.
    if (openxr.autores.slider_applies != m_slider_applies) {
        m_slider_applies = openxr.autores.slider_applies;
        m_state.start_level = m_state.level = ar::nearest_level(eye_size(openxr), m_base, m_widen);
        m_on_ladder = eye_size(openxr) == ar::level_size(m_state.level, m_base, m_widen);
        SPDLOG_INFO("[autores] Resolution Scale applied: {}x{} is the new start ({})", openxr.get_width(), openxr.get_height(),
            ar::LEVELS[m_state.start_level].name);
    }

    // An actuator the game turned out not to support: its steps are undone in the state (the game thread puts the
    // value back). A step down that moved nothing is taken again by the next actuator; a held step up is done already.
    const auto report = cvars.get_autores_report();
    const auto undone = [&](int& steps, int to) {
        if (steps <= to) {
            return;
        }

        steps = to;

        if (m_state.gpu == ar::Notification::impaired) {
            m_pending = ar::Request::down;
            m_pending_why = "a step that moved nothing";
        } else if (m_pending == ar::Request::up || m_pending == ar::Request::cpu_up) {
            m_pending = ar::Request::none;
        }
    };

    if (m_available.sp && report.sp_unavailable) {
        m_available.sp = false;
        m_sp_check.reset();
        undone(m_state.sp, 0);
    }

    if (m_available.upscaler && report.upscaler_unavailable) {
        m_available.upscaler = false;
        undone(m_state.upscaler, 0);
    }

    // The game's own mode leaves fewer upscaler steps: a step past them moved nothing.
    if (m_available.upscaler && report.upscaler_max_steps >= 0 && report.upscaler_max_steps < m_available.upscaler_steps) {
        m_available.upscaler_steps = report.upscaler_max_steps;
        undone(m_state.upscaler, m_available.upscaler_steps);
    }

    m_sp_percent = report.sp_percent;
    m_upscaler_steps = report.upscaler_steps;

    for (const auto& event : openxr.take_perf_settings_events()) {
        take_edge(event.domain, event.subDomain, event.fromLevel, event.toLevel, "runtime");
    }

    // VR_AutoResToggleSeconds, from VR_AutoResToggleDelaySeconds after the start: GPU IMPAIRED until nothing is left to
    // step down, then NORMAL until nothing is left to step up, and again, so every actuator and level takes its turn.
    if (inputs.toggle_seconds > 0 && now >= m_next_toggle) {
        if (m_next_toggle.time_since_epoch().count() == 0) {
            m_next_toggle = now + std::chrono::seconds{(std::max)(0, inputs.toggle_delay_seconds)};
        } else {
            m_next_toggle = now + std::chrono::seconds{inputs.toggle_seconds};

            if (m_toggle_down ? !ar::next_down(m_state, m_available) : !ar::next_up(m_state, m_available)) {
                m_toggle_down = !m_toggle_down;
            }

            const auto to = m_toggle_down ? XR_PERF_SETTINGS_NOTIF_LEVEL_IMPAIRED_EXT : XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT;
            take_edge(XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_SUB_DOMAIN_RENDERING_EXT, (uint32_t)m_state.gpu, to,
                "toggle");
        }
    }

    check_guards(openxr, cvars, inputs);

    // The extension reports changes only: while the last edge's state lasts, its request is asked again once the last
    // step had time to show (5 s down, 10 s up; a level step also waits for the level interval). A runtime that keeps
    // sending edges resets the wait with each one.
    if (m_pending == ar::Request::none && m_last_request != ar::Request::none && ar::still_wanted(m_state, m_last_request) &&
        now - m_last_action >= settle_time(m_last_request)) {
        const auto again = m_last_request == ar::Request::down ? ar::next_down(m_state, m_available) : ar::next_up(m_state, m_available);

        if (again) {
            m_pending = m_last_request;
            m_pending_why = still_why(m_last_request);
            m_waiting_logged = false;
        }

        m_last_action = now;
    }

    if (m_pending != ar::Request::none) {
        const auto level_wait = std::chrono::seconds{(std::max)(0, inputs.level_interval_seconds)} - (now - m_last_level_change);
        // A CPU-bound step up is the same step as a GPU one: SP and the upscaler back first, then pixels.
        const auto picked = m_pending == ar::Request::down ? ar::next_down(m_state, m_available) : ar::next_up(m_state, m_available);

        if (!ar::still_wanted(m_state, m_pending)) {
            m_pending = ar::Request::none;
        } else if (!picked) {
            SPDLOG_INFO("[autores] {}: nothing left to step {}", m_pending_why, m_pending == ar::Request::down ? "down" : "up");
            m_pending = ar::Request::none;
        } else if (inputs.loading) {
            if (!m_waiting_logged) {
                SPDLOG_INFO("[autores] {}: waiting while the game is loading", m_pending_why);
                m_waiting_logged = true;
            }
        } else if (picked->actuator == ar::Actuator::level && m_last_level_change.time_since_epoch().count() != 0 &&
            level_wait.count() > 0) {
            if (!m_waiting_logged) {
                SPDLOG_INFO("[autores] {}: the level step waits {} s (one level change per {} s)", m_pending_why,
                    std::chrono::duration_cast<std::chrono::seconds>(level_wait).count() + 1, inputs.level_interval_seconds);
                m_waiting_logged = true;
            }
        } else if (carry_out(openxr, cvars, *picked, m_pending_why)) {
            m_pending = ar::Request::none;
        }
    }

    send_hints(openxr, inputs);
}

void AutoResController::activate(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs) {
    m_openxr = &openxr;
    m_session = (uint64_t)openxr.session;

    // The ladder's Base is the runtime's recommended eye size (with UEVR's eye proportion, as the slider's scale 1).
    m_base = ar::Size{openxr.get_width_for_scale(1.0f), openxr.get_height_for_scale(1.0f)};
    m_widen = openxr.is_mopic_display;
    m_state = ar::State{};
    m_state.start_level = m_state.level = ar::nearest_level(eye_size(openxr), m_base, m_widen);
    m_on_ladder = eye_size(openxr) == ar::level_size(m_state.level, m_base, m_widen);
    m_slider_applies = openxr.autores.slider_applies;

    // The cap: maxImageRect, and the double-wide swapchain and UE's stereo render target within D3D's 16384.
    const auto& view = openxr.view_configs[0];
    const auto max = ar::Size{(std::min)(view.maxImageRectWidth, 8192u), (std::min)(view.maxImageRectHeight, 16384u)};
    m_state.level_cap = ar::highest_level_within(m_base, max, m_widen);
    m_available = ar::Available{};

    cvars.set_autores_sp(100);
    cvars.set_autores_upscaler(0);
    cvars.set_autores_active(true);

    m_pending = ar::Request::none;
    m_last_request = ar::Request::none;
    m_activated_at = clock::now();
    m_last_action = m_activated_at;
    m_last_level_change = {};
    m_last_guard_step = {};
    m_next_toggle = {};
    m_toggle_down = true;
    m_fps_window_start = {};
    m_vram_over_seconds = 0;
    m_sp_check.reset();
    m_active = true;

    SPDLOG_INFO("[autores] on{}: eye {}x{}, Base {}x{} (start {}), cap {} (runtime max {}x{}), up-steps {}, extension {}",
        inputs.enabled ? "" : " (VR_AutoResToggleSeconds only)", openxr.get_width(), openxr.get_height(), m_base.width,
        m_base.height, ar::LEVELS[m_state.start_level].name, ar::LEVELS[m_state.level_cap].name, view.maxImageRectWidth,
        view.maxImageRectHeight, m_widen ? "widen (Mopic display)" : "uniform",
        openxr.enabled_extensions.contains(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME) ? "enabled" : "not enabled");
}

void AutoResController::deactivate(runtimes::OpenXR& openxr, CVarManager& cvars) {
    cvars.set_autores_sp(100);
    cvars.set_autores_upscaler(0);
    cvars.set_autores_active(false);

    // Back to the size the slider path reports without the level (its last applied size while a slider change waits for
    // the next injection). If that can't rebuild live, the level's size is the one rendered until then, so it stays.
    if (openxr.has_autores_level() && !openxr.view_configs.empty()) {
        if (openxr.apply_autores_level(openxr.get_width_without_level(), openxr.get_height_without_level(), "autores off")) {
            openxr.clear_autores_level();
        } else {
            SPDLOG_WARN("[autores] off: the eye size stays {}x{} until the next injection", openxr.get_width(), openxr.get_height());
        }
    }

    m_active = false;
    m_pending = ar::Request::none;
    m_sp_check.reset();
    uevr::perf::set_autores(0, 100, 0, -2, -1);
    SPDLOG_INFO("[autores] off");
}

void AutoResController::take_edge(uint32_t domain, uint32_t sub_domain, uint32_t from, uint32_t to, const char* source) {
    m_last_event = domain * 1000 + sub_domain * 100 + to / 25;
    const auto request = ar::on_edge(m_state, (ar::Domain)domain, (ar::SubDomain)sub_domain, (ar::Notification)to);

    SPDLOG_INFO("[autores] edge qpc={} {} {}/{} {}->{}: {}", uevr::perf::now_ns(), source, domain_name(domain),
        sub_domain_name(sub_domain), notification_name(from), notification_name(to),
        request == ar::Request::down ? "step down" : request == ar::Request::none ? "hold" : "step up");

    m_last_action = clock::now();

    if (request == ar::Request::none) {
        if (m_state.gpu == ar::Notification::warning) {
            m_pending = ar::Request::none;
        }

        return;
    }

    m_pending = request;
    m_last_request = request;
    m_pending_why = source;
    m_waiting_logged = false;
}

bool AutoResController::carry_out(runtimes::OpenXR& openxr, CVarManager& cvars, const ar::Step& step, const char* why) {
    const auto now = clock::now();

    // Any other step would mix into the SP check's frame rate.
    m_sp_check.reset();

    switch (step.actuator) {
    case ar::Actuator::sp:
        cvars.set_autores_sp(ar::SP_PERCENT[step.to]);

        // A step down is checked against the frame rate before it (check_sp_step).
        if (step.to > step.from && m_fps) {
            m_sp_check = SpCheck{.at = now, .sp = step.to, .fps_before = *m_fps};
        }
        break;
    case ar::Actuator::upscaler:
        cvars.set_autores_upscaler(step.to);
        break;
    case ar::Actuator::level: {
        const auto size = ar::level_size(step.to, m_base, m_widen);

        if (!openxr.apply_autores_level(size.width, size.height, "autores")) {
            // No live rebuild on this engine path: no level steps this session; the request picks another actuator.
            SPDLOG_WARN("[autores] {} -> {} was not applied live; no level steps on this engine path", ar::LEVELS[step.from].name,
                ar::LEVELS[step.to].name);
            m_available.level = false;
            return false;
        }

        m_on_ladder = true;
        m_last_level_change = now;
        break;
    }
    }

    ar::apply(m_state, step);
    m_last_action = now;
    publish();
    const auto down = step.actuator == ar::Actuator::level ? step.to < step.from : step.to > step.from;
    SPDLOG_INFO("[autores] step qpc={} {} {} {}->{} ({}); now {}", uevr::perf::now_ns(), actuator_name(step.actuator),
        down ? "down" : "up", step.from, step.to, why, describe());
    return true;
}

// The local guards' steps don't wait for the level interval.
void AutoResController::step_down_now(runtimes::OpenXR& openxr, CVarManager& cvars, int count, const char* why) {
    for (int i = 0; i < count; ++i) {
        const auto picked = ar::next_down(m_state, m_available);

        if (!picked || !carry_out(openxr, cvars, *picked, why)) {
            break;
        }
    }

    m_last_guard_step = clock::now();
}

// App-side guards (design v1 §3-3), once a second.
void AutoResController::check_guards(runtimes::OpenXR& openxr, CVarManager& cvars, const Inputs& inputs) {
    const auto now = clock::now();

    if (now < m_next_guard_check) {
        return;
    }

    m_next_guard_check = now + std::chrono::seconds{1};

    // VR frames per second over 3 s.
    const auto frames = uevr::perf::new_frames();

    if (m_fps_window_start.time_since_epoch().count() == 0 || inputs.loading) {
        m_fps_window_start = now;
        m_fps_window_frames = frames;
        m_fps.reset();
    } else if (now - m_fps_window_start >= std::chrono::seconds{3}) {
        m_fps = (float)(frames - m_fps_window_frames) / std::chrono::duration<float>(now - m_fps_window_start).count();
        m_fps_window_start = now;
        m_fps_window_frames = frames;
    }

    check_sp_step(cvars, inputs);

    // Over the budget: UE fills its texture pools close to it (TEKKEN 8 demo ran at 96-98 % of it on 8 GB at 50 fps,
    // 2026-10-28), so only usage past it means the OS is already demoting memory.
    // For 5 s after a level change the old targets may not be released yet (UE deletes them a few frames later), so those
    // seconds don't count.
    m_vram_percent = query_vram_percent();
    const auto settling = m_last_level_change.time_since_epoch().count() != 0 && now - m_last_level_change < std::chrono::seconds{5};
    m_vram_over_seconds = m_vram_percent && *m_vram_percent > 100.0f && !settling ? m_vram_over_seconds + 1 : 0;
    publish();

    const auto settled = !inputs.loading && now - m_activated_at > std::chrono::seconds{15} &&
        (m_last_guard_step.time_since_epoch().count() == 0 || now - m_last_guard_step > std::chrono::seconds{10});

    if (!settled) {
        return;
    }

    // GPU memory over the DXGI budget for 3 s: one level down, and that level is the cap for the session. Only the eye
    // size changes the memory (SP renders into the same targets), so nothing else moves for it.
    if (m_vram_over_seconds >= 3) {
        if (m_available.level && m_state.level > ar::MIN_LEVEL &&
            carry_out(openxr, cvars, ar::Step{ar::Actuator::level, m_state.level, m_state.level - 1}, "VRAM guard")) {
            m_state.level_cap = (std::min)(m_state.level_cap, m_state.level);
            SPDLOG_WARN("[autores] VRAM {:.0f} % of the budget: capped at {} for this session", *m_vram_percent,
                ar::LEVELS[m_state.level_cap].name);
        }

        m_last_guard_step = now;
        m_vram_over_seconds = 0;
        return;
    }

    // Collapse: under 10 VR fps for 3 s without a stall, while the runtime says the GPU is IMPAIRED (design v1: GPU
    // twice its time and under 10 fps; a CPU stall such as shader compiling is not a reason): two steps down at once.
    if (m_fps && *m_fps < 10.0f && m_state.gpu == ar::Notification::impaired) {
        SPDLOG_WARN("[autores] {:.1f} fps over 3 s: two steps down", *m_fps);
        step_down_now(openxr, cvars, 2, "collapse guard");
        m_fps.reset();
    }
}

// An SP step down that leaves the game slower (Dead as Disco ran heavier at a lower r.ScreenPercentage, 2026-10-15):
// the frame rate 2-5 s after the step against the 3 s before it. 10 % slower without a stall in between leaves SP out
// for the session and puts it back; a request still wanted then goes to the next actuator.
void AutoResController::check_sp_step(CVarManager& cvars, const Inputs& inputs) {
    if (!m_sp_check) {
        return;
    }

    auto& check = *m_sp_check;
    const auto now = clock::now();
    check.loading = check.loading || inputs.loading;

    if (m_state.sp != check.sp) {
        m_sp_check.reset();
        return;
    }

    if (now - check.at < std::chrono::seconds{2}) {
        return;
    }

    if (check.window_start.time_since_epoch().count() == 0) {
        check.window_start = now;
        check.window_frames = uevr::perf::new_frames();
        return;
    }

    if (now - check.window_start < std::chrono::seconds{3}) {
        return;
    }

    const auto fps_after = (float)(uevr::perf::new_frames() - check.window_frames) /
        std::chrono::duration<float>(now - check.window_start).count();
    const auto slower = !check.loading && fps_after < 0.9f * check.fps_before;
    SPDLOG_INFO("[autores] SP step: {:.1f} fps before, {:.1f} after{}", check.fps_before, fps_after,
        check.loading ? " (a stall in between, not judged)" : slower ? ": slower, SP is left out" : "");
    m_sp_check.reset();

    if (slower) {
        m_available.sp = false;
        m_state.sp = 0;
        cvars.set_autores_sp(100);
        m_pending = m_state.gpu == ar::Notification::impaired ? ar::Request::down : m_pending;
    }
}

// Hints to the runtime (XR_EXT_performance_settings). GPU levels need UEVR's own GPU busy time, which it doesn't
// measure yet; frame generation is sent as CPU BOOST (Mopic's design v1 draft: "FG on, freeze adjustments").
void AutoResController::send_hints(runtimes::OpenXR& openxr, const Inputs& inputs) {
    const auto now = clock::now();

    if (now < m_next_hint) {
        return;
    }

    m_next_hint = now + std::chrono::seconds{1};

    if (inputs.frame_generation) {
        if (openxr.send_perf_settings_level(XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT) &&
            !m_frame_generation_hinted) {
            SPDLOG_INFO("[autores] frame generation is on: hinting CPU BOOST");
        }

        m_frame_generation_hinted = true;
    } else if (m_frame_generation_hinted) {
        m_frame_generation_hinted = false;
        openxr.send_perf_settings_level(XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
        SPDLOG_INFO("[autores] frame generation is off: hinting CPU SUSTAINED_HIGH");
    }
}

// The game's GPU memory against the budget DXGI gives the process (QueryVideoMemoryInfo, local segment).
std::optional<float> AutoResController::query_vram_percent() {
    const auto now = clock::now();

    if (m_adapter == nullptr && now >= m_next_adapter_lookup) {
        m_next_adapter_lookup = now + std::chrono::seconds{5};

        if (g_framework->is_dx12()) {
            const auto& hook = g_framework->get_d3d12_hook();
            const auto device = hook != nullptr ? hook->get_device() : nullptr;
            const auto swap_chain = hook != nullptr ? hook->get_swap_chain() : nullptr;
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory{};

            if (device != nullptr && swap_chain != nullptr && SUCCEEDED(swap_chain->GetParent(IID_PPV_ARGS(&factory)))) {
                factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&m_adapter));
            }
        } else if (g_framework->is_dx11()) {
            const auto& hook = g_framework->get_d3d11_hook();
            const auto device = hook != nullptr ? hook->get_device() : nullptr;
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device{};
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter{};

            if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
                SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
                adapter.As(&m_adapter);
            }
        }
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};

    if (m_adapter == nullptr || FAILED(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ||
        info.Budget == 0) {
        return std::nullopt;
    }

    return 100.0f * (float)info.CurrentUsage / (float)info.Budget;
}

void AutoResController::publish() {
    uevr::perf::set_autores(m_last_event, (uint32_t)m_sp_percent, (uint32_t)m_upscaler_steps, m_on_ladder ? m_state.level : -1,
        m_vram_percent ? (int32_t)*m_vram_percent : -1);
}

std::string AutoResController::describe() const {
    if (!m_active) {
        return {};
    }

    std::string text = std::string{"Auto "} + (m_on_ladder ? ar::LEVELS[m_state.level].name : "start");

    if (m_sp_percent < 100) {
        text += "  SP " + std::to_string(m_sp_percent) + "%";
    }

    if (m_upscaler_steps > 0) {
        text += "  DLSS -" + std::to_string(m_upscaler_steps);
    }

    return text;
}
