#pragma once

// vulkan_ray_tracing_context.hpp — D-007 C3/RT-1: the Vulkan RAY-TRACING context (the device half of the C3↔B9 pair). Builds
// hardware acceleration structures (BLAS/TLAS) and runs an INLINE ray-query compute dispatch against them (VK_KHR_ray_query),
// the minimal RT vertical: a compute kernel casting rays at a TLAS with no shader binding table. It owns its own command pool,
// descriptor pool, pipeline, and device-address buffers (the AS-build inputs need buffer device addresses), reusing the shared
// VkDevice from the VulkanGpuContext (the C2 one-device unification). The full RT PIPELINE (raygen/hit/miss + SBT), SER, OMM,
// cluster-AS, and LSS curves land in RT-2..RT-3; this is the inline-ray-query core (shadows / AO / the B14 ReSTIR visibility leaf).

#include <crd/gpu/vulkan_context.hpp>

#include <crd/containers/span.hpp>
#include <crd/gpu/rt_capabilities.hpp>

#include <memory>

namespace crd::gpu
{

// A built scene acceleration structure (one BLAS from a triangle soup + one identity-instance TLAS). Opaque; the RT context
// keeps the backing buffers alive for the scene's lifetime.
class RtScene
{
public:
    virtual ~RtScene() = default;
};

class VulkanRayTracingContext
{
public:
    explicit VulkanRayTracingContext(VulkanGpuContext& ctx);
    ~VulkanRayTracingContext();
    VulkanRayTracingContext(const VulkanRayTracingContext&)            = delete;
    VulkanRayTracingContext& operator=(const VulkanRayTracingContext&) = delete;

    [[nodiscard]] bool valid() const noexcept;

    // The RT capabilities this adapter enables (inline query · RT pipeline · SER · OMM · cluster-AS) — the portable query a
    // consumer uses to decide HW-feature-vs-fallback. See rt_capabilities.hpp.
    [[nodiscard]] RtCapabilities capabilities() const noexcept;

    // Build a BLAS from `ntris` triangles (vertices = ntris*3 packed float3, no index buffer) + an identity-instance TLAS.
    [[nodiscard]] std::unique_ptr<RtScene> build_scene(const float* vertices, crd::u32 ntris);

    // Build one BLAS from the triangle soup + a TLAS of `ninst` INSTANCES of it, each with a row-major 3×4 world transform
    // (`transforms` = ninst × 12 floats). The hardware applies each instance's transform during traversal — the portable,
    // both-backend scale capability (instancing / many-object scenes). ninst==1 with an identity transform == build_scene.
    // `opaque=false` builds NON-OPAQUE geometry so an any-hit shader is invoked (the P4 alpha fallback needs it).
    [[nodiscard]] std::unique_ptr<RtScene>
    build_scene_instanced(const float* vertices, crd::u32 ntris, const float* transforms, crd::u32 ninst, bool opaque = true);

    // P4 (graceful degradation): build an ALPHA-TESTED scene — a hardware opacity micromap when the adapter supports it (fast),
    // ELSE a NON-OPAQUE scene the caller pairs with the CKIR any-hit alpha shader (correct, slower). Sets `*fell_back` on the
    // HW→shader downgrade. The portable OMM entry.
    [[nodiscard]] std::unique_ptr<RtScene>
    build_scene_alpha(const float* vertices, crd::u32 ntris, const crd::u8* omm_bits, crd::u32 subdiv, bool* fell_back = nullptr);

    // FA-1: build a scene whose FIRST triangle carries an OPACITY MICROMAP (VK_EXT_opacity_micromap) — alpha-tested geometry
    // resolved during AS traversal, no any-hit shader. `omm_bits` packs 4^subdiv 2-state opacity bits (bit=1 ⇒ opaque micro-tri)
    // for triangle 0; the remaining triangles are fully opaque. A ray through a transparent micro-triangle passes THROUGH the
    // front triangle (traversal continues). Returns nullptr if the adapter lacks VK_EXT_opacity_micromap.
    [[nodiscard]] std::unique_ptr<RtScene>
    build_scene_omm(const float* vertices, crd::u32 ntris, const crd::u8* omm_bits, crd::u32 subdiv);

