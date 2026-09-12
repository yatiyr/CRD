#pragma once

// dx12_ray_tracing_context.hpp — D-007 C3/RT: the DIRECT3D 12 RAY-TRACING context — the DX12 mirror of VulkanRayTracingContext.
// Builds DXR acceleration structures (BLAS/TLAS via ID3D12Device5 / ID3D12GraphicsCommandList4) and runs an INLINE ray-query
// (DXR 1.1 / SM 6.5 `RayQuery<>`) compute dispatch against them — the same minimal RT vertical as the Vulkan side, so a CKIR RT
// kernel that lowers to HLSL runs bit-for-bit the same algorithm on BOTH backends (RT traversal is not bit-exact across vendors,
// so the cross-backend gate is GEOMETRIC tolerance vs the shared CPU oracle). Standalone (creates its own D3D12 device + compute
// queue, mirroring Dx12ComputeContext); WINDOWS-ONLY, and `valid()` is false unless the adapter reports DXR tier ≥ 1.1 (inline).

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/gpu/raster_context.hpp> // REN-38-A9: IAccelerationStructure — the portable AS handle
#include <crd/gpu/rt_capabilities.hpp>

#include <memory>

namespace crd::gpu
{

// A built DXR scene (one BLAS from a triangle soup + one identity-instance TLAS). Opaque; the RT context keeps the backing
// resources alive for the scene's lifetime.
// ⭐ REN-38-A9: the DX12 mirror — a scene IS the portable `IAccelerationStructure`.
class Dx12RtScene : public IAccelerationStructure
{
public:
    ~Dx12RtScene() override = default;
};

// REN-38-A9: the TLAS's GPU VIRTUAL ADDRESS, which is exactly what a `RaytracingAccelerationStructure` SRV binds
// (DXR takes the AS by address, not by resource). 0 when the scene has none.
[[nodiscard]] crd::u64 dx12_scene_tlas(const IAccelerationStructure& scene) noexcept;

class Dx12RayTracingContext
{
public:
    Dx12RayTracingContext();
    ~Dx12RayTracingContext();
    Dx12RayTracingContext(const Dx12RayTracingContext&)            = delete;
    Dx12RayTracingContext& operator=(const Dx12RayTracingContext&) = delete;

    // True iff a D3D12 device came up AND it reports RaytracingTier ≥ 1.1 (inline RayQuery in compute). Tests soft-skip otherwise.
    [[nodiscard]] bool valid() const noexcept;

    // The RT capabilities this context implements on DX12. Today: inline ray query (the vendor OMM/pipeline/SER/cluster paths are
    // Vulkan-only so far, so a portable consumer requesting them here takes the documented fallback + warning).
    [[nodiscard]] RtCapabilities capabilities() const noexcept;

    // Build a BLAS from `ntris` triangles (vertices = ntris*3 packed float3, no index buffer) + an identity-instance TLAS.
    [[nodiscard]] std::unique_ptr<Dx12RtScene> build_scene(const float* vertices, crd::u32 ntris);

    // Build one BLAS + a TLAS of `ninst` INSTANCES of it, each with a row-major 3×4 world transform (`transforms` = ninst×12
    // floats). The DXR twin of VulkanRayTracingContext::build_scene_instanced — instanced traversal, portable across backends.
    // `opaque=false` builds NON-OPAQUE geometry so an any-hit shader is invoked (matches the Vulkan twin).
    [[nodiscard]] std::unique_ptr<Dx12RtScene>
    build_scene_instanced(const float* vertices, crd::u32 ntris, const float* transforms, crd::u32 ninst,
                          bool opaque = true);

    // B18-f: build a scene of HAIR STRANDS as PROCEDURAL geometry — the DXR twin of
    // VulkanRayTracingContext::build_scene_curves. Each segment becomes one AABB in the BLAS; the shader intersects the
    // linear-swept sphere (round cone) analytically inside the ray-query candidate loop — see ckir_lss.hpp.
    //
    // ⭐ WHY PROCEDURAL AND NOT TRIANGLES. A 170K-strand groom at 30 segments is 5M segments. Tessellated tubes at 8-24
    //   triangles each is 40M+ triangles: a multi-gigabyte AS, minutes of build time, and a silhouette that is STILL
    //   faceted under a close camera. One AABB per segment is ~24 bytes and the swept surface stays exact at any zoom.
    //
    // ⛔ DXR HAS NO NATIVE SWEPT-SPHERE PRIMITIVE AT ANY TIER — there is no equivalent of
    //   VK_NV_ray_tracing_linear_swept_spheres. The procedural path is therefore not a fallback here, it IS the DX12
    //   strand tier, which is exactly why the intersector lives in CKIR and not in a vendor extension.
    //
    // `segments` is 8 floats per segment: [ax, ay, az, ra, bx, by, bz, rb] — endpoints and their radii, so a strand can
    // taper. The caller ALSO binds the same segment array as a UAV for the intersector to read; this call only builds
    // the traversal structure. Returns nullptr if the RT context is invalid or nseg == 0.
    [[nodiscard]] std::unique_ptr<Dx12RtScene> build_scene_curves(const float* segments, crd::u32 nseg);

    // One storage-buffer binding for an inline-RT dispatch. `binding` is the UAV register slot uN (the TLAS is implicit as the
    // root SRV t0). `upload`/`readback` mirror the Vulkan Binding; `bytes` sizes the device buffer.
    struct Binding
    {
        const void* upload   = nullptr;
        void*       readback = nullptr;
        crd::u64    bytes    = 0;
        crd::u32    binding  = 0;
    };

    // Run an inline-ray-query compute kernel `dxil` against `scene`: binds the TLAS as the root SRV t0 and each `Binding` as a
    // RWByteAddressBuffer UAV at uN, uploads inputs, dispatches `groups` thread-groups, reads back outputs. The DX12 twin of
    // VulkanRayTracingContext::trace_dispatch.
    [[nodiscard]] bool trace_dispatch(const Dx12RtScene& scene, crd::containers::ConstSpan<crd::u8> dxil,
                                      crd::containers::ConstSpan<Binding> bindings, crd::u32 groups);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::gpu
