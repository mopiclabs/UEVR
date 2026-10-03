#include "mods/vr/PerfLog.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// VR_PerfLog's counters and writer, without a game: drives the frame paths by hand and reads the CSVs back.

namespace perf = uevr::perf;
static int failures{};
static void expect(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream{line};
    std::string field;
    while (std::getline(stream, field, ',')) { fields.push_back(field); }
    if (!line.empty() && line.back() == ',') { fields.emplace_back(); }
    return fields;
}

struct Csv {
    std::vector<std::string> header; // the first line, as tools/mopic-test/perfreport.py reads it
    std::vector<std::map<std::string, std::string>> rows;
    bool aligned{true};
};

static Csv read_csv(const std::filesystem::path& path) {
    Csv csv;
    std::ifstream file{path};
    std::string line;
    while (std::getline(file, line)) {
        if (csv.header.empty()) {
            csv.header = split(line);
            continue;
        }
        const auto fields = split(line);
        csv.aligned = csv.aligned && fields.size() == csv.header.size();
        std::map<std::string, std::string> row;
        for (size_t i = 0; i < fields.size() && i < csv.header.size(); ++i) { row[csv.header[i]] = fields[i]; }
        csv.rows.push_back(std::move(row));
    }
    return csv;
}

static bool has_columns(const Csv& csv, std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        if (std::find(csv.header.begin(), csv.header.end(), name) == csv.header.end()) {
            std::fprintf(stderr, "missing column %s\n", name);
            return false;
        }
    }
    return true;
}

static uint64_t sum(const Csv& csv, const char* column) {
    uint64_t total = 0;
    for (const auto& row : csv.rows) { total += std::stoull(row.at(column)); }
    return total;
}

