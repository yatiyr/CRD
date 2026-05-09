#pragma once

#include <crd/core/types.hpp>
#include <crd/resources/crdr.hpp>

namespace crd::preset
{
// LensModel — projection family for the camera. Persistent enum stored in
// CameraPreset; new entries append (never insert) to keep CRDR bytes stable.
enum class LensModel : crd::u8
{
    Perspective  = 0,
    Orthographic = 1,
};

// ExposureMode — drives whether the renderer reads the manual aperture /
// shutter / ISO triplet directly or auto-derives EV100 from the scene's
// luminance histogram clamped to [ev100_min, ev100_max].
enum class ExposureMode : crd::u8
{
    Manual    = 0,
    AutoEV100 = 1,
};

// CameraPreset — Phase 3.0 v1n3 (ADR-0059 §1, §7).
//
// Cross-cutting camera bag. Consumed by `Camera::apply` (the
// camera-instance-level overload of `IPresetTarget::apply`). Same five-layer
// resolution semantics as QualityPreset — the resolved value is cached on
// the target so per-frame cost is zero.
//
// Field semantics:
//   fov_y_radians     — vertical field-of-view in radians. Default ≈ 60°.
//                       Ignored when lens_model == Orthographic.
//   near_plane        — near clip distance in world units. Default 0.1.
//   far_plane         — far clip distance in world units. Default 1000.
//   aperture_f_stop   — lens aperture as an f-number (e.g. 2.8 = f/2.8).
//                       Default 2.8.
//   shutter_seconds   — shutter open duration in seconds (1.0/60.0 = 1/60s).
//                       Default 1/60.
//   iso               — sensor sensitivity. Default 100.
//   exposure_comp_ev  — exposure compensation in stops. Default 0.
//   ev100_min         — auto-exposure low clamp; only honored when
//                       exposure_mode == AutoEV100. Default -8.
//   ev100_max         — auto-exposure high clamp. Default 16.
//   lens_model        — Perspective (default) or Orthographic.
//   exposure_mode     — Manual (default) or AutoEV100.
//
// Binary layout is FROZEN at version=1 (40 B, 4-byte aligned). Adding fields
// bumps `version`; the loader's payload-size check converts a mismatch into
// LoadState::Failed. Migration tables remain a v1n+1 follow-up per
// ADR-0059 §"Open questions".
struct alignas(4) CameraPreset
{
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P', 'R', 'C', 'M');
    static constexpr crd::u32 version = 1U;

    // Projection.
    crd::f32 fov_y_radians      = 1.0471975512F; // ≈ 60° vertical FOV
    crd::f32 near_plane         = 0.1F;
    crd::f32 far_plane          = 1000.0F;

    // Physical exposure triplet.
    crd::f32 aperture_f_stop    = 2.8F;
    crd::f32 shutter_seconds    = 1.0F / 60.0F;
    crd::f32 iso                = 100.0F;
    crd::f32 exposure_comp_ev   = 0.0F;

    // Auto-exposure clamps (only honored when exposure_mode == AutoEV100).
    crd::f32 ev100_min          = -8.0F;
    crd::f32 ev100_max          = 16.0F;

    // Mode flags + explicit padding to keep the on-disk layout deterministic
    // across compilers regardless of enum-class storage choice.
    LensModel    lens_model     = LensModel::Perspective;
    ExposureMode exposure_mode  = ExposureMode::Manual;
    crd::u8      _reserved[2]   = {};
};

static_assert(sizeof(CameraPreset)  == 40,
              "CameraPreset size pinned at 40 bytes for version=1");
static_assert(alignof(CameraPreset) == 4,
              "CameraPreset alignment pinned at 4 bytes for version=1");

} // namespace crd::preset
