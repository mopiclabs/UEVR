#include "mods/vr/AutoResPolicy.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

// The app side of automatic resolution (AutoResPolicy.hpp): the ladder, the edges and the step order.

namespace ar = uevr::autores;
static int failures{};
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

// Applies the step a request picks; false when nothing moved.
static bool step(ar::State& state, const ar::Available& available, ar::Request request) {
    const auto picked = request == ar::Request::down ? ar::next_down(state, available)
        : request == ar::Request::up || request == ar::Request::cpu_up ? ar::next_up(state, available)
        : std::nullopt;

    if (picked) {
        ar::apply(state, *picked);
    }

    return picked.has_value();
}

static void test_ladder() {
    constexpr ar::Size mopic{1920, 2160};
    expect(ar::level_size(0, mopic) == ar::Size{1280, 1440} && ar::level_size(1, mopic) == ar::Size{1600, 1800} &&
        ar::level_size(2, mopic) == ar::Size{1920, 2160} && ar::level_size(3, mopic) == ar::Size{2880, 2160} &&
        ar::level_size(4, mopic) == ar::Size{3840, 2160}, "on Mopic's eye the ladder is design v1's");

    constexpr ar::Size min_start{1280, 1440};
    expect(ar::level_size(2, min_start) == ar::Size{1920, 2160} && ar::level_size(3, min_start) == ar::Size{2880, 2160} &&
        ar::level_size(4, min_start) == ar::Size{3840, 2160}, "on Mopic's display a session that starts at Min climbs the same table");
    expect(ar::nearest_level(min_start, min_start) == ar::MIN_LEVEL, "a Min start is Min");

    constexpr ar::Size headset{2064, 2208};
    expect(ar::level_size(0, headset, false) == ar::Size{1376, 1472} && ar::level_size(3, headset, false) == ar::Size{2522, 2698} &&
        ar::level_size(4, headset, false) == ar::Size{2924, 3128}, "other displays: uniform up-steps");
    expect(ar::highest_level_within(headset, ar::Size{2600, 2800}, false) == 3, "uniform levels are capped too");
    expect(ar::level_size(1, ar::Size{1001, 1001}, false) == ar::Size{834, 834}, "sizes are even");

    expect(ar::highest_level_within(mopic, ar::Size{3840, 2160}) == ar::MAX_LEVEL, "a max of 3840x2160 allows Max");
    expect(ar::highest_level_within(mopic, ar::Size{1920, 2160}) == ar::BASE_LEVEL, "a max of the eye itself stops at Base");
    expect(ar::highest_level_within(mopic, ar::Size{16384, 16384}) == ar::MAX_LEVEL, "a large max still stops at Max");
    expect(ar::highest_level_within(mopic, ar::Size{0, 0}) == ar::MIN_LEVEL, "no max: Min at least");

    expect(ar::nearest_level(mopic, mopic) == ar::BASE_LEVEL, "the recommended eye is Base");
    expect(ar::nearest_level(ar::Size{960, 1080}, mopic) == ar::MIN_LEVEL, "below the ladder is Min");
    expect(ar::nearest_level(ar::Size{1536, 1728}, mopic) == 1, "a 0.8 slider is closest to Saver");
    expect(ar::nearest_level(ar::Size{2700, 2160}, mopic) == 3, "a wide eye is closest to Sharp");
}

static void test_frame_bound() {
    ar::FrameBoundJudge judge{};
    judge.update(0.5f, 30.0f, false);
    expect(judge.state == ar::FrameBound::unknown, "one slow second is not enough");
    expect(judge.update(0.5f, 30.0f, false) && judge.state == ar::FrameBound::bound, "two slow seconds are bound");
    expect(!judge.update(1.0f, 30.0f, false) && judge.state == ar::FrameBound::bound, "bound stays");

    ar::FrameBoundJudge real{};
    real.update(1.0f, 30.0f, false);
    expect(real.state == ar::FrameBound::unknown, "one real-time second is not enough");
    expect(real.update(0.99f, 30.0f, false) && real.state == ar::FrameBound::not_bound, "real time at 30 fps is not bound");

    ar::FrameBoundJudge fast{};
    for (int i = 0; i < 10; ++i) {
        fast.update(0.5f, 60.0f, false);
    }
    expect(fast.state == ar::FrameBound::unknown, "at 60 fps nothing is judged");

    ar::FrameBoundJudge loading{};
    loading.update(0.5f, 30.0f, false);
    loading.update(0.5f, 30.0f, true);
    loading.update(0.5f, 30.0f, false);
    expect(loading.state == ar::FrameBound::unknown, "a load in between starts the count over");
    expect(loading.update(0.5f, 30.0f, false) && loading.state == ar::FrameBound::bound, "then two in a row");

    ar::FrameBoundJudge slow{};
    for (int i = 0; i < 10; ++i) {
        slow.update(0.5f, 18.0f, false);
    }
    expect(slow.state == ar::FrameBound::unknown, "under 22 fps nothing is judged");

    ar::FrameBoundJudge between{};
    between.update(0.5f, 40.0f, false);
    between.update(0.85f, 40.0f, false);
    between.update(0.5f, 40.0f, false);
    expect(between.state == ar::FrameBound::unknown, "a second between the thresholds breaks the run");

    // A game whose time is its 60 frames reads r = fps / 60: at the judge's 55 fps that is 0.917, and at 54 fps 0.9,
    // which 0.9 took for not bound (PR #29 review).
    for (const float fps : {54.0f, 55.0f}) {
        ar::FrameBoundJudge fixed60{};
        for (int i = 0; i < 10; ++i) {
            fixed60.update(fps / 60.0f, fps, false);
        }
        expect(fixed60.state == ar::FrameBound::unknown, "a fixed 60 game at 54-55 fps is never not bound");
    }

    ar::FrameBoundJudge near_real{};
    near_real.update(0.95f, 40.0f, false);
    expect(near_real.update(0.96f, 40.0f, false) && near_real.state == ar::FrameBound::not_bound,
        "r 0.95 or more twice is not bound");

    ar::FrameBoundJudge weak{};
    for (int i = 0; i < 10; ++i) {
        weak.update(0.93f, 50.0f, false);
    }
    expect(weak.state == ar::FrameBound::unknown, "a weak slowdown (3080L TEKKEN, r 0.92-0.97) stays unknown at 0.93");
}

