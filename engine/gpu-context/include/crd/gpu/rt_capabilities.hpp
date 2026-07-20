#pragma once

// rt_capabilities.hpp — the BACKEND-AGNOSTIC ray-tracing capability contract. A portable consumer queries `RtCapabilities` and
// either uses a hardware feature or FALLS BACK to a correct path (with a diagnostic) — graceful degradation, never a hard failure.
// Each backend's RT context reports exactly what its adapter enables, so the same portable code runs everywhere and only *warns*
// when a requested vendor feature degrades. This is the CKIR "portable, all-backends" mission applied to the vendor RT frontier.

#include <crd/core/types.hpp>

namespace crd::gpu
{

// One flag per ray-tracing hardware capability. Two natures live behind these: SHADER features (RtPipeline, ShaderReorder — they
// are CKIR shader concepts) and ACCELERATION-STRUCTURE-BUILD features (OpacityMicromap, ClusterAS — they are scene/AS-build
// options). The capability query is uniform; the fallback semantics differ (perf-only no-op / result-equivalent / needs-shader-fallback).
enum class RtFeature : crd::u32
{
    InlineQuery     = 1U << 0U, // inline ray query in a compute kernel (VK_KHR_ray_query / DXR-1.1 inline) — the baseline
    RtPipeline      = 1U << 1U, // raygen/hit/miss pipeline + shader binding table (VK_KHR_ray_tracing_pipeline / DXR)
    ShaderReorder   = 1U << 2U, // SER (VK_NV/EXT_ray_tracing_invocation_reorder / DXR SER) — PERF only, results identical
    OpacityMicromap = 1U << 3U, // OMM alpha-tested geometry resolved in traversal (VK_EXT_opacity_micromap / DXR OMM)
    ClusterAS       = 1U << 4U, // cluster / mega-geometry acceleration structures (VK_NV_cluster_acceleration_structure)
    // B18-f: LINEAR SWEPT SPHERES — the native hair-strand primitive (a sphere swept along a segment = a round cone).
    // Blackwell-class silicon only, and DXR has NO equivalent primitive at all, so on most hardware the portable path
    // (procedural AABBs + the analytic intersector in ckir_lss.hpp) is not a degraded fallback — it IS the strand tier.
    // The flag exists so a consumer can take the native primitive where it is genuinely present.
    LinearSweptSpheres = 1U << 5U,
};

// A queryable set of the RtFeatures an RT context's adapter has enabled.
struct RtCapabilities
{
    crd::u32 flags = 0;

    [[nodiscard]] bool has(RtFeature f) const noexcept { return (flags & static_cast<crd::u32>(f)) != 0U; }
    void               set(RtFeature f, bool on) noexcept
    {
        if (on) { flags |= static_cast<crd::u32>(f); }
        else { flags &= ~static_cast<crd::u32>(f); }
    }
};

// The policy a portable RT call applies when a requested feature is absent — the caller's intent, surfaced in the diagnostic.
enum class RtFallback : crd::u32
{
    NoOp,       // perf-only (SER): silently drop the feature; a debug log at most
    Equivalent, // result-identical (clusters → standard BLAS): transparent fallback + info log
    ShaderPath, // result-affecting (OMM → any-hit alpha shader): fall back to the portable shader path + warn on the downgrade
};

// A human-readable name for a feature (for the diagnostic message).
[[nodiscard]] inline const char* rt_feature_name(RtFeature f) noexcept
{
    switch (f)
    {
    case RtFeature::InlineQuery: return "inline ray query";
    case RtFeature::RtPipeline: return "ray-tracing pipeline";
    case RtFeature::ShaderReorder: return "shader execution reordering (SER)";
    case RtFeature::OpacityMicromap: return "opacity micromaps (OMM)";
    case RtFeature::ClusterAS: return "cluster acceleration structures";
    case RtFeature::LinearSweptSpheres: return "linear swept spheres (LSS curve primitive)";
    }
    return "?";
}

} // namespace crd::gpu