// One VR frame: Tick, Present (with xrWaitFrame, a fence wait and xrEndFrame inside it, like the very-late sync path).
// Without `tick`, a Present with no engine Tick before it (frame generation's second Present of an engine frame).
static void frame(uint32_t engine_frame, bool projection, bool ok, int64_t display_time, bool tick_first = true,
    perf::NativePair pair = perf::NativePair::None) {
    if (tick_first) {
        perf::TickTimer tick;
        tick.begin_jobs();
        tick.end_jobs();
        tick.begin_tick();
        tick.end_tick();
        tick.finish(1.0f / 60.0f);
    }

    perf::PresentTimer present;
    present.begin_present();
    perf::note_blocked(1'000'000); // blocked inside the original Present: present_us, not UEVR's
    present.end_present();
    perf::note_xr_wait(2'000'000, true, true, 16'666'666, 6, "vr_very_late_post_present");
    perf::note_native_stereo(perf::NativeStereo::Reused);
    perf::note_native_pair(pair);
    perf::note_fence_wait(300'000);
    const auto now = perf::now_ns();
    perf::note_xr_end(ok, projection, engine_frame, now, 500'000, display_time, 16'666'666);
    present.finish();
}

int main() try {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto dir = std::filesystem::path{temp} / L"uevr-perflog-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    expect(perf::qpc_ticks_to_ns(0) == 0, "qpc 0 maps to 0 ns");
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const auto steady = perf::now_ns();
    const auto converted = perf::qpc_ticks_to_ns(qpc.QuadPart);
    expect(converted <= steady && steady - converted < 50'000'000, "QPC ticks convert onto the steady_clock time base");

    // perfreport.py's FLAG_BITS / SYNC_STAGES / METHODS.
    constexpr auto packed = perf::flag::pack(perf::flag::D3D12 | perf::flag::FOREGROUND, 2, 1);
    static_assert(perf::flag::D3D12 == 1u << 7 && perf::flag::FRAMEGEN_SWAPCHAIN == 1u << 8 && perf::flag::DLSSG == 1u << 9 &&
        perf::flag::MENU == 1u << 10 && perf::flag::FOCUSED == 1u << 12 && perf::flag::MONO == 1u << 14);
    static_assert(packed == ((1u << 7) | (1u << 13) | (2u << 16) | (1u << 20)));
    static_assert(perf::flag::sync_stage(packed) == 2 && perf::flag::method(packed) == 1);

    // Nothing is counted before the log is enabled.
    frame(1, true, true, 500);
    expect(perf::g_counters.xr_new.load() == 0, "counters stay still until the log is enabled");

    expect(perf::start(dir) == 0, "writer starts");
    expect(perf::is_running(), "writer is running");
    perf::set_state(perf::flag::pack(perf::flag::D3D12 | perf::flag::HMD | perf::flag::DLSSG, 2, 0));
    perf::set_enabled(true);

    // 4 new frames, 1 repeat, 1 empty, 1 failed.
    frame(10, true, true, 1'000);
    frame(11, true, true, 2'000);
    frame(11, true, true, 3'000);
    frame(0, false, true, 4'000);
    frame(12, true, false, 5'000);
    frame(12, true, true, 6'000);
    perf::note_xr_wait_skipped();
    perf::note_duplicate_present();
    perf::note_time_pair(qpc.QuadPart, 123'456'789);

    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    frame(13, true, true, 7'000);
    perf::stop();
    expect(!perf::is_running(), "writer stopped");

    const auto summary = read_csv(dir / "perf.csv");
    const auto frames = read_csv(dir / "perf-frames.csv");
    expect(!summary.header.empty() && summary.header.front() == "qpc_ns", "perf.csv starts with its header");
    expect(!frames.header.empty() && frames.header.front() == "qpc_ns", "perf-frames.csv starts with its header");
    // The columns tools/mopic-test/perfreport.py reads.
    expect(has_columns(frames, {"qpc_ns", "kind", "frame", "display_time", "period_ns", "end_us", "wait_us", "nsf", "nsf_pair",
        "flags", "presents", "dup_presents", "present_us", "ticks", "ticks_skipped", "tick_us", "uevr_gt_us", "uevr_rt_us",
        "rt_blocked_us"}),
        "perf-frames.csv has perfreport.py's columns");
    expect(has_columns(summary, {"qpc_ns", "unix_ms", "window_ms", "xr_new", "xr_repeat", "xr_empty", "xr_fail", "xr_older",
        "wait_ok", "presents_per_tick", "nsf_live", "nsf_snapshot", "frames_dropped", "engine_delta_ms", "xr_pair_qpc_ns",
        "xr_pair_time"}),
        "perf.csv has perfreport.py's columns");
    {
        std::string decoded;
        for (const auto& name : summary.header) { decoded += name + ","; }
        expect(decoded.find(std::string{"flags,"} + perf::flag::NAMES + ",sync_stage,method,") != std::string::npos,
            "perf.csv decodes the flag bits in flag::NAMES order");
    }
    expect(summary.aligned, "perf.csv rows match the header");
    expect(frames.aligned, "perf-frames.csv rows match the header");
    expect(summary.rows.size() >= 2, "a row per second plus the final partial one");
    expect(frames.rows.size() == 7, "one perf-frames.csv row per xrEndFrame");

    std::map<std::string, int> kinds;
    for (const auto& row : frames.rows) { ++kinds[row.at("kind")]; }
    expect(kinds["P"] == 4 && kinds["R"] == 1 && kinds["E"] == 1 && kinds["F"] == 1, "submits classified P/R/E/F");
    expect(sum(summary, "xr_frames") == 7, "perf.csv counts every xrEndFrame");
    expect(sum(summary, "xr_new") == 4, "perf.csv counts new frames");
    expect(sum(summary, "xr_repeat") == 1 && sum(summary, "xr_empty") == 1 && sum(summary, "xr_fail") == 1,
        "perf.csv counts repeats, empty and failed submits");
    expect(sum(summary, "xr_older") == 0, "a repeat of the newest engine frame is not an older one");
    expect(sum(summary, "presents") == 7 && sum(summary, "ticks") == 7, "presents and ticks counted");
    expect(sum(summary, "wait_ok") == 7 && sum(summary, "wait_skip") == 1, "xrWaitFrame counted");
    expect(sum(summary, "dup_presents") == 1 && sum(summary, "fence_waits") == 7, "duplicate presents and fence waits counted");
    expect(sum(summary, "nsf_reused") == 7, "Native Stereo Fix reuse counted");
    expect(sum(summary, "frames_dropped") == 0 && sum(summary, "frame_rows_unwritten") == 0, "nothing dropped");

    const auto& first = summary.rows.front();
    expect(first.at("callsite") == "vr_very_late_post_present", "sync callsite reported");
    expect(first.at("sync_stage") == "2" && first.at("method") == "0" && first.at("d3d12") == "1" && first.at("dlssg") == "1" &&
        first.at("afr") == "0" && first.at("foreground") == "0", "mode flags decoded");
    expect(first.at("xr_pair_time") == "123456789" && first.at("xr_pair_qpc_ns") == std::to_string(converted),
        "XrTime/QPC pair reported");
    expect(first.at("presents_per_tick") == "1.00", "one Present per engine tick");
    expect(std::stod(first.at("new_int_max_ms")) > 0.0 && std::stod(first.at("new_int_p50_ms")) <= std::stod(first.at("new_int_max_ms")),
        "new-frame interval statistics");
    // Every Present above blocked for 2 ms (xrWaitFrame) + 0.3 ms (fence) + 0.5 ms (xrEndFrame) inside the hook; the
    // 1 ms inside the original Present is not UEVR's.
    expect(std::stod(first.at("rt_blocked_avg_ms")) > 2.7 && std::stod(first.at("rt_blocked_avg_ms")) < 2.9,
        "blocked time inside the Present hook");
    expect(std::stod(first.at("uevr_rt_avg_ms")) < 1.0, "UEVR's own render-thread cost leaves the blocked time out");

    const auto& p0 = frames.rows.front();
    expect(p0.at("nsf") == "2" && p0.at("callsite") == "vr_very_late_post_present", "per-frame Native Stereo Fix and callsite");
    expect(p0.at("frame") == "10" && p0.at("display_time") == "1000" && p0.at("period_ns") == "16666666",
        "per-frame engine frame, display time and period");
    expect(std::stoul(p0.at("flags")) == perf::flag::pack(perf::flag::D3D12 | perf::flag::HMD | perf::flag::DLSSG, 2, 0),
        "per-frame flags");
    expect(p0.at("presents") == "0" && frames.rows[1].at("presents") == "1", "per-frame counts are since the previous row");
    expect(p0.at("wait_us") == "2000" && p0.at("end_us") == "500", "per-frame xrWaitFrame and xrEndFrame time");
    expect(std::stoll(p0.at("rt_blocked_us")) == 0 && std::stoll(frames.rows[1].at("rt_blocked_us")) >= 2800 &&
        std::stoll(frames.rows[1].at("rt_blocked_us")) < 3500, "per-frame blocked time");
    expect(std::stoll(frames.rows[1].at("uevr_rt_us")) < 1000, "per-frame UEVR time without the blocked time");
    expect(frames.rows[3].at("kind") == "E" && frames.rows[3].at("nsf") == "0" && frames.rows[3].at("frame") == "0",
        "empty submits carry no engine frame or Native Stereo Fix state");
    expect(p0.at("nsf_pair") == "0" && sum(summary, "nsf_live") == 0 && sum(summary, "nsf_snapshot") == 0,
        "no eye-pair source without an eye-pair copy");

    // A new session keeps the previous one as *.prev.csv.
    expect(perf::start(dir) == 0, "writer restarts");
    perf::stop();
    expect(std::filesystem::exists(dir / "perf.prev.csv") && std::filesystem::exists(dir / "perf-frames.prev.csv"),
        "previous session rotated");
    expect(read_csv(dir / "perf.prev.csv").rows.size() == summary.rows.size(), "rotated file is the previous session");

    // 2x frame generation: every engine frame is presented twice, the second Present (no Tick before it) resubmitting
    // the previous engine frame. Only frames past the newest one submitted are new; a frame far behind it is a
    // restarted counter (a new OpenXR session), new again.
    expect(perf::start(dir) == 0, "writer restarts for the frame-generation pattern");
    frame(14, true, true, 9'000, true, perf::NativePair::Snapshot);
    frame(13, true, true, 9'000, false, perf::NativePair::Snapshot);
    frame(15, true, true, 10'000, true, perf::NativePair::Live);
    frame(14, true, true, 10'000, false, perf::NativePair::Snapshot);
    frame(15, true, true, 10'000, false);
    frame(100'000, true, true, 11'000, true);
    frame(7, true, true, 12'000, true);
    frame(8, true, true, 13'000, true);
    perf::stop();
    {
        const auto fg_summary = read_csv(dir / "perf.csv");
        const auto fg_frames = read_csv(dir / "perf-frames.csv");
        std::string fg_kinds;
        for (const auto& row : fg_frames.rows) { fg_kinds += row.at("kind"); }
        expect(fg_kinds == "PRPRRPPP", "frame generation's resubmits of older engine frames are repeats");
        expect(sum(fg_summary, "xr_new") == 5 && sum(fg_summary, "xr_repeat") == 3 && sum(fg_summary, "xr_older") == 2,
            "perf.csv counts older-frame resubmits apart from repeats of the newest frame");
        expect(sum(fg_summary, "presents") == 8 && sum(fg_summary, "ticks") == 5, "Presents without a Tick counted");
        bool ratio_ok = !fg_summary.rows.empty();
        for (const auto& row : fg_summary.rows) {
            const auto presents = std::stod(row.at("presents"));
            const auto ticks = std::stod(row.at("ticks")) + std::stod(row.at("ticks_skipped"));
            const auto& ratio = row.at("presents_per_tick");
            ratio_ok = ratio_ok && (ticks == 0 ? ratio.empty() : !ratio.empty() && std::abs(std::stod(ratio) - presents / ticks) < 0.006);
        }
        expect(ratio_ok, "presents_per_tick is presents / (ticks + ticks_skipped) of the row, empty without a Tick hook pass");
        expect(sum(fg_summary, "nsf_snapshot") == 3 && sum(fg_summary, "nsf_live") == 1, "eye-pair copies counted by source");
        expect(fg_frames.rows.size() == 8 && fg_frames.rows[0].at("nsf_pair") == "2" && fg_frames.rows[2].at("nsf_pair") == "1" &&
            fg_frames.rows[4].at("nsf_pair") == "0", "per-frame eye-pair source, consumed by its submit");
    }

    // Disabled: nothing is counted.
    perf::set_enabled(false);
    const auto before = perf::g_counters.xr_new.load();
    frame(20, true, true, 8'000);
    expect(perf::g_counters.xr_new.load() == before, "disabled counters stay still");

    std::filesystem::remove_all(dir);
    if (failures == 0) { std::puts("perf log tests passed"); }
    return failures == 0 ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