static void test_edges() {
    ar::State state{};
    expect(ar::on_edge(state, ar::Domain::gpu, ar::SubDomain::rendering, ar::Notification::impaired) == ar::Request::down,
        "GPU IMPAIRED steps down");
    expect(ar::on_edge(state, ar::Domain::gpu, ar::SubDomain::rendering, ar::Notification::warning) == ar::Request::none,
        "GPU WARNING holds");
    expect(ar::on_edge(state, ar::Domain::cpu, ar::SubDomain::rendering, ar::Notification::impaired) == ar::Request::none,
        "CPU IMPAIRED during GPU WARNING does not step up");
    expect(ar::on_edge(state, ar::Domain::gpu, ar::SubDomain::rendering, ar::Notification::normal) == ar::Request::up,
        "GPU NORMAL steps up");
    expect(ar::on_edge(state, ar::Domain::cpu, ar::SubDomain::rendering, ar::Notification::impaired) == ar::Request::cpu_up,
        "CPU IMPAIRED with the GPU NORMAL steps up");
    expect(ar::still_wanted(state, ar::Request::cpu_up), "a CPU step up stands while CPU-bound");
    ar::on_edge(state, ar::Domain::cpu, ar::SubDomain::rendering, ar::Notification::normal);
    expect(!ar::still_wanted(state, ar::Request::cpu_up) && ar::still_wanted(state, ar::Request::up),
        "a held CPU step up lapses when the CPU is NORMAL again; a GPU one does not");
    expect(ar::on_edge(state, ar::Domain::gpu, ar::SubDomain::thermal, ar::Notification::warning) == ar::Request::none &&
        ar::on_edge(state, ar::Domain::gpu, ar::SubDomain::compositing, ar::Notification::impaired) == ar::Request::none &&
        state.gpu == ar::Notification::normal, "THERMAL and COMPOSITING are display only");
    expect(!ar::still_wanted(state, ar::Request::down), "a held step down lapses when the GPU is NORMAL");
}

static void test_order() {
    // A DLSS game (SP is out under DLSS): the internal resolution to its floor first, then the eye size.
    const ar::Available dlss{.sp = false, .upscaler_steps = 3};
    ar::State state{};
    std::vector<ar::Actuator> downs;

    while (const auto picked = ar::next_down(state, dlss)) {
        downs.push_back(picked->actuator);
        ar::apply(state, *picked);
    }

    expect((downs == std::vector{ar::Actuator::upscaler, ar::Actuator::upscaler, ar::Actuator::upscaler, ar::Actuator::level,
        ar::Actuator::level}), "down: DLSS to its floor, then the level");
    expect(state.upscaler == 3 && state.level == ar::MIN_LEVEL, "the bottom is the floor at Min");

    std::vector<ar::Actuator> ups;
    while (const auto picked = ar::next_up(state, dlss)) {
        ups.push_back(picked->actuator);
        ar::apply(state, *picked);
    }

    expect((ups == std::vector{ar::Actuator::level, ar::Actuator::level, ar::Actuator::upscaler, ar::Actuator::upscaler,
        ar::Actuator::upscaler, ar::Actuator::level, ar::Actuator::level}),
        "up: the level back to the anchor, the game's DLSS mode, then pixels above the anchor");
    expect(state.level == ar::MAX_LEVEL && state.upscaler == 0, "the top is Max with the game's mode");

    // From above the anchor the internal resolution still goes first.
    expect(step(state, dlss, ar::Request::down) && state.level == ar::MAX_LEVEL && state.upscaler == 1,
        "down from Max takes DLSS down first");

    // No DLSS: SP, to 50 % under a temporal upscaler, else 70 %.
    const ar::Available temporal{.sp_steps = ar::SP_STEPS_TEMPORAL, .upscaler = false};
    state = ar::State{};
    int sp_steps = 0;
    while (step(state, temporal, ar::Request::down) && state.level == ar::BASE_LEVEL) {
        ++sp_steps;
    }
    expect(sp_steps == 4 && ar::SP_PERCENT[state.sp] == 50 && state.level == 1, "SP to 50 %, then the level");

    const ar::Available plain{.upscaler = false};
    state = ar::State{};
    step(state, plain, ar::Request::down);
    step(state, plain, ar::Request::down);
    expect(ar::SP_PERCENT[state.sp] == 70 && step(state, plain, ar::Request::down) && state.level == 1,
        "without a temporal upscaler SP stops at 70 %");

    // DLSS found: SP is not used even when it would still be available.
    expect(ar::next_down(ar::State{.upscaler = 2}, ar::Available{.upscaler_steps = 2})->actuator == ar::Actuator::level,
        "at the DLSS floor the level moves, not SP");
}

static void test_limits() {
    // A game where SP is ignored and there is no DLSS plugin: the level only.
    const ar::Available level_only{.sp = false, .upscaler = false};
    ar::State state{};
    expect(step(state, level_only, ar::Request::down) && state.level == 1 && state.sp == 0, "without SP the level moves");

    // A cap below the start: back up only to the cap.
    state = ar::State{.level = ar::MIN_LEVEL, .level_cap = 1};
    expect(step(state, level_only, ar::Request::up) && state.level == 1, "up to the cap");
    expect(!step(state, level_only, ar::Request::up), "not past the cap");

    // An actuator that went away is still restored.
    state = ar::State{.sp = 2};
    expect(step(state, ar::Available{.sp = false, .upscaler = false, .level = false}, ar::Request::up) && state.sp == 1,
        "SP restored after it became unavailable");

    // Nothing available, nothing at the bottom.
    state = ar::State{};
    expect(!step(state, ar::Available{.sp = false, .upscaler = false, .level = false}, ar::Request::down), "no actuator, no step");
    state = ar::State{.level = ar::MIN_LEVEL, .start_level = ar::MIN_LEVEL};
    expect(!step(state, level_only, ar::Request::down), "not below Min");

    // A game already one mode above the floor: one upscaler step, then the level.
    ar::Available one_mode{.sp = false};
    one_mode.upscaler_steps = 1;
    state = ar::State{};
    expect(step(state, one_mode, ar::Request::down) && state.upscaler == 1, "the reachable upscaler step");
    expect(step(state, one_mode, ar::Request::down) && state.upscaler == 1 && state.level == 1, "then the level, no empty step");

    // At the floor already (the game runs DLSS Performance): straight to the level.
    state = ar::State{};
    expect(step(state, ar::Available{.sp = false, .upscaler_steps = 0}, ar::Request::down) && state.level == 1 && state.upscaler == 0,
        "no room under DLSS: the level");
}

