// burstcap: bursts of consecutive frames of one display, for analysis\eyesampler.py --burst-every (an mss / BitBlt
// grab of the whole 3840x2160 Mopic display takes ~56 ms, so it sees every third refresh at best).
//
//   burstcap.exe --rect L,T,W,H [--scale 2] [--source auto|dupl|wgc]
//   burstcap.exe --list            every output, and whether Desktop Duplication works on it
//
// Two ways to see every frame the desktop composes for the display whose rectangle is L,T,W,H
// (gamepilot.mopic_monitor()):
//   dupl  DXGI Desktop Duplication. On a hybrid laptop it fails (DXGI_ERROR_UNSUPPORTED) for displays wired to the
//         integrated GPU while the process runs on the discrete one: on blade18 the Mopic display and the panel.
//   wgc   Windows.Graphics.Capture of the monitor (works across GPUs; no capture border where Windows lets it off).
// auto (default) takes dupl where it works, else wgc.
//
// Then one command per stdin line:
//   burst <frames> <out file>   the next <frames> frames with a new picture (pointer-only updates skipped), each scaled
//                               down by --scale (box filter), written to <out file>; answers
//                               "ok <frames grabbed> <refreshes skipped> <ms>" or "error <text>"
//   quit
//
// <out file>: "BRST", u32 version 1, u32 width, u32 height, u32 frames, then per frame i64 grab_qpc_ns (when the frame
// was taken), i64 present_qpc_ns (dupl: LastPresentTime; wgc: the frame's SystemRelativeTime), u32 refreshes (dupl:
// AccumulatedFrames; wgc: the time since the previous frame in display refreshes, rounded: more than 1 means one was
// missed or the picture didn't change), u32 0, and width * height * 3 bytes BGR, top row first. QPC ns are Python's
// time.perf_counter_ns(). The first frame of a burst only starts it (its picture may be old): it isn't written.
//
// Build: capture\build.cmd (cl from a Visual Studio x64 environment).

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <DirectXPackedVector.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

template <class T> struct Com {
    T* p{};
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

static int64_t g_qpc_freq = 0;

static int64_t qpc_to_ns(int64_t ticks) {
    return (ticks / g_qpc_freq) * 1000000000LL + (ticks % g_qpc_freq) * 1000000000LL / g_qpc_freq;
}

static int64_t now_ns() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return qpc_to_ns(t.QuadPart);
}

static std::string hr_text(const char* what, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s failed 0x%08lx", what, (unsigned long)hr);
    return buf;
}

struct Frame {
    int64_t grab_ns{}, present_ns{};
    uint32_t refreshes{};
    std::vector<uint8_t> bgr;
};

// An output in an advanced color (HDR) mode can only be duplicated as FP16 scRGB (1.0 = SDR white): every half value
// -> 8-bit sRGB, clamped at SDR white.
static uint8_t g_half_to_srgb[65536];

static void init_half_lut() {
    for (uint32_t h = 0; h < 65536; ++h) {
        float v = DirectX::PackedVector::XMConvertHalfToFloat(static_cast<DirectX::PackedVector::HALF>(h));
        if (!(v > 0.0f)) {
            v = 0.0f;   // negatives and NaN
        }
        v = std::min(v, 1.0f);
        const float s = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        g_half_to_srgb[h] = static_cast<uint8_t>(std::lround(s * 255.0f));
    }
}

// width x height BGRA bytes (or RGBA halves) -> (width / s) x (height / s) BGR bytes, box filtered, on 4 threads
static void scale_down(const uint8_t* src, UINT pitch, UINT width, UINT height, UINT s, bool fp16,
                       std::vector<uint8_t>& dst) {
    const UINT ow = width / s, oh = height / s;
    dst.resize(size_t(ow) * oh * 3);
    auto rows = [&](UINT y0, UINT y1) {
        std::vector<uint32_t> acc(size_t(ow) * 3);
        for (UINT y = y0; y < y1; ++y) {
            std::fill(acc.begin(), acc.end(), 0u);
            for (UINT dy = 0; dy < s; ++dy) {
                const uint8_t* row = src + size_t(y * s + dy) * pitch;
                for (UINT x = 0; x < ow; ++x) {
                    uint32_t* a = &acc[size_t(x) * 3];
                    if (fp16) {
                        const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + size_t(x) * s * 4;
                        for (UINT dx = 0; dx < s; ++dx, p += 4) {
                            a[0] += g_half_to_srgb[p[2]];
                            a[1] += g_half_to_srgb[p[1]];
                            a[2] += g_half_to_srgb[p[0]];
                        }
                    } else {
                        const uint8_t* p = row + size_t(x) * s * 4;
                        for (UINT dx = 0; dx < s; ++dx, p += 4) {
                            a[0] += p[0];
                            a[1] += p[1];
                            a[2] += p[2];
                        }
                    }
                }
            }
            uint8_t* out = &dst[size_t(y) * ow * 3];
            const uint32_t n = s * s;
            for (size_t i = 0; i < size_t(ow) * 3; ++i) {
                out[i] = static_cast<uint8_t>((acc[i] + n / 2) / n);
            }
        }
    };
    const UINT threads = 4;
    std::vector<std::thread> pool;
    for (UINT t = 0; t < threads; ++t) {
        pool.emplace_back(rows, oh * t / threads, oh * (t + 1) / threads);
    }
    for (auto& th : pool) {
        th.join();
    }
}

