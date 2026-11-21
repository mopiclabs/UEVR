#pragma once

// EXPERIMENT (VR_ExperimentPassMarkersSeconds = N, default 0 = off): per-pass timing of both Native Stereo Fix
// families, without addresses of a game build. Every N seconds, kCaptureFrames consecutive presents' worth of records
// go to <persistent dir>/passmarkers/capture-<k>.bin, and their GPU timestamps to capture-<k>.gpu, for offline
// analysis (patches/right-eye-cost/tools/passmarkers.py).
//
// Sources:
// - UE's D3D12 RHI (4.26+) hands every draw event to NVIDIA Aftermath when GPU crash debugging is on
//   (GFSDK_Aftermath_SetEventMarker, an export of GFSDK_Aftermath_Lib.x64.dll). Hogwarts Legacy ships with draw
//   events off (2026-11-08); the hook stays for titles that emit them.
// - D3D12 command lists: SetPipelineState (through D3D12Hook), OMSetRenderTargets, Close, draws and dispatches (vtable
//   pointer swaps on the first list seen). The time from one record to the next on a thread goes to the targets that
//   thread's last OMSetRenderTargets bound, which splits the RHI thread's translation time by pass.
// - GPU: while capturing, every SetPipelineState / OMSetRenderTargets / Close record of a direct or compute list also
//   writes a timestamp query (EndQuery) into the list; Close resolves the list's queries into a readback buffer, read
//   30 presents after the capture. The GPU time of a pass = its record's timestamp to the next one in the same list.
// - PreRenderViewFamily_RenderThread: where each family's render starts on the render thread.
//
// File: "UEVRPM02", int64 QPC frequency, then records: int64 qpc, uint32 tid, uint32 kind, uint64 context, uint32 size,
// size bytes of payload (capped at kMaxPayload). Kinds: 0 Aftermath marker (payload as given), 1 present,
// 2 SetPipelineState, 3 OMSetRenderTargets, 4 Close (context = the command list), 6 family render (context = the
// family's render target; payload frame count). Kinds 2-4 start with 14 words: calls[6] and QPC ticks inside the calls
// [6] since the thread's last record, per class (draw, ExecuteIndirect, dispatch, ray tracing build/dispatch, barrier,
// copy), then the timestamp query index (0xffffffff = none) and the list type. Kind 2 then has the pipeline state
// lo/hi; kind 3 RTV count, RTV0 handle lo/hi, DSV handle lo/hi, RTV0 width/height/format, DSV width/height/format
// (sizes 0 for views created before the hooks).
// GPU file: "UEVRPG01", uint64 timestamp frequency, uint64 calibration GPU timestamp, uint64 calibration QPC (the game
// queue's GetClockCalibration at the capture's end), uint32 count, then count uint64 timestamps by query index (0 for
// queries never resolved).

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
#include <utility/PointerHook.hpp>

