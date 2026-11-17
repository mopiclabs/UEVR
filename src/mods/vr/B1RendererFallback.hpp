#pragma once

// Mopic: where joeyhodge's Black Myth: Wukong renderer adapter (B1RendererEntry.hpp) sits in
// resolve_begin_rendering_viewfamilies_from_stack. The adapter validates one exe (file version 5.0.0.0, DX12) down
// to its code and unwind bytes; a Wukong patch that keeps the file version but moves those bytes fails it. The
// generic callback-caller path (Mopic "VR: Resolve the renderer entry from the view-extension callback's caller")
// resolved Wukong before the adapter existed, so a rejected adapter falls through to it instead of leaving the
// renderer unhooked (which turned the Native Stereo Fix off after 20 s).

namespace uevr::b1 {

enum class ResolverStep {
    generic,                         // not the adapter's exe: the generic resolver as before
    use_adapter_entry,               // the adapter validated the entry: use it
    fall_through_to_callback_caller, // the adapter rejected it: continue with the generic resolver
};

inline ResolverStep resolver_step(bool adapter_runtime, bool adapter_validated) {
    if (!adapter_runtime) {
        return ResolverStep::generic;
    }
    return adapter_validated ? ResolverStep::use_adapter_entry : ResolverStep::fall_through_to_callback_caller;
}

} // namespace uevr::b1
