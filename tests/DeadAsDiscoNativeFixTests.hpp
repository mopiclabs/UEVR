#pragma once

#include "HalloweenRenderTargetsTests.hpp"
#include "mods/vr/DeadAsDiscoNativeFix.hpp"

void test_deadasdisco_native_family() {
    namespace d = uevr::deadasdisco_native;
    using uevr::games::is_deadasdisco_ue574_dx12_runtime;
    expect(is_deadasdisco_ue574_dx12_runtime(
        L"C:\\Steam\\steamapps\\common\\Dead as Disco Demo\\Pagoda\\Binaries\\Win64\\PagodaSteamDemo-Win64-Shipping.exe",
        0x50007, 0x40000, true), "Dead as Disco exact 5.7.4 DX12 gate");
    expect(!is_deadasdisco_ue574_dx12_runtime(L"Other-Win64-Shipping.exe", 0x50007, 0x40000, true), "other UE5.7.4 games unchanged");
    expect(!is_deadasdisco_ue574_dx12_runtime(L"PagodaSteamDemo-Win64-Shipping.exe.bak", 0x50007, 0x40000, true),
        "Dead as Disco basename lookalike rejected");
    expect(!is_deadasdisco_ue574_dx12_runtime(L"PagodaSteamDemo-Win64-Shipping.exe", 0x50007, 0x40000, false),
        "Dead as Disco DX11 unchanged");
    for (const auto version : {0x50005u, 0x50006u, 0x50008u}) {
        expect(!is_deadasdisco_ue574_dx12_runtime(L"PagodaSteamDemo-Win64-Shipping.exe", version, 0x40000, true),
            "other engine minors unchanged");
    }
    expect(!is_deadasdisco_ue574_dx12_runtime(L"PagodaSteamDemo-Win64-Shipping.exe", 0x50007, 0x50000, true),
        "unproven patch version unchanged");

    halloween_tests::Memory m;
    const auto entry = m.base + 0x100, table = m.base + 0x2000;
    const auto call = [&](halloween_tests::Memory& memory, uintptr_t at, uintptr_t target) {
        memory.put(at, uint8_t{0xe8});
        memory.put(at + 1, static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at + 5)));
    };
    m.put(entry, d::copy_entry);
    m.put(entry + 0x1e, d::views);
    m.put(entry + 0x28, std::array<uint8_t, 3>{0x48,0x8d,0x05});
    m.put(entry + 0x2b, static_cast<int32_t>(table - (entry + 0x2f)));
    m.put(entry + 0x2f, d::copy_source);
    m.put(entry + 0x55, d::element_size);
    m.put(entry + 0xa6, d::element_size);
    m.put(entry + 0x73, d::array_copy);
    m.put(entry + 0xc4, d::array_copy);
    m.put(entry + 0x82, d::all_views);
    m.put(entry + 0xd3, d::targets);
    m.put(entry + 0x13d, d::additional);
    m.put(entry + 0x3de, d::owned);
    m.put(entry + 0x468, d::copy_tail);
    // Views and AllViews share the allocation (+0x6b/+0xbc) and copy (+0x7d/+0xce) helpers.
    const auto helper = [&](size_t index) -> uintptr_t {
        if (index == 2) { return m.base + 0x900; }
        if (index == 3) { return m.base + 0x910; }
        return m.base + 0x900 + 0x10 * static_cast<uintptr_t>(index);
    };
    for (size_t i = 0; i < d::member_calls.size(); ++i) {
        call(m, entry + d::member_calls[i], helper(i));
    }
    const auto resolve = [&](halloween_tests::Memory& memory, size_t size = d::family_copy_size) {
        return d::family_copy_vtable(memory.view(), entry, size, m.base, m.bytes.size());
    };
    expect(resolve(m) == table, "Dead as Disco rbx-source family copy validates without execution");
    expect(!resolve(m, 0x229), "unrelated StateTree-sized copy must not be accepted");
    expect(!resolve(m, d::family_copy_size - 1), "truncated unwind extent rejected");
    for (const auto offset : {0x0u, 0x1du, 0x1eu, 0x28u, 0x33u, 0x34u, 0x56u, 0xa7u, 0x77u, 0xc8u, 0x85u, 0xd5u,
                              0x140u, 0x141u, 0x3e1u, 0x40fu, 0x46bu, 0x4b0u}) {
        auto changed = m; changed.bytes[entry + offset - m.base] ^= 1;
        expect(!resolve(changed), "changed copy ABI/layout/pointer stride must fail closed");
    }
    auto changed = m; changed.bytes[entry + 0x34 - m.base] = 0xfa;
    expect(!resolve(changed), "the stock rdi-source form stays with the generic resolver");
    changed = m; call(changed, entry + d::member_calls[2], m.base + 0x9f0);
    expect(!resolve(changed), "Views and AllViews must use the same allocation helper");
    changed = m; call(changed, entry + d::member_calls[3], m.base + 0x9f0);
    expect(!resolve(changed), "Views and AllViews must use the same copy helper");
    changed = m; changed.put(entry + 0x2b, int32_t{0x100000});
    expect(!resolve(changed), "vtable must stay inside the owning image");
    changed = m; changed.put(entry + d::member_calls[5] + 1, int32_t{0x100000});
    expect(!resolve(changed), "out-of-image member-copy call rejected");
    changed = m; changed.executable = false;
    expect(!resolve(changed), "non-executable constructor rejected");
    changed = m; changed.readable = false;
    expect(!resolve(changed), "unreadable constructor rejected");
}