namespace uevr::pass_markers {
namespace detail {
constexpr uint32_t kCaptureFrames = 60;
constexpr uint32_t kMaxPayload = 256;
constexpr size_t kMaxBytes = 128ull * 1024 * 1024;
// UE5 records hundreds of small lists per frame (Wukong: 1024-query blocks ran out after ~256 lists, 2026-11-21)
constexpr uint32_t kQueryCapacity = 1u << 20;
constexpr uint32_t kQueryBlock = 64;
constexpr uint32_t kNoQuery = 0xffffffffu;
constexpr uint32_t kReadbackDelayPresents = 30;

struct State {
    std::mutex mtx{};
    std::vector<uint8_t> buffer{};
    std::atomic<bool> capturing{false};
    std::atomic<bool> hooked{false};
    bool tried{false};
    uint32_t frames{0};
    uint32_t captures{0};
    uint64_t records{0};
    std::chrono::steady_clock::time_point last_end{std::chrono::steady_clock::now()};
    safetyhook::InlineHook hook{};
    std::filesystem::path dir{};
};

inline State& state() {
    static State s{};
    return s;
}

struct ListQueries {
    uint32_t start{kNoQuery};
    uint32_t used{};
    std::vector<std::pair<uint32_t, uint32_t>> ranges{};
};

struct Gpu {
    std::mutex mtx{};
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap{};
    Microsoft::WRL::ComPtr<ID3D12Resource> readback{};
    std::atomic<bool> ready{false};    // heap and readback exist
    std::atomic<bool> active{false};   // queries are taken (while capturing)
    bool create_failed{false};
    uint32_t next{0};
    uint64_t exhausted{0};
    std::unordered_map<void*, ListQueries> lists{};
    // the pending readback (present thread)
    uint32_t read_in{0};
    uint32_t read_count{0};
    std::filesystem::path read_path{};
    uint64_t frequency{};
    uint64_t calib_gpu{};
    uint64_t calib_cpu{};
};

inline Gpu& gpu() {
    static Gpu g{};
    return g;
}

inline void append(int64_t qpc, uint32_t tid, uint32_t kind, uint64_t context, const void* data, uint32_t size) {
    auto& s = state();
    size = size > kMaxPayload ? kMaxPayload : size;
    std::scoped_lock _{s.mtx};
    if (s.buffer.size() + 28 + size > kMaxBytes) {
        return;
    }
    const auto at = s.buffer.size();
    s.buffer.resize(at + 28 + size);
    auto* p = s.buffer.data() + at;
    std::memcpy(p, &qpc, 8);
    std::memcpy(p + 8, &tid, 4);
    std::memcpy(p + 12, &kind, 4);
    std::memcpy(p + 16, &context, 8);
    std::memcpy(p + 24, &size, 4);
    if (size != 0 && data != nullptr) {
        std::memcpy(p + 28, data, size);
    }
    ++s.records;
}

inline int32_t set_event_marker(void* context, const void* data, uint32_t size) {
    auto& s = state();
    if (s.capturing.load(std::memory_order_acquire)) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        append(qpc.QuadPart, GetCurrentThreadId(), 0, (uint64_t)context, data, size);
    }
    return s.hook.call<int32_t>(context, data, size);
}

inline void try_hook_aftermath() {
    auto& s = state();
    const auto module = GetModuleHandleW(L"GFSDK_Aftermath_Lib.x64.dll");
    if (module == nullptr) {
        spdlog::info("[PassMarkers] GFSDK_Aftermath_Lib.x64.dll is not loaded: no engine event markers");
        return;
    }
    const auto fn = GetProcAddress(module, "GFSDK_Aftermath_SetEventMarker");
    if (fn == nullptr) {
        spdlog::info("[PassMarkers] GFSDK_Aftermath_Lib.x64.dll has no GFSDK_Aftermath_SetEventMarker");
        return;
    }
    s.hook = safetyhook::create_inline((void*)fn, (void*)&set_event_marker);
    s.hooked = (bool)s.hook;
    spdlog::info("[PassMarkers] Hooked GFSDK_Aftermath_SetEventMarker ({})", s.hooked ? "ok" : "failed");
}

// --- GPU timestamps ---
// Called with the list's recording thread; takes the next query of the list's block and writes it into the list.
inline uint32_t take_query(ID3D12GraphicsCommandList* list, uint32_t& list_type) {
    auto& g = gpu();
    list_type = (uint32_t)list->GetType();
    if (!g.active.load(std::memory_order_acquire) ||
        (list_type != D3D12_COMMAND_LIST_TYPE_DIRECT && list_type != D3D12_COMMAND_LIST_TYPE_COMPUTE))
    {
        return kNoQuery;
    }
    std::scoped_lock _{g.mtx};
    if (!g.ready.load(std::memory_order_relaxed)) {
        if (g.create_failed) {
            return kNoQuery;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> device{};
        D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kQueryCapacity, 0};
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = (UINT64)kQueryCapacity * 8;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) ||
            FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&g.heap))) ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&g.readback))))
        {
            g.create_failed = true;
            g.heap.Reset();
            g.readback.Reset();
            spdlog::warn("[PassMarkers] Creating the timestamp query heap failed: no GPU times");
            return kNoQuery;
        }
        g.ready.store(true, std::memory_order_release);
        spdlog::info("[PassMarkers] Timestamp query heap: {} queries", kQueryCapacity);
    }
    auto& lq = g.lists[(void*)list];
    if (lq.start == kNoQuery || lq.used == kQueryBlock) {
        if (lq.start != kNoQuery) {
            lq.ranges.emplace_back(lq.start, lq.used);
        }
        lq.start = kNoQuery;
        lq.used = 0;
        if (g.next + kQueryBlock > kQueryCapacity) {
            ++g.exhausted;
            return kNoQuery;
        }
        lq.start = g.next;
        g.next += kQueryBlock;
    }
    const auto index = lq.start + lq.used++;
    list->EndQuery(g.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    return index;
}

