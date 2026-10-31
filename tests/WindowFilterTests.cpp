// WindowFilter must decide windows without waiting on their window thread, and Present must never wait for that
// decision. Black Myth: Wukong hung at startup (2026-10-30): the filter's job thread held its mutex inside
// GetWindowTextA, which waits for the game thread to answer WM_GETTEXT; the game thread was waiting for the Present
// thread, and the Present thread was waiting for the mutex in is_filtered.
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "WindowFilter.hpp"

namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

HWND create_window(const wchar_t* title) {
    static const auto registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WindowFilterTests";
        return RegisterClassW(&wc) != 0;
    }();
    if (!registered) {
        return nullptr;
    }
    return CreateWindowExW(0, L"WindowFilterTests", title, WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

void pump_messages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// The windows' own thread waits for "Present" without pumping messages while Present keeps asking the filter about
// them, across the job thread's first lookups.
void test_present_while_window_thread_waits() {
    const auto game = create_window(L"Game");
    const auto ue4ss = create_window(L"UE4SS Debugging Tools");
    expect(game != nullptr && ue4ss != nullptr, "test windows are created");

    std::atomic<bool> first_sight_filtered{false};
    std::atomic<bool> game_filtered{true};
    std::atomic<bool> ue4ss_filtered{false};
    const auto present_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::thread present{[&] {
        auto& filter = WindowFilter::get();
        // Asked twice before the job thread's first tick: both times still pending.
        first_sight_filtered = filter.is_filtered(game) && filter.is_filtered(ue4ss) && filter.is_filtered(game) &&
                               filter.is_filtered(ue4ss);

        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (std::chrono::steady_clock::now() < end) {
            filter.is_filtered(game);
            filter.is_filtered(ue4ss);
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }

        game_filtered = filter.is_filtered(game);
        ue4ss_filtered = filter.is_filtered(ue4ss);
        SetEvent(present_done);
    }};

    if (WaitForSingleObject(present_done, 10000) != WAIT_OBJECT_0) {
        std::cerr << "FAILED: Present blocked in is_filtered while the window thread waited for it (deadlock)\n";
        ExitProcess(1); // the stuck threads can't be joined
    }

    present.join();
    expect(first_sight_filtered, "a window is filtered until the job thread has looked at it");
    expect(!game_filtered, "the game window is decided while its thread waits");
    expect(ue4ss_filtered, "the UE4SS window is decided while its thread waits");

    CloseHandle(present_done);
    DestroyWindow(game);
    DestroyWindow(ue4ss);
    pump_messages();
}

// A window first seen once the job thread has had nothing to do for longer than is_filtered's 2 s liveness check
// still gets looked at.
void test_window_seen_after_idle() {
    std::this_thread::sleep_for(std::chrono::milliseconds{2500});

    const auto pimax = create_window(L"PimaxXR Mirror");
    const auto other = create_window(L"Other");
    auto& filter = WindowFilter::get();
    filter.is_filtered(pimax);
    filter.is_filtered(other);

    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    expect(filter.is_filtered(pimax), "a PimaxXR window seen later is filtered");
    expect(!filter.is_filtered(other), "another window seen later is not");

    DestroyWindow(pimax);
    DestroyWindow(other);
    pump_messages();
}
}

int main() {
    // First: WindowFilter::get() starts the job thread, and this test needs it running when Present first asks.
    test_present_while_window_thread_waits();
    test_window_seen_after_idle();
    expect(WindowFilter::get().is_filtered(nullptr), "a null window is filtered");

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "window-filter-tests passed\n";
    return EXIT_SUCCESS;
}