// Captured textures -> scaled down on the GPU (GenerateMips: each mip level a 2x2 average, so --scale's power-of-2
// part; any rest on the CPU) -> a ring of staging textures, read one frame behind, so frame n's copy overlaps frame
// n-1's readback (a full 3840x2160 readback and CPU scale per frame kept up with every other refresh only) -> BGR
struct Readback {
    static constexpr int RING = 4;
    ID3D11Device* dev{};
    ID3D11DeviceContext* ctx{};
    UINT scale = 2, level = 0, rest = 1;
    D3D11_TEXTURE2D_DESC src{};
    winrt::com_ptr<ID3D11Texture2D> mip;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    winrt::com_ptr<ID3D11Texture2D> staging[RING];
    int next = 0;
    std::deque<std::pair<int, Frame>> pending;

    void reset() {
        mip = nullptr;
        srv = nullptr;
        for (auto& s : staging) {
            s = nullptr;
        }
        pending.clear();
        src = {};
    }

    std::string setup(ID3D11Texture2D* tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (staging[0] && d.Width == src.Width && d.Height == src.Height && d.Format == src.Format) {
            return "";
        }
        reset();
        src = d;
        level = 0;
        while (scale % (2u << level) == 0) {
            ++level;
        }
        rest = scale >> level;
        HRESULT hr;
        if (level) {
            D3D11_TEXTURE2D_DESC m = d;
            m.MipLevels = level + 1;
            m.ArraySize = 1;
            m.SampleDesc = {1, 0};
            m.Usage = D3D11_USAGE_DEFAULT;
            m.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            m.CPUAccessFlags = 0;
            m.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(hr = dev->CreateTexture2D(&m, nullptr, mip.put()))) {
                return hr_text("CreateTexture2D (mips)", hr);
            }
            if (FAILED(hr = dev->CreateShaderResourceView(mip.get(), nullptr, srv.put()))) {
                return hr_text("CreateShaderResourceView", hr);
            }
        }
        D3D11_TEXTURE2D_DESC s = d;
        s.Width = d.Width >> level;
        s.Height = d.Height >> level;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.SampleDesc = {1, 0};
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        for (auto& st : staging) {
            if (FAILED(hr = dev->CreateTexture2D(&s, nullptr, st.put()))) {
                return hr_text("CreateTexture2D (staging)", hr);
            }
        }
        return "";
    }

    // Queues frame f's picture: tex is only valid until the source releases the frame
    std::string push(ID3D11Texture2D* tex, Frame&& f) {
        std::string err = setup(tex);
        if (!err.empty()) {
            return err;
        }
        const int slot = next;
        next = (next + 1) % RING;
        if (level) {
            ctx->CopySubresourceRegion(mip.get(), 0, 0, 0, 0, tex, 0, nullptr);
            ctx->GenerateMips(srv.get());
            ctx->CopySubresourceRegion(staging[slot].get(), 0, 0, 0, 0, mip.get(), level, nullptr);
        } else {
            ctx->CopyResource(staging[slot].get(), tex);
        }
        ctx->Flush();
        pending.emplace_back(slot, std::move(f));
        return "";
    }

    // Reads queued frames, oldest first, into out until `keep` are left queued
    std::string pop(std::vector<Frame>& out, size_t keep) {
        while (pending.size() > keep) {
            auto& [slot, f] = pending.front();
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT hr = ctx->Map(staging[slot].get(), 0, D3D11_MAP_READ, 0, &m);
            if (FAILED(hr)) {
                return hr_text("Map", hr);
            }
            scale_down(static_cast<const uint8_t*>(m.pData), m.RowPitch, src.Width >> level, src.Height >> level,
                       rest, src.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, f.bgr);
            ctx->Unmap(staging[slot].get(), 0);
            out.push_back(std::move(f));
            pending.pop_front();
        }
        return "";
    }
};