// Before the list's Close: resolves its queries into the readback buffer (at their indices).
inline void resolve_queries(ID3D12GraphicsCommandList* list) {
    auto& g = gpu();
    if (!g.ready.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock _{g.mtx};
    const auto it = g.lists.find((void*)list);
    if (it == g.lists.end()) {
        return;
    }
    auto& lq = it->second;
    if (lq.start != kNoQuery && lq.used != 0) {
        lq.ranges.emplace_back(lq.start, lq.used);
    }
    for (const auto& [start, count] : lq.ranges) {
        list->ResolveQueryData(g.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, start, count, g.readback.Get(), (UINT64)start * 8);
    }
    g.lists.erase(it);
}

// --- command list records ---
// Per thread, since its last record: calls and QPC ticks spent inside them, per class. Every record's payload starts
// with these 12 words (calls[6], ticks[6]), then the query index and the list type.
enum CallClass : uint32_t { kDraw = 0, kIndirect, kDispatch, kRayTracing, kBarrier, kCopy, kClassCount };
constexpr size_t kClose = 9;
constexpr size_t kOMSetRenderTargets = 46;
constexpr size_t kMaxSlot = 80;

struct TargetInfo {
    uint32_t width{};
    uint32_t height{};
    uint32_t format{};
};

struct ListHooks {
    std::atomic<bool> enabled{false};
    std::atomic<bool> installed{false};
    std::mutex install_mtx{};
    std::vector<std::unique_ptr<PointerHook>> hooks{};
    // Read from the vtable before the swaps: another thread may call a swapped slot before the PointerHook is stored.
    std::atomic<void*> original[kMaxSlot]{};
    std::shared_mutex targets_mtx{};
    std::unordered_map<uint64_t, TargetInfo> targets{};
};

inline ListHooks& list_hooks() {
    static ListHooks h{};
    return h;
}

inline thread_local uint32_t t_calls[kClassCount]{};
inline thread_local uint32_t t_ticks[kClassCount]{};

inline void record_list(uint32_t kind, ID3D12GraphicsCommandList* list, const uint32_t* extra, uint32_t extra_count) {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    uint32_t list_type{};
    const auto query = take_query(list, list_type);
    uint32_t words[2 * kClassCount + 2 + 16]{};
    std::memcpy(words, t_calls, sizeof(t_calls));
    std::memcpy(words + kClassCount, t_ticks, sizeof(t_ticks));
    words[2 * kClassCount] = query;
    words[2 * kClassCount + 1] = list_type;
    if (extra_count > 16) {
        extra_count = 16;
    }
    if (extra_count != 0) {
        std::memcpy(words + 2 * kClassCount + 2, extra, extra_count * 4);
    }
    append(qpc.QuadPart, GetCurrentThreadId(), kind, (uint64_t)list, words, (2 * kClassCount + 2 + extra_count) * 4);
    std::memset(t_calls, 0, sizeof(t_calls));
    std::memset(t_ticks, 0, sizeof(t_ticks));
}

// A vtable slot detour that times the original call and counts it under Class.
template <size_t Slot, uint32_t Class, typename... Args>
void STDMETHODCALLTYPE timed_call(ID3D12GraphicsCommandList* list, Args... args) {
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, Args...);
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    ((Fn)list_hooks().original[Slot].load(std::memory_order_acquire))(list, args...);
    QueryPerformanceCounter(&after);
    ++t_calls[Class];
    t_ticks[Class] += (uint32_t)(after.QuadPart - before.QuadPart);
}

