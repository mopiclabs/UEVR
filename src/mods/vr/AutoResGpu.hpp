#pragma once

// VR_AutoRes's GPU time (AutoRes.hpp): how busy the GPU is with this process, once a second, from Windows' "GPU
// Engine" performance counters (the numbers Task Manager shows). The step checks turn it into GPU time per frame.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace uevr::autores {
class GpuBusySampler {
public:
    using clock = std::chrono::steady_clock;

    struct Sample {
        clock::time_point at{};
        float busy_pct{}; // this process's busiest graphics or compute engine over the second before
        uint64_t frames{}; // uevr::perf::new_frames() at the same moment
    };

    GpuBusySampler() = default;
    GpuBusySampler(const GpuBusySampler&) = delete;
    GpuBusySampler& operator=(const GpuBusySampler&) = delete;
    ~GpuBusySampler() { stop(); }

    // Starts the sampler thread once; later calls do nothing. stop() ends it (the destructor too).
    void start();
    void stop();

    // False when the counters could not be opened (the step checks then judge by the frame rate alone).
    bool unavailable() const { return m_unavailable.load(); }

    // The GPU time per frame in ms over the samples taken in (from, to]: the busy share of each second times its length,
    // over the frames in the same span. Empty when fewer than two samples fall in it or no frame was presented.
    std::optional<float> gpu_ms_per_frame(clock::time_point from, clock::time_point to) const;

private:
    void run(std::stop_token stop);

    mutable std::mutex m_mutex{};
    std::condition_variable_any m_wake{};
    std::deque<Sample> m_samples{}; // the last minute
    std::jthread m_thread{};
    std::atomic<bool> m_unavailable{false};
};
}