struct Source {
    RECT want{};
    UINT scale = 2, width = 0, height = 0;
    Readback rb;
    virtual ~Source() = default;
    virtual const char* name() const = 0;
    virtual std::string open() = 0;
    virtual void drain() {}
    // The next new picture: its times into f and, when keep, its picture queued in rb; false with err set on
    // failure, false with err empty on a timeout, true with f.grab_ns 0 for a pointer-only update. err "reopened"
    // after a lost source: the burst starts again.
    virtual bool next(Frame& f, bool keep, std::string& err, int timeout_ms) = 0;
    UINT out_w() const { return width / scale; }
    UINT out_h() const { return height / scale; }
};

// DuplicateOutput, or DuplicateOutput1 (8-bit or FP16) where the plain one is unsupported
static HRESULT duplicate(IDXGIOutput1* out, ID3D11Device* dev, IDXGIOutputDuplication** dup) {
    HRESULT hr = out->DuplicateOutput(dev, dup);
    if (hr != DXGI_ERROR_UNSUPPORTED) {
        return hr;
    }
    Com<IDXGIOutput5> out5;
    if (FAILED(out->QueryInterface(__uuidof(IDXGIOutput5), (void**)out5.put()))) {
        return hr;
    }
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
    return out5->DuplicateOutput1(dev, 0, 2, formats, dup);
}

struct DuplSource : Source {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<IDXGIOutputDuplication> dupl;

    const char* name() const override { return "dupl"; }

    std::string open() override {
        dupl = nullptr;
        rb.reset();
        context = nullptr;
        device = nullptr;
        winrt::com_ptr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
        if (FAILED(hr)) {
            return hr_text("CreateDXGIFactory1", hr);
        }
        winrt::com_ptr<IDXGIAdapter1> adapter;
        winrt::com_ptr<IDXGIOutput1> output;
        for (UINT a = 0; !output; ++a) {
            winrt::com_ptr<IDXGIAdapter1> ad;
            if (factory->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            for (UINT o = 0;; ++o) {
                winrt::com_ptr<IDXGIOutput> out;
                if (ad->EnumOutputs(o, out.put()) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                DXGI_OUTPUT_DESC desc{};
                out->GetDesc(&desc);
                if (std::memcmp(&desc.DesktopCoordinates, &want, sizeof(RECT)) == 0) {
                    output = out.as<IDXGIOutput1>();
                    adapter = ad;
                    break;
                }
            }
        }
        if (!output) {
            return "no output at that rect";
        }
        D3D_FEATURE_LEVEL fl{};
        hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                               device.put(), &fl, context.put());
        if (FAILED(hr)) {
            return hr_text("D3D11CreateDevice", hr);
        }
        hr = duplicate(output.get(), device.get(), dupl.put());
        if (FAILED(hr)) {
            return hr_text("DuplicateOutput", hr);
        }
        DXGI_OUTDUPL_DESC dd{};
        dupl->GetDesc(&dd);
        width = dd.ModeDesc.Width;
        height = dd.ModeDesc.Height;
        rb.dev = device.get();
        rb.ctx = context.get();
        rb.scale = scale;
        return "";
    }

    bool next(Frame& f, bool keep, std::string& err, int timeout_ms) override {
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> res;
        HRESULT hr = dupl->AcquireNextFrame(timeout_ms, &info, res.put());
        err.clear();
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return false;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            err = open();
            if (err.empty()) {
                err = "reopened";
            }
            return false;
        }
        if (FAILED(hr)) {
            err = hr_text("AcquireNextFrame", hr);
            return false;
        }
        f.grab_ns = now_ns();
        if (info.LastPresentTime.QuadPart == 0) {   // only the pointer moved
            dupl->ReleaseFrame();
            f.grab_ns = 0;
            return true;
        }
        f.present_ns = qpc_to_ns(info.LastPresentTime.QuadPart);
        f.refreshes = info.AccumulatedFrames;
        if (keep) {
            Frame copy = f;
            err = rb.push(res.as<ID3D11Texture2D>().get(), std::move(copy));
        }
        dupl->ReleaseFrame();
        return err.empty();
    }
};

struct WgcSource : Source {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::handle arrived{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    int64_t last_ns = 0;
    double refresh_ns = 1e9 / 60.0;
    bool border_off = false;

    const char* name() const override { return border_off ? "wgc" : "wgc (with border)"; }