static void test_dlss_floor() {
    // The UE5 plugin's modes as (optimal %, mode): DLAA 100, Ultra Quality 77, Quality 66.7, Balanced 58, Performance 50,
    // Ultra Performance 33.3; Auto (1) reports Performance's.
    const std::vector<std::pair<float, uint8_t>> modes{{100.0f, 2}, {77.0f, 3}, {66.7f, 4}, {58.0f, 5}, {50.0f, 6},
        {33.3f, 7}, {50.0f, 1}};
    expect((ar::dlss_modes_below(modes, 58.0f, ar::UpscalerFloor::performance) == std::vector<uint8_t>{6}),
        "Balanced: one step to Performance");
    expect((ar::dlss_modes_below(modes, 58.0f, ar::UpscalerFloor::ultra_performance) == std::vector<uint8_t>{6, 7}),
        "Balanced with the Ultra Performance floor: two");
    expect((ar::dlss_modes_below(modes, 100.0f, ar::UpscalerFloor::performance) == std::vector<uint8_t>{3, 4, 5, 6}),
        "DLAA: four steps to Performance, no two-step limit");
    expect(ar::dlss_modes_below(modes, 50.0f, ar::UpscalerFloor::performance).empty(), "Performance already: none");

    // Fractions as some plugin versions report them.
    const std::vector<std::pair<float, uint8_t>> fractions{{0.667f, 4}, {0.58f, 5}, {0.5f, 6}, {0.333f, 7}};
    expect((ar::dlss_modes_below(fractions, 0.667f, ar::UpscalerFloor::performance) == std::vector<uint8_t>{5, 6}),
        "fractions rank the same");

    expect(ar::dlss_quality_floor(ar::UpscalerFloor::performance) == -1 &&
        ar::dlss_quality_floor(ar::UpscalerFloor::ultra_performance) == -2, "UE4 quality floor: -1 / -2");
}

static void test_internal_recovery() {
    ar::InternalRecovery r{};
    using Act = ar::InternalRecovery::Act;

    for (int i = 0; i < 59; ++i) {
        expect(r.tick(false, false, 50.0f, true) == Act::none, "not before 60 calm seconds");
    }
    expect(r.tick(false, false, 50.0f, true) == Act::step_up, "60 calm seconds: one step up");

    // IMPAIRED within 30 s: undone, and blocked for 5 minutes.
    for (int i = 0; i < 10; ++i) {
        r.tick(false, false, 50.0f, true);
    }
    expect(r.tick(true, false, 90.0f, true) == Act::undo && r.blocked == 300, "IMPAIRED within 30 s undoes it");
    bool stepped = false;
    for (int i = 0; i < 299; ++i) {
        stepped = stepped || r.tick(false, false, 50.0f, true) == Act::step_up;
    }
    expect(!stepped, "blocked for 5 minutes");
    for (int i = 0; i < 60 && !stepped; ++i) {
        stepped = r.tick(false, false, 50.0f, true) == Act::step_up;
    }
    expect(stepped, "then again");
    for (int i = 0; i < 31; ++i) {
        r.tick(false, false, 50.0f, true);
    }
    expect(r.watch == 0 && !(r.tick(true, false, 90.0f, true) == Act::undo), "IMPAIRED after 30 s is not ours to undo");

    // A busy GPU or a load resets the count; nothing below the game's own: no step.
    ar::InternalRecovery busy{};
    for (int i = 0; i < 120; ++i) {
        expect(busy.tick(false, false, 75.0f, true) == Act::none, "GPU 70 % busy or more: no step");
    }
    ar::InternalRecovery top{};
    for (int i = 0; i < 120; ++i) {
        expect(top.tick(false, false, 40.0f, false) == Act::none, "nothing to take back up");
    }

    // The block doubles, up to an hour.
    ar::InternalRecovery doubling{};
    for (int round = 0; round < 6; ++round) {
        while (doubling.tick(false, false, 40.0f, true) != Act::step_up) {
        }
        doubling.tick(true, false, 90.0f, true);
    }
    expect(doubling.block == 3600 && doubling.undos == 6, "5, 10, 20, 40 minutes, then an hour");
}

static void test_anchor_gate() {
    // Hogwarts Legacy as the harness plays it: the title for 40 s at 45 % (a stall at start-up), the save's load at 41-50 s,
    // then the castle at 95 %. The menu's 45 % must not decide it.
    ar::AnchorGate hog{};
    bool due = false;
    for (int s = 1; s <= 60 && !due; ++s) {
        const bool loading = s == 3 || (s >= 41 && s <= 50);
        due = hog.tick(loading, s <= 40 ? 45.0f : 95.0f);
        expect(!due || s > 50, "not in the title menu, not during the load");
    }
    expect(!due, "not before 30 s after the load");
    for (int s = 61; s <= 80 && !due; ++s) {
        due = hog.tick(false, 95.0f);
        expect(!due || s == 80, "due 30 s after the load");
    }
    expect(due && hog.load_seen && hog.vram_max && *hog.vram_max == 95.0f, "decided on the castle's memory");

    // A load starts the 30 s over and forgets the memory before it.
    ar::AnchorGate again{};
    for (int s = 1; s <= 70; ++s) {
        again.tick(s == 20, s < 60 ? 80.0f : 50.0f);
    }
    expect(again.tick(true, 50.0f) == false && !again.vram_max, "a load: not due, memory forgotten");
    for (int s = 0; s < 29; ++s) {
        expect(!again.tick(false, 50.0f), "29 s after it");
    }
    expect(again.tick(false, 50.0f) && *again.vram_max == 50.0f, "30 s after it, on its own memory");

    // Start-up stalls are not the load; a session without one decides at 3 minutes.
    ar::AnchorGate none{};
    bool none_due = false;
    int at = 0;
    for (int s = 1; s <= 200 && !none_due; ++s) {
        none_due = none.tick(s <= 10 && s % 3 == 0, 40.0f);
        at = s;
    }
    expect(none_due && at == 180 && !none.load_seen, "no load: due at 180 s");

    // The title left alone for 3 minutes, then the save's load at 170-185 s: the 30 s after it still count, not the
    // 3 minutes (review, 2026-11-13).
    ar::AnchorGate late{};
    int late_at = 0;
    for (int s = 1; s <= 300 && late_at == 0; ++s) {
        if (late.tick(s >= 170 && s <= 185, s < 170 ? 40.0f : 95.0f)) {
            late_at = s;
        }
    }
    expect(late_at == 215 && *late.vram_max == 95.0f, "a load at 170-185 s: due at 215 s on the level's memory");

    // No GPU memory reading: never due.
    ar::AnchorGate blind{};
    bool blind_due = false;
    for (int s = 1; s <= 300; ++s) {
        blind_due = blind_due || blind.tick(s == 20, std::nullopt);
    }
    expect(!blind_due, "no memory reading: not due");
}

