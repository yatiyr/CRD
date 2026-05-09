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
//   shadow_resolution    — pixels per shadow map cascade (square). Common
//                          presets: 1024 (low), 2048 (med), 4096 (high),
//                          8192 (ultra). Default: 2048. Consumed by the
//                          shadow path in Phase 3.5+; v1o3 caches the
//                          value on `ForwardRenderPath` for read-back.
//   msaa_samples         — multisample count for the colour resolve. Valid
//                          values: 1 (off), 2, 4, 8. Default: 4. Cached
//                          on the render path; an MSAA-capable color
//                          target lands when the rhi grows multisample
//                          support.
//   ssr_quality          — screen-space reflection quality tier (0=off,
//                          1=low, 2=med, 3=high). Default: 2.
//   ssao_quality         — same scale, for screen-space ambient occlusion.
//                          Default: 2.
//   post_fx_count        — count of valid entries in `post_fx[]`. 0–8.
//   enable_depth_prepass — v2 field. 1 = render the depth-prepass; 0 =
//                          skip the prepass draws (color pass still
//                          clears depth). Drives ForwardRenderPath
//                          behaviour today, observable in the sandbox
//                          via the Quality panel checkbox + frame
//                          profiler. The field repurposes one byte of
//                          v1's `_reserved[8]` so the binary layout
//                          stays at 144 bytes; the version bump signals
//                          that consumers must read the byte as the new
//                          named field rather than padding.
//   post_fx[8]           — ResourceIds of cooked PostFXPreset artifacts
//                          (FourCC 'PRPP'); applied in array order. Phase
//                          3.5+ when the post-fx preset type ships;
//                          v1n2 reserves the slot. Default: all-null.
//
// Binary layout pinned at 144 bytes. Adding fields BUMPS `version`. The
// loader's payload-size check enforces "no silent migration" (mismatched
// bytes → LoadState::Failed). Migration tables land as a v1n+1 follow-up
// per ADR-0059 §"Open questions / debt".
//
// History:
//   v1 (2026-05-09)  — initial schema; 144 B; fields shadow_resolution,
//                      msaa_samples, ssr_quality, ssao_quality,
//                      post_fx_count, post_fx[8].
//   v2 (2026-05-09)  — `enable_depth_prepass` introduced by repurposing
//                      one byte of `_reserved[8]`. Layout binary-stable
//                      (still 144 B); the version bump tells loaders /
//                      consumers to interpret offset 8 as the new
//                      named field rather than padding. v1 cooked
//                      assets need to be re-cooked to upgrade.
//
// On-disk format mirrors the in-memory layout — the cooker writes
// `sizeof(QualityPreset)` raw bytes into the PDAT chunk, and the loader
// memcpy's them back. Determinism: explicit padding fields are zero-init,
// so the bit pattern is stable across cook runs of the same source.
struct alignas(8) QualityPreset
{
    static constexpr crd::u32 fourcc  = crd::resources::make_fourcc('P', 'R', 'Q', 'L');
    static constexpr crd::u32 version = 2U;

    crd::u32                    shadow_resolution    = 2048U;

    crd::u8                     msaa_samples         = 4U;
    crd::u8                     ssr_quality          = 2U;
    crd::u8                     ssao_quality         = 2U;
    crd::u8                     post_fx_count        = 0U;

    // v2 field repurposing one byte of v1's `_reserved[8]`. Default = 1
    // (depth prepass enabled) matches the v1 hardcoded behaviour, so a
    // v2 default-constructed preset is observably identical to v1's.
    crd::u8                     enable_depth_prepass = 1U;

    // Remaining 7 bytes of explicit padding. Zero-init for deterministic
    // wire bytes. The post_fx[] array sits at offset 16 (alignof
    // ResourceId == 8 means 16 is the next valid alignment past the
    // 9-byte run above).
    crd::u8                     _reserved[7]         = {};

    crd::resources::ResourceId  post_fx[8]           = {};
};

// Total size pinned: 4 (shadow) + 4 (u8 ×4) + 8 (reserved) + 128 (8 ×
// ResourceId) = 144 bytes. Any change here IS a schema-version bump.
static_assert(sizeof(QualityPreset)  == 144,
              "QualityPreset size pinned at 144 bytes for version=1");
static_assert(alignof(QualityPreset) == 8,
              "QualityPreset alignment pinned at 8 bytes for version=1");

} // namespace crd::preset
