#include "WindowFilter.hpp"

// To prevent usage of statics (TLS breaks the present thread...?)
std::unique_ptr<WindowFilter> g_window_filter{};

WindowFilter& WindowFilter::get() {
    if (g_window_filter == nullptr) {
        g_window_filter = std::make_unique<WindowFilter>();
    }

    return *g_window_filter;
}

WindowFilter::WindowFilter() {
    // is_filtered counts the job thread as dead once this is 2 s old, so it must not start at the clock's epoch:
    // windows queued before the first tick would be let through.
    m_last_job_tick = std::chrono::steady_clock::now();

    // We create a job thread because looking up a window can actually deadlock inside
    // the present thread...
    m_job_thread = std::make_unique<std::jthread>([this](std::stop_token s){
        while (!s.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});

            std::vector<HWND> jobs{};

            {
                std::scoped_lock _{m_mutex};
                m_last_job_tick = std::chrono::steady_clock::now();
                jobs.assign(m_window_jobs.begin(), m_window_jobs.end());
            }

            // Not under the lock: is_filtered takes it on the present thread, and the game thread can be
            // waiting for that present (Black Myth: Wukong hung at startup this way). The jobs stay queued
            // until they're decided, so their windows stay filtered meanwhile.
            for (const auto hwnd : jobs) {
                const auto filtered = is_filtered_nocache(hwnd);

                std::scoped_lock _{m_mutex};

                if (filtered) {
                    filter_window(hwnd);
                }

                m_window_jobs.erase(hwnd);
            }
        }
    });
}

WindowFilter::~WindowFilter() {
    m_job_thread->request_stop();
    m_job_thread->join();
}

bool WindowFilter::is_filtered(HWND hwnd) {
    if (hwnd == nullptr) {
        return true;
    }
    
    std::scoped_lock _{m_mutex};

    if (m_filtered_windows.find(hwnd) != m_filtered_windows.end()) {
        return true;
    }

    // If there is a job for this window, filter it until the job is done
    if (m_window_jobs.find(hwnd) != m_window_jobs.end()) {
        // If the thread is dead for some reason, do not filter it.
        return std::chrono::steady_clock::now() - m_last_job_tick <= std::chrono::seconds{2};
    }

    // if we havent even seen this window yet, add it to the job queue
    // and return true;
    if (m_seen_windows.find(hwnd) == m_seen_windows.end()) {
        m_seen_windows.insert(hwnd);
        m_window_jobs.insert(hwnd);
        return true;
    }

    return false;
}

bool WindowFilter::is_filtered_nocache(HWND hwnd) {
    // get window name
    // InternalGetWindowText reads the title Windows keeps for the window. GetWindowText would send WM_GETTEXT
    // to a window of this process and wait until its thread answers, which it can't while it waits for a frame.
    wchar_t window_name[256]{};
    InternalGetWindowText(hwnd, window_name, static_cast<int>(std::size(window_name)));

    const auto sv = std::wstring_view{window_name};

    if (sv.find(L"UE4SS") != std::wstring_view::npos) {
        return true;
    }

    if (sv.find(L"PimaxXR") != std::wstring_view::npos) {
        return true;
    }

    // TODO: more problematic windows
    return false;
}