static void test_upscaler_probe() {
    using R = ar::ProbeReading;

    // TEKKEN 8 Demo: its DLSS mode reads as off until the title, about 30 s in (2026-11-12).
    ar::UpscalerProbe late{};
    for (int i = 0; i < 14; ++i) {
        expect(!late.update(R::off) && late.kind == ar::Upscaler::unknown, "nothing in the first 14 tries: unknown");
    }
    expect(late.update(R::off) && late.kind == ar::Upscaler::none, "the 15th try (30 s): none for now");
    expect(!late.update(R::no_answer) && late.kind == ar::Upscaler::none, "a try with no answer changes nothing");
    expect(late.update(R::dlss_mode) && late.kind == ar::Upscaler::dlss_mode, "DLSS found later takes over");
    expect(late.dlss(), "found DLSS is DLSS");

    // Found, then turned off by the player: two reads, not one, and not a try with no answer (a load).
    ar::UpscalerProbe off{};
    off.update(R::dlss_quality);
    expect(off.kind == ar::Upscaler::dlss_quality, "the UE4 plugin's cvars");
    expect(!off.update(R::off) && off.kind == ar::Upscaler::dlss_quality, "one off read is not enough");
    expect(!off.update(R::no_answer) && off.kind == ar::Upscaler::dlss_quality, "a try with no answer breaks the run");
    expect(!off.update(R::off) && off.kind == ar::Upscaler::dlss_quality, "the count starts over");
    expect(off.update(R::off) && off.kind == ar::Upscaler::none, "two off reads in a row: none");
    expect(off.update(R::dlss_quality) && off.kind == ar::Upscaler::dlss_quality, "and back on");

    // A DLSS found at once (Black Myth: Wukong, about 12 s in) never passes through none.
    ar::UpscalerProbe early{};
    for (int i = 0; i < 5; ++i) {
        early.update(R::no_answer);
    }
    expect(early.update(R::dlss_mode) && early.kind == ar::Upscaler::dlss_mode, "found on the 6th try");

    expect(ar::UpscalerProbe::interval_seconds(0) == 2 && ar::UpscalerProbe::interval_seconds(119) == 2 &&
        ar::UpscalerProbe::interval_seconds(120) == 10 && ar::UpscalerProbe::interval_seconds(3600) == 10,
        "every 2 s for 2 minutes, then every 10 s");
}

static void test_step_effect() {
    // design v1 Appendix A-3: one 0.7 step's GPU time.
    expect(ar::judge_step(30.0f, 30.0f * 0.87f) == ar::StepEffect::works, "SP in TEKKEN (0.87) works");
    expect(ar::judge_step(30.0f, 30.0f * 0.90f) == ar::StepEffect::works, "E33's DLSS mode (0.90) works");
    expect(ar::judge_step(30.0f, 30.0f * 0.84f) == ar::StepEffect::works, "Hogwarts Legacy's DLSS quality (0.84) works");
    expect(ar::judge_step(30.0f, 30.0f * 0.96f) == ar::StepEffect::nothing, "Wukong's DLSS mode (0.96) does nothing worth keeping");
    expect(ar::judge_step(30.0f, 30.5f) == ar::StepEffect::nothing, "SP under DLSS, or slower: nothing");
    expect(ar::judge_step(30.0f, 30.0f * 0.94f) == ar::StepEffect::unclear, "6 % less: kept, not judged either way");
    expect(ar::judge_step(0.0f, 10.0f) == ar::StepEffect::unclear, "no GPU time before: not judged");
}

static void test_step_strikes() {
    ar::StepStrikes s{};
    expect(!s.verdict(true, 100.0), "the first verdict against a step only puts it back");
    expect(s.verdict(true, 112.0), "the second in a row (its retake) leaves the actuator out");
    expect(s.count == 0, "the count starts over once the actuator goes out or is capped");
    expect(!s.verdict(true, 400.0), "a capped actuator taken again later: one misread only puts it back");

    ar::StepStrikes t{};
    expect(!t.verdict(true, 10.0), "Wukong at the load->play cut: put back");
    expect(!t.verdict(false, 22.0), "taken again, -7 %: kept, the count starts over");
    expect(!t.verdict(true, 300.0), "a later verdict against the next step is a first again");
    expect(t.verdict(true, 312.0), "and its retake's leaves it out");

    ar::StepStrikes u{};
    expect(!u.verdict(true, 10.0), "a misread, then the GPU went NORMAL: no retake");
    expect(!u.verdict(true, 200.0), "minutes later another misread is a first again, not the second");

    ar::StepStrikes v{};
    expect(!v.verdict(true, 10.0, 0.97f), "against at 0.97: put back");
    expect(!v.verdict(true, 22.0, 1.18f), "against at 1.18 disagrees: put back again");
    expect(v.verdict(true, 34.0, 1.12f), "against at 1.12 agrees with 1.18: out");

    ar::StepStrikes w{};
    expect(!w.verdict(true, 10.0, 0.80f) && !w.verdict(true, 22.0, 1.00f) && !w.verdict(true, 34.0, 1.20f),
        "three against, none agreeing");
    expect(w.verdict(true, 46.0, 1.40f), "the fourth against in a row goes out anyway");
    expect(!w.verdict(true, 58.0, 1.40f), "the count started over");
}

// compare_scene / judge_trial / TrialDiscards: a trial whose seconds show the content changing is not a verdict.
struct FpsGpu {
    float fps;
    float gpu_ms;
};

static std::vector<ar::SecondReading> seconds_at(std::initializer_list<FpsGpu> fps_and_gpu_ms) {
    std::vector<ar::SecondReading> out{};

    for (const auto& [fps, gpu_ms] : fps_and_gpu_ms) {
        // busy = GPU ms per frame x frames per second / 10
        out.push_back(ar::SecondReading{1.0f, (uint64_t)fps, gpu_ms * fps / 10.0f});
    }

    return out;
}

