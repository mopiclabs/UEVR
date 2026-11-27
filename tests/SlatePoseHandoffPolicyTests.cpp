#include "mods/vr/SlatePoseHandoffPolicy.hpp"

#include <cstdint>
#include <cstdio>

using namespace uevr::slate_pose_handoff;

static int g_failures = 0;

static void expect(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// One RenderTexture call as the Slate worker runs it. hook_ok = what hook_new_rhi_command returns (false: it rejected
// the command and enqueued directly).
static Action call(State& state, uint32_t frame, bool have_command, bool hook_ok = true) {
    const auto action = decide(state, frame, have_command);
    if (action == Action::HookCommand) {
        note_hook_result(state, frame, hook_ok);
    }
    return action;
}

static void test_hooked_then_same_frame() {
    State s{};
    expect(call(s, 10, true) == Action::HookCommand, "first call with a command hooks it");
    expect(call(s, 10, true) == Action::Skip, "second call of a carried frame is skipped");
    expect(call(s, 10, false) == Action::Skip, "carried frame stays skipped without a command");
}

static void test_no_command_both_calls() {
    State s{};
    expect(call(s, 10, false) == Action::EnqueueDirect, "no command: enqueue directly");
    expect(call(s, 10, false) == Action::EnqueueDirect, "no command again, same frame: enqueue again, as before");
}

static void test_no_command_then_command() {
    State s{};
    expect(call(s, 10, false) == Action::EnqueueDirect, "no command: enqueue directly");
    expect(call(s, 10, true) == Action::HookCommand, "a command on the second call is hooked");
    expect(call(s, 10, true) == Action::Skip, "third call after the hook is skipped");
}

static void test_command_then_no_command_next_frame() {
    State s{};
    expect(call(s, 10, true) == Action::HookCommand, "frame 10 hooked");
    expect(call(s, 11, false) == Action::EnqueueDirect, "frame 11 without a command: enqueue directly");
    expect(call(s, 11, false) == Action::EnqueueDirect, "frame 11 again without a command: enqueue again");
}

static void test_number_advancing() {
    State s{};
    for (uint32_t frame = 100; frame < 110; ++frame) {
        expect(call(s, frame, true) == Action::HookCommand, "each new frame is hooked");
        expect(call(s, frame, true) == Action::Skip, "each frame's repeat is skipped");
    }
}

static void test_rejected_command() {
    // hook_new_rhi_command rejected the candidate and enqueued directly: not a verified submission.
    State s{};
    expect(call(s, 10, true, false) == Action::HookCommand, "rejected candidate still goes through the hook call");
    expect(call(s, 10, true, false) == Action::HookCommand, "rejected frame is not skipped on the next call");
    expect(call(s, 10, false) == Action::EnqueueDirect, "rejected frame without a command: enqueue directly");
    expect(call(s, 10, true) == Action::HookCommand, "accepted on a later call");
    expect(call(s, 10, true) == Action::Skip, "then skipped");
}

static void test_rejected_after_verified() {
    // A rejection on a later frame does not keep the earlier verified number from mattering, and does not
    // make the later frame skippable.
    State s{};
    expect(call(s, 10, true) == Action::HookCommand, "frame 10 hooked");
    expect(call(s, 11, true, false) == Action::HookCommand, "frame 11 rejected");
    expect(call(s, 11, false) == Action::EnqueueDirect, "frame 11 repeat enqueues directly");
}

static void test_initial_state() {
    const uint32_t frames[] = {0u, 1u, UINT32_MAX};
    for (const uint32_t frame : frames) {
        State s{};
        expect(call(s, frame, false) == Action::EnqueueDirect, "fresh state never skips");
        expect(call(s, frame, true) == Action::HookCommand, "fresh state hooks");
        expect(call(s, frame, true) == Action::Skip, "then skips, at any frame number");
    }
}

int main() {
    test_hooked_then_same_frame();
    test_no_command_both_calls();
    test_no_command_then_command();
    test_command_then_no_command_next_frame();
    test_number_advancing();
    test_rejected_command();
    test_rejected_after_verified();
    test_initial_state();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("slate-pose-handoff-policy-tests: all passed\n");
    return 0;
}