    // FA-3: build a scene via CLUSTER acceleration structures (VK_NV_cluster_acceleration_structure / RTX Mega-Geometry) — the
    // triangle soup becomes ONE triangle cluster (CLAS), a cluster BLAS is built over it, and a TLAS over that. Two GPU-driven
    // INDIRECT builds (BUILD_TRIANGLE_CLUSTER → BUILD_CLUSTERS_BOTTOM_LEVEL) with the CLAS address chained into the BLAS build.
    // Traversal is identical to a normal BLAS, so results match the oracle. Returns nullptr without VK_NV_cluster_acceleration_structure.
    [[nodiscard]] std::unique_ptr<RtScene>
    build_scene_clusters(const float* vertices, crd::u32 ntris);

    // B18-f: build a scene of HAIR STRANDS as PROCEDURAL geometry. Each segment becomes one AABB in the BLAS; the shader
    // intersects the linear-swept sphere (round cone) analytically inside the ray-query candidate loop — see ckir_lss.hpp.
    //
    // ⭐ WHY PROCEDURAL AND NOT TRIANGLES. A 170K-strand groom at 30 segments is 5M segments. Tessellated tubes at 8-24
    //   triangles each is 40M+ triangles: a multi-gigabyte AS, minutes of build time, and a silhouette that is STILL
    //   faceted under a close camera. One AABB per segment is ~24 bytes and the swept surface stays exact at any zoom.
    //
    // `segments` is 8 floats per segment: [ax, ay, az, ra, bx, by, bz, rb] — endpoints and their radii, so a strand can
    // taper. The caller ALSO binds the same segment array as a storage buffer for the intersector to read; this call only
    // builds the traversal structure. Returns nullptr if the RT context is invalid or nseg == 0.
    [[nodiscard]] std::unique_ptr<RtScene>
    build_scene_curves(const float* segments, crd::u32 nseg);

    // P5 (graceful degradation): build a "scalable" scene — cluster-AS (mega-geometry) when the adapter supports it, ELSE a
    // transparent fallback to a standard BLAS (identical traversal + result; clusters are a memory-layout optimisation). Sets
    // `*fell_back` to true when it degraded, so the caller can log the info. The portable cluster-topology entry.
    [[nodiscard]] std::unique_ptr<RtScene>
    build_scene_scalable(const float* vertices, crd::u32 ntris, bool prefer_clusters, bool* fell_back = nullptr);

    // One storage-buffer binding for an inline-RT dispatch. `binding` is the SSBO descriptor slot (the TLAS is implicit at
    // binding 0). If `upload != nullptr` the host data is copied in before the dispatch; if `readback != nullptr` the buffer
    // is copied back out after. `bytes` sizes the device buffer.
    struct Binding
    {
        const void* upload   = nullptr;
        void*       readback = nullptr;
        crd::u64    bytes    = 0;
        crd::u32    binding  = 0;
    };

    // Run an inline-ray-query compute kernel `spirv` against `scene`: binds the TLAS at binding 0 and each `Binding` as a
    // storage buffer at its slot, uploads inputs, dispatches `groups` workgroups, reads back outputs. The reusable inline-RT
    // dispatch every effect (shadows / AO / reflections / GI / path tracing) rides on.
    [[nodiscard]] bool trace_dispatch(const RtScene& scene, crd::containers::ConstSpan<crd::u8> spirv,
                                      crd::containers::ConstSpan<Binding> bindings, crd::u32 groups);

    // FA-2: run a full RAY-TRACING PIPELINE (raygen + miss + closest-hit shaders, SPIR-V) against `scene` — the "big rig" RT path
    // (VK_KHR_ray_tracing_pipeline): builds the pipeline + a 3-group shader binding table and `vkCmdTraceRaysKHR(width,height,1)`.
    // The raygen shader may use SER (hitObjectNV/reorderThreadNV) when the adapter reports invocation_reorder — that's baked into
    // the raygen SPIR-V, so no flag here. Binds the TLAS at binding 0 + each Binding at its slot. Returns false without the ext.
    // `rahit` is an OPTIONAL any-hit shader (empty = none) — the hit group gains it, so alpha-tested geometry works without a
    // hardware opacity micromap (the P4 OMM fallback authored in CKIR).
    [[nodiscard]] bool trace_rays_pipeline(const RtScene& scene, crd::containers::ConstSpan<crd::u8> rgen,
                                           crd::containers::ConstSpan<crd::u8> rmiss, crd::containers::ConstSpan<crd::u8> rchit,
                                           crd::containers::ConstSpan<Binding> bindings, crd::u32 width, crd::u32 height,
                                           crd::containers::ConstSpan<crd::u8> rahit = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::gpu