inline TargetInfo find_target(uint64_t handle) {
    auto& h = list_hooks();
    std::shared_lock _{h.targets_mtx};
    if (const auto it = h.targets.find(handle); it != h.targets.end()) {
        return it->second;
    }
    return {};
}

inline void STDMETHODCALLTYPE om_set_render_targets(ID3D12GraphicsCommandList* list, UINT count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    if (state().capturing.load(std::memory_order_acquire)) {
        const uint64_t rtv0 = (count != 0 && rtvs != nullptr) ? rtvs[0].ptr : 0;
        const uint64_t dsv0 = dsv != nullptr ? dsv->ptr : 0;
        const auto rt_info = rtv0 != 0 ? find_target(rtv0) : TargetInfo{};
        const auto ds_info = dsv0 != 0 ? find_target(dsv0) : TargetInfo{};
        const uint32_t words[]{count, (uint32_t)rtv0, (uint32_t)(rtv0 >> 32), (uint32_t)dsv0, (uint32_t)(dsv0 >> 32),
            rt_info.width, rt_info.height, rt_info.format, ds_info.width, ds_info.height, ds_info.format};
        record_list(3, list, words, (uint32_t)std::size(words));
    }
    using Fn = decltype(&om_set_render_targets);
    ((Fn)list_hooks().original[kOMSetRenderTargets].load(std::memory_order_acquire))(list, count, rtvs, single_range,
        dsv);
}

inline HRESULT STDMETHODCALLTYPE close_list(ID3D12GraphicsCommandList* list) {
    if (state().capturing.load(std::memory_order_acquire)) {
        record_list(4, list, nullptr, 0);
    }
    resolve_queries(list);
    using Fn = decltype(&close_list);
    return ((Fn)list_hooks().original[kClose].load(std::memory_order_acquire))(list);
}

inline void hook_slot(void** vtable, size_t slot, void* detour) {
    auto& h = list_hooks();
    h.original[slot].store(vtable[slot], std::memory_order_release);
    h.hooks.push_back(std::make_unique<PointerHook>(&vtable[slot], detour));
}

inline void write_capture(std::filesystem::path path, std::vector<uint8_t> bytes) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    std::ofstream out{path, std::ios::binary};
    out.write("UEVRPM02", 8);
    const int64_t f = frequency.QuadPart;
    out.write((const char*)&f, 8);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

// Present thread, kReadbackDelayPresents after a capture: its timestamps to capture-<k>.gpu.
inline void read_timestamps() {
    auto& g = gpu();
    std::vector<uint64_t> values(g.read_count);
    {
        std::scoped_lock _{g.mtx};
        void* mapped{};
        D3D12_RANGE range{0, (SIZE_T)g.read_count * 8};
        if (g.read_count == 0 || FAILED(g.readback->Map(0, &range, &mapped))) {
            return;
        }
        std::memcpy(values.data(), mapped, (size_t)g.read_count * 8);
        D3D12_RANGE none{0, 0};
        g.readback->Unmap(0, &none);
    }
    std::ofstream out{g.read_path, std::ios::binary};
    out.write("UEVRPG01", 8);
    out.write((const char*)&g.frequency, 8);
    out.write((const char*)&g.calib_gpu, 8);
    out.write((const char*)&g.calib_cpu, 8);
    out.write((const char*)&g.read_count, 4);
    out.write((const char*)values.data(), (std::streamsize)values.size() * 8);
}