// Captured bytes of the real constructor, its call-target pages and its vtable slot (same format as the Halloween
// fixture) run the production validator without loading the game.
void test_deadasdisco_native_family_fixture(const char* path) {
    namespace d = uevr::deadasdisco_native;
    struct Segment { uintptr_t address{}; std::vector<uint8_t> bytes{}; bool executable{}; };
    struct Image { std::vector<Segment> segments{}; } image;
    std::ifstream input(path, std::ios::binary);
    std::array<uint64_t, 5> header{}; // constructor, unused, vtable, image base, image size
    uint32_t count{};
    input.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    expect(input && count > 0 && count <= 32, "Dead as Disco family fixture header is complete and bounded");
    if (!input || count == 0 || count > 32) { return; }
    for (uint32_t i = 0; i < count; ++i) {
        Segment segment;
        uint32_t size{}; uint8_t executable{};
        input.read(reinterpret_cast<char*>(&segment.address), sizeof(segment.address));
        input.read(reinterpret_cast<char*>(&size), sizeof(size));
        input.read(reinterpret_cast<char*>(&executable), sizeof(executable));
        if (!input || size == 0 || size > 0x1000) { expect(false, "bad family fixture segment"); return; }
        segment.bytes.resize(size); segment.executable = executable != 0;
        input.read(reinterpret_cast<char*>(segment.bytes.data()), size);
        if (!input) { expect(false, "truncated family fixture segment"); return; }
        image.segments.push_back(std::move(segment));
    }
    const sdk::discovery::Memory memory{&image,
        [](void* context, uintptr_t address, void* out, size_t size) {
            for (const auto& s : static_cast<Image*>(context)->segments) {
                if (d::in_module(address, size, s.address, s.bytes.size())) {
                    std::memcpy(out, s.bytes.data() + address - s.address, size); return true;
                }
            }
            return false;
        }, [](void* context, uintptr_t address, size_t size) {
            for (const auto& s : static_cast<Image*>(context)->segments) {
                if (s.executable && d::in_module(address, size, s.address, s.bytes.size())) { return true; }
            }
            return false;
        }};
    expect(d::family_copy_vtable(memory, header[0], d::family_copy_size, header[3], header[4]) == header[2],
        "real Dead as Disco family constructor passes production validation");
    image.segments.front().bytes[0x3e1] ^= 1;
    expect(!d::family_copy_vtable(memory, header[0], d::family_copy_size, header[3], header[4]),
        "changed real owned-interface copy fails closed");
}