    ~WgcSource() override {
        if (session) {
            session.Close();
        }
        if (pool) {
            pool.Close();
        }
    }

    std::string open() override {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
        if (FAILED(hr)) {
            return hr_text("D3D11CreateDevice", hr);
        }
        winrt::com_ptr<::IInspectable> insp;
        hr = CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), insp.put());
        if (FAILED(hr)) {
            return hr_text("CreateDirect3D11DeviceFromDXGIDevice", hr);
        }
        auto rtdevice = insp.as<wdx::Direct3D11::IDirect3DDevice>();
        HMONITOR mon = MonitorFromPoint({want.left + 1, want.top + 1}, MONITOR_DEFAULTTONULL);
        if (!mon) {
            return "no monitor at that rect";
        }
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
            refresh_ns = 1e9 / dm.dmDisplayFrequency;
        }
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        hr = interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item));
        if (FAILED(hr)) {
            return hr_text("CreateForMonitor", hr);
        }
        const auto size = item.Size();
        width = size.Width;
        height = size.Height;
        rb.dev = device.get();
        rb.ctx = context.get();
        rb.scale = scale;
        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            rtdevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 4, size);
        pool.FrameArrived([this](auto&&, auto&&) { SetEvent(arrived.get()); });
        session = pool.CreateCaptureSession(item);
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) {
            session.IsCursorCaptureEnabled(false);
        }
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
            try {
                session.IsBorderRequired(false);
                border_off = true;
            } catch (...) {
            }
        }
        session.StartCapture();
        return "";
    }

    void drain() override {
        while (auto f = pool.TryGetNextFrame()) {
            f.Close();
        }
        last_ns = 0;
    }

    bool next(Frame& f, bool keep, std::string& err, int timeout_ms) override {
        err.clear();
        auto frame = pool.TryGetNextFrame();
        if (!frame) {
            if (WaitForSingleObject(arrived.get(), timeout_ms) != WAIT_OBJECT_0) {
                return false;
            }
            frame = pool.TryGetNextFrame();
            if (!frame) {
                return false;
            }
        }
        f.grab_ns = now_ns();
        f.present_ns = frame.SystemRelativeTime().count() * 100;   // QPC in 100 ns units
        f.refreshes = last_ns ? (uint32_t)std::max<int64_t>(1, std::llround((f.present_ns - last_ns) / refresh_ns)) : 1;
        last_ns = f.present_ns;
        if (keep) {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
            if (FAILED(hr)) {
                err = hr_text("GetInterface (texture)", hr);
            } else {
                Frame copy = f;
                err = rb.push(tex.get(), std::move(copy));
            }
        }
        frame.Close();
        return err.empty();
    }
};

static std::string burst(Source& src, int frames, const std::string& path) {
    const int64_t t0 = now_ns();
    std::vector<Frame> got;
    got.reserve(frames);
    std::string err;
    // the first new picture only starts the burst: the source wasn't read since the last burst, so it may be old
    src.drain();
    src.rb.pending.clear();
    bool started = false;
    int kept = 0;
    int64_t idle_since = now_ns();
    for (int restarts = 0; kept < frames;) {
        Frame f;
        if (!src.next(f, started, err, 100)) {
            if (!err.empty()) {
                if (err != "reopened" || ++restarts > 3) {
                    return "error " + err;
                }
                got.clear();
                kept = 0;
                started = false;
            } else if (now_ns() - idle_since > 3000000000LL) {
                break;   // the display hasn't changed for 3 s: write what there is
            }
            continue;
        }
        if (f.grab_ns == 0) {
            continue;   // pointer only
        }
        idle_since = f.grab_ns;
        if (!started) {
            started = true;
            continue;
        }
        ++kept;
        err = src.rb.pop(got, 1);   // the frame before this one
        if (!err.empty()) {
            return "error " + err;
        }
    }
    err = src.rb.pop(got, 0);
    if (!err.empty()) {
        return "error " + err;
    }
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) {
        return "error cannot write " + path;
    }
    const uint32_t head[4] = {1, src.out_w(), src.out_h(), (uint32_t)got.size()};
    std::fwrite("BRST", 1, 4, fp);
    std::fwrite(head, sizeof(head), 1, fp);
    uint32_t skipped = 0;
    for (const Frame& f : got) {
        const uint32_t r[2] = {f.refreshes, 0};
        std::fwrite(&f.grab_ns, 8, 1, fp);
        std::fwrite(&f.present_ns, 8, 1, fp);
        std::fwrite(r, sizeof(r), 1, fp);
        std::fwrite(f.bgr.data(), 1, f.bgr.size(), fp);
        if (f.refreshes > 1) {
            skipped += f.refreshes - 1;
        }
    }
    std::fclose(fp);
    std::ostringstream ok;
    ok << "ok " << got.size() << " " << skipped << " " << (now_ns() - t0) / 1000000;
    return ok.str();
}