static void test_scene_check() {
    // Wukong, 3080L, 2026-11-23: the step inside a loading picture rendered at 37 fps / 22 ms, the play after it at
    // 29 fps / 26 ms from 3 s before the verdict.
    const auto picture = seconds_at({{37, 22.1f}, {37, 22.0f}, {38, 22.2f}, {37, 22.1f}});
    const auto picture_then_play = seconds_at({{37, 22.0f}, {37, 22.1f}, {29, 26.2f}, {29, 26.0f}, {30, 26.1f}});
    const auto incident = ar::compare_scene(picture, picture_then_play);
    expect(incident.read == ar::SceneRead::changed, "the loading picture -> play inside the after-window is a scene change");
    expect(incident.after.jump > ar::SCENE_JUMP && incident.before.jump < 0.05f, "the jump is after the step, the before is steady");
    // With the cut inside a second, that second's mix dilutes the jump under SCENE_JUMP: the scene check alone misses it,
    // and the strikes' agreement rule is what keeps it from leaving the actuator out (test_judge_trial).
    const auto mixed = seconds_at({{37, 22.0f}, {37, 22.1f}, {33, 24.0f}, {29, 26.0f}, {30, 26.1f}});
    expect(ar::compare_scene(picture, mixed).read == ar::SceneRead::same, "a cut inside a second can read as one scene");

    // The incident's own perf.csv rows (3080L, 2026-11-23). Step 2 at 05:55:57.7 inside the loading picture: before
    // 05:55:54-57, after 05:55:59-05:56:04 with the 143 ms tick second at 05:56:01. GPU per second is not in the data:
    // 22.1 ms on the picture, 26.1 in play (the trial's own readings), so the frame rate carries it.
    {
        const auto before = seconds_at({{37, 22.1f}, {38, 22.1f}, {37, 22.1f}, {38, 22.1f}});
        const auto after = seconds_at({{39, 22.1f}, {36, 22.1f}, {39, 22.1f}, {23, 22.1f}, {27, 26.1f}, {31, 26.1f}});
        const auto real = ar::compare_scene(before, after);
        expect(real.read == ar::SceneRead::changed && !real.after.gpu && real.after.jump > 0.40f,
            "the incident's trial: fps 38 -> 27 after the step, a 41 % jump");
        const auto clean = seconds_at({{39, 22.1f}, {36, 22.1f}, {39, 22.1f}, {27, 26.1f}, {31, 26.1f}});
        expect(ar::compare_scene(before, clean).read == ar::SceneRead::changed, "without the tick second still 31 %");
        // Its first step (05:55:29, in play, read no effect): one scene, judged as before.
        const auto play_before = seconds_at({{29, 28.9f}, {28, 28.9f}, {28, 28.9f}, {26, 28.9f}});
        const auto play_after = seconds_at({{31, 27.7f}, {30, 27.7f}, {27, 27.7f}, {28, 27.7f}, {25, 27.7f}});
        expect(ar::compare_scene(play_before, play_after).read == ar::SceneRead::same, "the incident's first trial: same");
    }

    // A step that works changes the level once, at the step: each side stays level.
    const auto play = seconds_at({{35, 25.0f}, {36, 24.6f}, {34, 25.3f}, {35, 25.1f}});
    const auto play_after_step = seconds_at({{40, 22.0f}, {41, 21.8f}, {39, 22.3f}, {40, 22.1f}, {40, 22.0f}});
    expect(ar::compare_scene(play, play_after_step).read == ar::SceneRead::same, "a step's own change is not a scene change");

    // Play's own variation, Wukong-like: +-10 % a second and a hitch second, stays one scene.
    const auto noisy = seconds_at({{38, 24.0f}, {33, 26.0f}, {36, 25.0f}, {30, 25.5f}, {37, 24.2f}});
    expect(ar::compare_scene(play, noisy).read == ar::SceneRead::same, "play's own +-10 % is one scene");

    // A scene change in the before-window (the step 0.7 s into play after a menu).
    const auto menu_then_play = seconds_at({{60, 9.0f}, {60, 9.2f}, {38, 24.0f}, {37, 24.5f}});
    expect(ar::compare_scene(menu_then_play, play_after_step).read == ar::SceneRead::changed, "a change before the step");

    // GPU time alone: a GPU-bound jump at a fixed frame rate (TEKKEN at 60).
    const auto fixed_light = seconds_at({{60, 10.0f}, {60, 10.1f}, {60, 9.9f}, {60, 10.0f}});
    const auto fixed_heavy_mid = seconds_at({{60, 10.0f}, {60, 10.2f}, {60, 13.8f}, {60, 14.0f}, {60, 13.9f}});
    const auto fixed = ar::compare_scene(fixed_light, fixed_heavy_mid);
    expect(fixed.read == ar::SceneRead::changed && fixed.after.gpu, "a GPU-time jump at a fixed frame rate is a change");

    // The engine stopped for seconds on one side (a stall the stall clock missed): a change.
    const auto stopped = seconds_at({{0, 0.0f}, {0, 0.0f}, {40, 22.0f}, {40, 22.1f}});
    expect(ar::compare_scene(play, stopped).read == ar::SceneRead::changed, "no frames on one side is a change");

    // Too few seconds (the sampler is off or just started): unknown, judged as before.
    expect(ar::compare_scene({}, play_after_step).read == ar::SceneRead::unknown, "no sampler seconds: unknown");
    expect(ar::compare_scene(seconds_at({{35, 25.0f}, {35, 25.0f}, {35, 25.0f}}), play_after_step).read == ar::SceneRead::unknown,
        "three seconds before: unknown");
}

