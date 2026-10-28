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

    constexpr ar::Size headset{2064, 2208};
    expect(ar::level_size(0, headset) == ar::Size{1376, 1472} && ar::level_size(4, headset) == ar::Size{4128, 2208},
        "another runtime's eye: the same ratios");
    expect(ar::level_size(0, headset, false) == ar::Size{1376, 1472} && ar::level_size(3, headset, false) == ar::Size{2522, 2698} &&
        ar::level_size(4, headset, false) == ar::Size{2924, 3128}, "other displays: uniform up-steps");
    expect(ar::highest_level_within(headset, ar::Size{2600, 2800}, false) == 3, "uniform levels are capped too");
    expect(ar::level_size(1, ar::Size{1001, 1001}) == ar::Size{834, 834}, "sizes are even");

    expect(ar::highest_level_within(mopic, ar::Size{3840, 2160}) == ar::MAX_LEVEL, "a max of 3840x2160 allows Max");
    expect(ar::highest_level_within(mopic, ar::Size{1920, 2160}) == ar::BASE_LEVEL, "a max of the eye itself stops at Base");
    expect(ar::highest_level_within(mopic, ar::Size{16384, 16384}) == ar::MAX_LEVEL, "a large max still stops at Max");
    expect(ar::highest_level_within(mopic, ar::Size{0, 0}) == ar::MIN_LEVEL, "no max: Min at least");

    expect(ar::nearest_level(mopic, mopic) == ar::BASE_LEVEL, "the recommended eye is Base");
    expect(ar::nearest_level(ar::Size{960, 1080}, mopic) == ar::MIN_LEVEL, "below the ladder is Min");
    expect(ar::nearest_level(ar::Size{1536, 1728}, mopic) == 1, "a 0.8 slider is closest to Saver");
    expect(ar::nearest_level(ar::Size{2700, 2160}, mopic) == 3, "a wide eye is closest to Sharp");
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
    const ar::Available available{};
    ar::State state{};
    std::vector<ar::Actuator> downs;

    while (const auto picked = ar::next_down(state, available)) {
        downs.push_back(picked->actuator);
        ar::apply(state, *picked);
    }

    expect((downs == std::vector{ar::Actuator::sp, ar::Actuator::sp, ar::Actuator::upscaler, ar::Actuator::upscaler,
        ar::Actuator::level, ar::Actuator::level}), "down: SP, the upscaler, then the level");
    expect(state.sp == 2 && state.upscaler == 2 && state.level == ar::MIN_LEVEL, "the bottom is SP 70 %, two modes, Min");

    std::vector<ar::Actuator> ups;
    while (const auto picked = ar::next_up(state, available)) {
        ups.push_back(picked->actuator);
        ar::apply(state, *picked);
    }

    expect((ups == std::vector{ar::Actuator::level, ar::Actuator::level, ar::Actuator::upscaler, ar::Actuator::upscaler,
        ar::Actuator::sp, ar::Actuator::sp, ar::Actuator::level, ar::Actuator::level}),
        "up: the same steps in reverse, then pixels above the start");
    expect(state.level == ar::MAX_LEVEL && state.sp == 0 && state.upscaler == 0, "the top is Max with everything restored");

    // From above the start, the extra pixels go first.
    expect(step(state, available, ar::Request::down) && state.level == 3 && state.sp == 0, "down from Max drops a level first");
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

    // A game already one mode above the lowest: one upscaler step, then the level.
    ar::Available one_mode{};
    one_mode.upscaler_steps = 1;
    state = ar::State{.sp = 2};
    expect(step(state, one_mode, ar::Request::down) && state.upscaler == 1, "the reachable upscaler step");
    expect(step(state, one_mode, ar::Request::down) && state.upscaler == 1 && state.level == 1, "then the level, no empty step");
}

int main() {
    test_ladder();
    test_edges();
    test_order();
    test_limits();

    if (failures == 0) {
        std::printf("autores-policy-tests: all passed\n");
    }

    return failures == 0 ? 0 : 1;
}
