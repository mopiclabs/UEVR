// Mopic: included in the public part of class VR (VR.hpp), next to upstream's Native Stereo Fix getters, so a
// joeyhodge release that changes those meets one include line instead of these (MOPIC.md, "Updating to a new
// joeyhodge release"). The options themselves stay in VR.hpp beside upstream's, in the order they are registered.

    // VR_NativeStereoFixShareExposure (default on): the right eye renders as a primary view of its own and meters its
    // own auto exposure; this gives it the left eye's, as stock stereo gives the secondary view the primary's (Black
    // Myth: Wukong shrine: right eye -5.5% -> +0.3%). Goes with VR_NativeStereoFixLumenOriginRefreshFrames: without that,
    // the right eye's own exposure partly hid its dimmer Lumen lighting.
    bool is_native_stereo_fix_share_exposure_enabled() const {
        return m_native_stereo_fix_share_exposure->value();
    }

    // EXPERIMENT (exp/samepass-exposure, default off): see SecondaryPreExposureHold.
    bool is_experiment_secondary_pre_exposure_enabled() const {
        return m_experiment_secondary_pre_exposure->value();
    }
    bool is_experiment_secondary_exposure_state_enabled() const {
        return m_experiment_secondary_exposure_state->value();
    }

    // EXPERIMENT (exp/right-eye-cost, default off): see vr/PassMarkers.hpp and ExperimentSecondFamilyHold.
    int32_t get_experiment_pass_markers_seconds() const {
        return m_experiment_pass_markers_seconds->value();
    }
    std::string get_experiment_second_family_cvars() const {
        return m_experiment_second_family_cvars->value();
    }
    // EXPERIMENT (exp/right-eye-cost, default 0 = off): see vr/GdfShare.hpp.
    int32_t get_experiment_right_eye_gdf_share() const {
        return m_experiment_right_eye_gdf_share->value();
    }
    // EXPERIMENT (exp/right-eye-cost, default false): GDF probes 4 and 5 (log only; probe 5 stalls the game thread).
    bool get_experiment_gdf_probes() const {
        return m_experiment_gdf_probes->value();
    }

    // VR_NativeStereoFixLumenOriginRefreshFrames (default 30, 0 = off): r.LumenScene.UpdateViewOrigin is held at 0 for
    // both eye renders except on every Nth frame, when both move Lumen's view origin (see begin_render_viewfamily_real).
    int32_t get_native_stereo_fix_lumen_origin_refresh_frames() const {
        return m_native_stereo_fix_lumen_origin_refresh_frames->value();
    }

    // VR_NativeStereoFixSecondLumenSurfaceCacheFreeze (default on): r.LumenScene.SurfaceCache.Freeze is held at 1 for the
    // right eye's render, which then skips updating the surface cache the left eye's render just updated (Black Myth:
    // Wukong, UE5.0, 4090L: 1.27 +- 0.33 ms GPU per frame, ~+6 % fps, eyes unchanged; no effect on Expedition 33, UE5.4).
    bool is_native_stereo_fix_second_lumen_surface_cache_freeze_enabled() const {
        return m_native_stereo_fix_second_lumen_surface_cache_freeze->value();
    }

    // VR_NativeStereoFixSecondGlobalDistanceFieldRefreshFrames (default 15, 0 = off): r.AOGlobalDistanceFieldForceFullUpdate
    // is held at 1 for every Nth right-eye render, so its own Global Distance Field clipmaps catch up with the primitives
    // that moved, which only the left eye's render is told about (see begin_render_viewfamily_real).
    int32_t get_native_stereo_fix_second_gdf_refresh_frames() const {
        return m_native_stereo_fix_second_gdf_refresh_frames->value();
    }

    // VR_NativeStereoFixSecondGlobalDistanceFieldRefreshOnChangeFrames (default 8, 0 = off): FScene's AddPrimitive /
    // RemovePrimitive / UpdatePrimitiveTransform are hooked to count static meshes that appear, disappear or move, and the
    // right eye's distance field is rebuilt in the frame of such a change, at most every M frames; the N above then only
    // caps the wait (N = 0 turns the rebuild off altogether), and where no such entry is found or called, the cap is
    // 20 N. An update that leaves the component's world transform as it was does not count, and a component that moves
    // in every frame for 60 frames (or, where the world transform isn't found, is sent in every frame) counts no more
    // while it keeps that up (see gdf_change_signal in FFakeStereoRenderingHook.cpp).
    int32_t get_native_stereo_fix_second_gdf_refresh_on_change_frames() const {
        return m_native_stereo_fix_second_gdf_refresh_on_change_frames->value();
    }

    // VR_ExperimentRightEyeRTShare (EXPERIMENT, default 0 = off, 2 in Hogwarts Legacy (VR::on_config_load); 1 = observe
    // and validate, 2 = share once validated, 3 = share in even 10-second QPC windows): the Native Stereo Fix right
    // eye's renderer borrows the ray tracing scene the left eye's renderer built in the same frame (see RtSceneShare.hpp).
    int32_t get_experiment_right_eye_rt_share() const {
        return m_experiment_right_eye_rt_share->value();
    }

    // VR_ExperimentRightEyeRTShareCullGather (EXPERIMENT, default off, on in Hogwarts Legacy): in frames whose right eye
    // borrows the left eye's scene (one decision per frame, see RtSceneShare.hpp), the right eye's own (unused) ray
    // tracing instance gathering is culled (see uevr::rt_share::RightGatherCullHold).
    bool is_experiment_right_eye_rt_share_cull_gather() const {
        return m_experiment_right_eye_rt_share_cull_gather->value();
    }