static void test_judge_trial() {
    // The incident: the first verdict (in play, -3 %) was "no effect"; the second came across the loading picture.
    {
        ar::StepStrikes strikes{};
        ar::TrialDiscards discards{};
        const auto first = ar::judge_trial(ar::TrialReading{.gpu_ms_before = 25.0f, .gpu_ms_after = 24.3f, .fps_before = 35.0f,
            .fps_after = 35.5f, .scene = ar::SceneRead::same}, strikes, discards, 100.0);
        expect(first.outcome == ar::TrialOutcome::put_back && first.effect == ar::StepEffect::nothing, "in play: no effect, put back");
        const auto second = ar::judge_trial(ar::TrialReading{.gpu_ms_before = 22.1f, .gpu_ms_after = 26.1f, .fps_before = 37.3f,
            .fps_after = 29.3f, .scene = ar::SceneRead::changed}, strikes, discards, 130.0);
        expect(second.outcome == ar::TrialOutcome::rescheduled && second.slower, "across the picture: rescheduled, not out");
        expect(strikes.count == 1, "the rescheduled trial moves no strike");
        const auto retake = ar::judge_trial(ar::TrialReading{.gpu_ms_before = 26.0f, .gpu_ms_after = 23.0f, .fps_before = 29.0f,
            .fps_after = 33.0f, .scene = ar::SceneRead::same}, strikes, discards, 142.0);
        expect(retake.outcome == ar::TrialOutcome::works && strikes.count == 0, "the retake in play judges it: works");
    }

    // The same with the scene check missing the cut: the two verdicts against disagree (0.97 and 1.18), so the second
    // only puts the step back; a retake in play that agrees with the first leaves it out, as a step that does nothing.
    {
        ar::StepStrikes strikes{};
        ar::TrialDiscards discards{};
        const auto in_play = ar::TrialReading{.gpu_ms_before = 25.0f, .gpu_ms_after = 24.3f, .fps_before = 35.0f,
            .fps_after = 35.5f, .scene = ar::SceneRead::same};
        expect(ar::judge_trial(in_play, strikes, discards, 100.0).outcome == ar::TrialOutcome::put_back, "1st: put back");
        const auto across = ar::judge_trial(ar::TrialReading{.gpu_ms_before = 22.1f, .gpu_ms_after = 26.1f, .fps_before = 37.3f,
            .fps_after = 29.3f, .scene = ar::SceneRead::same}, strikes, discards, 130.0);
        expect(across.outcome == ar::TrialOutcome::put_back, "across the picture, disagreeing: put back, not out");
        expect(ar::judge_trial(in_play, strikes, discards, 142.0).outcome == ar::TrialOutcome::out,
            "a third, agreeing with the first: out");
    }

    // Without the scene check (unknown) and agreeing readings: two against in a row leave it out, as before.
    {
        ar::StepStrikes strikes{};
        ar::TrialDiscards discards{};
        const auto reading = ar::TrialReading{.gpu_ms_before = 22.1f, .gpu_ms_after = 26.1f, .fps_before = 37.3f, .fps_after = 29.3f};
        expect(ar::judge_trial(reading, strikes, discards, 10.0).outcome == ar::TrialOutcome::put_back, "unknown: judged, put back");
        expect(ar::judge_trial(reading, strikes, discards, 22.0).outcome == ar::TrialOutcome::out, "unknown: judged, out");
    }

    // A scene that keeps changing: two reschedules, then the third is kept unjudged; no strike at all.
    {
        ar::StepStrikes strikes{};
        ar::TrialDiscards discards{};
        const auto reading = ar::TrialReading{.gpu_ms_before = 20.0f, .gpu_ms_after = 20.0f, .fps_before = 40.0f,
            .fps_after = 40.0f, .scene = ar::SceneRead::changed};
        expect(ar::judge_trial(reading, strikes, discards, 10.0).outcome == ar::TrialOutcome::rescheduled, "1st: rescheduled");
        expect(ar::judge_trial(reading, strikes, discards, 22.0).outcome == ar::TrialOutcome::rescheduled, "2nd: rescheduled");
        expect(ar::judge_trial(reading, strikes, discards, 34.0).outcome == ar::TrialOutcome::kept_unjudged, "3rd: kept unjudged");
        expect(strikes.count == 0, "no strike from a changing scene");
        expect(ar::judge_trial(reading, strikes, discards, 46.0).outcome == ar::TrialOutcome::rescheduled,
            "after a kept one the count starts over");
    }

    // A reading for the step with the scene changing is kept and doesn't clear a strike either.
    {
        ar::StepStrikes strikes{};
        ar::TrialDiscards discards{};
        (void)ar::judge_trial(ar::TrialReading{.gpu_ms_before = 20.0f, .gpu_ms_after = 20.0f, .fps_before = 40.0f,
            .fps_after = 40.0f, .scene = ar::SceneRead::same}, strikes, discards, 10.0);
        const auto lighter = ar::judge_trial(ar::TrialReading{.gpu_ms_before = 20.0f, .gpu_ms_after = 15.0f, .fps_before = 40.0f,
            .fps_after = 50.0f, .scene = ar::SceneRead::changed}, strikes, discards, 22.0);
        expect(lighter.outcome == ar::TrialOutcome::kept && strikes.count == 1, "a lighter scene's works: kept, the strike stays");
    }

    // A stall: not judged, nothing moves (as before).
    {
        ar::StepStrikes strikes{};
        ar::TrialDiscards discards{};
        const auto stalled = ar::judge_trial(ar::TrialReading{.stall = true, .gpu_ms_before = 20.0f, .gpu_ms_after = 30.0f,
            .fps_before = 40.0f, .fps_after = 20.0f, .scene = ar::SceneRead::changed}, strikes, discards, 10.0);
        expect(stalled.outcome == ar::TrialOutcome::not_judged && strikes.count == 0 && discards.count == 0, "a stall: not judged");
    }

    // A step kept unjudged is put back once a minute later for a fresh trial, not kept for the session.
    {
        ar::State state{};
        state.upscaler = 1;
        ar::UnjudgedStep unjudged{};
        unjudged.kept(ar::Step{ar::Actuator::upscaler, 0, 1}, 100.0);
        expect(!unjudged.take_due(state, 130.0, false, false), "not before a minute");
        expect(!unjudged.take_due(state, 161.0, true, false), "not while loading");
        expect(!unjudged.take_due(state, 162.0, false, true), "not while a trial runs");
        const auto back = unjudged.take_due(state, 163.0, false, false);
        expect(back && back->actuator == ar::Actuator::upscaler && back->from == 1 && back->to == 0, "then put back, once");
        expect(!unjudged.take_due(state, 200.0, false, false), "only once");

        unjudged.kept(ar::Step{ar::Actuator::upscaler, 0, 1}, 300.0);
        state.upscaler = 2;
        expect(!unjudged.take_due(state, 400.0, false, false), "a later step over it: waits");
        state.upscaler = 1;
        expect(unjudged.take_due(state, 401.0, false, false).has_value(), "back at it (the later step capped): put back");

        unjudged.kept(ar::Step{ar::Actuator::upscaler, 0, 1}, 500.0);
        state.upscaler = 0;
        expect(!unjudged.take_due(state, 501.0, false, false) && !unjudged.step, "put back or gone out meanwhile: ends");
    }

    // Discards are forgotten like strikes: two minutes apart are firsts.
    {
        ar::TrialDiscards discards{};
        expect(discards.discard(10.0) && discards.discard(22.0), "two in a row");
        expect(discards.discard(200.0) && discards.count == 1, "minutes later a first again");
    }
}

// StallClock: UEVR's stall (no new engine frame for 250 ms, or no Present for max(250 ms, 4x the usual interval)) and
// loading (a stall in the last 3 s).
static void test_stall_clock() {
    constexpr int64_t MS = 1'000'000;
    const int64_t t0 = 1'000'000 * MS;

    {
        ar::StallClock c{};
        auto frame = t0;
        auto stalls = 0;

        for (int i = 1; i <= 120; ++i) {
            frame = t0 + i * 16 * MS;
            stalls += c.on_present(frame, frame) ? 1 : 0;
        }

        expect(stalls == 0 && !c.loading(frame), "stall: Presents with new engine frames are none");
        expect(!c.on_present(frame + 200 * MS, frame), "stall: the engine still within 250 ms of its last frame");
        expect(c.on_present(frame + 256 * MS, frame), "stall: no new engine frame for over 250 ms");
        expect(c.loading(frame + 256 * MS + 2900 * MS) && !c.loading(frame + 256 * MS + 3000 * MS), "stall: loading for 3 s after it");
    }

    {
        ar::StallClock c{};
        auto t = t0;

        for (int i = 0; i < 200; ++i) {
            t += 100 * MS;
            c.on_present(t, t);
        }

        t += 350 * MS;
        expect(!c.on_present(t, t), "stall: a 10 Hz game presenting after 350 ms is merely slow");
        t += 2000 * MS;
        expect(c.on_present(t, t), "stall: no Present for 4x the usual interval");
    }

    {
        ar::StallClock c{};
        expect(c.on_present(t0, 0) && c.loading(t0), "stall: no engine frame yet");
    }
}