// --list: every adapter's outputs, and whether a device on each adapter can duplicate each output
static int list_outputs() {
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
        return 1;
    }
    std::vector<winrt::com_ptr<IDXGIAdapter1>> adapters;
    for (UINT a = 0;; ++a) {
        winrt::com_ptr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        adapters.push_back(ad);
    }
    for (size_t a = 0; a < adapters.size(); ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapters[a]->GetDesc1(&ad);
        std::wprintf(L"adapter %zu: %s\n", a, ad.Description);
        for (UINT o = 0;; ++o) {
            winrt::com_ptr<IDXGIOutput> out;
            if (adapters[a]->EnumOutputs(o, out.put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            const RECT& r = od.DesktopCoordinates;
            std::wprintf(L"  output %u: %s rect %ld,%ld,%ld,%ld\n", o, od.DeviceName, r.left, r.top, r.right - r.left,
                         r.bottom - r.top);
            auto out1 = out.as<IDXGIOutput1>();
            for (size_t b = 0; b < adapters.size(); ++b) {
                winrt::com_ptr<ID3D11Device> dev;
                winrt::com_ptr<ID3D11DeviceContext> ctx;
                HRESULT hr = D3D11CreateDevice(adapters[b].get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
                winrt::com_ptr<IDXGIOutputDuplication> dup;
                if (SUCCEEDED(hr)) {
                    hr = duplicate(out1.get(), dev.get(), dup.put());
                }
                DXGI_OUTDUPL_DESC dd{};
                if (dup) {
                    dup->GetDesc(&dd);
                }
                std::wprintf(L"    device on adapter %zu: 0x%08lx%s\n", b, (unsigned long)hr,
                             dd.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? L" (FP16)" : L"");
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_freq = freq.QuadPart;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // physical px; DuplicateOutput1 needs it
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    init_half_lut();
    if (argc == 2 && std::string(argv[1]) == "--list") {
        return list_outputs();
    }
    RECT want{};
    bool have_rect = false;
    UINT scale = 2;
    std::string which = "auto";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--rect") {
            long l, t, w, h;
            if (sscanf_s(argv[i + 1], "%ld,%ld,%ld,%ld", &l, &t, &w, &h) == 4) {
                want = {l, t, l + w, t + h};
                have_rect = true;
            }
        } else if (k == "--scale") {
            scale = (UINT)std::max(1, std::atoi(argv[i + 1]));
        } else if (k == "--source") {
            which = argv[i + 1];
        }
    }
    if (!have_rect) {
        std::printf("error usage: burstcap.exe --rect L,T,W,H [--scale 2] [--source auto|dupl|wgc] | --list\n");
        return 2;
    }
    std::unique_ptr<Source> src;
    std::string err;
    for (const char* kind : {"dupl", "wgc"}) {
        if (which != "auto" && which != kind) {
            continue;
        }
        std::unique_ptr<Source> s;
        if (std::strcmp(kind, "dupl") == 0) {
            s = std::make_unique<DuplSource>();
        } else {
            s = std::make_unique<WgcSource>();
        }
        s->want = want;
        s->scale = scale;
        try {
            err = s->open();
        } catch (const winrt::hresult_error& e) {
            err = hr_text(kind, e.code());
        }
        if (err.empty()) {
            src = std::move(s);
            break;
        }
        std::fprintf(stderr, "%s: %s\n", kind, err.c_str());
    }
    if (!src) {
        std::printf("error %s\n", err.c_str());
        return 1;
    }
    std::printf("ready %s %u %u\n", src->name(), src->out_w(), src->out_h());
    std::fflush(stdout);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) {
            line.erase(0, 3);   // PowerShell pipes a BOM first
        }
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "quit") {
            break;
        }
        if (cmd == "burst") {
            int frames = 0;
            in >> frames;
            std::string path;
            std::getline(in >> std::ws, path);
            std::string res;
            try {
                res = frames > 0 && !path.empty() ? burst(*src, frames, path) : "error usage: burst <frames> <out file>";
            } catch (const winrt::hresult_error& e) {
                res = "error " + hr_text("capture", e.code());
            }
            std::printf("%s\n", res.c_str());
        } else {
            std::printf("error unknown command\n");
        }
        std::fflush(stdout);
    }
    return 0;
}
