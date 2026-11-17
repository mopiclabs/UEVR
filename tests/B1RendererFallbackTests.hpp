#pragma once

#include "mods/vr/B1RendererEntry.hpp"
#include "mods/vr/B1RendererFallback.hpp"
#include "B1RendererFixture.hpp"

// Mopic: a Black Myth: Wukong exe the B1 adapter can't validate goes on to the generic callback-caller path.
namespace b1_renderer_fallback_tests {

template <typename Expect>
void run(const Expect& expect) {
    namespace b = uevr::b1;
    namespace f = b1_renderer_fixture;
    constexpr auto exe = L"D:\\Steam\\steamapps\\common\\BlackMythWukong\\b1\\Binaries\\Win64\\b1-Win64-Shipping.exe";

    const auto step_for = [&](uint32_t version_ms, uint32_t version_ls, std::span<const uint8_t> code) {
        return b::resolver_step(b::is_supported_runtime(exe, version_ms, version_ls, true),
            b::has_callable_renderer_entry(code, f::unwind, f::callback_return));
    };

    expect(step_for(0x00050000, 0, f::renderer) == b::ResolverStep::use_adapter_entry,
        "the validated B1 exe and bytes use the adapter's entry");

    // A game patch that keeps file version 5.0.0.0 but changes the renderer's prologue or its view-extension loop.
    for (const size_t offset : {size_t{0x00}, size_t{0x16}, size_t{0x4B}, size_t{0x14C}}) {
        auto patched = f::renderer;
        patched[offset] ^= 0x01;
        expect(step_for(0x00050000, 0, patched) == b::ResolverStep::fall_through_to_callback_caller,
            "a B1 renderer the adapter rejects falls through to the callback-caller path");
    }
    expect(step_for(0x00050000, 0, std::span{f::renderer}.first(0x100)) ==
            b::ResolverStep::fall_through_to_callback_caller,
        "a truncated B1 renderer falls through to the callback-caller path");

    // A patch that changes the file version never reaches the adapter.
    expect(step_for(0x00050000, 1, f::renderer) == b::ResolverStep::generic &&
           step_for(0x00050001, 0, f::renderer) == b::ResolverStep::generic,
        "another B1 file version takes the generic resolver");
    expect(b::resolver_step(false, true) == b::ResolverStep::generic &&
           b::resolver_step(false, false) == b::ResolverStep::generic,
        "other games take the generic resolver whatever the adapter would say");
}

} // namespace b1_renderer_fallback_tests