// Frame generation (FrameGenCheck), fed as VR::update_autores feeds it (StallClock in front). It also counts the seconds
// the ratio alone, the check before 2026-11-20, read as frame generation, to show a trace is one that fooled it.
struct FrameGenFeed {
    static constexpr int64_t MS = 1'000'000;

    ar::FrameGenCheck check{};
    ar::StallClock stalls{};
    int64_t now{1'000'000 * MS};
    int64_t last_frame{now};
    int changes{0};
    bool ever_on{false};
    size_t step{0};
    int64_t old_start{0};
    uint32_t old_presents{0};
    uint32_t old_frames{0};
    int old_seconds{0};

    void present(int64_t dt, bool new_frame) {
        now += dt;

        if (new_frame) {
            last_frame = now;
        }

        const auto stall = stalls.on_present(now, last_frame);
        changes += check.on_present(now, new_frame, stall) ? 1 : 0;
        ever_on = ever_on || check.on;

        ++old_presents;
        old_frames += new_frame ? 1 : 0;

        if (now - old_start >= 1000 * MS) {
            old_seconds += old_frames >= 10 && old_presents * 2 >= old_frames * 3 ? 1 : 0;
            old_start = now;
            old_presents = old_frames = 0;
        }
    }

    // Presents at `hz` for `seconds`; `pattern` per Present, repeating: 'N' brings a new engine frame, 'd' does not.
    FrameGenFeed& run(double seconds, int hz, const char* pattern) {
        const auto n = static_cast<int>(seconds * hz + 0.5);
        const auto len = std::strlen(pattern);

        for (int i = 0; i < n; ++i) {
            present(1000 * MS / hz, pattern[step++ % len] == 'N');
        }

        return *this;
    }

    // No Present for `ms` (a synchronous load).
    FrameGenFeed& pause(int ms) {
        now += ms * MS;
        return *this;
    }
};

static void test_frame_generation() {
    {
        FrameGenFeed f{};
        f.run(10.0, 60, "N");
        expect(!f.check.on && f.changes == 0, "frame generation: a Present per engine frame is none");
    }

    {
        FrameGenFeed f{};
        f.run(1.5, 120, "Nd");
        expect(!f.check.on, "frame generation: one second of two Presents per engine frame is not enough");
        f.run(1.0, 120, "Nd");
        expect(f.check.on, "frame generation: two seconds in a row are");
        f.run(5.0, 120, "NNdd");
        expect(f.check.on && f.changes == 1, "frame generation: 1 and 3 Presents per engine frame in turn is still 2:1");
        f.run(5.0, 160, "Nddd");
        expect(f.check.on && f.changes == 1, "frame generation: 4 Presents per engine frame is too");
        f.run(3.0, 60, "d");
        expect(f.check.on, "frame generation: a loading screen does not turn it off");
        f.run(5.0, 120, "Nd");
        expect(f.check.on && f.changes == 1, "frame generation: and it goes on after the load");
        f.run(2.5, 60, "N");
        expect(!f.check.on && f.changes == 2, "frame generation: a judged second without it turns it off");
    }

    // TEKKEN 8 after a stage select (2026-11-20): UEVR holds its submits for 250 ms while the game presents 7-32 times,
    // then a loading screen presents at 51-60 Hz with the engine frame put, then the fight; sometimes a second hold with
    // 60-73 Presents, shorter than a stall. The second hold at every 5 ms from 2 to 4 s after the load (PR #38 review: 27
    // of these 400 passed the ratio in two seconds in a row), the load at 20 phases of the second.
    auto fooled_old = 0;

    for (int i = 0; i < 400; ++i) {
        FrameGenFeed f{};
        f.run(2.0 + (i % 20) * 0.05, 60, "N").run(0.25, 128, "d").run(4.0, 55, "d");
        f.run(2.0 + i * 0.005, 60, "N").run(0.25, 292, "d").run(5.0, 60, "N");
        fooled_old += f.old_seconds > 0 ? 1 : 0;
        expect(!f.ever_on && f.changes == 0, "frame generation: TEKKEN's stage load is none, at any phase");
    }

    expect(fooled_old >= 200, "frame generation: the TEKKEN trace fooled the one-second check");

    // Holds shorter than a stall (the engine back within 250 ms), at 200 phases of the second (PR #38 review: 14, 3 and
    // 20 of the first three passed the ratio in two seconds in a row), and seconds of short hitches.
    struct Hold {
        int fps;
        double seconds;
        int hz;
    };

    for (const auto& h : {Hold{60, 0.25, 292}, Hold{60, 0.24, 250}, Hold{30, 0.24, 292}, Hold{60, 0.2, 150}}) {
        auto fooled = 0;

        for (int i = 0; i < 200; ++i) {
            FrameGenFeed f{};
            f.run(4.0 + i * 0.005, h.fps, "N").run(h.seconds, h.hz, "d").run(5.0, h.fps, "N");
            fooled += f.old_seconds > 0 ? 1 : 0;
            expect(!f.ever_on && f.changes == 0, "frame generation: a hold is one frame, at any phase");
        }

        expect(fooled >= 100, "frame generation: the hold fooled the one-second check");
    }

    {
        FrameGenFeed f{};
        f.run(3.0, 60, "N").run(5.0, 110, "NNNNNNddddd").run(5.0, 60, "N");
        expect(!f.ever_on && f.old_seconds >= 4, "frame generation: hitches on a sixth of the frames are none");
    }

    // A stall every few seconds (PR #38 review): real frame generation still turns on, and turns off when the game stops
    // generating, with the stalls going on.
    for (const auto every : {1.5, 2.0, 3.0, 4.0}) {
        FrameGenFeed f{};
        f.run(0.5, 120, "Nd");
        auto on = 0;

        for (int i = 0; i < 20; ++i) {
            f.pause(300).run(every - 0.3, 120, "Nd");
            on += i >= 3 && f.check.on ? 1 : 0;
        }

        expect(on == 17, "frame generation: on with a stall every few seconds");

        for (int i = 0; i < 20; ++i) {
            f.run(0.3, 60, "d").run(every - 0.3, 60, "N");
        }

        expect(!f.check.on, "frame generation: and off once it stops, the stalls going on");
    }

    {
        FrameGenFeed f{};
        f.run(10.0, 120, "Nd").pause(300).run(1.2, 60, "N");
        auto off = !f.check.on;

        for (int i = 0; i < 10; ++i) {
            f.pause(300).run(1.2, 60, "N");
            off = off && !f.check.on;
        }

        expect(off, "frame generation: turned off at a stall, then a stall every 1.5 s: off at the first second");
    }
}

