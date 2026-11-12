#include "mods/vr/AutoResPolicy.hpp"

#include <cstdio>
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

    if (failures == 0) {
        std::printf("autores-policy-tests: all passed\n");
    }

    return failures == 0 ? 0 : 1;
}
