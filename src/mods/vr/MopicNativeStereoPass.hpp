#pragma once

// Mopic: Native Stereo Fix pass decisions of FFakeStereoRenderingHook that upstream does not have, as pure functions
// so sceneview_constructor keeps one call where the decision is used (MOPIC.md, "Updating to a new joeyhodge release").

namespace uevr::native_stereo_pass {

// On UE5.5/5.6 the right eye renders in a view family of its own, where a SECONDARY view expects a PRIMARY view of the
// same family to have done the work the engine shares between the eyes. Hozy (UE5.6) lost the directional light's
// shadow in the right eye: its Attic came out washed white (L/R 13.6 %, 0.05 % relabeled PRIMARY). From UE5.7 on
// joeyhodge's behavior stays: 5.7 renders it in a family linked to the left eye's (use_ue57_linked_family_transaction),
// whose validation needs the SECONDARY identity (Dead as Disco fails closed without it); 5.8+ is untested.
inline constexpr bool right_eye_renders_alone(bool is_ue55_or_newer, bool is_ue57_or_newer) {
    return is_ue55_or_newer && !is_ue57_or_newer;
}

} // namespace uevr::native_stereo_pass
