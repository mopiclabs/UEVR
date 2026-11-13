// VR_AutoRes's GPU time. See AutoResGpu.hpp.

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "PerfLog.hpp"
#include "utility/Logging.hpp"

#include "AutoResGpu.hpp"

namespace uevr::autores {
namespace {
// pdh.dll is loaded when the sampler starts, so UEVR does not link it.
struct Pdh {
    decltype(&PdhOpenQueryW) open_query{};
    decltype(&PdhAddEnglishCounterW) add_english_counter{};
    decltype(&PdhCollectQueryData) collect{};
    decltype(&PdhGetFormattedCounterArrayW) get_array{};
    decltype(&PdhCloseQuery) close_query{};

    bool load() {
        // From System32 only, never a pdh.dll in the game's folder.
        const auto module = LoadLibraryExW(L"pdh.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

        if (module == nullptr) {
            return false;
        }

        open_query = (decltype(open_query))GetProcAddress(module, "PdhOpenQueryW");
        add_english_counter = (decltype(add_english_counter))GetProcAddress(module, "PdhAddEnglishCounterW");
        collect = (decltype(collect))GetProcAddress(module, "PdhCollectQueryData");
        get_array = (decltype(get_array))GetProcAddress(module, "PdhGetFormattedCounterArrayW");
        close_query = (decltype(close_query))GetProcAddress(module, "PdhCloseQuery");
        return open_query != nullptr && add_english_counter != nullptr && collect != nullptr && get_array != nullptr &&
            close_query != nullptr;
    }
};

// Instances are pid_<pid>_luid_<hi>_<lo>_phys_<n>_eng_<n>_engtype_<type>. Graphics and compute engines count: a copy
// engine busy streaming during a load is not what a frame waits for.
bool counts(const wchar_t* name) {
    const auto type = std::wcsstr(name, L"engtype_");

    if (type == nullptr) {
        return false;
    }

    const auto rest = type + 8;
    return std::wcsncmp(rest, L"3D", 2) == 0 || std::wcsncmp(rest, L"Graphics", 8) == 0 || std::wcsncmp(rest, L"Compute", 7) == 0;
}
}

void GpuBusySampler::start() {
    if (m_thread.joinable() || m_unavailable) {
        return;
    }

    m_thread = std::jthread{[this](std::stop_token stop) { run(stop); }};
}

void GpuBusySampler::stop() {
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_wake.notify_all();
        m_thread.join();
        m_thread = {};
    }

    std::scoped_lock _{m_mutex};
    m_samples.clear();
}

void GpuBusySampler::run(std::stop_token stop) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    Pdh pdh{};
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    auto status = pdh.load() ? pdh.open_query(nullptr, 0, &query) : (PDH_STATUS)PDH_CSTATUS_NO_OBJECT;

    if (status == ERROR_SUCCESS) {
        status = pdh.add_english_counter(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
    }

    if (status == ERROR_SUCCESS) {
        status = pdh.collect(query); // a rate counter: the first collect is the baseline
    }

    if (status != ERROR_SUCCESS) {
        SPDLOG_WARN("[autores] the GPU Engine counters are not available (0x{:08x}): steps are checked by the frame rate only",
            (uint32_t)status);
        m_unavailable = true;

        if (query != nullptr) {
            pdh.close_query(query);
        }

        return;
    }

    const auto prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    std::vector<uint8_t> buffer{};

    struct Engine {
        std::wstring name; // the instance up to "_engtype_": luid, phys and engine
        double busy;
    };
    std::vector<Engine> engines{};
    bool first_logged = false;

    while (!stop.stop_requested()) {
        {
            std::unique_lock lock{m_mutex};
            m_wake.wait_for(lock, stop, std::chrono::seconds{1}, [] { return false; });
        }

        if (stop.stop_requested()) {
            break;
        }

        if (pdh.collect(query) != ERROR_SUCCESS) {
            continue;
        }

        const auto frames = uevr::perf::new_frames();
        const auto now = clock::now();
        DWORD size = (DWORD)buffer.size();
        DWORD count = 0;
        constexpr DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
        status = pdh.get_array(counter, format, &size, &count, (PDH_FMT_COUNTERVALUE_ITEM_W*)buffer.data());

        if (status == (PDH_STATUS)PDH_MORE_DATA) {
            buffer.resize(size);
            status = pdh.get_array(counter, format, &size, &count, (PDH_FMT_COUNTERVALUE_ITEM_W*)buffer.data());
        }

        if (status != ERROR_SUCCESS) {
            continue;
        }

        // This process's engines; the busy share is the busiest one, as Task Manager shows a GPU.
        engines.clear();
        const auto items = (const PDH_FMT_COUNTERVALUE_ITEM_W*)buffer.data();

        for (DWORD i = 0; i < count; ++i) {
            const auto& item = items[i];

            if (item.szName == nullptr || std::wcsncmp(item.szName, prefix.c_str(), prefix.size()) != 0 || !counts(item.szName)) {
                continue;
            }

            if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }

            const auto value = item.FmtValue.doubleValue;

            if (!(value > 0.0)) {
                continue;
            }

            const auto type = std::wcsstr(item.szName, L"_engtype_");
            std::wstring name{item.szName, type != nullptr ? (size_t)(type - item.szName) : std::wcslen(item.szName)};
            auto it = std::find_if(engines.begin(), engines.end(), [&](const Engine& e) { return e.name == name; });

            if (it == engines.end()) {
                engines.push_back(Engine{std::move(name), value});
            } else {
                it->busy += value;
            }
        }

        double busy = 0.0;

        for (const auto& engine : engines) {
            busy = (std::max)(busy, engine.busy);
        }

        if (!first_logged) {
            first_logged = true;
            SPDLOG_INFO("[autores] GPU time: this process's busiest of {} engines, {:.0f} % now", engines.size(), busy);
        }

        std::scoped_lock _{m_mutex};
        m_samples.push_back(Sample{now, (float)(std::min)(busy, 100.0), frames});

        while (m_samples.size() > 60) {
            m_samples.pop_front();
        }
    }

    pdh.close_query(query);
}

std::optional<float> GpuBusySampler::gpu_ms_per_frame(clock::time_point from, clock::time_point to) const {
    std::scoped_lock _{m_mutex};
    double busy_seconds = 0.0;
    uint64_t frames = 0;
    int used = 0;

    for (size_t i = 1; i < m_samples.size(); ++i) {
        const auto& before = m_samples[i - 1];
        const auto& sample = m_samples[i];

        if (sample.at <= from || sample.at > to) {
            continue;
        }

        const auto seconds = std::chrono::duration<double>(sample.at - before.at).count();

        // A gap of more than 2 s (the thread stalled) is no second's share.
        if (seconds <= 0.0 || seconds > 2.0) {
            return std::nullopt;
        }

        busy_seconds += sample.busy_pct / 100.0 * seconds;
        frames += sample.frames - before.frames;
        ++used;
    }

    if (used < 2 || frames == 0) {
        return std::nullopt;
    }

    return (float)(busy_seconds * 1000.0 / (double)frames);
}
}