// Presents at 40 a second for `seconds` from `start_s`; repeats_per_second of each second's Presents (the first ones)
// have no new engine frame. Returns the bursts started (their times in seconds).
static std::vector<double> run_presents(ar::RepeatBurstClock& clock, double start_s, int seconds, int repeats_per_second) {
    std::vector<double> bursts{};

    for (int s = 0; s < seconds; ++s) {
        for (int i = 0; i < 40; ++i) {
            const auto t = start_s + s + i / 40.0;

            if (clock.on_present((int64_t)(t * 1e9), i < repeats_per_second)) {
                bursts.push_back(t);
            }
        }
    }

    return bursts;
}

static void test_repeat_burst() {
    // Black Myth: Wukong's death fade (4090 laptop perf.csv, 2026-11-26): no repeats in play, 5 then 4 in the fade's two
    // seconds; one burst per fade, at its first second's third repeat.
    {
        ar::RepeatBurstClock clock{};
        expect(run_presents(clock, 100.0, 30, 0).empty(), "fade: play makes no burst");
        const auto fade = run_presents(clock, 130.0, 1, 5);
        expect(fade.size() == 1 && fade[0] > 130.0 && fade[0] < 130.1, "fade: a burst at its start");
        expect((int64_t)(fade[0] * 1e9) == clock.last_burst_ns, "fade: last_burst_ns is the burst");
        expect(run_presents(clock, 131.0, 1, 4).empty(), "fade: its second second is the same burst");
        expect(run_presents(clock, 132.0, 50, 0).empty(), "fade: the picture and the play after it make none");
        expect(run_presents(clock, 182.0, 1, 5).size() == 1, "fade: the next fade a minute later is a burst of its own");
    }

    // A game that always presents twice: once the first seconds are counted, never a burst; a lone hitch with two
    // repeats in a second neither.
    {
        ar::RepeatBurstClock clock{};
        run_presents(clock, 10.0, 6, 20);
        expect(run_presents(clock, 16.0, 120, 20).empty(), "always twice: no burst");

        ar::RepeatBurstClock hitch{};
        run_presents(hitch, 10.0, 10, 0);
        expect(run_presents(hitch, 20.0, 1, 2).empty() && run_presents(hitch, 21.0, 10, 0).empty(), "a hitch: no burst");
    }

    // Two fades 3 s apart: the second has the first's repeats in its quiet seconds and is no burst; the trial still has
    // the first in it.
    {
        ar::RepeatBurstClock clock{};
        run_presents(clock, 0.0, 10, 0);
        expect(run_presents(clock, 10.0, 1, 5).size() == 1, "close fades: the first is a burst");
        run_presents(clock, 11.0, 2, 0);
        expect(run_presents(clock, 13.0, 1, 5).empty(), "close fades: the second inside the quiet seconds is none");
    }

    // with_repeat_burst: a burst in the trial reads the scene as changed whatever the jumps said; none leaves it.
    {
        ar::SceneCheck same{};
        same.read = ar::SceneRead::same;
        expect(ar::with_repeat_burst(same, std::nullopt).read == ar::SceneRead::same, "no burst: same stays same");
        const auto marked = ar::with_repeat_burst(same, 6.2f);
        expect(marked.read == ar::SceneRead::changed && marked.repeat_burst && *marked.repeat_burst == 6.2f,
            "a burst 6.2 s after the step: changed");
        expect(ar::with_repeat_burst(ar::SceneCheck{}, -3.0f).read == ar::SceneRead::changed, "unknown with a burst: changed");
    }

    // The 3080 laptop's synthetic-edge run (2026-11-26) through judge_trial with the burst: a reading for the step across the fade (11:44:09, -36 %,
    // "scene same") is kept with no strike moved, so the strike from the trial before it stays; a reading against
    // across the fade is rescheduled.
    {
        ar::StepStrikes strikes{};
        ar::TrialDiscards discards{};
        ar::SceneCheck same{};
        same.read = ar::SceneRead::same;
        const auto across = ar::with_repeat_burst(same, 5.0f).read;
        expect(ar::judge_trial(ar::TrialReading{.gpu_ms_before = 44.0f, .gpu_ms_after = 44.4f, .fps_before = 17.0f,
            .fps_after = 17.1f, .scene = ar::SceneRead::same}, strikes, discards, 100.0).outcome == ar::TrialOutcome::put_back,
            "D3 11:43:24 in play, +1 %: put back");
        const auto favourable = ar::judge_trial(ar::TrialReading{.gpu_ms_before = 45.0f, .gpu_ms_after = 28.8f,
            .fps_before = 17.5f, .fps_after = 22.7f, .scene = across}, strikes, discards, 145.0);
        expect(favourable.outcome == ar::TrialOutcome::kept && favourable.effect == ar::StepEffect::works,
            "D3 11:44:09 across the fade, -36 %: kept, not judged...");
        expect(strikes.count == 1, "...without resetting the strike");
        const auto against = ar::judge_trial(ar::TrialReading{.gpu_ms_before = 37.9f, .gpu_ms_after = 43.0f,
            .fps_before = 21.2f, .fps_after = 18.4f, .scene = across}, strikes, discards, 160.0);
        expect(against.outcome == ar::TrialOutcome::rescheduled && strikes.count == 1, "against across the fade: rescheduled");
    }
}

int main() {
    test_ladder();
    test_frame_bound();
    test_edges();
    test_order();
    test_limits();
    test_upscaler_probe();
    test_step_effect();
    test_dlss_floor();
    test_internal_recovery();
    test_anchor_gate();
    test_step_strikes();
    test_scene_check();
    test_judge_trial();
    test_repeat_burst();
    test_stall_clock();
    test_frame_generation();

    if (failures == 0) {
        std::printf("autores-policy-tests: all passed\n");
    }

    return failures == 0 ? 0 : 1;
}