// Present thread, before a capture: index 0 again, every slot zeroed (queries never resolved then read 0).
inline void reset_queries() {
    auto& g = gpu();
    std::scoped_lock _{g.mtx};
    g.lists.clear();
    g.next = 0;
    if (g.ready.load(std::memory_order_relaxed)) {
        void* mapped{};
        D3D12_RANGE none{0, 0};
        if (SUCCEEDED(g.readback->Map(0, &none, &mapped))) {
            std::memset(mapped, 0, (size_t)kQueryCapacity * 8);
            D3D12_RANGE written{0, (SIZE_T)kQueryCapacity * 8};
            g.readback->Unmap(0, &written);
        }
    }
}
} // namespace detail

// D3D12Hook::create_render_target_view / create_depth_stencil_view: remember each view's size and format.
inline void on_create_view(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t view_format) {
    auto& h = detail::list_hooks();
    if (!h.enabled.load(std::memory_order_acquire) || resource == nullptr) {
        return;
    }
    const auto desc = resource->GetDesc();
    std::unique_lock _{h.targets_mtx};
    h.targets[handle.ptr] = detail::TargetInfo{(uint32_t)desc.Width, desc.Height,
        view_format != 0 ? view_format : (uint32_t)desc.Format};
}

// D3D12Hook::set_pipeline_state, before the original: hooks the list's draws, dispatches, render target binds and Close
// once, and records the PSO change while capturing.
inline void on_set_pipeline_state(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline_state) {
    using namespace detail;
    auto& h = list_hooks();
    if (!h.enabled.load(std::memory_order_acquire) || list == nullptr) {
        return;
    }
    if (!h.installed.load(std::memory_order_acquire)) {
        std::scoped_lock _{h.install_mtx};
        if (!h.installed.load(std::memory_order_relaxed)) {
            auto** vtable = *(void***)list;
            hook_slot(vtable, kClose, (void*)&close_list);
            hook_slot(vtable, 12, (void*)&timed_call<12, kDraw, UINT, UINT, UINT, UINT>);
            hook_slot(vtable, 13, (void*)&timed_call<13, kDraw, UINT, UINT, UINT, INT, UINT>);
            hook_slot(vtable, 14, (void*)&timed_call<14, kDispatch, UINT, UINT, UINT>);
            hook_slot(vtable, 15, (void*)&timed_call<15, kCopy, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64,
                UINT64>);
            hook_slot(vtable, 16, (void*)&timed_call<16, kCopy, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*>);
            hook_slot(vtable, 17, (void*)&timed_call<17, kCopy, ID3D12Resource*, ID3D12Resource*>);
            hook_slot(vtable, 26, (void*)&timed_call<26, kBarrier, UINT, const D3D12_RESOURCE_BARRIER*>);
            hook_slot(vtable, 59, (void*)&timed_call<59, kIndirect, ID3D12CommandSignature*, UINT, ID3D12Resource*,
                UINT64, ID3D12Resource*, UINT64>);
            hook_slot(vtable, kOMSetRenderTargets, (void*)&om_set_render_targets);
            ID3D12GraphicsCommandList4* list4{};
            const auto has_rt = SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))) && list4 != nullptr;
            if (has_rt) {
                list4->Release();
                hook_slot(vtable, 72, (void*)&timed_call<72, kRayTracing,
                    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT,
                    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*>);
                hook_slot(vtable, 76, (void*)&timed_call<76, kRayTracing, const D3D12_DISPATCH_RAYS_DESC*>);
            }
            h.installed.store(true, std::memory_order_release);
            spdlog::info("[PassMarkers] Hooked command list vtable {:x}: Close, draws, ExecuteIndirect, dispatches, copies, "
                "barriers, OMSetRenderTargets{}", (uintptr_t)vtable, has_rt ? ", ray tracing builds and dispatches" : "");
        }
    }
    if (state().capturing.load(std::memory_order_acquire)) {
        const auto pso = (uint64_t)pipeline_state;
        const uint32_t words[]{(uint32_t)pso, (uint32_t)(pso >> 32)};
        record_list(2, list, words, 2);
    }
}

