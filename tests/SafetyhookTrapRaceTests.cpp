// Hook writes on two threads at once must not leave a shared trampoline page (or a target page) non-executable,
// and a page left non-executable by someone else must fault instead of spinning forever in safetyhook's trap handler.
// TEKKEN 8 demo froze at boot that way (2026-10-03, 2026-10-08): the game thread looped on the first byte of a new
// mid hook stub whose allocator page had stayed PAGE_READWRITE.
#define NOMINMAX
#include <Windows.h>
#include <safetyhook.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
DWORD protection(const void* p) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(p, &info, sizeof(info));
    return info.Protect & 0xff;
}
bool executable(const void* p) {
    const auto prot = protection(p);
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

using Fn = int (*)();
int replacement_a() { return 100; }
int replacement_b() { return 200; }

// `mov eax, imm32; ret` functions, each on its own page, protected like game code
std::vector<Fn> make_targets(int count) {
    std::vector<Fn> fns;
    for (int i = 0; i < count; ++i) {
        auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        const uint8_t code[] = {0xB8, static_cast<uint8_t>(i + 1), 0, 0, 0, 0xC3};
        std::memcpy(page, code, sizeof(code));
        DWORD old{};
        VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
        fns.push_back(reinterpret_cast<Fn>(page));
    }
    return fns;
}

// Two threads hook and unhook their own targets as fast as they can while a third hook keeps the allocator's
// trampoline page alive, so every trampoline lands on the same page.
void test_concurrent_hook_writes() {
    auto targets = make_targets(3);
    auto keeper = safetyhook::create_inline(reinterpret_cast<void*>(targets[2]), reinterpret_cast<void*>(replacement_a));
    expect(static_cast<bool>(keeper), "keeper hook installs");
    if (!keeper) { return; }

    std::atomic<bool> go{false};
    const auto churn = [&](Fn target, Fn replacement) {
        while (!go.load()) { std::this_thread::yield(); }
        for (int i = 0; i < 20000; ++i) {
            auto hook = safetyhook::create_inline(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement));
            if (!hook) { continue; }
        }
    };
    std::thread a{churn, targets[0], replacement_a};
    std::thread b{churn, targets[1], replacement_b};
    go = true;
    a.join();
    b.join();

    expect(executable(keeper.trampoline().data()), "the shared trampoline page is still executable after concurrent hook writes");
    expect(executable(reinterpret_cast<void*>(targets[0])), "target page A is still executable");
    expect(executable(reinterpret_cast<void*>(targets[1])), "target page B is still executable");
    if (executable(keeper.trampoline().data())) {
        expect(keeper.call<int>() == 3, "the keeper's original still runs through its trampoline");
    }
}

// A trampoline page left non-executable outside any hook write: running there has to fault (and reach the caller's
// handler) instead of spinning in safetyhook's trap handler.
void test_stale_trap_page_faults() {
    auto targets = make_targets(1);
    auto hook = safetyhook::create_inline(reinterpret_cast<void*>(targets[0]), reinterpret_cast<void*>(replacement_a));
    expect(static_cast<bool>(hook), "hook installs");
    if (!hook) { return; }

    auto* tramp = hook.trampoline().data();
    DWORD old{};
    VirtualProtect(tramp, 1, PAGE_READWRITE, &old);

    std::atomic<int> outcome{0};   // 1 = returned, 2 = faulted
    std::thread t{[&] {
        __try {
            (void)reinterpret_cast<Fn>(tramp)();
            outcome = 1;
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            outcome = 2;
        }
    }};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (outcome.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (outcome.load() == 0) { std::cerr << "  (still spinning after 5 s)\n"; }
    if (outcome.load() == 1) { std::cerr << "  (returned: the trap handler sent it somewhere else)\n"; }
    expect(outcome.load() == 2, "running a stale non-executable trampoline page faults instead of spinning");

    VirtualProtect(tramp, 1, old, &old);
    if (outcome.load() == 0) {
        t.join();   // the spinning thread resumes now that the page is executable again
    } else {
        t.join();
    }
}
}

int main() {
    test_concurrent_hook_writes();
    test_stale_trap_page_faults();
    if (failures == 0) { std::cout << "safetyhook trap race tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
