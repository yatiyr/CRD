#pragma once

#include <crd/core/types.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::preset
{
// QualityPreset — Phase 3.0 v1n2 (ADR-0059 §1, §7).
//
// Cross-cutting renderer-quality bag. Consumed by `IRenderPath::apply` (the
// render-path-level overload of `IPresetTarget::apply`); the resolved value
// is cached on the target so per-frame cost is zero.
//
// Field semantics:
//   shadow_resolution — pixels per shadow map cascade (square). Common
//                       presets: 1024 (low), 2048 (med), 4096 (high), 8192
//                       (ultra). Default: 2048.
//   msaa_samples      — multisample count for the colour resolve. Valid
//                       values: 1 (off), 2, 4, 8. Backend rejects others.
//                       Default: 4.
//   ssr_quality       — screen-space reflection quality tier (0=off, 1=low,
//                       2=med, 3=high). Default: 2.
//   ssao_quality      — same scale, for screen-space ambient occlusion.
//                       Default: 2.
//   post_fx_count     — count of valid entries in `post_fx[]`. 0–8.
//   post_fx[8]        — ResourceIds of cooked PostFXPreset artifacts (FourCC
//                       'PRPP'); applied in array order. Phase 3.5+ when
//                       the post-fx preset type ships; v1n2 reserves the
//                       slot. Default: all-null.
//
// Binary layout is FROZEN at version=1. Adding fields bumps `version`; the
// loader's payload-size check enforces "no silent migration" (mismatched
// bytes → LoadState::Failed). Migration tables land as a v1n+1 follow-up
// per ADR-0059 §"Open questions / debt".
//
// On-disk format mirrors the in-memory layout — the cooker writes
// `sizeof(QualityPreset)` raw bytes into the PDAT chunk, and the loader
// memcpy's them back. Determinism: explicit padding fields are zero-init,
// so the bit pattern is stable across cook runs of the same source.
struct alignas(8) QualityPreset
{
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P', 'R', 'Q', 'L');
    static constexpr crd::u32 version = 1U;

    crd::u32                    shadow_resolution = 2048U;

    crd::u8                     msaa_samples      = 4U;
    crd::u8                     ssr_quality       = 2U;
    crd::u8                     ssao_quality      = 2U;
    crd::u8                     post_fx_count     = 0U;

    // Padding to align post_fx[] (ResourceId is 16-byte; align to 8 here is
    // sufficient — the array will sit at offset 16). Explicit so the wire
    // format is stable and the static_assert below is meaningful.
    crd::u8                     _reserved[8]      = {};

    crd::resources::ResourceId  post_fx[8]        = {};
};

// Total size pinned: 4 (shadow) + 4 (u8 ×4) + 8 (reserved) + 128 (8 ×
// ResourceId) = 144 bytes. Any change here IS a schema-version bump.
static_assert(sizeof(QualityPreset)  == 144,
              "QualityPreset size pinned at 144 bytes for version=1");
static_assert(alignof(QualityPreset) == 8,
              "QualityPreset alignment pinned at 8 bytes for version=1");

} // namespace crd::preset