// PreRenderViewFamily_RenderThread: where each family's render starts on the render thread.
inline void on_family_render(uint32_t frame_count, uintptr_t render_target) {
    using namespace detail;
    if (!state().capturing.load(std::memory_order_acquire)) {
        return;
    }
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    append(qpc.QuadPart, GetCurrentThreadId(), 6, render_target, &frame_count, 4);
}

// On the presenting thread, every Present. persistent_dir: Framework::get_persistent_dir(); queue: the game's command
// queue (timestamp frequency and calibration), may be null.
inline void on_present(int32_t seconds, const std::filesystem::path& persistent_dir, ID3D12CommandQueue* queue) {
    using namespace detail;
    auto& s = state();
    auto& g = gpu();
    if (seconds <= 0) {
        return;
    }
    list_hooks().enabled.store(true, std::memory_order_release);
    if (!s.tried) {
        s.tried = true;
        try_hook_aftermath();
        s.dir = persistent_dir / "passmarkers";
        std::error_code ec{};
        std::filesystem::create_directories(s.dir, ec);
    }

    if (g.read_in != 0 && --g.read_in == 0) {
        read_timestamps();
    }

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);

    if (s.capturing.load(std::memory_order_acquire)) {
        append(qpc.QuadPart, GetCurrentThreadId(), 1, 0, nullptr, 0);
        if (++s.frames < kCaptureFrames) {
            return;
        }
        s.capturing.store(false, std::memory_order_release);
        g.active.store(false, std::memory_order_release);
        std::vector<uint8_t> bytes{};
        uint64_t records{};
        {
            std::scoped_lock _{s.mtx};
            bytes.swap(s.buffer);
            records = s.records;
            s.records = 0;
        }
        ++s.captures;
        const auto path = s.dir / ("capture-" + std::to_string(s.captures) + ".bin");
        uint32_t queries{};
        uint64_t exhausted{};
        {
            std::scoped_lock _{g.mtx};
            queries = g.next;
            exhausted = g.exhausted;
        }
        g.frequency = 0;
        g.calib_gpu = 0;
        g.calib_cpu = 0;
        if (queue != nullptr) {
            queue->GetTimestampFrequency(&g.frequency);
            queue->GetClockCalibration(&g.calib_gpu, &g.calib_cpu);
        }
        if (g.ready.load(std::memory_order_acquire) && queries != 0) {
            g.read_count = queries;
            g.read_path = s.dir / ("capture-" + std::to_string(s.captures) + ".gpu");
            g.read_in = kReadbackDelayPresents;
        }
        spdlog::info("[PassMarkers] Capture {}: {} presents, {} records, {} bytes, {} query slots (blocks out: {}) -> {}",
            s.captures, s.frames, records, bytes.size(), queries, exhausted, path.string());
        std::thread{write_capture, path, std::move(bytes)}.detach();
        s.last_end = std::chrono::steady_clock::now();
        return;
    }

    if (g.read_in == 0 && std::chrono::steady_clock::now() - s.last_end >= std::chrono::seconds(seconds)) {
        {
            std::scoped_lock _{s.mtx};
            s.buffer.clear();
            s.buffer.reserve(32ull * 1024 * 1024);
            s.records = 0;
        }
        reset_queries();
        s.frames = 0;
        append(qpc.QuadPart, GetCurrentThreadId(), 1, 0, nullptr, 0);
        g.active.store(true, std::memory_order_release);
        s.capturing.store(true, std::memory_order_release);
    }
}
} // namespace uevr::pass_markers
