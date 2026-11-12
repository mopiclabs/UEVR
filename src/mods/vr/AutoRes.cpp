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
    // Only with XR_EXT_performance_settings in the session: without it no edge can come, and the local guards (the VRAM
    // guard's level step and its session cap) would act alone. VR_AutoRes is on by default, so that covers every runtime
    // without the extension (another runtime, a Mopic service before auto-resolution). VR_AutoResToggleSeconds
    // (measurements) makes its own edges and runs without it.
    const auto has_extension = openxr.performance_settings_enabled;
    const auto wanted = (inputs.enabled && has_extension) || inputs.toggle_seconds > 0;

    if (inputs.enabled && !has_extension && inputs.toggle_seconds <= 0 && openxr.ready() && !m_no_extension_logged) {
        m_no_extension_logged = true;
        SPDLOG_INFO("[autores] the runtime did not enable XR_EXT_performance_settings: automatic resolution stays off");
    }

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

    sync_available(cvars);

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
    check_frame_bound(inputs);

    // The extension reports changes only: while the last edge's state lasts, its request is asked again once the last
    // step had time to show (5 s down, 10 s up; a level step also waits for the level interval). A runtime that keeps
    // sending edges resets the wait with each one. A step's check (check_step) ends first: it takes 6 s, and a step
    // taken before would cancel it while the GPU stays IMPAIRED, the case it is there for.
    if (m_pending == ar::Request::none && !m_step_check && m_last_request != ar::Request::none &&
        ar::still_wanted(m_state, m_last_request) && now - m_last_action >= settle_time(m_last_request)) {
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
    // The upscaler joins once CVarManager finds DLSS (sync_available).
    m_available.upscaler = false;
    m_sp_left_out = false;
    m_upscaler_left_out = false;
    m_sp_cap = -1;
    m_upscaler_cap = -1;
    m_gpu.start();

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
    m_step_check.reset();
    // Frame-bound is found again in each session: the runtime keeps nothing between them either.
    m_tick_samples.clear();
    m_frame_bound = ar::FrameBoundJudge{};
    m_time_ratio.reset();
    m_engine_frame_rate_seen = false;
    m_gpu_hinted = false;
    m_next_frame_bound_check = {};
    m_next_frame_bound_log = {};
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
    m_step_check.reset();
    m_gpu.stop();
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

    // Any other step would mix into the check's numbers.
    m_step_check.reset();

    switch (step.actuator) {
    case ar::Actuator::sp:
        cvars.set_autores_sp(ar::SP_PERCENT[step.to]);
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

    // An SP or upscaler step down is checked against the 3 s before it (check_step).
    if (step.actuator != ar::Actuator::level && step.to > step.from && m_fps) {
        m_step_check = StepCheck{.actuator = step.actuator, .to = step.to, .at = now, .fps_before = *m_fps,
            .gpu_ms_before = m_gpu.gpu_ms_per_frame(now - std::chrono::milliseconds{3500}, now)};
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

    check_step(cvars, inputs);

    // Over the budget: UE fills its texture pools close to it (TEKKEN 8 demo ran at 96-98 % of it on 8 GB at 50 fps,
    // 2026-10-28), so only usage past it means the OS is already demoting memory.
    // For 5 s after a level change the old targets may not be released yet (UE deletes them a few frames later), so those
    // seconds don't count.
    // Loading seconds don't count either: a load fills the pools past the budget for a moment (115-127 % at loads on an
    // 8 GB 3080 laptop, 2026-11-04), which the level is not the cause of.
    m_vram_percent = query_vram_percent();
    const auto settling = m_last_level_change.time_since_epoch().count() != 0 && now - m_last_level_change < std::chrono::seconds{5};
    m_vram_over_seconds = m_vram_percent && *m_vram_percent > 100.0f && !settling && !inputs.loading ? m_vram_over_seconds + 1 : 0;
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

// What can move now: CVarManager's report (SP is out while DLSS sets the input resolution, the upscaler while no DLSS is
// found; either can come back) and the step check's verdicts (out for the session). An actuator that goes out has its
// steps undone in the state and its target put back; a step down it held is taken again by the next actuator.
void AutoResController::sync_available(CVarManager& cvars) {
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

    const auto sp = !report.sp_unavailable && !m_sp_left_out;
    const auto upscaler = !report.upscaler_unavailable && !m_upscaler_left_out;

    if (sp != m_available.sp) {
        m_available.sp = sp;

        if (!sp) {
            if (m_step_check && m_step_check->actuator == ar::Actuator::sp) {
                m_step_check.reset();
            }

            undone(m_state.sp, 0);
            cvars.set_autores_sp(ar::SP_PERCENT[0]);
        }

        SPDLOG_INFO("[autores] SP steps {}", sp ? "on" : m_sp_left_out ? "off for the session" : "off (DLSS sets the input resolution)");
    }

    if (upscaler != m_available.upscaler) {
        m_available.upscaler = upscaler;

        if (!upscaler) {
            if (m_step_check && m_step_check->actuator == ar::Actuator::upscaler) {
                m_step_check.reset();
            }

            undone(m_state.upscaler, 0);
            cvars.set_autores_upscaler(0);
        } else {
            m_available.upscaler_steps = ar::UPSCALER_STEPS;
        }

        SPDLOG_INFO("[autores] upscaler steps {}", upscaler ? "on" : m_upscaler_left_out ? "off for the session" : "off (no DLSS sets the input resolution)");
    }

    // The game's own mode leaves fewer upscaler steps: a step past them moved nothing.
    if (m_available.upscaler && report.upscaler_max_steps >= 0 && report.upscaler_max_steps < m_available.upscaler_steps) {
        m_available.upscaler_steps = report.upscaler_max_steps;
        undone(m_state.upscaler, m_available.upscaler_steps);
        cvars.set_autores_upscaler(m_state.upscaler);
    }

    // A step past the first that moved nothing: back to the one before it, and no further for the session.
    if (m_available.sp && m_sp_cap >= 0 && m_sp_cap < m_available.sp_steps) {
        m_available.sp_steps = m_sp_cap;
        undone(m_state.sp, m_sp_cap);
        cvars.set_autores_sp(ar::SP_PERCENT[m_state.sp]);
    }

    if (m_available.upscaler && m_upscaler_cap >= 0 && m_upscaler_cap < m_available.upscaler_steps) {
        m_available.upscaler_steps = m_upscaler_cap;
        undone(m_state.upscaler, m_upscaler_cap);
        cvars.set_autores_upscaler(m_state.upscaler);
    }

    m_sp_percent = report.sp_percent;
    m_upscaler_steps = report.upscaler_steps;
}

// An SP or upscaler step down, against the 3 s before it (design v1 Appendix B, self-calibration): the GPU time per
// frame 2-5 s after the step (AutoResPolicy.hpp judge_step) and the frame rate over the same window. A step that does
// nothing worth keeping is put back and its actuator is left out for the session; so is one that leaves the game 10 %
// slower (Dead as Disco ran heavier at a lower r.ScreenPercentage, 2026-10-15). A request still wanted then goes to the
// next actuator. A window with a stall or a load in it is not judged.
void AutoResController::check_step(CVarManager& cvars, const Inputs& inputs) {
    if (!m_step_check) {
        return;
    }

    auto& check = *m_step_check;
    const auto now = clock::now();
    check.loading = check.loading || inputs.loading;

    const auto current = check.actuator == ar::Actuator::sp ? m_state.sp : m_state.upscaler;

    if (current != check.to) {
        m_step_check.reset();
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

    // From 2 s after the step, 3 s or more: the GPU sampler takes a second at a time, so the window waits for its third.
    if (now - check.window_start < std::chrono::seconds{4}) {
        return;
    }

    // A level change in the 4 s before the step or since makes the GPU time another eye size's.
    check.loading = check.loading ||
        (m_last_level_change.time_since_epoch().count() != 0 && m_last_level_change > check.at - std::chrono::seconds{4});

    const auto fps_after = (float)(uevr::perf::new_frames() - check.window_frames) /
        std::chrono::duration<float>(now - check.window_start).count();
    const auto gpu_ms_after = m_gpu.gpu_ms_per_frame(check.window_start, now);
    const auto name = check.actuator == ar::Actuator::sp ? "SP" : "upscaler";
    const auto slower = !check.loading && fps_after < 0.9f * check.fps_before;
    const auto effect = check.loading || !check.gpu_ms_before || !gpu_ms_after ? ar::StepEffect::unclear
        : ar::judge_step(*check.gpu_ms_before, *gpu_ms_after);
    const auto fmt_ms = [](const std::optional<float>& ms) { return ms ? fmt::format("{:.1f}", *ms) : std::string{"-"}; };
    const auto out = slower || effect == ar::StepEffect::nothing;
    // The steps before this one were kept (each was checked), so only this one goes back.
    const auto keep = check.to - 1;
    const auto out_what = keep > 0 ? fmt::format("back to step {} and no further for the session", keep)
                                   : std::string{"put back and left out for the session"};

    SPDLOG_INFO("[autores] {} step {}: GPU {} -> {} ms per frame{}, {:.1f} -> {:.1f} fps{}: {}", name, check.to,
        fmt_ms(check.gpu_ms_before), fmt_ms(gpu_ms_after),
        check.gpu_ms_before && gpu_ms_after && *check.gpu_ms_before > 0.0f
            ? fmt::format(" ({:+.0f} %)", 100.0f * (*gpu_ms_after / *check.gpu_ms_before - 1.0f)) : std::string{},
        check.fps_before, fps_after, check.loading ? " (a stall or a level change in between, not judged)" : "",
        slower ? fmt::format("slower, {}", out_what)
        : effect == ar::StepEffect::nothing ? fmt::format("no effect, {}", out_what)
        : effect == ar::StepEffect::works ? "works" : "kept");

    const auto actuator = check.actuator;
    m_step_check.reset();

    if (out) {
        if (keep > 0) {
            (actuator == ar::Actuator::sp ? m_sp_cap : m_upscaler_cap) = keep;
        } else {
            (actuator == ar::Actuator::sp ? m_sp_left_out : m_upscaler_left_out) = true;
        }

        sync_available(cvars); // undoes the step and takes a request still wanted to the next actuator
    }
}

// VR_AutoResFrameBoundDetect, once a second (AutoResPolicy.hpp FrameBoundJudge). The engine's own fixed frame rate is
// bound at once (static); otherwise the game time against the wall time over the last 3 s (dynamic).
void AutoResController::check_frame_bound(const Inputs& inputs) {
    if (!inputs.frame_bound_detect) {
        m_tick_samples.clear();
        m_time_ratio.reset();
        uevr::perf::set_autores_frame_bound(-1, -1);
        return;
    }

    const auto now = clock::now();

    if (now < m_next_frame_bound_check) {
        return;
    }

    m_next_frame_bound_check = now + std::chrono::seconds{1};

    if (inputs.engine_frame_rate != nullptr && !m_engine_frame_rate_seen) {
        m_engine_frame_rate_seen = true;
        const auto& engine = *inputs.engine_frame_rate;
        const auto flag = [](const std::optional<bool>& value) { return value ? (*value ? "true" : "false") : "-"; };
        const auto number = [](const std::optional<float>& value) { return value ? fmt::format("{:.1f}", *value) : std::string{"-"}; };
        SPDLOG_INFO("[autores] engine: fixed={} {} smooth={} {}-{}", flag(engine.fixed), number(engine.fixed_fps),
            flag(engine.smooth), number(engine.smooth_min), number(engine.smooth_max));

        if (engine.fixed.value_or(false) && m_frame_bound.state != ar::FrameBound::bound) {
            m_frame_bound.state = ar::FrameBound::bound;
            SPDLOG_INFO("[autores] frame-bound: r=- fps={:.1f} static -> GPU BOOST", m_time_ratio_fps);
        }
    }

    m_tick_samples.push_back(TickSample{.at = now, .delta_us = inputs.tick_delta_us, .real_us = inputs.tick_real_us,
        .frames = uevr::perf::new_frames(), .loading = inputs.loading});

    // FRAME_BOUND_WINDOW_SECONDS between the first and the last sample.
    constexpr size_t window_samples = ar::FRAME_BOUND_WINDOW_SECONDS + 1;

    while (m_tick_samples.size() > window_samples) {
        m_tick_samples.pop_front();
    }

    m_time_ratio.reset();
    bool loading = false;

    if (m_tick_samples.size() == window_samples) {
        const auto& first = m_tick_samples.front();
        const auto& last = m_tick_samples.back();
        const auto real_us = last.real_us - first.real_us;
        const auto seconds = std::chrono::duration<float>(last.at - first.at).count();

        if (real_us > 0) {
            m_time_ratio = (float)(last.delta_us - first.delta_us) / (float)real_us;
        }

        m_time_ratio_fps = seconds > 0.0f ? (float)(last.frames - first.frames) / seconds : 0.0f;

        for (size_t i = 1; i < m_tick_samples.size(); ++i) {
            loading = loading || m_tick_samples[i].loading;
        }
    }

    if (m_frame_bound.update(m_time_ratio, m_time_ratio_fps, loading)) {
        const auto bound = m_frame_bound.state == ar::FrameBound::bound;
        SPDLOG_INFO("[autores] frame-bound: r={:.2f} fps={:.1f} dynamic -> {}", *m_time_ratio, m_time_ratio_fps,
            bound ? "GPU BOOST" : "not bound, GPU SUSTAINED_HIGH");
    }

    if (now >= m_next_frame_bound_log) {
        m_next_frame_bound_log = now + std::chrono::seconds{10};
        SPDLOG_INFO("[autores] time ratio: r={} fps={:.1f}{} state={}", m_time_ratio ? fmt::format("{:.2f}", *m_time_ratio) : "-",
            m_time_ratio_fps, loading ? " (loading)" : "",
            m_frame_bound.state == ar::FrameBound::bound ? "bound" : m_frame_bound.state == ar::FrameBound::not_bound ? "not bound" : "unknown");
    }

    uevr::perf::set_autores_frame_bound(m_time_ratio ? (int32_t)(*m_time_ratio * 1000.0f + 0.5f) : -1, (int32_t)m_frame_bound.state);
}

// Hints to the runtime (XR_EXT_performance_settings, design v1 §3-2), once a second. GPU: BOOST when the game is
// frame-bound (it needs 60), SUSTAINED_HIGH while the detector runs and has not found it; a runtime takes a session
// without GPU hints for one without the detector, so they go every second. CPU: frame generation as BOOST ("FG on,
// freeze adjustments").
void AutoResController::send_hints(runtimes::OpenXR& openxr, const Inputs& inputs) {
    const auto now = clock::now();

    if (now < m_next_hint) {
        return;
    }

    m_next_hint = now + std::chrono::seconds{1};

    if (inputs.frame_bound_detect) {
        const auto bound = m_frame_bound.state == ar::FrameBound::bound;

        if (openxr.send_perf_settings_level(XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                bound ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT) && !m_gpu_hinted) {
            SPDLOG_INFO("[autores] frame-bound detector on: hinting GPU {} once a second", bound ? "BOOST" : "SUSTAINED_HIGH");
            m_gpu_hinted = true;
        }
    }

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
