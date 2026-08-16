// test_vulkan_rt.cpp — D-007 RT-1: the inline ray-query VERTICAL end-to-end on Vulkan. Builds a hardware BLAS+TLAS from a
// triangle soup (VulkanRayTracingContext), dispatches the CKIR-emitted inline-rayQuery compute kernel against it, and checks
// the GPU closest-hit distances against the CPU brute-force ray-triangle oracle (eval_cpu_kernel). RT traversal is not
// bit-exact across vendors, so the gate is GEOMETRIC tolerance — the honest RT correctness contract.

#include <crd/gpu/context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_ray_tracing_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_lss.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_rt.hpp>
#include <crd/kir/ckir_asset.hpp> // CEIR-19c: load the AUTHORED rt_witness.ckir (ckir_read)

// CEIR-19c: the ceir.rt->gpu execution bridge under test — an authored ceir.rt program lowered + driven onto this
// VulkanRayTracingContext through the caller hooks (crd-ceir-gpu names no backend).
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/rt.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream> // CEIR-19c: read the committed .ckir asset

namespace gpu  = crd::gpu;
namespace kir  = crd::kir;
namespace ceir = crd::ceir;      // CEIR-19c
namespace cg   = crd::ceir::gpu; // CEIR-19c: lower_region / execute_rt_lowered / RtHooks

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace
{
// The one-thread-per-ray inline-ray-query kernel: TLAS at binding 0, rays (6 floats each) at binding 1, out distance at 2.
// REN-38: `n_rays` bounds the tail threads (a dispatch rounds up); 0 keeps the historical unguarded shape
kir::KEntry build_trace_kernel(kir::KGraph& g, int local_size, crd::u32 n_rays = 0U)
{
    const kir::Shape sh1 = kir::make_shape({1});
    const auto       cf  = [&](double v) { return g.constant(v, sh1, kir::DType::F32); };
    const auto       cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, kir::DType::U32); };
    const int        as   = g.accel_struct_decl(0, 0);
    const int        rays = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int        out  = g.buffer_decl(kir::DType::F32, 0, 2, true);
    const int        mark = g.kernel_stmt_mark();
    const int        tid  = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(local_size))), g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int        guard = n_rays > 0U ? g.stmt_if_begin(g.binary(kir::KOp::CmpLt, tid, cu(n_rays))) : -1;
    const int        base = g.binary(kir::KOp::Mul, tid, cu(6U));
    const auto       ld   = [&](crd::u32 k) { return g.buffer_load(rays, g.binary(kir::KOp::Add, base, cu(k))); };
    const int        t    = g.trace_ray_closest(as, ld(0), ld(1), ld(2), ld(3), ld(4), ld(5), cf(0.001), cf(1.0e30));
    g.stmt_buffer_store(out, tid, t);
    if (guard >= 0) { g.stmt_if_end(guard); }
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
} // namespace

TEST_CASE("D-007 RT-1: inline rayQuery on Vulkan (GPU BLAS/TLAS build + trace) == CPU ray-triangle reference",
          "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping the RT trace"); return; }

    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    constexpr crd::u32 num_tris = 1U;
    constexpr crd::u32 num_rays = 4U;
    const int          local  = 64;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_trace_kernel(g, local, num_rays);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_trace", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // The triangle (z=2) — GPU wants raw float3 verts; the CPU oracle wants [triCount, v0.xyz, v1.xyz, v2.xyz].
    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    const float rays[num_rays][6] = {
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},  // hit at t=2
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, -1.0F}, // miss
        {5.0F, 5.0F, 0.0F, 0.0F, 0.0F, 1.0F},  // miss (outside)
        {0.1F, 0.1F, 1.0F, 0.0F, 0.0F, 1.0F},  // hit at t=1
    };

    // ── CPU oracle (the reference) ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tris);
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> rays64(&alloc);
    rays64.resize(static_cast<crd::usize>(num_rays) * 6U, 0.0);
    for (crd::u32 r = 0; r < num_rays; ++r)
    {
        for (int c = 0; c < 6; ++c) { rays64[static_cast<crd::usize>(r) * 6U + static_cast<crd::usize>(c)] = static_cast<crd::f64>(rays[r][c]); }
    }
    crd::containers::Array<crd::f64> ref(&alloc);
    ref.resize(num_rays, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {rays64.data(), static_cast<int>(rays64.size()), 0, 1}, {ref.data(), static_cast<int>(num_rays), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, static_cast<crd::u32>(local), &alloc, 1U);

    // ── GPU: build the AS + trace ──
    auto scene = rt.build_scene(verts, num_tris);
    REQUIRE(scene != nullptr);
    float gpu_t[num_rays] = {0.0F, 0.0F, 0.0F, 0.0F};
    gpu::VulkanRayTracingContext::Binding bind[2] = {
        {&rays[0][0], nullptr, static_cast<crd::u64>(num_rays) * 6U * sizeof(float), 1U}, // rays in at binding 1
        {nullptr, gpu_t, static_cast<crd::u64>(num_rays) * sizeof(float), 2U},            // distances out at binding 2
    };
    const crd::u32 groups = (num_rays + static_cast<crd::u32>(local) - 1U) / static_cast<crd::u32>(local);
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), groups));

    for (crd::u32 r = 0; r < num_rays; ++r)
    {
        INFO("ray " << r << ": GPU t=" << gpu_t[r] << "  CPU ref=" << ref[r]);
        const bool cpu_hit = ref[r] < 1.0e29;
        const bool gpu_hit = static_cast<double>(gpu_t[r]) < 1.0e29;
        CHECK(gpu_hit == cpu_hit); // same hit/miss decision
        if (cpu_hit) { CHECK(crd::math::abs(static_cast<double>(gpu_t[r]) - ref[r]) < 1.0e-3); } // hit distance within tolerance
    }
}

// D-007 RT-2: RT HARD SHADOWS — the first real RT rendering effect on the inline primitive. An occluder floats at y=2 between
// a grid of ground points (y=0) and a light at (0,5,0); a shadow ray per point tests occlusion. GPU visibility vs the CPU
// ray-triangle oracle. This is the direct RT visibility leaf the B14 ReSTIR/GI shadow term consumes.
TEST_CASE("D-007 RT-2: RT hard shadows on Vulkan (per-point shadow ray) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::rt::RtShadowConfig    scfg;
    scfg.light[0] = 0.0F; scfg.light[1] = 5.0F; scfg.light[2] = 0.0F;
    scfg.local_size = 64U;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_rt_shadow_kernel(g, scfg);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_shadow", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // occluder: one triangle at y=2 over the origin (blocks the light for points near x=z=0).
    const float verts[9] = {-1.0F, 2.0F, -1.0F, 1.0F, 2.0F, -1.0F, 0.0F, 2.0F, 1.5F};
    constexpr crd::u32 num_pts = 64U; // 8x8 grid of ground points
    crd::containers::Array<float> pos(&alloc);
    pos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j)
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            pos[p * 3U + 0U] = -3.5F + 1.0F * static_cast<float>(i); // x ∈ [-3.5, 3.5]
            pos[p * 3U + 1U] = 0.0F;                                 // ground plane
            pos[p * 3U + 2U] = -3.5F + 1.0F * static_cast<float>(j); // z ∈ [-3.5, 3.5]
        }
    }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = 1.0;
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc);
    pos64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    for (crd::usize i = 0; i < pos.size(); ++i) { pos64[i] = static_cast<crd::f64>(pos[i]); }
    crd::containers::Array<crd::f64> refv(&alloc);
    refv.resize(num_pts, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {refv.data(), static_cast<int>(num_pts), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, scfg.local_size, &alloc, (num_pts + scfg.local_size - 1U) / scfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, 1U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_v(&alloc);
    gpu_v.resize(num_pts, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[2] = {
        {pos.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 1U},
        {nullptr, gpu_v.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), (num_pts + scfg.local_size - 1U) / scfg.local_size));

    int lit = 0;
    int shadowed = 0;
    int mism = 0;
    for (crd::u32 p = 0; p < num_pts; ++p)
    {
        if (crd::math::abs(static_cast<double>(gpu_v[p]) - refv[p]) > 1.0e-6) { ++mism; }
        if (refv[p] > 0.5) { ++lit; } else { ++shadowed; }
    }
    INFO("shadow: mismatches=" << mism << "  lit=" << lit << "  shadowed=" << shadowed);
    CHECK(mism == 0);       // GPU visibility == CPU ray-triangle reference for every point
    CHECK(lit > 0);         // ...on a non-trivial scene: some points see the light
    CHECK(shadowed > 0);    // ...and some are occluded (otherwise the test proves nothing)
}

// D-007 RT-2: RT AMBIENT OCCLUSION — the batch-of-rays-per-pixel effect (the path-tracing loop pattern). A ground grid under a
// floating occluder quad; each point casts a cosine-hemisphere batch of rays and returns ambient visibility. GPU AO vs the CPU
// ray-triangle oracle (deterministic sampling ⇒ they agree per-ray except at grazing edges); the scene shows real spatial AO.
TEST_CASE("D-007 RT-2: RT ambient occlusion on Vulkan (hemisphere batch trace) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::rt::RtaoConfig        acfg;
    acfg.samples    = 32U;
    acfg.radius     = 3.0F;
    acfg.local_size = 64U;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_rtao_kernel(g, acfg);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_ao", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // occluder: a quad (2 triangles) at y=2 over x,z ∈ [-2,2].
    const float verts[18] = {-2.0F, 2.0F, -2.0F, 2.0F, 2.0F, -2.0F, 2.0F, 2.0F, 2.0F,
                             -2.0F, 2.0F, -2.0F, 2.0F, 2.0F, 2.0F, -2.0F, 2.0F, 2.0F};
    constexpr crd::u32 num_tri = 2U;
    constexpr crd::u32 num_pts    = 64U;
    crd::containers::Array<float> pos(&alloc);
    crd::containers::Array<float> nrm(&alloc);
    pos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    nrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j)
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            pos[p * 3U + 0U] = -4.0F + 8.0F / 7.0F * static_cast<float>(i); // x ∈ [-4, 4]
            pos[p * 3U + 1U] = 0.0F;
            pos[p * 3U + 2U] = -4.0F + 8.0F / 7.0F * static_cast<float>(j); // z ∈ [-4, 4]
            nrm[p * 3U + 1U] = 1.0F;                                        // normal straight up
        }
    }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + static_cast<crd::usize>(num_tri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tri);
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    pos64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    for (crd::usize i = 0; i < pos.size(); ++i) { pos64[i] = static_cast<crd::f64>(pos[i]); nrm64[i] = static_cast<crd::f64>(nrm[i]); }
    crd::containers::Array<crd::f64> refv(&alloc);
    refv.resize(num_pts, 0.0);
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {refv.data(), static_cast<int>(num_pts), 0, 3}};
    kir::eval_cpu_kernel(g, e, bufs, 4, acfg.local_size, &alloc, (num_pts + acfg.local_size - 1U) / acfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, num_tri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_ao(&alloc);
    gpu_ao.resize(num_pts, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[3] = {
        {pos.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 1U},
        {nrm.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 2U},
        {nullptr, gpu_ao.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 3U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 3), (num_pts + acfg.local_size - 1U) / acfg.local_size));

    double worst   = 0.0;
    double sum_gpu = 0.0;
    double sum_ref = 0.0;
    double ao_min  = 1.0;
    double ao_max  = 0.0;
    for (crd::u32 p = 0; p < num_pts; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_ao[p]) - refv[p]);
        if (d > worst) { worst = d; }
        sum_gpu += static_cast<double>(gpu_ao[p]);
        sum_ref += refv[p];
        if (refv[p] < ao_min) { ao_min = refv[p]; }
        if (refv[p] > ao_max) { ao_max = refv[p]; }
    }
    INFO("RTAO: worst |GPU-ref|=" << worst << "  mean GPU=" << sum_gpu / num_pts << " ref=" << sum_ref / num_pts << "  AO range=[" << ao_min << ", " << ao_max << "]");
    CHECK(worst < 0.12);                                    // per-point AO matches (deterministic sampling; ≤~3/32 grazing-edge ray flips)
    CHECK(crd::math::abs(sum_gpu - sum_ref) / num_pts < 0.02);   // ...and the mean is tight (edge noise averages out)
    // physics: a quad at y=2 over x,z∈[-2,2] subtends ~45° from a point below ⇒ a cosine-weighted hemisphere hits it ~50% of
    // the time ⇒ the darkest AO is ~0.5; far-corner points are fully open (~1.0).
    CHECK(ao_min < 0.65);                                   // real occlusion under the quad
    CHECK(ao_max > 0.9);                                    // open points bright
    CHECK(ao_max - ao_min > 0.25);                          // a genuine AO gradient across the surface
}

// D-007 RT-2: RT REFLECTIONS — the RICH-HIT effect (reflected ray → hit → fetch geometry → shade). A reflective floor under a
// lit ceiling quad: each floor point reflects the view about its normal, traces, and either shades the ceiling it reflects
// (via the hit's primId) or sees the sky. GPU reflection colour vs the CPU ray-triangle+primId oracle. This is the last
// primitive a path-tracing bounce / ReSTIR GI needs.
TEST_CASE("D-007 RT-2: RT reflections on Vulkan (reflected ray + primId shade) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::rt::RtReflectConfig    rcfg;
    rcfg.view[0] = 0.0F; rcfg.view[1] = -1.0F; rcfg.view[2] = 0.0F; // looking straight down ⇒ reflection goes straight up
    rcfg.light[0] = 0.577F; rcfg.light[1] = 0.577F; rcfg.light[2] = 0.577F;
    rcfg.albedo[0] = 0.8F; rcfg.albedo[1] = 0.5F; rcfg.albedo[2] = 0.3F;
    rcfg.ntri = 2U;
    rcfg.local_size = 64U;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_rt_reflection_kernel(g, rcfg);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_reflect", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // ceiling quad (2 triangles) at y=3 over x,z∈[-2,2]; its SHADING normals face up (toward the light) so reflections are lit.
    const float verts[18] = {-2.0F, 3.0F, -2.0F, 2.0F, 3.0F, -2.0F, 2.0F, 3.0F, 2.0F,
                             -2.0F, 3.0F, -2.0F, 2.0F, 3.0F, 2.0F, -2.0F, 3.0F, 2.0F};
    const float tri_n[6] = {0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F}; // per-triangle flat normal (up)
    constexpr crd::u32 num_tri = 2U;
    constexpr crd::u32 num_pts    = 64U;
    crd::containers::Array<float> pos(&alloc);
    crd::containers::Array<float> nrm(&alloc);
    pos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    nrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j)
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            pos[p * 3U + 0U] = -4.0F + 8.0F / 7.0F * static_cast<float>(i);
            pos[p * 3U + 2U] = -4.0F + 8.0F / 7.0F * static_cast<float>(j);
            nrm[p * 3U + 1U] = 1.0F; // floor normal up
        }
    }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + static_cast<crd::usize>(num_tri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tri);
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    crd::containers::Array<crd::f64> tn64(&alloc);
    pos64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    tn64.resize(6U, 0.0);
    for (crd::usize i = 0; i < pos.size(); ++i) { pos64[i] = static_cast<crd::f64>(pos[i]); nrm64[i] = static_cast<crd::f64>(nrm[i]); }
    for (int i = 0; i < 6; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    crd::containers::Array<crd::f64> refc(&alloc);
    refc.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, rcfg.local_size, &alloc, (num_pts + rcfg.local_size - 1U) / rcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, num_tri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[4] = {
        {pos.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 1U},
        {nrm.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 2U},
        {tri_n, nullptr, 6U * sizeof(float), 3U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 4U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 4), (num_pts + rcfg.local_size - 1U) / rcfg.local_size));

    double worst   = 0.0;
    int    hit_col = 0;
    int    sky_col = 0;
    for (crd::u32 p = 0; p < num_pts * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
    }
    for (crd::u32 p = 0; p < num_pts; ++p) // classify by the blue channel: sky (~0.8) vs shaded ceiling (~0.2)
    {
        if (refc[p * 3U + 2U] > 0.6) { ++sky_col; } else { ++hit_col; }
    }
    INFO("reflect: worst |GPU-ref|=" << worst << "  hit(ceiling)=" << hit_col << "  miss(sky)=" << sky_col);
    CHECK(worst < 1.0e-3); // GPU reflection colour == CPU ray-triangle+primId reference (deterministic hit shading)
    CHECK(hit_col > 0);    // some points reflect the ceiling (rich-hit shading path taken)
    CHECK(sky_col > 0);    // ...and some reflect the sky (miss path) — a real reflection, not a constant
}

// D-007 RT-2: PATH-TRACING MEGAKERNEL — the full light-transport integrator. Floor points shoot multi-bounce diffuse paths; a
// ceiling quad blocks/bounces the sky light, so points under it are dimmer + colour-attenuated (real GI). GPU radiance vs the
// CPU path-tracer oracle (deterministic sampling ⇒ agreement to transcendental ULP, bar the odd grazing-edge ray).
TEST_CASE("D-007 RT-2: path-tracing megakernel on Vulkan (multi-bounce diffuse GI) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::rt::PathTraceConfig   pcfg;
    pcfg.samples = 16U;
    pcfg.bounces = 3U;
    pcfg.albedo[0] = 0.4F; pcfg.albedo[1] = 0.4F; pcfg.albedo[2] = 0.4F;
    pcfg.ntri = 2U;
    pcfg.local_size = 64U;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_pathtrace_kernel(g, pcfg);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_pt", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // LOW, HALF-COVERING ceiling (2 tris) at y=1 over x∈[-6,0], z∈[-6,6], normals DOWN. It subtends most of the hemisphere for
    // floor points on the LEFT (x<0) — those are strongly shadowed/attenuated — while RIGHT-half points (x>0) see open sky.
    // The coverage contrast is what makes multi-bounce GI show up as clear spatial variation in the radiance.
    const float verts[18] = {-6.0F, 1.0F, -6.0F, 0.0F, 1.0F, -6.0F, 0.0F, 1.0F, 6.0F,
                             -6.0F, 1.0F, -6.0F, 0.0F, 1.0F, 6.0F, -6.0F, 1.0F, 6.0F};
    const float tri_n[6] = {0.0F, -1.0F, 0.0F, 0.0F, -1.0F, 0.0F};
    constexpr crd::u32 num_tri = 2U;
    constexpr crd::u32 num_pts    = 64U;
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j)
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            ppos[p * 3U + 0U] = -6.0F + 12.0F / 7.0F * static_cast<float>(i);
            ppos[p * 3U + 2U] = -6.0F + 12.0F / 7.0F * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + static_cast<crd::usize>(num_tri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tri);
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    crd::containers::Array<crd::f64> tn64(&alloc);
    pos64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    tn64.resize(6U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 6; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    crd::containers::Array<crd::f64> refc(&alloc);
    refc.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, (num_pts + pcfg.local_size - 1U) / pcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, num_tri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[4] = {
        {ppos.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 1U},
        {pnrm.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 2U},
        {tri_n, nullptr, 6U * sizeof(float), 3U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 4U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 4), (num_pts + pcfg.local_size - 1U) / pcfg.local_size));

    double worst = 0.0;
    double sumd  = 0.0;
    double lmin  = 10.0;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < num_pts * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
        sumd += d;
    }
    for (crd::u32 p = 0; p < num_pts; ++p) // luminance of the ref radiance, to check GI spatial variation
    {
        const double lum = 0.3 * refc[p * 3U] + 0.6 * refc[p * 3U + 1U] + 0.1 * refc[p * 3U + 2U];
        if (lum < lmin) { lmin = lum; }
        if (lum > lmax) { lmax = lum; }
    }
    INFO("pathtrace: worst |GPU-ref|=" << worst << "  mean|Δ|=" << sumd / (num_pts * 3U) << "  radiance lum range=[" << lmin << ", " << lmax << "]");
    CHECK(worst < 0.06);              // GPU path radiance == CPU path-tracer oracle (transcendental ULP + rare grazing flips)
    CHECK(sumd / (num_pts * 3U) < 0.004);  // ...the bulk matches tightly
    CHECK(lmax - lmin > 0.15);        // real GI: points under the ceiling are darker than open points (multi-bounce transport)
}

// D-007 RT-4: NEE + MIS AREA-LIGHT PATH TRACER — direct lighting done right. A small occluder between the floor and a
// rectangular area light casts a SOFT SHADOW (penumbra); the MIS estimator combines light-sampling + BSDF-sampling. GPU MIS
// radiance vs the CPU oracle (deterministic ⇒ agreement to transcendental ULP, bar rare grazing shadow-ray flips at the
// penumbra edge), and the shadow must be real (clear lit-vs-shadowed spatial variation across the floor).
TEST_CASE("D-007 RT-4: NEE+MIS area-light path tracer on Vulkan (soft shadows) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator     alloc(48U << 20U);
    kir::rt::PathTraceNeeConfig    pcfg;
    pcfg.samples     = 32U;
    pcfg.bounces     = 2U;
    pcfg.albedo[0]   = 0.6F; pcfg.albedo[1] = 0.6F; pcfg.albedo[2] = 0.6F;
    pcfg.light_p0[0] = -1.5F; pcfg.light_p0[1] = 3.0F; pcfg.light_p0[2] = -1.5F;
    pcfg.light_eu[0] = 3.0F;  pcfg.light_eu[1] = 0.0F; pcfg.light_eu[2] = 0.0F;
    pcfg.light_ev[0] = 0.0F;  pcfg.light_ev[1] = 0.0F; pcfg.light_ev[2] = 3.0F;
    pcfg.light_nl[0] = 0.0F;  pcfg.light_nl[1] = -1.0F; pcfg.light_nl[2] = 0.0F;
    pcfg.light_le[0] = 8.0F;  pcfg.light_le[1] = 8.0F;  pcfg.light_le[2] = 8.0F;
    pcfg.ntri        = 4U;
    pcfg.light_prim0 = 2U;
    pcfg.light_ntri  = 2U;
    pcfg.strategy    = kir::rt::PtStrategy::Mis;
    pcfg.local_size  = 64U;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_pathtrace_nee_kernel(g, pcfg);
    kir::GlslKernel   kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_nee", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // AS geometry: occluder quad (prims 0-1, 1×1 at y=2) + light quad (prims 2-3, 3×3 at y=3, matching pcfg).
    const float verts[36] = {
        -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F,   // occluder tri 0
        -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, 0.5F,   // occluder tri 1
        -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F,   // light tri 2
        -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, 1.5F};  // light tri 3
    const float tri_n[12] = {0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, -1.0F, 0.0F};
    constexpr crd::u32 num_tri = 4U;
    constexpr crd::u32 num_pts    = 64U;
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j) // 8×8 floor grid over [-3,3], normals up
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / 7.0F * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / 7.0F * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + static_cast<crd::usize>(num_tri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tri);
    for (int i = 0; i < 36; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    crd::containers::Array<crd::f64> tn64(&alloc);
    crd::containers::Array<crd::f64> refc(&alloc);
    pos64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    tn64.resize(12U, 0.0);
    refc.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 12; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, (num_pts + pcfg.local_size - 1U) / pcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, num_tri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[4] = {
        {ppos.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 1U},
        {pnrm.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 2U},
        {tri_n, nullptr, 12U * sizeof(float), 3U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 4U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 4), (num_pts + pcfg.local_size - 1U) / pcfg.local_size));

    double worst = 0.0;
    double sumd  = 0.0;
    double lmin  = 1.0e30;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < num_pts * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
        sumd += d;
    }
    for (crd::u32 p = 0; p < num_pts; ++p)
    {
        const double v = refc[p * 3U];
        if (v < lmin) { lmin = v; }
        if (v > lmax) { lmax = v; }
    }
    INFO("nee/mis: worst |GPU-ref|=" << worst << "  mean|Δ|=" << sumd / (num_pts * 3U) << "  radiance range=[" << lmin << ", " << lmax << "]");
    CHECK(worst < 0.05);             // GPU MIS radiance == CPU oracle (transcendental ULP + rare grazing shadow-ray flips)
    CHECK(sumd / (num_pts * 3U) < 0.003); // ...the bulk matches tightly
    CHECK(lmin >= 0.0);              // radiance is non-negative everywhere (no MIS-weight sign error)
    CHECK(lmax - lmin > 0.10);       // a REAL soft shadow: floor under the occluder is measurably darker than the lit floor
}

// D-007 RT-5: ReSTIR DI on Vulkan — the RIS reservoir (streaming WRS over M light candidates + one visibility ray for the
// survivor) shading the floor under an occluder. GPU vs the CPU reservoir oracle (deterministic hashing + WRS ⇒ agreement to
// ULP, bar rare grazing shadow-ray flips), and the reservoir must produce a real soft shadow (spatial variation).
TEST_CASE("D-007 RT-5: ReSTIR DI RIS reservoir on Vulkan (soft shadows) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(48U << 20U);
    kir::rt::RestirDiConfig    rcfg;
    rcfg.frames     = 16U;
    rcfg.candidates = 16U;
    rcfg.albedo[0]  = 0.6F; rcfg.albedo[1] = 0.6F; rcfg.albedo[2] = 0.6F;
    rcfg.light_p0[0] = -1.5F; rcfg.light_p0[1] = 3.0F; rcfg.light_p0[2] = -1.5F;
    rcfg.light_eu[0] = 3.0F;  rcfg.light_eu[1] = 0.0F; rcfg.light_eu[2] = 0.0F;
    rcfg.light_ev[0] = 0.0F;  rcfg.light_ev[1] = 0.0F; rcfg.light_ev[2] = 3.0F;
    rcfg.light_nl[0] = 0.0F;  rcfg.light_nl[1] = -1.0F; rcfg.light_nl[2] = 0.0F;
    rcfg.light_le[0] = 8.0F;  rcfg.light_le[1] = 8.0F;  rcfg.light_le[2] = 8.0F;
    rcfg.local_size  = 64U;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_restir_di_kernel(g, rcfg);
    kir::GlslKernel   kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_restir", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // occluder quad (prims 0-1, 1×1 at y=2) + light quad (prims 2-3, 3×3 at y=3, matching rcfg).
    const float verts[36] = {
        -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, 0.5F,
        -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, 1.5F};
    constexpr crd::u32 num_tri = 4U;
    constexpr crd::u32 num_pts    = 64U;
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j)
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / 7.0F * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / 7.0F * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + static_cast<crd::usize>(num_tri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tri);
    for (int i = 0; i < 36; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    crd::containers::Array<crd::f64> refc(&alloc);
    pos64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    refc.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {refc.data(), static_cast<int>(refc.size()), 0, 3}};
    kir::eval_cpu_kernel(g, e, bufs, 4, rcfg.local_size, &alloc, (num_pts + rcfg.local_size - 1U) / rcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, num_tri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[3] = {
        {ppos.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 1U},
        {pnrm.data(), nullptr, static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 2U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(num_pts) * 3U * sizeof(float), 3U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 3), (num_pts + rcfg.local_size - 1U) / rcfg.local_size));

    double worst = 0.0;
    double sumd  = 0.0;
    double lmin  = 1.0e30;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < num_pts * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
        sumd += d;
    }
    for (crd::u32 p = 0; p < num_pts; ++p)
    {
        const double v = refc[p * 3U];
        if (v < lmin) { lmin = v; }
        if (v > lmax) { lmax = v; }
    }
    INFO("restir-di: worst |GPU-ref|=" << worst << "  mean|Δ|=" << sumd / (num_pts * 3U) << "  radiance range=[" << lmin << ", " << lmax << "]");
    CHECK(worst < 0.05);             // GPU ReSTIR radiance == CPU reservoir oracle (deterministic WRS; rare grazing shadow flips)
    CHECK(sumd / (num_pts * 3U) < 0.004); // ...the bulk matches tightly
    CHECK(lmin >= 0.0);              // non-negative everywhere (no reservoir-weight sign error)
    CHECK(lmax - lmin > 0.10);       // a REAL soft shadow from the reservoir's visibility ray
}

// D-007 RT-5b: ReSTIR DI TEMPORAL reuse on Vulkan — the persistent-reservoir pipeline (temporal-combine pass → shade pass),
// ping-ponged across frames on the GPU. Two gold-standard properties: (1) UNBIASED — the per-frame mean converges to the same
// direct lighting as a converged single-pass reference; (2) VARIANCE REDUCTION — a warmed-up temporal frame (accumulated
// effective samples) is far closer to the reference than single-pass RIS at the SAME per-frame candidate budget. That variance
// win at fixed 1-spp cost is the entire point of ReSTIR.
TEST_CASE("D-007 RT-5b: ReSTIR DI temporal reuse on Vulkan (unbiased + variance reduction)", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(64U << 20U);
    constexpr crd::u32 img_w = 16U; // 256 pixels = 4×64 (exact workgroup cover, no bounds guard needed)
    constexpr crd::u32 img_h = 16U;
    constexpr crd::u32 num_pts = img_w * img_h;

    // scene: JUST the area light (no occluder) — a fully-visible light so V=1 everywhere and the estimator variance is pure
    // area-sampling noise. That isolates what TEMPORAL reuse fixes (accumulating effective samples ⇒ crushing sampling noise);
    // the shadow-boundary variance an occluder adds is the SPATIAL-reuse story, exercised in the spatiotemporal test.
    const float verts[18] = {-1.5F, 3.0F, -1.5F, 1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F,
                             -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, 1.5F};
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < img_h; ++j)
    {
        for (crd::u32 i = 0; i < img_w; ++i)
        {
            const crd::u32 p = j * img_w + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(img_w - 1U) * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(img_h - 1U) * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }
    auto scene = rt.build_scene(verts, 2U);
    REQUIRE(scene != nullptr);

    const auto compile = [&](const kir::KEntry& e, kir::KGraph& g, const char* tag) {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), tag, &alloc);
        INFO(spv.error_message.c_str());
        REQUIRE(spv.ok);
        return spv;
    };
    const auto rms = [&](const crd::containers::Array<float>& a, const crd::containers::Array<float>& b) {
        double s = 0.0;
        for (crd::usize i = 0; i < a.size(); ++i) { const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]); s += d * d; }
        return crd::math::sqrt(s / static_cast<double>(a.size()));
    };
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pos_bytes = static_cast<crd::u64>(num_pts) * 3U * sizeof(float);
    const crd::u64 res_bytes = static_cast<crd::u64>(num_pts) * static_cast<crd::u64>(kir::rt::kRestirReservoirStride) * sizeof(float);

    // ── converged REFERENCE: single-pass ReSTIR DI, 256 frames of M=16 (we proved this estimator unbiased vs NEE) ──
    crd::containers::Array<float> ref(&alloc);
    ref.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    {
        kir::rt::RestirDiConfig rc;
        rc.frames = 256U; rc.candidates = 16U; rc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_restir_di_kernel(g, rc);
        const auto spv = compile(e, g, "ref");
        B bind[3] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U}, {nullptr, ref.data(), pos_bytes, 3U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                  crd::containers::ConstSpan<B>(bind, 3), num_pts / 64U));
    }

    // ── single-pass BASELINE (no reuse): 1 frame of M=2 — the noisy per-frame image temporal reuse must beat ──
    crd::containers::Array<float> single(&alloc);
    single.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    {
        kir::rt::RestirDiConfig rc;
        rc.frames = 1U; rc.candidates = 2U; rc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_restir_di_kernel(g, rc);
        const auto spv = compile(e, g, "single");
        B bind[3] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U}, {nullptr, single.data(), pos_bytes, 3U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                  crd::containers::ConstSpan<B>(bind, 3), num_pts / 64U));
    }

    // ── TEMPORAL ping-pong: build the temporal + shade kernels once, dispatch F frames feeding rprev←rcur each frame ──
    kir::rt::RestirStConfig sc;
    sc.width = img_w; sc.height = img_h; sc.m_initial = 2U; sc.local_size = 64U;
    kir::KGraph gt(&alloc);
    kir::KGraph gs(&alloc);
    const kir::KEntry et = kir::rt::build_restir_temporal_kernel(gt, sc);
    const kir::KEntry es = kir::rt::build_restir_shade_kernel(gs, sc);
    const auto tspv = compile(et, gt, "temporal");
    const auto sspv = compile(es, gs, "shade");

    crd::containers::Array<float> rprev(&alloc);
    crd::containers::Array<float> rcur(&alloc);
    crd::containers::Array<float> rad(&alloc);
    crd::containers::Array<float> accum(&alloc);
    crd::containers::Array<float> last(&alloc);
    rprev.resize(static_cast<crd::usize>(num_pts) * kir::rt::kRestirReservoirStride, 0.0F); // frame 0 history = empty (M=0)
    rcur.resize(rprev.size(), 0.0F);
    rad.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    accum.resize(rad.size(), 0.0F);
    last.resize(rad.size(), 0.0F);
    constexpr crd::u32 n_frames = 48U;
    for (crd::u32 f = 0; f < n_frames; ++f)
    {
        crd::u32 frame = f;
        B tb[5] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U},
                   {rprev.data(), nullptr, res_bytes, 3U}, {nullptr, rcur.data(), res_bytes, 4U},
                   {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()),
                                  crd::containers::ConstSpan<B>(tb, 5), num_pts / 64U));
        B sb[4] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U},
                   {rcur.data(), nullptr, res_bytes, 3U}, {nullptr, rad.data(), pos_bytes, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()),
                                  crd::containers::ConstSpan<B>(sb, 4), num_pts / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { accum[i] += rad[i]; }
        for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rcur[i]; } // temporal feedback
        if (f == n_frames - 1U) { for (crd::usize i = 0; i < rad.size(); ++i) { last[i] = rad[i]; } }
    }
    crd::containers::Array<float> mean(&alloc);
    mean.resize(rad.size(), 0.0F);
    for (crd::usize i = 0; i < mean.size(); ++i) { mean[i] = accum[i] / static_cast<float>(n_frames); }

    const auto spatial_mean = [&](const crd::containers::Array<float>& a) {
        double s = 0.0;
        for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); }
        return s / static_cast<double>(a.size());
    };
    const double ref_mean   = spatial_mean(ref);
    const double t_mean     = spatial_mean(mean); // frame-averaged temporal image, spatially averaged
    const double err_last   = rms(last, ref);    // per-pixel noise of a single warmed-up frame (1-spp — inherently noisy)
    const double err_single = rms(single, ref);  // per-pixel noise of single-pass RIS at the same M
    INFO("temporal ReSTIR: ref_mean=" << ref_mean << "  t_mean=" << t_mean << "  rms(warm-ref)=" << err_last << "  rms(single-ref)=" << err_single);
    // UNBIASED: the spatial mean (bias shows as a systematic offset; per-pixel Monte-Carlo noise cancels in the spatial average).
    CHECK(ref_mean > 0.05);                        // the reference actually has lit direct illumination
    CHECK(crd::math::abs(t_mean - ref_mean) / ref_mean < 0.03);
    // VARIANCE REDUCTION: a warmed-up temporal frame is markedly less noisy than single-pass RIS at the SAME per-frame budget —
    // the whole point of ReSTIR (the residual noise is what the denoiser then cleans up).
    CHECK(err_last < err_single * 0.6);
}

// D-007 RT-5c: FULL SPATIOTEMPORAL ReSTIR DI on Vulkan — the complete 3-pass pipeline (temporal-combine → spatial-neighbour
// resampling with unbiased Z-normalisation → shade), ping-ponged across frames, on a scene WITH an occluder (soft shadow). This
// is where SPATIAL reuse earns its keep: near the shadow boundary, temporal reuse alone hits a per-pixel visibility-variance
// floor, but sharing samples across neighbours lets a pixel find a visible light sample, driving the noise below that floor.
// Verifies unbiasedness (spatial mean == converged reference) and a large variance win over single-pass RIS despite the shadow.
TEST_CASE("D-007 RT-5c: full spatiotemporal ReSTIR DI on Vulkan (soft shadow, unbiased + variance win)", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(80U << 20U);
    constexpr crd::u32 img_w = 16U;
    constexpr crd::u32 img_h = 16U;
    constexpr crd::u32 num_pts = img_w * img_h;

    // occluder (prims 0-1, 1×1 at y=2) + area light (prims 2-3, 3×3 at y=3).
    const float verts[36] = {
        -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, 0.5F,
        -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, 1.5F};
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < img_h; ++j)
    {
        for (crd::u32 i = 0; i < img_w; ++i)
        {
            const crd::u32 p = j * img_w + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(img_w - 1U) * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(img_h - 1U) * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }
    auto scene = rt.build_scene(verts, 4U);
    REQUIRE(scene != nullptr);

    const auto compile = [&](const kir::KEntry& e, kir::KGraph& g, const char* tag) {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), tag, &alloc);
        INFO(spv.error_message.c_str());
        REQUIRE(spv.ok);
        return spv;
    };
    const auto rms = [&](const crd::containers::Array<float>& a, const crd::containers::Array<float>& b) {
        double s = 0.0;
        for (crd::usize i = 0; i < a.size(); ++i) { const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]); s += d * d; }
        return crd::math::sqrt(s / static_cast<double>(a.size()));
    };
    const auto spatial_mean = [&](const crd::containers::Array<float>& a) {
        double s = 0.0;
        for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); }
        return s / static_cast<double>(a.size());
    };
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pos_bytes = static_cast<crd::u64>(num_pts) * 3U * sizeof(float);
    const crd::u64 res_bytes = static_cast<crd::u64>(num_pts) * static_cast<crd::u64>(kir::rt::kRestirReservoirStride) * sizeof(float);

    // converged reference (single-pass ReSTIR DI, 256×M16) + noisy single-pass baseline (1×M2), both WITH the occluder's shadow.
    crd::containers::Array<float> ref(&alloc);
    crd::containers::Array<float> single(&alloc);
    ref.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    single.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (int pass = 0; pass < 2; ++pass)
    {
        kir::rt::RestirDiConfig rc;
        rc.frames = pass == 0 ? 256U : 1U; rc.candidates = pass == 0 ? 16U : 2U; rc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_restir_di_kernel(g, rc);
        const auto spv = compile(e, g, pass == 0 ? "ref" : "single");
        auto& dst = pass == 0 ? ref : single;
        B bind[3] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U}, {nullptr, dst.data(), pos_bytes, 3U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                  crd::containers::ConstSpan<B>(bind, 3), num_pts / 64U));
    }

    // build the 3 spatiotemporal passes once.
    kir::rt::RestirStConfig sc;
    sc.width = img_w; sc.height = img_h; sc.m_initial = 2U; sc.spatial_k = 5U; sc.spatial_radius = 3.0F; sc.local_size = 64U;
    kir::KGraph gt(&alloc);
    kir::KGraph gsp(&alloc);
    kir::KGraph gsh(&alloc);
    const kir::KEntry et  = kir::rt::build_restir_temporal_kernel(gt, sc);
    const kir::KEntry esp = kir::rt::build_restir_spatial_kernel(gsp, sc);
    const kir::KEntry esh = kir::rt::build_restir_shade_kernel(gsh, sc);
    const auto tspv = compile(et, gt, "st_temporal");
    const auto pspv = compile(esp, gsp, "st_spatial");
    const auto sspv = compile(esh, gsh, "st_shade");

    crd::containers::Array<float> rprev(&alloc);
    crd::containers::Array<float> rtmp(&alloc);
    crd::containers::Array<float> rspa(&alloc);
    crd::containers::Array<float> rad(&alloc);
    crd::containers::Array<float> accum(&alloc);
    crd::containers::Array<float> last(&alloc);
    rprev.resize(static_cast<crd::usize>(num_pts) * kir::rt::kRestirReservoirStride, 0.0F);
    rtmp.resize(rprev.size(), 0.0F);
    rspa.resize(rprev.size(), 0.0F);
    rad.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    accum.resize(rad.size(), 0.0F);
    last.resize(rad.size(), 0.0F);
    constexpr crd::u32 n_frames = 48U;
    for (crd::u32 f = 0; f < n_frames; ++f)
    {
        crd::u32 frame = f;
        B tb[5] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U}, {rprev.data(), nullptr, res_bytes, 3U}, {nullptr, rtmp.data(), res_bytes, 4U}, {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 5), num_pts / 64U));
        B pb[5] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U}, {rtmp.data(), nullptr, res_bytes, 3U}, {nullptr, rspa.data(), res_bytes, 4U}, {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(pspv.spirv.data(), pspv.spirv.size()), crd::containers::ConstSpan<B>(pb, 5), num_pts / 64U));
        B sb[4] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U}, {rspa.data(), nullptr, res_bytes, 3U}, {nullptr, rad.data(), pos_bytes, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), num_pts / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { accum[i] += rad[i]; }
        // Feed back the PRE-spatial (temporal) reservoir. Feeding the post-spatial reservoir back would compound spatially-borrowed
        // samples across frames and DARKEN the estimate (the classic ReSTIR bias that only GRIS / pairwise-MIS weights fix); the
        // clean-temporal-history choice keeps the pipeline provably unbiased. Spatial reuse then acts as a per-frame refinement.
        for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rtmp[i]; }
        if (f == n_frames - 1U) { for (crd::usize i = 0; i < rad.size(); ++i) { last[i] = rad[i]; } }
    }
    crd::containers::Array<float> mean(&alloc);
    mean.resize(rad.size(), 0.0F);
    for (crd::usize i = 0; i < mean.size(); ++i) { mean[i] = accum[i] / static_cast<float>(n_frames); }

    const double ref_mean   = spatial_mean(ref);
    const double st_mean    = spatial_mean(mean);
    const double err_last   = rms(last, ref);
    const double err_single = rms(single, ref);
    INFO("spatiotemporal ReSTIR: ref_mean=" << ref_mean << "  st_mean=" << st_mean << "  rms(warm-ref)=" << err_last << "  rms(single-ref)=" << err_single);
    CHECK(ref_mean > 0.05);
    CHECK(crd::math::abs(st_mean - ref_mean) / ref_mean < 0.04);  // UNBIASED — the spatial Z-normalisation keeps the estimator unbiased
    // Spatiotemporal beats single-pass RIS even across the shadow edge. The margin is bounded by the hard-shadow penumbra's
    // irreducible 1-spp visibility variance (binary V per pixel) — the residual a denoiser then resolves; the noise-dominated
    // (fully-visible) win is the 5.8× shown in RT-5b.
    CHECK(err_last < err_single * 0.85);
}

// D-007 RT-6: MULTI-INSTANCE TLAS on Vulkan — one BLAS instanced at 3 translations, so the hardware applies each instance's 3×4
// transform during traversal. Rays hit each translated copy at the right t; the CPU oracle traces the SAME geometry pre-baked to
// world space. This is the portable, both-backend SCALE capability (instancing / many-object scenes) — the frontier-accel core
// that fits the bit-exact-vs-oracle CKIR contract (vs the vendor-locked SER/OMM/cluster hardware extensions).
TEST_CASE("D-007 RT-6: multi-instance TLAS on Vulkan (per-instance transforms) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_trace_kernel(g, 64);

    // BLAS = one triangle at z=2 in LOCAL space; 3 instances translated along x by 0, +2, +4.
    const float verts[9]        = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    const float transforms[36]  = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,   // identity
                                   1, 0, 0, 2, 0, 1, 0, 0, 0, 0, 1, 0,   // +2 x
                                   1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 0};  // +4 x
    constexpr crd::u32 num_pts = 64U;
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(num_pts) * 6U, 0.0F);
    const float rd[4][6] = {{0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // → instance 0 (t=2)
                            {2.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // → instance 1 (t=2)
                            {4.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // → instance 2 (t=2)
                            {6.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}};  // → nothing (miss)
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }

    // ── CPU oracle: the 3 instances pre-baked to world space (translate each local triangle) ──
    crd::containers::Array<crd::f64> geo(&alloc);
    crd::containers::Array<crd::f64> rays64(&alloc);
    crd::containers::Array<crd::f64> oref(&alloc);
    geo.resize(1U + 3U * 9U, 0.0);
    geo[0] = 3.0;
    for (int n = 0; n < 3; ++n)
    {
        const double tx = 2.0 * static_cast<double>(n);
        for (int v = 0; v < 3; ++v)
        {
            geo[1U + static_cast<crd::usize>(n) * 9U + static_cast<crd::usize>(v) * 3U + 0U] = static_cast<double>(verts[v * 3 + 0]) + tx;
            geo[1U + static_cast<crd::usize>(n) * 9U + static_cast<crd::usize>(v) * 3U + 1U] = static_cast<double>(verts[v * 3 + 1]);
            geo[1U + static_cast<crd::usize>(n) * 9U + static_cast<crd::usize>(v) * 3U + 2U] = static_cast<double>(verts[v * 3 + 2]);
        }
    }
    rays64.resize(rays.size(), 0.0);
    for (crd::usize i = 0; i < rays.size(); ++i) { rays64[i] = static_cast<crd::f64>(rays[i]); }
    oref.resize(num_pts, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {rays64.data(), static_cast<int>(rays64.size()), 0, 1}, {oref.data(), static_cast<int>(oref.size()), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, 64U, &alloc, 1U);

    // ── GPU: the instanced TLAS ──
    auto scene = rt.build_scene_instanced(verts, 1U, transforms, 3U);
    REQUIRE(scene != nullptr);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_inst", &alloc);
    REQUIRE(spv.ok);
    crd::containers::Array<float> got(&alloc);
    got.resize(num_pts, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));

    INFO("instanced t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    for (int r = 0; r < 3; ++r) { CHECK(crd::math::abs(static_cast<double>(got[r]) - 2.0) < 1.0e-4); } // each instance hit at t=2
    CHECK(got[3] > 1.0e29F);                                                                             // the gap ⇒ miss
    for (int r = 0; r < 4; ++r) { CHECK(crd::math::abs(static_cast<double>(got[r]) - oref[r]) < 1.0e-4); } // GPU == oracle
}

// D-007 RT-7: MANY-LIGHTS NEE on Vulkan — the integrator-breadth capability RIS/ReSTIR exist for. The N lights live in a runtime
// buffer; each sample picks a light uniformly and shadow-rays it. UNBIASEDNESS: the N-light kernel's mean == the SUM of the
// per-light direct-lighting integrals (run each light alone). Done at high spp on the GPU (the CPU oracle is too slow for this
// heavier kernel). A dummy triangle far below keeps every upward shadow ray unoccluded, isolating the light-selection math.
TEST_CASE("D-007 RT-7: many-lights uniform-selection NEE on Vulkan == sum of per-light direct (unbiased)", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(32U << 20U);
    constexpr crd::u32 num_pts = 64U;
    const float lights[4 * 15] = {
        -3.0F, 3.0F, -3.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 6.0F, 6.0F, 6.0F,
         2.0F, 4.0F, -2.0F, 1.5F, 0.0F, 0.0F, 0.0F, 0.0F, 1.5F, 0.0F, -1.0F, 0.0F, 4.0F, 4.0F, 4.0F,
        -2.0F, 3.5F,  2.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 2.0F, 0.0F, -1.0F, 0.0F, 5.0F, 5.0F, 5.0F,
         2.5F, 5.0F,  2.5F, 2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 3.0F, 3.0F, 3.0F};
    const float dumb[9] = {-1.0F, -10.0F, -1.0F, 1.0F, -10.0F, -1.0F, 0.0F, -10.0F, 1.0F};
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j)
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / 7.0F * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / 7.0F * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }
    auto scene = rt.build_scene(dumb, 1U);
    REQUIRE(scene != nullptr);
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pos_bytes = static_cast<crd::u64>(num_pts) * 3U * sizeof(float);

    crd::containers::Array<float> img(&alloc);
    img.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    // run the many-lights kernel with `nl` of the lights (buffer `lp`); fill `img`, return the spatial-mean channel-0 radiance.
    const auto run = [&](const float* lp, crd::u32 nl, crd::u32 spp, bool power) {
        kir::rt::ManyLightConfig mc;
        mc.samples = spp; mc.nlights = nl; mc.albedo[0] = 0.6F; mc.albedo[1] = 0.6F; mc.albedo[2] = 0.6F; mc.power_sampling = power; mc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_manylight_nee_kernel(g, mc);
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ml", &alloc);
        REQUIRE(spv.ok);
        B bind[4] = {{ppos.data(), nullptr, pos_bytes, 1U}, {pnrm.data(), nullptr, pos_bytes, 2U}, {lp, nullptr, static_cast<crd::u64>(nl) * 15U * sizeof(float), 3U}, {nullptr, img.data(), pos_bytes, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<B>(bind, 4), num_pts / 64U));
        double mean = 0.0;
        for (crd::u32 p = 0; p < num_pts; ++p) { mean += static_cast<double>(img[p * 3U]); }
        return mean / static_cast<double>(num_pts);
    };
    crd::containers::Array<float> ref_img(&alloc);
    crd::containers::Array<float> uni_img(&alloc);
    crd::containers::Array<float> pow_img(&alloc);
    ref_img.resize(img.size(), 0.0F); uni_img.resize(img.size(), 0.0F); pow_img.resize(img.size(), 0.0F);
    const auto rmsv = [&](const crd::containers::Array<float>& a) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { const double d = static_cast<double>(a[i]) - static_cast<double>(ref_img[i]); s += d * d; } return crd::math::sqrt(s / static_cast<double>(a.size())); };

    double sum_per = 0.0;
    for (crd::u32 l = 0; l < 4U; ++l) { sum_per += run(&lights[l * 15U], 1U, 2048U, false); }
    const double all = run(lights, 4U, 8192U, false);
    for (crd::usize i = 0; i < img.size(); ++i) { ref_img[i] = img[i]; }   // converged reference (uniform, 8192 spp)
    const double all_pow = run(lights, 4U, 8192U, true);
    // low-spp images for the variance comparison (equal budget):
    run(lights, 4U, 64U, false); for (crd::usize i = 0; i < img.size(); ++i) { uni_img[i] = img[i]; }
    run(lights, 4U, 64U, true);  for (crd::usize i = 0; i < img.size(); ++i) { pow_img[i] = img[i]; }
    const double e_uni = rmsv(uni_img);
    const double e_pow = rmsv(pow_img);
    INFO("many-lights: Σ per-light=" << sum_per << "  uniform=" << all << "  power=" << all_pow << "  rms64 uniform=" << e_uni << " power=" << e_pow);
    CHECK(sum_per > 0.05);
    CHECK(crd::math::abs(all - sum_per) / sum_per < 0.03);    // uniform selection unbiased ⇒ == the true multi-light sum
    CHECK(crd::math::abs(all_pow - sum_per) / sum_per < 0.03); // POWER selection is ALSO unbiased (correct CDF + pdf)
    CHECK(e_pow < e_uni);                                     // power sampling has LOWER variance (importance-samples brighter lights)
}

// D-007 IB-1: FULL PRODUCTION PATH TRACER on Vulkan — many-lights NEE+MIS + emissive-triangle hits + Russian roulette + GI.
// Scene: 2 emissive area lights (different brightness) + a non-emissive ceiling that bounces GI. Validates (1) GPU==oracle
// (deterministic, incl. the RR hash decisions), (2) RUSSIAN ROULETTE IS UNBIASED — the RR-on mean equals the RR-off mean,
// (3) the lights actually illuminate the floor (emissive + many-lights direct works).
TEST_CASE("D-007 IB-1: full path tracer on Vulkan (many-lights+emissive+RR+GI, unbiased)", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(48U << 20U);
    constexpr crd::u32 num_pts = 64U;
    constexpr crd::u32 num_tri = 6U;
    // prims 0-1 ceiling (y=6, big), prims 2-3 light0 (y=3), prims 4-5 light1 (y=3).
    const float verts[num_tri * 9] = {
        -4,6,-4,  4,6,-4,  4,6,4,      -4,6,-4,  4,6,4,  -4,6,4,       // ceiling
        -2.5F,3,-0.5F, -1.5F,3,-0.5F, -1.5F,3,0.5F,  -2.5F,3,-0.5F, -1.5F,3,0.5F, -2.5F,3,0.5F, // light0
         1.5F,3,-0.5F,  2.5F,3,-0.5F,  2.5F,3,0.5F,   1.5F,3,-0.5F,  2.5F,3,0.5F,  1.5F,3,0.5F}; // light1
    const float tri_n[num_tri * 3] = {0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0};
    const float lights[2 * 15] = {
        -2.5F, 3.0F, -0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 8.0F, 8.0F, 8.0F,
         1.5F, 3.0F, -0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 6.0F, 6.0F, 6.0F};
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < 8U; ++j)
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / 7.0F * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / 7.0F * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }
    auto scene = rt.build_scene(verts, num_tri);
    REQUIRE(scene != nullptr);
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pb = static_cast<crd::u64>(num_pts) * 3U * sizeof(float);

    const auto run = [&](crd::u32 spp, crd::u32 rr_start, crd::containers::Array<float>& outc) {
        kir::rt::PathTraceFullConfig pc;
        pc.samples = spp; pc.bounces = 4U; pc.rr_start = rr_start;
        pc.albedo[0] = 0.6F; pc.albedo[1] = 0.6F; pc.albedo[2] = 0.6F;
        pc.ntri = num_tri; pc.nlights = 2U; pc.light_prim0 = 2U; pc.local_size = 64U; pc.count = num_pts; // REN-38: tail-thread guard
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_pathtrace_full_kernel(g, pc);
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ptfull", &alloc);
        REQUIRE(spv.ok);
        outc.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
        B bind[5] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, num_tri * 3U * sizeof(float), 3U}, {lights, nullptr, 2U * 15U * sizeof(float), 4U}, {nullptr, outc.data(), pb, 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<B>(bind, 5), num_pts / 64U));
        return e;
    };
    const auto smean = [&](const crd::containers::Array<float>& a) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); } return s / static_cast<double>(a.size()); };

    // ── GPU==oracle at low spp (RR on) ──
    crd::containers::Array<float> gpu16(&alloc);
    const kir::KEntry e16 = run(16U, 1U, gpu16);
    {
        kir::rt::PathTraceFullConfig pc; pc.samples = 16U; pc.bounces = 4U; pc.rr_start = 1U;
        pc.albedo[0] = 0.6F; pc.albedo[1] = 0.6F; pc.albedo[2] = 0.6F; pc.ntri = num_tri; pc.nlights = 2U; pc.light_prim0 = 2U; pc.local_size = 64U; pc.count = num_pts; // REN-38: tail-thread guard
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_pathtrace_full_kernel(g, pc);
        crd::containers::Array<crd::f64> geo(&alloc);
        crd::containers::Array<crd::f64> pos64(&alloc);
        crd::containers::Array<crd::f64> nrm64(&alloc);
        crd::containers::Array<crd::f64> tn64(&alloc);
        crd::containers::Array<crd::f64> lt64(&alloc);
        crd::containers::Array<crd::f64> refc(&alloc);
        geo.resize(1U + static_cast<crd::usize>(num_tri) * 9U, 0.0);
        geo[0] = static_cast<crd::f64>(num_tri);
        for (crd::u32 i = 0; i < num_tri * 9U; ++i) { geo[i + 1U] = static_cast<crd::f64>(verts[i]); }
        pos64.resize(ppos.size(), 0.0); nrm64.resize(pnrm.size(), 0.0); tn64.resize(num_tri * 3U, 0.0); lt64.resize(2U * 15U, 0.0); refc.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0);
        for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
        for (crd::u32 i = 0; i < num_tri * 3U; ++i) { tn64[i] = static_cast<crd::f64>(tri_n[i]); }
        for (crd::u32 i = 0; i < 2U * 15U; ++i) { lt64[i] = static_cast<crd::f64>(lights[i]); }
        kir::KernelBuffer bufs[6] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {lt64.data(), static_cast<int>(lt64.size()), 0, 4}, {refc.data(), static_cast<int>(refc.size()), 0, 5}};
        kir::eval_cpu_kernel(g, e, bufs, 6, 64U, &alloc, 1U);
        double worst = 0.0;
        for (crd::u32 p = 0; p < num_pts * 3U; ++p) { worst = crd::math::max(worst, crd::math::abs(static_cast<double>(gpu16[p]) - refc[p])); }
        INFO("full-pt GPU==oracle worst=" << worst);
        CHECK(worst < 0.05); // GPU == CPU oracle (deterministic, incl. the RR hash decisions + emissive MIS)
    }

    // ── Russian roulette unbiasedness: RR-on mean == RR-off mean ──
    crd::containers::Array<float> rr_on(&alloc);
    crd::containers::Array<float> rr_off(&alloc);
    run(256U, 1U, rr_on);    // RR active from bounce 1
    run(256U, 99U, rr_off);  // RR never fires (reference)
    const double m_on = smean(rr_on);
    const double m_off = smean(rr_off);
    double mxv = 0.0; crd::u32 mxi = 0;
    for (crd::u32 i = 0; i < rr_off.size(); ++i) { if (static_cast<double>(rr_off[i]) > mxv) { mxv = static_cast<double>(rr_off[i]); mxi = i; } }
    INFO("full-pt  RR-on mean=" << m_on << "  RR-off mean=" << m_off << "  max=" << mxv << " @elem " << mxi << "  pt0=[" << rr_off[0] << "," << rr_off[1] << "," << rr_off[2] << "]");
    CHECK(m_off > 0.05);                                  // the lights illuminate the floor (emissive + many-lights direct)
    CHECK(crd::math::abs(m_on - m_off) / m_off < 0.04);     // Russian roulette is UNBIASED (survivors ÷p keep the mean)
    (void)e16;
}

// D-007 IB-2: ReSTIR GI (temporal) on Vulkan — resampling INDIRECT light. Scene: the floor is lit ONLY by a 1-bounce bounce off a
// ceiling (the single area light faces UP, away from the floor, so the floor's direct light is zero — pure GI). ReSTIR GI stores
// the secondary sample point + its outgoing radiance in a reservoir and reuses it across frames. Validates (1) UNBIASED — the
// temporal mean converges to the brute-force 1-bounce-indirect reference, (2) VARIANCE REDUCTION — a warmed-up frame beats the
// no-reuse single-sample estimate.
TEST_CASE("D-007 IB-2: ReSTIR GI temporal reuse on Vulkan (indirect, unbiased + variance win)", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(48U << 20U);
    constexpr crd::u32 img_w = 16U;
    constexpr crd::u32 img_h = 16U;
    constexpr crd::u32 num_pts = img_w * img_h;
    // AS = the ceiling ONLY (a 10×10 quad at y=4, normal down). The light is analytic (in the light buffer, not the AS), at y=3
    // facing UP ⇒ it lights the ceiling but not the floor. Floor up-rays hit the ceiling ⇒ pure 1-bounce indirect.
    const float verts[18] = {-5,4,-5,  5,4,-5,  5,4,5,   -5,4,-5,  5,4,5,  -5,4,5};
    const float tri_n[6]  = {0,-1,0, 0,-1,0};
    const float light[15] = {-2.0F, 3.0F, -2.0F, 4.0F, 0.0F, 0.0F, 0.0F, 0.0F, 4.0F, 0.0F, 1.0F, 0.0F, 10.0F, 10.0F, 10.0F};
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < img_h; ++j)
    {
        for (crd::u32 i = 0; i < img_w; ++i)
        {
            const crd::u32 p = j * img_w + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(img_w - 1U) * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(img_h - 1U) * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }
    auto scene = rt.build_scene(verts, 2U);
    REQUIRE(scene != nullptr);
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pb = static_cast<crd::u64>(num_pts) * 3U * sizeof(float);
    const crd::u64 rbytes = static_cast<crd::u64>(num_pts) * kir::rt::kRestirGiStride * sizeof(float);

    kir::rt::RestirGiConfig sc;
    sc.width = img_w; sc.height = img_h; sc.ntri = 2U; sc.nlights = 1U; sc.local_size = 64U;
    kir::KGraph gt(&alloc);
    kir::KGraph gsh(&alloc);
    const kir::KEntry et = kir::rt::build_restir_gi_temporal_kernel(gt, sc);
    const kir::KEntry es = kir::rt::build_restir_gi_shade_kernel(gsh, sc);
    const auto tcomp = [&](const kir::KEntry& e, kir::KGraph& g, const char* tag) {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), tag, &alloc);
        INFO(spv.error_message.c_str()); REQUIRE(spv.ok); return spv;
    };
    const auto tspv = tcomp(et, gt, "gi_t");
    const auto sspv = tcomp(es, gsh, "gi_s");

    crd::containers::Array<float> rprev(&alloc);
    crd::containers::Array<float> rcur(&alloc);
    crd::containers::Array<float> rad(&alloc);
    crd::containers::Array<float> acc_ref(&alloc);
    crd::containers::Array<float> acc_r(&alloc);
    crd::containers::Array<float> warm(&alloc);
    crd::containers::Array<float> single(&alloc);
    rprev.resize(static_cast<crd::usize>(num_pts) * kir::rt::kRestirGiStride, 0.0F);
    rcur.resize(rprev.size(), 0.0F); rad.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    acc_ref.resize(rad.size(), 0.0F); acc_r.resize(rad.size(), 0.0F); warm.resize(rad.size(), 0.0F); single.resize(rad.size(), 0.0F);

    const auto frame_step = [&](crd::u32 f, bool feedback, crd::containers::Array<float>& outRad) {
        crd::u32 frame = f;
        B tb[8] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {light, nullptr, 15U * sizeof(float), 4U}, {rprev.data(), nullptr, rbytes, 5U}, {nullptr, rcur.data(), rbytes, 6U}, {&frame, nullptr, sizeof(crd::u32), 7U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 7), num_pts / 64U));
        B sb[4] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rcur.data(), nullptr, rbytes, 3U}, {nullptr, outRad.data(), pb, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), num_pts / 64U));
        if (feedback) { for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rcur[i]; } }
    };
    // reference: NO reuse (rprev stays 0) averaged over many frames.
    constexpr crd::u32 k_ref = 96U;
    for (crd::u32 f = 0; f < k_ref; ++f) { frame_step(f, false, rad); for (crd::usize i = 0; i < rad.size(); ++i) { acc_ref[i] += rad[i]; } if (f == 0) { for (crd::usize i = 0; i < rad.size(); ++i) { single[i] = rad[i]; } } }
    // ReSTIR GI: temporal reuse.
    for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = 0.0F; }
    constexpr crd::u32 n_frames = 64U;
    for (crd::u32 f = 0; f < n_frames; ++f) { frame_step(f, true, rad); for (crd::usize i = 0; i < rad.size(); ++i) { acc_r[i] += rad[i]; } if (f == n_frames - 1U) { for (crd::usize i = 0; i < rad.size(); ++i) { warm[i] = rad[i]; } } }

    const auto smean = [&](const crd::containers::Array<float>& a, double sc2) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); } return s / static_cast<double>(a.size()) * sc2; };
    const auto rms = [&](const crd::containers::Array<float>& a, const crd::containers::Array<float>& b, double sca) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { const double d = static_cast<double>(a[i]) * sca - static_cast<double>(b[i]); s += d * d; } return crd::math::sqrt(s / static_cast<double>(a.size())); };
    const double ref_mean = smean(acc_ref, 1.0 / k_ref);
    const double r_mean   = smean(acc_r, 1.0 / n_frames);
    // ref image (per-pixel) for rms:
    crd::containers::Array<float> ref_img(&alloc); ref_img.resize(rad.size(), 0.0F);
    for (crd::usize i = 0; i < ref_img.size(); ++i) { ref_img[i] = acc_ref[i] / static_cast<float>(k_ref); }
    const double err_warm = rms(warm, ref_img, 1.0);
    const double err_single = rms(single, ref_img, 1.0);
    INFO("ReSTIR GI  ref_mean=" << ref_mean << "  reuseMean=" << r_mean << "  rms(warm)=" << err_warm << "  rms(single)=" << err_single);
    CHECK(ref_mean > 0.01);                                 // the floor receives indirect light (pure GI, no direct)
    CHECK(crd::math::abs(r_mean - ref_mean) / ref_mean < 0.05); // UNBIASED: temporal reuse converges to the brute-force indirect
    CHECK(err_warm < err_single * 0.8);                      // VARIANCE REDUCTION vs the no-reuse single-sample estimate
}

// D-007 IB-2b: FULL ReSTIR GI SPATIOTEMPORAL on Vulkan — temporal → spatial (with the Jacobian reconnection) → shade. Same pure-
// indirect scene as IB-2. Verifies the Jacobian-corrected spatial reuse stays UNBIASED (spatial mean == the brute-force indirect
// reference) — if the reconnection Jacobian were wrong the mean would drift.
TEST_CASE("D-007 IB-2b: ReSTIR GI spatiotemporal on Vulkan (Jacobian reconnection, unbiased)", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(48U << 20U);
    constexpr crd::u32 img_w = 16U;
    constexpr crd::u32 img_h = 16U;
    constexpr crd::u32 num_pts = img_w * img_h;
    const float verts[18] = {-5,4,-5, 5,4,-5, 5,4,5, -5,4,-5, 5,4,5, -5,4,5};
    const float tri_n[6]  = {0,-1,0, 0,-1,0};
    const float light[15] = {-2.0F, 3.0F, -2.0F, 4.0F, 0.0F, 0.0F, 0.0F, 0.0F, 4.0F, 0.0F, 1.0F, 0.0F, 10.0F, 10.0F, 10.0F};
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F); pnrm.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F);
    for (crd::u32 j = 0; j < img_h; ++j) { for (crd::u32 i = 0; i < img_w; ++i) { const crd::u32 p = j * img_w + i; ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(img_w - 1U) * static_cast<float>(i); ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(img_h - 1U) * static_cast<float>(j); pnrm[p * 3U + 1U] = 1.0F; } }
    auto scene = rt.build_scene(verts, 2U);
    REQUIRE(scene != nullptr);
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pb = static_cast<crd::u64>(num_pts) * 3U * sizeof(float);
    const crd::u64 rbytes = static_cast<crd::u64>(num_pts) * kir::rt::kRestirGiStride * sizeof(float);

    kir::rt::RestirGiConfig sc; sc.width = img_w; sc.height = img_h; sc.ntri = 2U; sc.nlights = 1U; sc.spatial_k = 5U; sc.spatial_radius = 3.0F; sc.local_size = 64U;
    kir::KGraph gt(&alloc);
    kir::KGraph gsp(&alloc);
    kir::KGraph gsh(&alloc);
    const kir::KEntry et = kir::rt::build_restir_gi_temporal_kernel(gt, sc);
    const kir::KEntry ep = kir::rt::build_restir_gi_spatial_kernel(gsp, sc);
    const kir::KEntry es = kir::rt::build_restir_gi_shade_kernel(gsh, sc);
    const auto comp = [&](const kir::KEntry& e, kir::KGraph& g, const char* t) { kir::GlslKernel kern(&alloc); REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern)); auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), t, &alloc); INFO(spv.error_message.c_str()); REQUIRE(spv.ok); return spv; };
    const auto tspv = comp(et, gt, "gt");
    const auto pspv = comp(ep, gsp, "gp");
    const auto sspv = comp(es, gsh, "gs");

    crd::containers::Array<float> rprev(&alloc);
    crd::containers::Array<float> rtmp(&alloc);
    crd::containers::Array<float> rspa(&alloc);
    crd::containers::Array<float> rad(&alloc);
    crd::containers::Array<float> acc_ref(&alloc);
    crd::containers::Array<float> acc_st(&alloc);
    rprev.resize(static_cast<crd::usize>(num_pts) * kir::rt::kRestirGiStride, 0.0F); rtmp.resize(rprev.size(), 0.0F); rspa.resize(rprev.size(), 0.0F);
    rad.resize(static_cast<crd::usize>(num_pts) * 3U, 0.0F); acc_ref.resize(rad.size(), 0.0F); acc_st.resize(rad.size(), 0.0F);
    const auto smean = [&](const crd::containers::Array<float>& a, double s2) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); } return s / static_cast<double>(a.size()) * s2; };

    // reference: temporal-only, NO feedback, averaged (brute-force indirect).
    constexpr crd::u32 k_ref = 96U;
    for (crd::u32 f = 0; f < k_ref; ++f)
    {
        crd::u32 frame = f;
        B tb[7] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {light, nullptr, 15U * sizeof(float), 4U}, {rprev.data(), nullptr, rbytes, 5U}, {nullptr, rtmp.data(), rbytes, 6U}, {&frame, nullptr, sizeof(crd::u32), 7U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 7), num_pts / 64U));
        B sb[4] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rtmp.data(), nullptr, rbytes, 3U}, {nullptr, rad.data(), pb, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), num_pts / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { acc_ref[i] += rad[i]; }
    }
    // spatiotemporal: temporal → spatial(Jacobian) → shade, feed back the pre-spatial reservoir.
    for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = 0.0F; }
    constexpr crd::u32 n_frames = 64U;
    for (crd::u32 f = 0; f < n_frames; ++f)
    {
        crd::u32 frame = f;
        B tb[7] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {light, nullptr, 15U * sizeof(float), 4U}, {rprev.data(), nullptr, rbytes, 5U}, {nullptr, rtmp.data(), rbytes, 6U}, {&frame, nullptr, sizeof(crd::u32), 7U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 7), num_pts / 64U));
        B pbnd[5] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rtmp.data(), nullptr, rbytes, 3U}, {nullptr, rspa.data(), rbytes, 4U}, {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(pspv.spirv.data(), pspv.spirv.size()), crd::containers::ConstSpan<B>(pbnd, 5), num_pts / 64U));
        B sb[4] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rspa.data(), nullptr, rbytes, 3U}, {nullptr, rad.data(), pb, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), num_pts / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { acc_st[i] += rad[i]; }
        for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rtmp[i]; }
    }
    const double ref_mean = smean(acc_ref, 1.0 / k_ref);
    const double st_mean = smean(acc_st, 1.0 / n_frames);
    INFO("ReSTIR GI spatiotemporal  ref_mean=" << ref_mean << "  st_mean=" << st_mean);
    CHECK(ref_mean > 0.01);
    CHECK(crd::math::abs(st_mean - ref_mean) / ref_mean < 0.06); // Jacobian spatial reconnection keeps the GI estimator unbiased
}

// D-007 FA-1: OPACITY MICROMAPS on Vulkan (VK_EXT_opacity_micromap) — a real vendor RT frontier feature RUN on the RTX 4070. The
// FRONT triangle (z=1) carries a 2-state OMM at subdivision level 2 (16 micro-triangles) with HALF opaque, HALF transparent; a
// BACK triangle (z=2) is fully opaque. Rays through the OPAQUE micro-triangles hit the front (t≈1); rays through the TRANSPARENT
// micro-triangles pass THROUGH and hit the back (t≈2) — the hardware resolves alpha during traversal, no any-hit shader. That
// selective pass-through (a mix of front and back hits) is the proof the OMM is live.
TEST_CASE("D-007 FA-1: opacity micromap on Vulkan (alpha-tested traversal)", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    if (!vk->opacity_micromap()) { WARN("adapter has no VK_EXT_opacity_micromap; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_trace_kernel(g, 64);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "omm", &alloc);
    REQUIRE(spv.ok);

    // front triangle (prim 0, z=1) + back triangle (prim 1, z=2), same right-triangle footprint.
    const float verts[18] = {0,0,1, 2,0,1, 0,2,1,   0,0,2, 2,0,2, 0,2,2};
    const crd::u8 omm_bits[2] = {0xFFU, 0x00U}; // 16 micro-tris: 0-7 opaque, 8-15 transparent (~half/half by area)
    auto scene = rt.build_scene_omm(verts, 2U, omm_bits, 2U);
    REQUIRE(scene != nullptr);

    constexpr crd::u32 num_pts = 64U;
    crd::containers::Array<float> rays(&alloc);
    crd::containers::Array<float> got(&alloc);
    rays.resize(static_cast<crd::usize>(num_pts) * 6U, 0.0F);
    got.resize(num_pts, 0.0F);
    crd::u32 interior = 0;
    for (crd::u32 j = 0; j < 8U; ++j) // 8×8 grid over the triangle footprint, rays +z
    {
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const crd::u32 p = j * 8U + i;
            const float x = 0.15F + 1.7F / 7.0F * static_cast<float>(i);
            const float y = 0.15F + 1.7F / 7.0F * static_cast<float>(j);
            rays[p * 6U + 0U] = x; rays[p * 6U + 1U] = y; rays[p * 6U + 2U] = 0.0F;
            rays[p * 6U + 5U] = 1.0F; // dir +z
            if (x + y < 1.9F) { ++interior; }
        }
    }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));

    crd::u32 front_hits = 0;
    crd::u32 back_hits = 0;
    for (crd::u32 p = 0; p < num_pts; ++p)
    {
        const double t = static_cast<double>(got[p]);
        if (t > 0.9 && t < 1.1) { ++front_hits; }
        else if (t > 1.9 && t < 2.1) { ++back_hits; }
    }
    INFO("OMM: interior rays=" << interior << "  front(opaque)=" << front_hits << "  back(passed-through)=" << back_hits);
    CHECK(front_hits > 3);                       // some rays hit the OPAQUE micro-triangles (front)
    CHECK(back_hits > 3);                        // some rays PASSED THROUGH the transparent micro-triangles to the back
    CHECK(front_hits + back_hits >= interior - 2); // every interior ray resolved to front or back (nothing lost)
}

// D-007 FA-2: RAY-TRACING PIPELINE + SER on Vulkan (VK_KHR_ray_tracing_pipeline + VK_NV_ray_tracing_invocation_reorder) — the
// "big rig" RT path with raygen/miss/closest-hit shaders + a shader binding table, RUN on the RTX 4070. The raygen uses SHADER
// EXECUTION REORDERING (hitObjectNV + reorderThreadNV) when the adapter supports it — SER regroups threads by hit coherence, a
// PERF feature that must not change results, so the pipeline's closest-hit distances must still equal the CPU oracle.
TEST_CASE("D-007 FA-2: RT pipeline + SER on Vulkan == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    if (!vk->rt_pipeline()) { WARN("adapter has no VK_KHR_ray_tracing_pipeline; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());
    const bool ser = vk->invocation_reorder();

    crd::memory::TlsfAllocator alloc(16U << 20U);
    // raygen (SER when available), closest-hit, miss.
    const char* rgen_ser =
        "#version 460\n#extension GL_EXT_ray_tracing : require\n#extension GL_NV_shader_invocation_reorder : require\n"
        "layout(binding=0) uniform accelerationStructureEXT tlas;\n"
        "layout(binding=1, std430) buffer Rays { float r[]; };\nlayout(binding=2, std430) buffer Out { float o[]; };\n"
        "layout(location=0) rayPayloadEXT float pt;\n"
        "void main(){ uint i=gl_LaunchIDEXT.x; vec3 org=vec3(r[i*6+0],r[i*6+1],r[i*6+2]); vec3 dir=vec3(r[i*6+3],r[i*6+4],r[i*6+5]);\n"
        "  hitObjectNV h; hitObjectTraceRayNV(h, tlas, gl_RayFlagsNoneEXT, 0xFFu, 0u, 0u, 0u, org, 0.001, dir, 1e30, 0);\n"
        "  reorderThreadNV(h); hitObjectExecuteShaderNV(h, 0); o[i]=pt; }\n";
    const char* rgen_plain =
        "#version 460\n#extension GL_EXT_ray_tracing : require\n"
        "layout(binding=0) uniform accelerationStructureEXT tlas;\n"
        "layout(binding=1, std430) buffer Rays { float r[]; };\nlayout(binding=2, std430) buffer Out { float o[]; };\n"
        "layout(location=0) rayPayloadEXT float pt;\n"
        "void main(){ uint i=gl_LaunchIDEXT.x; vec3 org=vec3(r[i*6+0],r[i*6+1],r[i*6+2]); vec3 dir=vec3(r[i*6+3],r[i*6+4],r[i*6+5]);\n"
        "  traceRayEXT(tlas, gl_RayFlagsNoneEXT, 0xFFu, 0u, 0u, 0u, org, 0.001, dir, 1e30, 0); o[i]=pt; }\n";
    const char* rchit = "#version 460\n#extension GL_EXT_ray_tracing : require\nlayout(location=0) rayPayloadInEXT float pt;\nvoid main(){ pt = gl_HitTEXT; }\n";
    const char* rmiss = "#version 460\n#extension GL_EXT_ray_tracing : require\nlayout(location=0) rayPayloadInEXT float pt;\nvoid main(){ pt = 1e30; }\n";
    const auto compile = [&](gpu::ShaderStage stage, const char* src) {
        auto spv = gpu::compile_glsl_to_spirv(stage, crd::containers::StringView(src, static_cast<crd::usize>(std::strlen(src))), "rtp", &alloc);
        INFO(spv.error_message.c_str()); REQUIRE(spv.ok); return spv;
    };
    const auto rg = compile(gpu::ShaderStage::RayGen, ser ? rgen_ser : rgen_plain);
    const auto ch = compile(gpu::ShaderStage::ClosestHit, rchit);
    const auto ms = compile(gpu::ShaderStage::Miss, rmiss);

    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    auto scene = rt.build_scene(verts, 1U);
    REQUIRE(scene != nullptr);
    constexpr crd::u32 num_pts = 4U;
    const float rd[num_pts][6] = {{0.2F,0.2F,0,0,0,1}, {0.2F,0.2F,0,0,0,-1}, {5,5,0,0,0,1}, {0.1F,0.1F,1,0,0,1}};
    crd::containers::Array<float> rays(&alloc);
    crd::containers::Array<float> got(&alloc);
    rays.resize(static_cast<crd::usize>(num_pts) * 6U, 0.0F); got.resize(num_pts, 0.0F);
    for (crd::u32 i = 0; i < num_pts; ++i) { for (int c = 0; c < 6; ++c) { rays[i * 6U + c] = rd[i][c]; } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U}};
    REQUIRE(rt.trace_rays_pipeline(*scene, crd::containers::ConstSpan<crd::u8>(rg.spirv.data(), rg.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(ms.spirv.data(), ms.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(ch.spirv.data(), ch.spirv.size()),
                                   crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), num_pts, 1U));
    INFO("RT pipeline" << (ser ? " (SER)" : "") << " t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4); // closest-hit t=2 via the pipeline
    CHECK(got[1] > 1.0e29F);                                            // miss shader ⇒ 1e30
    CHECK(got[2] > 1.0e29F);                                            // miss
    CHECK(crd::math::abs(static_cast<double>(got[3]) - 1.0) < 1.0e-4); // t=1
}

// D-007 FA-3: CLUSTER ACCELERATION STRUCTURES on Vulkan (VK_NV_cluster_acceleration_structure / RTX Mega-Geometry) — RUN on the
// RTX 4070. The triangle becomes a CLAS (cluster-level AS) via a GPU-driven indirect build, a cluster BLAS is built over it, and
// a TLAS over that. Traversal is identical to a normal BLAS, so the inline-rayQuery hits must equal the CPU oracle.
TEST_CASE("D-007 FA-3: cluster acceleration structure on Vulkan == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    if (!vk->cluster_as()) { WARN("adapter has no VK_NV_cluster_acceleration_structure; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_trace_kernel(g, 64);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "clus", &alloc);
    REQUIRE(spv.ok);

    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    auto scene = rt.build_scene_clusters(verts, 1U);
    REQUIRE(scene != nullptr);

    constexpr crd::u32 num_pts = 64U;
    crd::containers::Array<float> rays(&alloc);
    crd::containers::Array<float> got(&alloc);
    rays.resize(static_cast<crd::usize>(num_pts) * 6U, 0.0F); got.resize(num_pts, 0.0F);
    const float rd[4][6] = {{0.2F,0.2F,0,0,0,1}, {0.2F,0.2F,0,0,0,-1}, {5,5,0,0,0,1}, {0.1F,0.1F,1,0,0,1}};
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));

    INFO("cluster-AS t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4); // cluster BLAS traverses like a normal BLAS
    CHECK(got[1] > 1.0e29F);
    CHECK(got[2] > 1.0e29F);
    CHECK(crd::math::abs(static_cast<double>(got[3]) - 1.0) < 1.0e-4);
}

// D-007 P1: the unified RtCapabilities query — the portable warn-on-unsupported foundation. On the RTX 4070 every RT feature is
// present; the point is that portable code can ASK, and (elsewhere) fall back + warn when a feature is absent.
TEST_CASE("D-007 P1: RtCapabilities query reports the adapter's RT feature set", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());
    const gpu::RtCapabilities c = rt.capabilities();
    INFO("caps: inline=" << c.has(gpu::RtFeature::InlineQuery) << " pipe=" << c.has(gpu::RtFeature::RtPipeline)
         << " ser=" << c.has(gpu::RtFeature::ShaderReorder) << " omm=" << c.has(gpu::RtFeature::OpacityMicromap)
         << " cluster=" << c.has(gpu::RtFeature::ClusterAS)
         << " lss=" << c.has(gpu::RtFeature::LinearSweptSpheres));
    CHECK(c.has(gpu::RtFeature::InlineQuery));                              // always true once the RT context is valid
    // The feature flags mirror the device accessors (which the FA-1/2/3 tests proved run) — the query is consistent with them.
    CHECK(c.has(gpu::RtFeature::RtPipeline) == vk->rt_pipeline());
    CHECK(c.has(gpu::RtFeature::OpacityMicromap) == vk->opacity_micromap());
    CHECK(c.has(gpu::RtFeature::ClusterAS) == vk->cluster_as());
    // B18-f: LSS is Blackwell-class silicon. On anything older this is legitimately FALSE, and the gate is that the
    // capability agrees with the device accessor — never that the feature is present. A consumer that needs strands
    // takes the portable procedural-AABB path (ckir_lss.hpp) when this is false, which is the common case today.
    CHECK(c.has(gpu::RtFeature::LinearSweptSpheres) == vk->linear_swept_spheres());
}

// B18-f: the PROCEDURAL STRAND acceleration structure actually BUILDS on the device. A curve BLAS is AABB geometry, a
// different geometry type from every other scene builder here, and a build failure is SILENT (nullptr) — so this gate
// exists to catch "the strand tier compiles but no acceleration structure was ever created", which would otherwise
// surface much later as an empty render with no error anywhere.
TEST_CASE("B18-f: procedural curve BLAS builds on Vulkan", "[gpu-context][vulkan][gpu][rt][lss]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    // a small groom: 4 strands x 8 segments, each tapering root-to-tip
    constexpr crd::u32 k_strands = 4;
    constexpr crd::u32 k_segs    = 8;
    float              segs[k_strands * k_segs * 8];
    for (crd::u32 s = 0; s < k_strands; ++s)
    {
        for (crd::u32 j = 0; j < k_segs; ++j)
        {
            const float t0 = static_cast<float>(j) / static_cast<float>(k_segs);
            const float t1 = static_cast<float>(j + 1U) / static_cast<float>(k_segs);
            float*      g  = segs + (s * k_segs + j) * 8U;
            g[0] = static_cast<float>(s) * 0.3F; g[1] = t0 * 2.0F; g[2] = 0.0F; g[3] = 0.05F * (1.0F - t0) + 0.01F;
            g[4] = static_cast<float>(s) * 0.3F; g[5] = t1 * 2.0F; g[6] = 0.0F; g[7] = 0.05F * (1.0F - t1) + 0.01F;
        }
    }
    auto scene = rt.build_scene_curves(segs, k_strands * k_segs);
    std::printf("[Vulkan B18-f] curve BLAS over %u swept segments: %s   (native LSS on this adapter: %s)\n",
                k_strands * k_segs, scene != nullptr ? "built" : "FAILED",
                rt.capabilities().has(gpu::RtFeature::LinearSweptSpheres) ? "yes" : "no - using procedural AABBs");
    REQUIRE(scene != nullptr);
    // Degenerate input must be REJECTED rather than producing an empty-but-valid AS that silently renders nothing.
    CHECK(rt.build_scene_curves(segs, 0U) == nullptr);
    CHECK(rt.build_scene_curves(nullptr, 4U) == nullptr);
}

// B18-f: the PROCEDURAL CURVE ray query running on real hardware traversal. This is the gate that matters for the strand
// tier: `TraceRayCurves` emits a rayQuery candidate loop that intersects each candidate AABB's linear swept sphere and
// COMMITS it with rayQueryGenerateIntersectionEXT — shader text no other test exercises, against a BLAS whose geometry
// type (AABBs) no other test builds.
//
// ⚠ The comparison is deliberately structured around WHICH rays hit, not just distances. Hardware narrows the query to
//   candidate boxes while the oracle brute-forces every segment; if the AABBs were not conservative, or the commit were
//   wrong, the device would MISS hits the oracle finds — and that shows up as a hit/miss disagreement long before it
//   shows up as a distance error. A tolerance-only check would quietly pass a groom full of holes.
// ⛔ OPEN DEFECT — this gate is CORRECTLY RED and is hidden (leading '.') only so the suite stays actionable.
//    It found a real bug and must NOT be deleted or loosened.
//
//    SYMPTOM: over 256 rays / 36 tapered segments the oracle reports ~155 hits, the device 57, with ZERO false
//    positives on the device side.
//    ESTABLISHED: the DEVICE is right and the ORACLE/IR maths is wrong. Ray 1's oracle hit is at z = -0.268 while every
//    segment in the scene lies at z in {0, 0.15, 0.3} with radius <= 0.11 — there is no geometry within 0.16 of that
//    point. The correct (tight) AABB was culling those phantom hits, which is why it first presented as the device
//    MISSING 63% of rays. Inflating the AABB to eps = 0.5 made the device reproduce all 155, confirming the direction.
//    FIXED SO FAR: (1) the candidate search seeded from the ray's original tmax instead of the currently committed t,
//    so a farther candidate could clobber a nearer committed hit via an out-of-range generate — measured worst error
//    0.95, now 7.7e-05; (2) the axial coordinate omitted the -ra*rr term (Quilez: y = m1 - ra*rr + t*m2), invisible for
//    a capsule where rr == 0. A tapered-MISS gate now exists in tests/kir/test_ckir_lss.cpp and passes.
//    STILL OPEN: a spurious-hit source remains. A clean PERPENDICULAR miss is correctly rejected (discriminant < 0,
//    verified numerically), so the remaining source is oblique rays or the end-cap branch. Next step: bisect by
//    disabling the cap branch and re-running this gate — if the phantom hits vanish, it is the cap ray-sphere test.
TEST_CASE("B18-f: CKIR TraceRayCurves runs on Vulkan hardware traversal == CPU oracle",
          "[gpu-context][vulkan][gpu][rt][lss][curvert]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());
    crd::memory::TlsfAllocator alloc(64U << 20U);
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    // ── a small groom: strands of tapering segments, spread so most rays hit something ──
    constexpr int k_str  = 6;
    constexpr int k_seg  = 6;
    constexpr int nseg  = k_str * k_seg;
    constexpr int nray  = 256;
    crd::containers::Array<float>  segf(&alloc);
    crd::containers::Array<double> segd(&alloc);
    segf.resize(uz(nseg * 8), 0.0F);
    segd.resize(uz(nseg * 8), 0.0);
    for (int s = 0; s < k_str; ++s)
    {
        const float x = -1.0F + 0.4F * static_cast<float>(s);
        for (int j = 0; j < k_seg; ++j)
        {
            const float t0 = static_cast<float>(j) / static_cast<float>(k_seg);
            const float t1 = static_cast<float>(j + 1) / static_cast<float>(k_seg);
            float*      q  = segf.data() + uz((s * k_seg + j) * 8);
            q[0] = x; q[1] = -0.5F + 2.0F * t0; q[2] = 0.15F * static_cast<float>(s % 3);
            q[3] = 0.09F * (1.0F - t0) + 0.02F;
            q[4] = x; q[5] = -0.5F + 2.0F * t1; q[6] = 0.15F * static_cast<float>(s % 3);
            q[7] = 0.09F * (1.0F - t1) + 0.02F;
        }
    }
    for (int i = 0; i < nseg * 8; ++i) { segd[uz(i)] = static_cast<double>(segf[uz(i)]); }

    crd::containers::Array<float>  rayf(&alloc);
    crd::containers::Array<double> rayd(&alloc);
    rayf.resize(uz(nray * 6), 0.0F);
    rayd.resize(uz(nray * 6), 0.0);
    crd::u32   st  = 0x18F00D01U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < nray; ++i)
    {
        float* r = rayf.data() + uz(i * 6);
        r[0] = static_cast<float>(rnd() * 3.0 - 1.5);
        r[1] = static_cast<float>(rnd() * 2.4 - 0.6);
        r[2] = -3.0F;
        r[3] = static_cast<float>(rnd() * 0.2 - 0.1);
        r[4] = static_cast<float>(rnd() * 0.2 - 0.1);
        r[5] = 1.0F;
    }
    for (int i = 0; i < nray * 6; ++i) { rayd[uz(i)] = static_cast<double>(rayf[uz(i)]); }

    // ── the kernel: one TraceRayCurves per lane ──
    kir::KGraph      g(&alloc);
    const kir::Shape shu = kir::make_shape({1});
    const auto       cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, kir::DType::U32); };
    const auto       ks  = [&](double v) { return g.constant(v, shu, kir::DType::F32); };
    const int as    = g.accel_struct_decl(0, 0);
    const int seg_b = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int ray_b = g.buffer_decl(kir::DType::F32, 0, 2, false);
    const int out_b = g.buffer_decl(kir::DType::F32, 0, 3, true);
    const int tid   = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int mark = g.kernel_stmt_mark();
    const int rb   = g.binary(kir::KOp::Mul, tid, cu(6));
    const auto rl  = [&](int k) {
        const int v = g.buffer_load(ray_b, g.binary(kir::KOp::Add, rb, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const auto hit = g.trace_ray_curves(as, seg_b, rl(0), rl(1), rl(2), rl(3), rl(4), rl(5), ks(1.0e-4), ks(50.0));
    const int  ob  = g.binary(kir::KOp::Mul, tid, cu(2));
    g.stmt_buffer_store(out_b, ob, hit.t);
    g.stmt_buffer_store(out_b, g.binary(kir::KOp::Add, ob, cu(1)), hit.u);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    // ── CPU oracle first ──
    crd::containers::Array<double> ref(&alloc);
    crd::containers::Array<double> as_stub(&alloc);
    ref.resize(uz(nray * 2), 0.0);
    as_stub.resize(1U, 0.0);
    kir::KernelBuffer obufs[4] = {{as_stub.data(), 1, 0, 0},
                                {segd.data(), nseg * 8, 0, 1},
                                {rayd.data(), nray * 6, 0, 2},
                                {ref.data(), nray * 2, 0, 3}};
    kir::eval_cpu_kernel(g, e, obufs, 4, e.local_size[0], &alloc, static_cast<crd::u32>(nray / 64));

    // ── then the device ──
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_curve_rt", &alloc, false);
    INFO("GLSL compile: " << spv.error_message.c_str());
    REQUIRE(spv.ok);

    auto scene = rt.build_scene_curves(segf.data(), static_cast<crd::u32>(nseg));
    REQUIRE(scene != nullptr);

    crd::containers::Array<float> outf(&alloc);
    outf.resize(uz(nray * 2), 0.0F);
    gpu::VulkanRayTracingContext::Binding bindings[3] = {};
    bindings[0].upload  = segf.data();
    bindings[0].bytes   = static_cast<crd::u64>(nseg) * 8U * sizeof(float);
    bindings[0].binding = 1U;
    bindings[1].upload  = rayf.data();
    bindings[1].bytes   = static_cast<crd::u64>(nray) * 6U * sizeof(float);
    bindings[1].binding = 2U;
    bindings[2].readback = outf.data();
    bindings[2].bytes    = static_cast<crd::u64>(nray) * 2U * sizeof(float);
    bindings[2].binding  = 3U;
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bindings, 3),
                              static_cast<crd::u32>(nray / 64)));

    constexpr double k_tmax = 50.0; // a MISS returns tmax, so classify against it, not a fixed sentinel
    int    hits = 0;
    int    disagree = 0;
    int    cpu_only = 0; // oracle hit, device missed  => AABB too small / commit rejected
    int    gpu_only = 0; // device hit, oracle missed  => false positive in the shader
    double worst = 0.0;
    for (int i = 0; i < nray; ++i)
    {
        const bool ch = ref[uz(i * 2)] < k_tmax - 0.5;
        const bool gh = static_cast<double>(outf[uz(i * 2)]) < k_tmax - 0.5;
        if (ch != gh)
        {
            ++disagree;
            if (ch) { ++cpu_only; } else { ++gpu_only; }
            if (disagree <= 6)
            {
                std::printf("      miss #%d: ray %d  oracle t=%.4f u=%.3f  device t=%.4f  ray=(%.3f,%.3f,%.3f)\n",
                            disagree, i, ref[uz(i * 2)], ref[uz(i * 2 + 1)], static_cast<double>(outf[uz(i * 2)]),
                            static_cast<double>(rayf[uz(i * 6)]), static_cast<double>(rayf[uz(i * 6 + 1)]),
                            static_cast<double>(rayf[uz(i * 6 + 2)]));
            }
            continue;
        }
        if (!ch) { continue; }
        ++hits;
        worst = std::max(worst, std::fabs(static_cast<double>(outf[uz(i * 2)]) - ref[uz(i * 2)]));
        worst = std::max(worst, std::fabs(static_cast<double>(outf[uz(i * 2 + 1)]) - ref[uz(i * 2 + 1)]));
    }
    std::printf("[Vulkan B18-f TraceRayCurves] %d rays over %d segments, %d hits  maxabs = %.3e  hit/miss disagreements = %d\n",
                nray, nseg, hits, worst, disagree);
    // The DIRECTION of a disagreement names the defect: oracle-only means the AABBs under-cover the swept volume or the
    // commit was rejected; device-only means the intersector reports hits in empty space. Only worth printing when there
    // is one to explain.
    if (disagree != 0)
    {
        std::printf("    disagreement direction: oracle-only %d (device MISSED), device-only %d (false POSITIVE)\n",
                    cpu_only, gpu_only);
    }
    CHECK(hits > 32);     // the groom must actually be hit, or the comparison proves nothing
    CHECK(disagree == 0); // hardware traversal must find exactly the hits the brute-force oracle finds
    CHECK(worst < 1.0e-3);
}

// D-007 P2/P3: the RT PIPELINE authored in CKIR — raygen/closest-hit/miss emitted from KEntry stages (traceRay op + ray payload +
// LaunchId/HitT builtins), plus the SER reorder HINT that the emitter honors on an SER adapter (hitObjectNV flow) and drops
// elsewhere. Same result as the hand-authored FA-2 pipeline and the oracle — proving the vendor shader features live in CKIR and
// degrade gracefully. Run on the RTX 4070 with SER active.
TEST_CASE("D-007 P2: CKIR-authored RT pipeline (+ SER hint) on Vulkan == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());
    if (!rt.capabilities().has(gpu::RtFeature::RtPipeline)) { WARN("no RT pipeline; skipping"); return; }
    const bool ser = rt.capabilities().has(gpu::RtFeature::ShaderReorder);

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph grg(&alloc);
    kir::KGraph gch(&alloc);
    kir::KGraph gms(&alloc);
    const kir::KEntry erg = kir::rt::build_rt_pipeline_raygen(grg, ser); // request the SER reorder hint iff the adapter has SER
    const kir::KEntry ech = kir::rt::build_rt_pipeline_closesthit(gch);
    const kir::KEntry ems = kir::rt::build_rt_pipeline_miss(gms);
    kir::GlslKernel krg(&alloc);
    kir::GlslKernel kch(&alloc);
    kir::GlslKernel kms(&alloc);
    REQUIRE(kir::emit_rt_stage_glsl(grg, erg, &alloc, krg, ser));
    REQUIRE(kir::emit_rt_stage_glsl(gch, ech, &alloc, kch, false));
    REQUIRE(kir::emit_rt_stage_glsl(gms, ems, &alloc, kms, false));
    INFO("raygen GLSL:\n" << krg.source.c_str());
    const auto crg = gpu::compile_glsl_to_spirv(gpu::ShaderStage::RayGen, crd::containers::to_view(krg.source), "rg", &alloc);
    const auto cch = gpu::compile_glsl_to_spirv(gpu::ShaderStage::ClosestHit, crd::containers::to_view(kch.source), "ch", &alloc);
    const auto cms = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Miss, crd::containers::to_view(kms.source), "ms", &alloc);
    INFO(crg.error_message.c_str());
    REQUIRE(crg.ok); REQUIRE(cch.ok); REQUIRE(cms.ok);

    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    auto scene = rt.build_scene(verts, 1U);
    REQUIRE(scene != nullptr);
    constexpr crd::u32 num_pts = 4U;
    const float rd[num_pts][6] = {{0.2F,0.2F,0,0,0,1}, {0.2F,0.2F,0,0,0,-1}, {5,5,0,0,0,1}, {0.1F,0.1F,1,0,0,1}};
    crd::containers::Array<float> rays(&alloc);
    crd::containers::Array<float> got(&alloc);
    rays.resize(static_cast<crd::usize>(num_pts) * 6U, 0.0F); got.resize(num_pts, 0.0F);
    for (crd::u32 i = 0; i < num_pts; ++i) { for (int c = 0; c < 6; ++c) { rays[i * 6U + c] = rd[i][c]; } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U}};
    REQUIRE(rt.trace_rays_pipeline(*scene, crd::containers::ConstSpan<crd::u8>(crg.spirv.data(), crg.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cms.spirv.data(), cms.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cch.spirv.data(), cch.spirv.size()),
                                   crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), num_pts, 1U));
    INFO("CKIR RT pipeline" << (ser ? " (SER)" : "") << " t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4);
    CHECK(got[1] > 1.0e29F);
    CHECK(got[2] > 1.0e29F);
    CHECK(crd::math::abs(static_cast<double>(got[3]) - 1.0) < 1.0e-4);
}

// D-007 P4: the portable OMM path + its CKIR ANY-HIT ALPHA fallback. (1) build_scene_alpha picks HW OMM when available (graceful
// degradation logic). (2) the fallback MECHANISM: a NON-OPAQUE triangle + the CKIR any-hit alpha shader (ignore where the
// barycentric u+v < 0.5) — rays through the "transparent" half PASS THROUGH (miss), the rest hit. Alpha-tested geometry without
// hardware OMM, from CKIR, correct.
TEST_CASE("D-007 P4: portable OMM + CKIR any-hit alpha fallback on Vulkan", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());
    if (!rt.capabilities().has(gpu::RtFeature::RtPipeline)) { WARN("no RT pipeline; skipping"); return; }

    crd::memory::TlsfAllocator alloc(16U << 20U);
    const float verts[9] = {0.0F, 0.0F, 1.0F, 2.0F, 0.0F, 1.0F, 0.0F, 2.0F, 1.0F};
    const crd::u8 omm_bits[2] = {0xFFU, 0x00U};

    // (1) graceful-degradation LOGIC: build_scene_alpha chooses HW OMM here (the RTX 4070 has it).
    bool fell_back = true;
    auto omm_scene = rt.build_scene_alpha(verts, 1U, omm_bits, 2U, &fell_back);
    REQUIRE(omm_scene != nullptr);
    CHECK(rt.capabilities().has(gpu::RtFeature::OpacityMicromap) == !fell_back); // used HW OMM iff the adapter has it

    // (2) the CKIR ANY-HIT ALPHA fallback mechanism: non-opaque scene + the 4-stage any-hit pipeline.
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    auto scene = rt.build_scene_instanced(verts, 1U, identity, 1U, /*opaque=*/false);
    REQUIRE(scene != nullptr);
    kir::KGraph grg(&alloc);
    kir::KGraph gch(&alloc);
    kir::KGraph gms(&alloc);
    kir::KGraph gah(&alloc);
    const kir::KEntry erg = kir::rt::build_rt_pipeline_raygen(grg, false);
    const kir::KEntry ech = kir::rt::build_rt_pipeline_closesthit(gch);
    const kir::KEntry ems = kir::rt::build_rt_pipeline_miss(gms);
    const kir::KEntry eah = kir::rt::build_rt_pipeline_anyhit_alpha(gah, 0.5); // transparent where u+v < 0.5
    const auto comp = [&](gpu::ShaderStage st, const kir::KGraph& g, const kir::KEntry& e) {
        kir::GlslKernel k(&alloc); REQUIRE(kir::emit_rt_stage_glsl(g, e, &alloc, k, false));
        auto spv = gpu::compile_glsl_to_spirv(st, crd::containers::to_view(k.source), "s", &alloc); INFO(spv.error_message.c_str()); REQUIRE(spv.ok); return spv;
    };
    const auto crg = comp(gpu::ShaderStage::RayGen, grg, erg);
    const auto cch = comp(gpu::ShaderStage::ClosestHit, gch, ech);
    const auto cms = comp(gpu::ShaderStage::Miss, gms, ems);
    const auto cah = comp(gpu::ShaderStage::AnyHit, gah, eah);

    constexpr crd::u32 num_pts = 64U;
    crd::containers::Array<float> rays(&alloc);
    crd::containers::Array<float> got(&alloc);
    rays.resize(static_cast<crd::usize>(num_pts) * 6U, 0.0F); got.resize(num_pts, 0.0F);
    crd::u32 interior = 0;
    for (crd::u32 j = 0; j < 8U; ++j) { for (crd::u32 i = 0; i < 8U; ++i) {
        const crd::u32 p = j * 8U + i; const float x = 0.15F + 1.7F / 7.0F * static_cast<float>(i); const float y = 0.15F + 1.7F / 7.0F * static_cast<float>(j);
        rays[p * 6U + 0U] = x; rays[p * 6U + 1U] = y; rays[p * 6U + 5U] = 1.0F; if (x + y < 1.9F) { ++interior; } } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U}};
    REQUIRE(rt.trace_rays_pipeline(*scene, crd::containers::ConstSpan<crd::u8>(crg.spirv.data(), crg.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cms.spirv.data(), cms.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cch.spirv.data(), cch.spirv.size()),
                                   crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), num_pts, 1U,
                                   crd::containers::ConstSpan<crd::u8>(cah.spirv.data(), cah.spirv.size())));
    crd::u32 hit = 0;
    crd::u32 pass = 0;
    for (crd::u32 p = 0; p < num_pts; ++p) { if (static_cast<double>(got[p]) > 0.9 && static_cast<double>(got[p]) < 1.1) { ++hit; } else if (static_cast<double>(got[p]) > 1.0e29) { ++pass; } }
    INFO("any-hit alpha: interior=" << interior << " hit(u+v>=0.5)=" << hit << " passed-through(u+v<0.5)=" << pass);
    CHECK(hit > 3);   // the opaque half (u+v>=0.5) hits
    CHECK(pass > 3);  // the transparent half (u+v<0.5) PASSES THROUGH — the any-hit alpha fallback works
}

// D-007 P5: portable cluster-topology scene-build with graceful degradation — clusters when supported (RTX 4070), transparent
// fallback to a standard BLAS otherwise. BOTH paths give the same oracle hits (clusters are a memory-layout optimisation).
TEST_CASE("D-007 P5: portable scalable scene-build (cluster or standard-BLAS fallback) == CPU reference", "[gpu-context][vulkan][gpu][rt]")
{
    gpu::GpuContextConfig cfg; cfg.backend = gpu::GpuBackend::Vulkan; cfg.headless = true;
    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph g(&alloc);
    const kir::KEntry e = build_trace_kernel(g, 64);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "p5", &alloc);
    REQUIRE(spv.ok);
    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    const float rd[4][6] = {{0.2F,0.2F,0,0,0,1}, {0.2F,0.2F,0,0,0,-1}, {5,5,0,0,0,1}, {0.1F,0.1F,1,0,0,1}};

    for (int mode = 0; mode < 2; ++mode) // mode 0 = prefer clusters (uses HW here); mode 1 = force standard BLAS
    {
        bool fell = false;
        auto scene = rt.build_scene_scalable(verts, 1U, /*prefer_clusters=*/mode == 0, &fell);
        REQUIRE(scene != nullptr);
        constexpr crd::u32 num_pts = 64U;
        crd::containers::Array<float> rays(&alloc);
        crd::containers::Array<float> got(&alloc);
        rays.resize(static_cast<crd::usize>(num_pts) * 6U, 0.0F); got.resize(num_pts, 0.0F);
        for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }
        gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(num_pts) * sizeof(float), 2U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));
        INFO("scalable mode=" << mode << " fell_back=" << fell << " t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
        CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4); // both cluster + std BLAS give identical hits
        CHECK(got[1] > 1.0e29F); CHECK(got[2] > 1.0e29F);
        CHECK(crd::math::abs(static_cast<double>(got[3]) - 1.0) < 1.0e-4);
    }
}

// ── CEIR-19c STAGE-1 DEVICE GATE: the ceir.rt->gpu execution BRIDGE. ──────────────────────────────────────────────────
// An AUTHORED ceir.rt program (blas_build -> instance_populate -> tlas_build -> ray_query) is lowered (lower_region) and
// DRIVEN onto this VulkanRayTracingContext through execute_rt_lowered's caller HOOKS (crd-ceir-gpu names no backend). The
// ray_query's kernel is the committed assets/ckir/rt_witness.ckir (mandate #1: the .ckir is the source; NO C++ builder). GPU
// hit prim-id + hit/miss are BIT-EXACT vs eval_cpu_kernel's TraceRayHit oracle (SAME loaded graph, AS-slot = triangle soup)
// AND vs KNOWN ANALYTIC values (t=[2,miss,miss,1], prim 0/miss/miss/0) — the analytic pin is MANDATORY: both sides run the
// SAME graph, so a hand-authoring error is invisible to parity alone (the cf(node) symmetric-bug scar). t is tolerance-class
// (the oracle traverses unrounded f64, the GPU f32 HW BVH). Axis-aligned exact-FP scene + clean-separation rays ⇒ hit/miss +
// prim-id are reliably bit-exact.
namespace
{
struct RtGateState
{
    gpu::VulkanRayTracingContext* rt         = nullptr;
    const float*                  verts      = nullptr;
    crd::u32                      ntris      = 0U;
    std::unique_ptr<gpu::RtScene> scene;
    const crd::u8*                spirv      = nullptr;
    crd::usize                    spirv_size = 0U;
    crd::u32                      last_gx    = 0U; // CEIR-19c stage 2: the last dispatch's grid.x — pins the count-driven dispatch
};
// CEIR-19c stage 2: the deterministic triple32 (Wellons) fold — the HOST-side decision-hash. u32 avalanche wraps mod 2^32 on
// both host + oracle; fold ONLY decision ints (indices/counts/flags), NEVER a t-derived float (the stage-1 trap).
crd::u32 rt_tri32(crd::u32 x)
{
    x ^= x >> 17U;
    x *= 0xED5AD4BBU;
    x ^= x >> 11U;
    x *= 0xAC4C1B51U;
    x ^= x >> 15U;
    x *= 0x31848BABU;
    x ^= x >> 14U;
    return x;
}
crd::u32 rt_fold(crd::u32 h, crd::u32 v) { return rt_tri32(h ^ v); }
cg::RtSceneHandle rt_gate_build_scene(const ceir::Operation* /*accel_op*/, void* user)
{
    auto* s = static_cast<RtGateState*>(user);
    if (s->scene == nullptr) { s->scene = s->rt->build_scene(s->verts, s->ntris); } // fused: build once on the first AccelBuild
    return (s->scene != nullptr) ? cg::RtSceneHandle{1U} : cg::RtSceneHandle{0U};
}
crd::containers::ConstSpan<crd::u8> rt_gate_kernel_bytes(const ceir::Operation* /*ray_query*/, void* user)
{
    auto* s = static_cast<RtGateState*>(user);
    return crd::containers::ConstSpan<crd::u8>(s->spirv, s->spirv_size);
}
bool rt_gate_trace_dispatch(cg::RtSceneHandle tlas, crd::containers::ConstSpan<crd::u8> bytes,
                            crd::containers::ConstSpan<cg::RtHostBinding> binds, crd::u32 gx, crd::u32 /*gy*/,
                            crd::u32 /*gz*/, void* user)
{
    auto* s = static_cast<RtGateState*>(user);
    if (tlas == 0U || s->scene == nullptr) { return false; }
    s->last_gx = gx; // pin: the harness CHECKs the shade dispatch's gx == the compact's hit_count readback
    gpu::VulkanRayTracingContext::Binding vb[8];
    const crd::u32                        n = static_cast<crd::u32>(binds.size() < 8U ? binds.size() : 8U);
    for (crd::u32 i = 0; i < n; ++i)
    {
        vb[i].upload   = binds[i].upload;
        vb[i].readback = binds[i].readback;
        vb[i].bytes    = binds[i].bytes;
        vb[i].binding  = i + 1U; // the TLAS is implicit @ descriptor 0; SSBOs follow at 1,2,3 (REN-38-A9)
    }
    return s->rt->trace_dispatch(*s->scene, bytes,
                                 crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(vb, n), gx);
}
} // namespace

TEST_CASE("CEIR-19c: the ceir.rt->gpu bridge (execute_rt_lowered) traces the authored rt_witness.ckir == oracle + analytic (Vulkan)",
          "[gpu-context][vulkan][gpu][rt][ceir19c]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);

    // 1. Load + compile the AUTHORED rt_witness.ckir (the .ckir is the SOURCE; mandate #1).
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/rt_witness.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);
    crd::containers::Array<char> src(&alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_witness", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // 2. The trivial scene: 1 axis-aligned tri @ z=2; 64 rays (one workgroup — the witness has no tail guard). First 4 = the
    //    RT-1 rays (hit@2, miss, miss, hit@1); rays 4..63 = a guaranteed miss (origin z=100 behind, dir +z pointing away).
    constexpr crd::u32 num_tris = 1U;
    constexpr crd::u32 nrays    = 64U;
    const float        verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(nrays) * 6U, 0.0F);
    const float seed[4][6] = {
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},  // hit @ t=2, prim 0
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, -1.0F}, // miss (points away)
        {5.0F, 5.0F, 0.0F, 0.0F, 0.0F, 1.0F},  // miss (outside the tri)
        {0.1F, 0.1F, 1.0F, 0.0F, 0.0F, 1.0F},  // hit @ t=1, prim 0
    };
    for (crd::u32 r = 0; r < nrays; ++r)
    {
        if (r < 4U)
        {
            for (int col = 0; col < 6; ++col) { rays[r * 6U + static_cast<crd::u32>(col)] = seed[r][col]; }
        }
        else
        {
            rays[r * 6U + 2U] = 100.0F; // origin.z = 100 (behind the tri)
            rays[r * 6U + 5U] = 1.0F;   // dir.z = +1 (away) -> miss
        }
    }
    crd::containers::Array<float>    gpu_t(&alloc);
    crd::containers::Array<crd::u32> gpu_prim(&alloc);
    gpu_t.resize(nrays, 0.0F);
    gpu_prim.resize(nrays, 0U);

    // 3. The AUTHORED ceir.rt program: blas -> instance -> tlas -> ray_query(grid, %tlas, rays, hit_t, hit_prim).
    ceir::Context c(&alloc);
    (void)ceir::func::register_dialect(c);
    (void)ceir::arith::register_arith_ops(c);
    (void)ceir::resource::register_resource_ops(c);
    (void)ceir::rt::register_dialect(c);
    ceir::Block* const body  = c.create_block(0U);
    const auto         mkbuf = [&]() {
        ceir::Operation* const d =
            c.create_operation(c.intern_op("resource", "declare"), {}, 1U, c.type_buffer(ceir::BufferMode::Plain, c.type_f32()));
        body->append(d);
        return d->result(0U);
    };
    const auto mkidx = [&](crd::i64 v) {
        ceir::Operation* const o = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_index());
        c.set_attr(o, "value", c.attr_int(v));
        body->append(o);
        return o->result(0U);
    };
    ceir::Value* const     geom = mkbuf();
    ceir::Operation* const blas = ceir::rt::build_blas_build(c, geom, ceir::rt::type_blas(c));
    body->append(blas);
    ceir::Value* const     xf   = mkbuf();
    ceir::Operation* const inst = ceir::rt::build_instance_populate(c, blas->result(0U), xf, c.attr_int(1), c.type_i32());
    body->append(inst);
    ceir::Operation* const tlas = ceir::rt::build_tlas_build(c, inst->result(0U), ceir::rt::type_tlas(c));
    body->append(tlas);
    ceir::Value* const rays_v  = mkbuf();
    ceir::Value* const hit_t_v = mkbuf();
    ceir::Value* const hit_p_v = mkbuf();
    ceir::Value* const g1      = mkidx(1); // groups (1,1,1): 64 threads = one workgroup
    ceir::Value*       rq_ops[7] = {g1, g1, g1, tlas->result(0U), rays_v, hit_t_v, hit_p_v};
    ceir::Operation* const rq   = c.create_operation(c.intern_op("rt", "ray_query"),
                                                     crd::containers::ConstSpan<ceir::Value*>(rq_ops, 7U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("rt_witness")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rww"))); // rays read, hit_t write, hit_prim write
    body->append(rq);

    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    // 4. Drive the lowered list through execute_rt_lowered's hooks onto the real RT context.
    RtGateState st;
    st.rt         = &rt;
    st.verts      = verts;
    st.ntris      = num_tris;
    st.spirv      = spv.spirv.data();
    st.spirv_size = spv.spirv.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_build_scene;
    hooks.kernel_bytes   = &rt_gate_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[3];
    binds[0] = {c.resource_root(rays_v), rays.data(), nullptr, static_cast<crd::u64>(nrays) * 6U * sizeof(float)};
    binds[1] = {c.resource_root(hit_t_v), nullptr, gpu_t.data(), static_cast<crd::u64>(nrays) * sizeof(float)};
    binds[2] = {c.resource_root(hit_p_v), nullptr, gpu_prim.data(), static_cast<crd::u64>(nrays) * sizeof(crd::u32)};
    const cg::ExecuteError err =
        cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                               crd::containers::ConstSpan<cg::RtHostBinding>(binds, 3U));
    REQUIRE(err == cg::ExecuteError::None);

    // 5. The oracle: eval_cpu_kernel on the SAME loaded graph, AS-slot bound as the triangle soup [ntri, v0..v2].
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tris);
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> rays64(&alloc);
    rays64.resize(static_cast<crd::usize>(nrays) * 6U, 0.0);
    for (crd::usize i = 0; i < rays64.size(); ++i) { rays64[i] = static_cast<crd::f64>(rays[i]); }
    crd::containers::Array<crd::f64> ref_t(&alloc);
    crd::containers::Array<crd::f64> ref_p(&alloc);
    ref_t.resize(nrays, 0.0);
    ref_p.resize(nrays, 0.0);
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                 {rays64.data(), static_cast<int>(rays64.size()), 0, 1},
                                 {ref_t.data(), static_cast<int>(nrays), 0, 2},
                                 {ref_p.data(), static_cast<int>(nrays), 0, 3}};
    kir::eval_cpu_kernel(kg, ke, bufs, 4, 64U, &alloc, 1U);

    // 6. GPU == oracle over ALL 64 threads: prim-id + hit/miss BIT-EXACT, t within tolerance.
    crd::u32 checked_hits = 0U;
    for (crd::u32 r = 0; r < nrays; ++r)
    {
        const bool cpu_hit = ref_t[r] < 1.0e29;
        const bool gpu_hit = static_cast<double>(gpu_t[r]) < 1.0e29;
        INFO("ray " << r << ": gpu_prim=" << gpu_prim[r] << " ref_prim=" << static_cast<crd::u32>(ref_p[r]) << " gpu_t="
                    << gpu_t[r] << " ref_t=" << ref_t[r]);
        CHECK(gpu_hit == cpu_hit);                             // hit/miss decision BIT-EXACT
        CHECK(gpu_prim[r] == static_cast<crd::u32>(ref_p[r])); // prim-id BIT-EXACT (the decision-derived integer)
        if (cpu_hit) { CHECK(crd::math::abs(static_cast<double>(gpu_t[r]) - ref_t[r]) < 1.0e-3); ++checked_hits; }
    }
    CHECK(checked_hits >= 2U); // the two analytic hits actually exercised the tolerance compare

    // 7. ANALYTIC pins (MANDATORY — parity alone is blind to a symmetric authoring error; the cf(node) scar).
    CHECK(crd::math::abs(static_cast<double>(gpu_t[0]) - 2.0) < 1.0e-3); // ray 0 hits the tri @ t=2
    CHECK(gpu_prim[0] == 0U);
    CHECK(gpu_prim[1] == 0xFFFFFFFFU); // ray 1 miss
    CHECK(gpu_prim[2] == 0xFFFFFFFFU); // ray 2 miss
    CHECK(crd::math::abs(static_cast<double>(gpu_t[3]) - 1.0) < 1.0e-3); // ray 3 hits the tri @ t=1
    CHECK(gpu_prim[3] == 0U);
}

// ── CEIR-19c STAGE 2: the SERIAL-COMPACT kernel, ISOLATED device gate (the first wavefront kernel, proven standalone). ──────
// The authored wavefront_compact.ckir is dispatched via a ceir.rt ray_query (a DEAD TLAS@0 — the compact never traces) through
// execute_rt_lowered. Synthetic hit_flags → the GPU compacted queue + count are BIT-EXACT vs eval_cpu_kernel (SAME graph —
// which ALSO proves the oracle tolerates the dead AccelStructDecl, the advisor's 2-min check) AND vs the HAND-COMPUTED dense
// queue (the mandatory analytic pin). groups=(1,1,1), the kernel is local_size=1 (one serial thread).
TEST_CASE("CEIR-19c: the authored serial-compact kernel compacts hit-flags == oracle + analytic (Vulkan)",
          "[gpu-context][vulkan][gpu][rt][ceir19c]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);

    // 1. Load + compile the AUTHORED wavefront_compact.ckir.
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/wavefront_compact.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);
    crd::containers::Array<char> src(&alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "wf_compact", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // 2. Synthetic flags [1,0,1,1,0,0,1,0] -> compacted=[0,2,3,6], count=4 (hand-computed).
    constexpr crd::u32 k_n         = 8U;
    const crd::u32     flags[k_n]  = {1U, 0U, 1U, 1U, 0U, 0U, 1U, 0U};
    crd::containers::Array<crd::u32> gpu_comp(&alloc);
    crd::containers::Array<crd::u32> gpu_count(&alloc);
    gpu_comp.resize(k_n, 0xEEEEEEEEU); // sentinel fill so an unwritten slot is visible
    gpu_count.resize(1U, 0U);

    // 3. The ceir.rt program: blas->instance->tlas->ray_query(grid, %tlas(DEAD), flags, compacted, count).
    ceir::Context c(&alloc);
    (void)ceir::func::register_dialect(c);
    (void)ceir::arith::register_arith_ops(c);
    (void)ceir::resource::register_resource_ops(c);
    (void)ceir::rt::register_dialect(c);
    ceir::Block* const body  = c.create_block(0U);
    const auto         mkbuf = [&]() {
        ceir::Operation* const d =
            c.create_operation(c.intern_op("resource", "declare"), {}, 1U, c.type_buffer(ceir::BufferMode::Plain, c.type_f32()));
        body->append(d);
        return d->result(0U);
    };
    const auto mkidx = [&](crd::i64 v) {
        ceir::Operation* const o = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_index());
        c.set_attr(o, "value", c.attr_int(v));
        body->append(o);
        return o->result(0U);
    };
    ceir::Value* const     geom = mkbuf();
    ceir::Operation* const blas = ceir::rt::build_blas_build(c, geom, ceir::rt::type_blas(c));
    body->append(blas);
    ceir::Value* const     xf   = mkbuf();
    ceir::Operation* const inst = ceir::rt::build_instance_populate(c, blas->result(0U), xf, c.attr_int(1), c.type_i32());
    body->append(inst);
    ceir::Operation* const tlas = ceir::rt::build_tlas_build(c, inst->result(0U), ceir::rt::type_tlas(c));
    body->append(tlas);
    ceir::Value* const flags_v = mkbuf();
    ceir::Value* const comp_v  = mkbuf();
    ceir::Value* const count_v = mkbuf();
    ceir::Value* const g1      = mkidx(1);
    ceir::Value*       rq_ops[7] = {g1, g1, g1, tlas->result(0U), flags_v, comp_v, count_v};
    ceir::Operation* const rq   = c.create_operation(c.intern_op("rt", "ray_query"),
                                                     crd::containers::ConstSpan<ceir::Value*>(rq_ops, 7U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("wavefront_compact")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rww")));
    body->append(rq);

    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    // 4. Drive it. The scene is DEAD (the compact never traces) but build_scene still runs (a valid 1-tri soup).
    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    RtGateState st;
    st.rt         = &rt;
    st.verts      = verts;
    st.ntris      = 1U;
    st.spirv      = spv.spirv.data();
    st.spirv_size = spv.spirv.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_build_scene;
    hooks.kernel_bytes   = &rt_gate_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[3];
    binds[0] = {c.resource_root(flags_v), &flags[0], nullptr, static_cast<crd::u64>(k_n) * sizeof(crd::u32)};
    binds[1] = {c.resource_root(comp_v), nullptr, gpu_comp.data(), static_cast<crd::u64>(k_n) * sizeof(crd::u32)};
    binds[2] = {c.resource_root(count_v), nullptr, gpu_count.data(), sizeof(crd::u32)};
    const cg::ExecuteError err =
        cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                               crd::containers::ConstSpan<cg::RtHostBinding>(binds, 3U));
    REQUIRE(err == cg::ExecuteError::None);

    // 5. Oracle: eval_cpu_kernel on the SAME graph — proves it tolerates the DEAD AccelStructDecl (no TraceRay reads it).
    crd::containers::Array<crd::f64> geo(&alloc); // the dead AS binding (bufs[0]); the compact never reads it
    geo.resize(1U, 0.0);
    crd::containers::Array<crd::f64> flags64(&alloc);
    flags64.resize(k_n, 0.0);
    for (crd::u32 i = 0; i < k_n; ++i) { flags64[i] = static_cast<crd::f64>(flags[i]); }
    crd::containers::Array<crd::f64> comp_ref(&alloc);
    crd::containers::Array<crd::f64> count_ref(&alloc);
    comp_ref.resize(k_n, 0.0);
    count_ref.resize(1U, 0.0);
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                 {flags64.data(), static_cast<int>(k_n), 0, 1},
                                 {comp_ref.data(), static_cast<int>(k_n), 0, 2},
                                 {count_ref.data(), 1, 0, 3}};
    kir::eval_cpu_kernel(kg, ke, bufs, 4, 1U, &alloc, 1U);

    // 6. GPU == oracle == analytic (the compacted queue + count are DECISION integers — bit-exact).
    const crd::u32 expect_comp[4] = {0U, 2U, 3U, 6U};
    CHECK(gpu_count[0] == 4U);                         // analytic count
    CHECK(static_cast<crd::u32>(count_ref[0]) == 4U);  // oracle count
    for (crd::u32 j = 0; j < 4U; ++j)
    {
        INFO("slot " << j << ": gpu=" << gpu_comp[j] << " oracle=" << static_cast<crd::u32>(comp_ref[j]) << " analytic="
                     << expect_comp[j]);
        CHECK(gpu_comp[j] == expect_comp[j]);                     // GPU == hand-computed (the mandatory analytic pin)
        CHECK(gpu_comp[j] == static_cast<crd::u32>(comp_ref[j])); // GPU == oracle (bit-exact)
    }
}

// ── CEIR-19c STAGE 2: the TRACE kernel, ISOLATED device gate. The authored wavefront_trace.ckir is dispatched via a ceir.rt
// ray_query (REAL TLAS — it traces) through execute_rt_lowered. Known rays → GPU hit_flag[] is BIT-EXACT vs eval_cpu_kernel +
// the ANALYTIC pin (hit rays flag=1, miss flag=0 — the DECISION int the compact consumes). hit_t is tolerance-class.
TEST_CASE("CEIR-19c: the authored wavefront trace kernel writes hit-flags == oracle + analytic (Vulkan)",
          "[gpu-context][vulkan][gpu][rt][ceir19c]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);

    std::ifstream f(CRD_REPO_DIR "/assets/ckir/wavefront_trace.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);
    crd::containers::Array<char> src(&alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "wf_trace", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // 1 tri @ z=2; 64 rays (first 4 = the stage-1 rays: hit@2, miss, miss, hit@1; rays 4..63 = guaranteed miss).
    constexpr crd::u32 num_tris = 1U;
    constexpr crd::u32 nrays    = 64U;
    const float        verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(nrays) * 6U, 0.0F);
    const float seed[4][6] = {{0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},
                              {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, -1.0F},
                              {5.0F, 5.0F, 0.0F, 0.0F, 0.0F, 1.0F},
                              {0.1F, 0.1F, 1.0F, 0.0F, 0.0F, 1.0F}};
    for (crd::u32 r = 0; r < nrays; ++r)
    {
        if (r < 4U)
        {
            for (int col = 0; col < 6; ++col) { rays[r * 6U + static_cast<crd::u32>(col)] = seed[r][col]; }
        }
        else
        {
            rays[r * 6U + 2U] = 100.0F;
            rays[r * 6U + 5U] = 1.0F;
        }
    }
    crd::containers::Array<crd::u32> gpu_flag(&alloc);
    crd::containers::Array<float>    gpu_t(&alloc);
    gpu_flag.resize(nrays, 0xEEEEEEEEU);
    gpu_t.resize(nrays, 0.0F);

    ceir::Context c(&alloc);
    (void)ceir::func::register_dialect(c);
    (void)ceir::arith::register_arith_ops(c);
    (void)ceir::resource::register_resource_ops(c);
    (void)ceir::rt::register_dialect(c);
    ceir::Block* const body  = c.create_block(0U);
    const auto         mkbuf = [&]() {
        ceir::Operation* const d =
            c.create_operation(c.intern_op("resource", "declare"), {}, 1U, c.type_buffer(ceir::BufferMode::Plain, c.type_f32()));
        body->append(d);
        return d->result(0U);
    };
    const auto mkidx = [&](crd::i64 v) {
        ceir::Operation* const o = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_index());
        c.set_attr(o, "value", c.attr_int(v));
        body->append(o);
        return o->result(0U);
    };
    ceir::Value* const     geom = mkbuf();
    ceir::Operation* const blas = ceir::rt::build_blas_build(c, geom, ceir::rt::type_blas(c));
    body->append(blas);
    ceir::Value* const     xf   = mkbuf();
    ceir::Operation* const inst = ceir::rt::build_instance_populate(c, blas->result(0U), xf, c.attr_int(1), c.type_i32());
    body->append(inst);
    ceir::Operation* const tlas = ceir::rt::build_tlas_build(c, inst->result(0U), ceir::rt::type_tlas(c));
    body->append(tlas);
    ceir::Value* const rays_v = mkbuf();
    ceir::Value* const flag_v = mkbuf();
    ceir::Value* const t_v    = mkbuf();
    ceir::Value* const g1     = mkidx(1);
    ceir::Value*       rq_ops[7] = {g1, g1, g1, tlas->result(0U), rays_v, flag_v, t_v};
    ceir::Operation* const rq   = c.create_operation(c.intern_op("rt", "ray_query"),
                                                     crd::containers::ConstSpan<ceir::Value*>(rq_ops, 7U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("wavefront_trace")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rww")));
    body->append(rq);

    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    RtGateState st;
    st.rt         = &rt;
    st.verts      = verts;
    st.ntris      = num_tris;
    st.spirv      = spv.spirv.data();
    st.spirv_size = spv.spirv.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_build_scene;
    hooks.kernel_bytes   = &rt_gate_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[3];
    binds[0] = {c.resource_root(rays_v), rays.data(), nullptr, static_cast<crd::u64>(nrays) * 6U * sizeof(float)};
    binds[1] = {c.resource_root(flag_v), nullptr, gpu_flag.data(), static_cast<crd::u64>(nrays) * sizeof(crd::u32)};
    binds[2] = {c.resource_root(t_v), nullptr, gpu_t.data(), static_cast<crd::u64>(nrays) * sizeof(float)};
    const cg::ExecuteError err =
        cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                               crd::containers::ConstSpan<cg::RtHostBinding>(binds, 3U));
    REQUIRE(err == cg::ExecuteError::None);

    // Oracle: eval_cpu_kernel on the SAME graph, AS-slot = the triangle soup.
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tris);
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> rays64(&alloc);
    rays64.resize(static_cast<crd::usize>(nrays) * 6U, 0.0);
    for (crd::usize i = 0; i < rays64.size(); ++i) { rays64[i] = static_cast<crd::f64>(rays[i]); }
    crd::containers::Array<crd::f64> flag_ref(&alloc);
    crd::containers::Array<crd::f64> t_ref(&alloc);
    flag_ref.resize(nrays, 0.0);
    t_ref.resize(nrays, 0.0);
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                 {rays64.data(), static_cast<int>(rays64.size()), 0, 1},
                                 {flag_ref.data(), static_cast<int>(nrays), 0, 2},
                                 {t_ref.data(), static_cast<int>(nrays), 0, 3}};
    kir::eval_cpu_kernel(kg, ke, bufs, 4, 64U, &alloc, 1U);

    // GPU hit_flag == oracle (bit-exact, all 64) + the ANALYTIC pin (rays 0,3 hit; 1,2 + fillers miss).
    for (crd::u32 r = 0; r < nrays; ++r)
    {
        INFO("ray " << r << ": gpu_flag=" << gpu_flag[r] << " ref_flag=" << static_cast<crd::u32>(flag_ref[r]));
        CHECK(gpu_flag[r] == static_cast<crd::u32>(flag_ref[r]));
        CHECK((gpu_flag[r] == 0U || gpu_flag[r] == 1U)); // a decision int, never garbage
    }
    CHECK(gpu_flag[0] == 1U); // hit @ t=2
    CHECK(gpu_flag[1] == 0U); // miss
    CHECK(gpu_flag[2] == 0U); // miss
    CHECK(gpu_flag[3] == 1U); // hit @ t=1
    CHECK(crd::math::abs(static_cast<double>(gpu_t[0]) - 2.0) < 1.0e-3); // hit_t tolerance (feeds shade only)
    CHECK(crd::math::abs(static_cast<double>(gpu_t[3]) - 1.0) < 1.0e-3);
}

// ── CEIR-19c STAGE 2: the SHADE kernel, ISOLATED device gate. The authored wavefront_shade.ckir is dispatched via a ceir.rt
// ray_query (REAL TLAS = the occluder; shadow rays) through execute_rt_lowered, groups=count (the count-driven dispatch). A
// deterministic occluder + synthetic hit points (one under the occluder, one clear) → GPU lit/shadowed decision[] BIT-EXACT vs
// eval_cpu_kernel + the ANALYTIC pin. Clean separation: the shadow rays cross the occluder plane at an INTERIOR point / a
// clearly-OUTSIDE point (no grazing edge — decisions robust).
TEST_CASE("CEIR-19c: the authored wavefront shade kernel writes lit/shadowed == oracle + analytic (Vulkan)",
          "[gpu-context][vulkan][gpu][rt][ceir19c]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);

    std::ifstream f(CRD_REPO_DIR "/assets/ckir/wavefront_shade.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);
    crd::containers::Array<char> src(&alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KGraph kg(&alloc);
    kir::KEntry ke;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "wf_shade", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // Occluder = ONE triangle at y=4 {(-1,4,-1),(1,4,-1),(0,4,1)}; light L=(0,8,0) (baked in the kernel). Synthetic hits:
    // slot 0 -> ray 0 -> hitpos (0,0,0): shadow ray up hits the occluder interior @ (0,4,0) -> SHADOWED (0).
    // slot 1 -> ray 1 -> hitpos (3,0,0): shadow ray L-P crosses y=4 @ (1.5,4,0), OUTSIDE the triangle -> LIT (1).
    const float    verts[9]     = {-1.0F, 4.0F, -1.0F, 1.0F, 4.0F, -1.0F, 0.0F, 4.0F, 1.0F};
    const crd::u32 compacted[2] = {0U, 1U};
    const float    rays_in[12]  = {0.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F, 3.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F};
    const float    hitt_in[2]   = {10.0F, 10.0F};
    crd::containers::Array<crd::u32> gpu_dec(&alloc);
    gpu_dec.resize(2U, 0xEEEEEEEEU);

    ceir::Context c(&alloc);
    (void)ceir::func::register_dialect(c);
    (void)ceir::arith::register_arith_ops(c);
    (void)ceir::resource::register_resource_ops(c);
    (void)ceir::rt::register_dialect(c);
    ceir::Block* const body  = c.create_block(0U);
    const auto         mkbuf = [&]() {
        ceir::Operation* const d =
            c.create_operation(c.intern_op("resource", "declare"), {}, 1U, c.type_buffer(ceir::BufferMode::Plain, c.type_f32()));
        body->append(d);
        return d->result(0U);
    };
    const auto mkidx = [&](crd::i64 v) {
        ceir::Operation* const o = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_index());
        c.set_attr(o, "value", c.attr_int(v));
        body->append(o);
        return o->result(0U);
    };
    ceir::Value* const     geom = mkbuf();
    ceir::Operation* const blas = ceir::rt::build_blas_build(c, geom, ceir::rt::type_blas(c));
    body->append(blas);
    ceir::Value* const     xf   = mkbuf();
    ceir::Operation* const inst = ceir::rt::build_instance_populate(c, blas->result(0U), xf, c.attr_int(1), c.type_i32());
    body->append(inst);
    ceir::Operation* const tlas = ceir::rt::build_tlas_build(c, inst->result(0U), ceir::rt::type_tlas(c));
    body->append(tlas);
    ceir::Value* const comp_v = mkbuf();
    ceir::Value* const rays_v = mkbuf();
    ceir::Value* const hitt_v = mkbuf();
    ceir::Value* const dec_v  = mkbuf();
    ceir::Value* const g2     = mkidx(2); // groups = count (2 compacted slots) — the count-driven dispatch
    ceir::Value* const g1     = mkidx(1);
    ceir::Value*       rq_ops[8] = {g2, g1, g1, tlas->result(0U), comp_v, rays_v, hitt_v, dec_v};
    ceir::Operation* const rq   = c.create_operation(c.intern_op("rt", "ray_query"),
                                                     crd::containers::ConstSpan<ceir::Value*>(rq_ops, 8U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("wavefront_shade")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rrrw"))); // compacted r, rays r, hit_t r, decision w
    body->append(rq);

    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    RtGateState st;
    st.rt         = &rt;
    st.verts      = verts;
    st.ntris      = 1U;
    st.spirv      = spv.spirv.data();
    st.spirv_size = spv.spirv.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_build_scene;
    hooks.kernel_bytes   = &rt_gate_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[4];
    binds[0] = {c.resource_root(comp_v), &compacted[0], nullptr, 2U * sizeof(crd::u32)};
    binds[1] = {c.resource_root(rays_v), &rays_in[0], nullptr, 12U * sizeof(float)};
    binds[2] = {c.resource_root(hitt_v), &hitt_in[0], nullptr, 2U * sizeof(float)};
    binds[3] = {c.resource_root(dec_v), nullptr, gpu_dec.data(), 2U * sizeof(crd::u32)};
    const cg::ExecuteError err =
        cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                               crd::containers::ConstSpan<cg::RtHostBinding>(binds, 4U));
    REQUIRE(err == cg::ExecuteError::None);

    // Oracle: eval_cpu_kernel on the SAME graph, AS-slot = the occluder soup [ntri, v0..v2].
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = 1.0;
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> comp64(&alloc);
    crd::containers::Array<crd::f64> rays64(&alloc);
    crd::containers::Array<crd::f64> hitt64(&alloc);
    crd::containers::Array<crd::f64> dec_ref(&alloc);
    comp64.resize(2U, 0.0);
    for (crd::u32 i = 0; i < 2U; ++i) { comp64[i] = static_cast<crd::f64>(compacted[i]); }
    rays64.resize(12U, 0.0);
    for (crd::u32 i = 0; i < 12U; ++i) { rays64[i] = static_cast<crd::f64>(rays_in[i]); }
    hitt64.resize(2U, 0.0);
    for (crd::u32 i = 0; i < 2U; ++i) { hitt64[i] = static_cast<crd::f64>(hitt_in[i]); }
    dec_ref.resize(2U, 0.0);
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                 {comp64.data(), 2, 0, 1},
                                 {rays64.data(), 12, 0, 2},
                                 {hitt64.data(), 2, 0, 3},
                                 {dec_ref.data(), 2, 0, 4}};
    kir::eval_cpu_kernel(kg, ke, bufs, 5, 1U, &alloc, 2U); // local_size=1, 2 groups (2 slots)

    // GPU lit/shadowed == oracle (bit-exact) + the ANALYTIC pin: slot 0 shadowed (0), slot 1 lit (1).
    INFO("gpu_dec=[" << gpu_dec[0] << ", " << gpu_dec[1] << "] oracle=[" << static_cast<crd::u32>(dec_ref[0]) << ", "
                     << static_cast<crd::u32>(dec_ref[1]) << "]");
    CHECK(gpu_dec[0] == static_cast<crd::u32>(dec_ref[0]));
    CHECK(gpu_dec[1] == static_cast<crd::u32>(dec_ref[1]));
    CHECK(gpu_dec[0] == 0U); // hitpos (0,0,0) is under the occluder -> SHADOWED
    CHECK(gpu_dec[1] == 1U); // hitpos (3,0,0) is clear -> LIT
}

// ── CEIR-19c STAGE 2 — the §134 WAVEFRONT HOST-LOOP (the culmination): raygen(host) -> trace -> compact -> [readback] ->
// shade -> compact, driven by a host while(count>0) loop through execute_rt_lowered per dispatch, all authored .ckir kernels,
// ZERO bridge changes. Single-bounce direct lighting: the loop runs EXACTLY ONCE and TERMINATES via a count READBACK (the
// second compact runs on host-zeroed continuation_flags -> next_count=0 by construction — the advisor's "shade emits a
// continuation count, zero by construction" resolved as the compact-is-the-count-producer; multi-bounce later adds one store
// to shade, nothing else). Per-iteration a HOST triple32 fold over DECISION INTS ONLY (hit_count, compacted[0..count-1],
// decisions[0..count-1], next_count) is compared GPU==oracle; the oracle mirrors the SAME host loop (eval_cpu_kernel per
// dispatch). ⛔ each execute_rt_lowered is submit+wait, so readback N is coherent before upload N+1 (the synchronous seam the
// loop determinism rests on; GPU-indirect gives it up). ⛔ the TLAS holds receiver AND occluder, so tmin=0.001 skips the
// receiver the shadow ray originates on (symmetric: rayQuery tMin == the oracle's t>tmin). ANALYTIC pin: decisions=[0,1,0,1].
TEST_CASE("CEIR-19c: the AUTHORED wavefront host-loop (trace->compact->shade) == oracle + analytic (Vulkan)",
          "[gpu-context][vulkan][gpu][rt][ceir19c]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->ray_query()) { WARN("adapter has no VK_KHR_ray_query; skipping"); return; }
    gpu::VulkanRayTracingContext rt(*vk);
    REQUIRE(rt.valid());

    crd::memory::TlsfAllocator alloc(64U << 20U);

    // Load + compile the 3 authored kernels (KGraph/KEntry for the oracle, SPIR-V blob for the GPU).
    kir::KGraph                     kg_tr(&alloc);
    kir::KGraph                     kg_co(&alloc);
    kir::KGraph                     kg_sh(&alloc);
    kir::KEntry                     ke_tr;
    kir::KEntry                     ke_co;
    kir::KEntry                     ke_sh;
    crd::containers::Array<crd::u8> spv_tr(&alloc);
    crd::containers::Array<crd::u8> spv_co(&alloc);
    crd::containers::Array<crd::u8> spv_sh(&alloc);
    const auto load_kernel = [&](const char* path, const char* name, kir::KGraph& kg, kir::KEntry& ke,
                                 crd::containers::Array<crd::u8>& spv) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&alloc);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &alloc, kern));
        const auto r = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), name, &alloc);
        INFO(r.error_message.c_str());
        REQUIRE(r.ok);
        spv.resize(r.spirv.size(), 0U);
        for (crd::usize i = 0; i < r.spirv.size(); ++i) { spv[i] = r.spirv[i]; }
    };
    load_kernel(CRD_REPO_DIR "/assets/ckir/wavefront_trace.ckir", "wf_trace", kg_tr, ke_tr, spv_tr);
    load_kernel(CRD_REPO_DIR "/assets/ckir/wavefront_compact.ckir", "wf_compact", kg_co, ke_co, spv_co);
    load_kernel(CRD_REPO_DIR "/assets/ckir/wavefront_shade.ckir", "wf_shade", kg_sh, ke_sh, spv_sh);

    // Scene: receiver quad @ y=0 (x,z in [-10,10], prims 0-1) + occluder triangle @ y=4 around x=2 (prim 2). Light L=(0,8,0)
    // is baked in wavefront_shade. Camera straight down: the shadow ray from a hit (cx,0,0) crosses y=4 at x=0.5·cx while the
    // camera crosses at x=cx, so an occluder at x≈2 shadows hits near x=4 without the camera ever hitting the occluder.
    const float verts[27] = {-10.0F, 0.0F, -10.0F, 10.0F,  0.0F, -10.0F, 10.0F, 0.0F, 10.0F,   // receiver tri 0
                             -10.0F, 0.0F, -10.0F, 10.0F,  0.0F, 10.0F,  -10.0F, 0.0F, 10.0F,  // receiver tri 1
                             1.0F,   4.0F, -1.0F,  3.0F,   4.0F, -1.0F,  2.0F,   4.0F, 1.0F};  // occluder tri 2
    constexpr crd::u32 num_tris = 3U;
    constexpr crd::u32 nrays    = 8U;  // meaningful camera rays (== the compact's unrolled N — the wavefront queue size)
    constexpr crd::u32 nbuf     = 64U; // buffer width = the trace kernel's workgroup (local_size=64) so no tail thread OOBs
    // 8 meaningful camera rays: 0-3 hit the receiver (decisions [0,1,0,1]); 4-7 miss (x>10). Slots 8..63 = guaranteed-miss
    // fillers (the trace has no tail guard) — the compact scans only [0..7] (its N), so the fillers never enter the queue.
    const float seed[nrays * 6] = {4.0F,  20.0F, 0.0F,  0.0F, -1.0F, 0.0F,   // ray0 -> (4,0,0)    SHADOWED
                                   -8.0F, 20.0F, 0.0F,  0.0F, -1.0F, 0.0F,   // ray1 -> (-8,0,0)   LIT
                                   4.0F,  20.0F, 0.4F,  0.0F, -1.0F, 0.0F,   // ray2 -> (4,0,0.4)  SHADOWED
                                   -6.0F, 20.0F, 0.0F,  0.0F, -1.0F, 0.0F,   // ray3 -> (-6,0,0)   LIT
                                   15.0F, 20.0F, 0.0F,  0.0F, -1.0F, 0.0F,   // ray4 miss
                                   16.0F, 20.0F, 0.0F,  0.0F, -1.0F, 0.0F,   // ray5 miss
                                   17.0F, 20.0F, 0.0F,  0.0F, -1.0F, 0.0F,   // ray6 miss
                                   18.0F, 20.0F, 0.0F,  0.0F, -1.0F, 0.0F};  // ray7 miss
    float rays[nbuf * 6] = {};
    for (crd::u32 r = 0; r < nbuf; ++r)
    {
        if (r < nrays)
        {
            for (int col = 0; col < 6; ++col) { rays[r * 6U + static_cast<crd::u32>(col)] = seed[r * 6U + static_cast<crd::u32>(col)]; }
        }
        else
        {
            rays[r * 6U + 0U] = 100.0F; // far origin
            rays[r * 6U + 1U] = 100.0F;
            rays[r * 6U + 2U] = 100.0F;
            rays[r * 6U + 5U] = 1.0F; // dir +z -> misses the scene
        }
    }

    // A ceir.rt program builder: blas->instance->tlas->ray_query(grid=(gx,1,1), %tlas, nbind bindings), kernel + access. The
    // bindings' resource_root Value*s are returned (for the RtHostBinding match). ⛔ built in ONE shared Context `c` so all
    // programs + their LoweredCommand `op` back-pointers stay alive through the loop.
    ceir::Context c(&alloc);
    (void)ceir::func::register_dialect(c);
    (void)ceir::arith::register_arith_ops(c);
    (void)ceir::resource::register_resource_ops(c);
    (void)ceir::rt::register_dialect(c);
    const auto build_prog = [&](const char* kernel, const char* access, crd::i64 gx, crd::u32 nbind,
                                crd::containers::Array<cg::LoweredCommand>& out_cmds, const ceir::Value** out_roots) {
        ceir::Block* const b     = c.create_block(0U);
        const auto         mkbuf = [&]() {
            ceir::Operation* const d = c.create_operation(c.intern_op("resource", "declare"), {}, 1U,
                                                          c.type_buffer(ceir::BufferMode::Plain, c.type_f32()));
            b->append(d);
            return d->result(0U);
        };
        const auto mkidx = [&](crd::i64 v) {
            ceir::Operation* const o = c.create_operation(c.intern_op("arith", "const"), {}, 1U, c.type_index());
            c.set_attr(o, "value", c.attr_int(v));
            b->append(o);
            return o->result(0U);
        };
        ceir::Value* const     geom = mkbuf();
        ceir::Operation* const blas = ceir::rt::build_blas_build(c, geom, ceir::rt::type_blas(c));
        b->append(blas);
        ceir::Value* const     xf   = mkbuf();
        ceir::Operation* const inst = ceir::rt::build_instance_populate(c, blas->result(0U), xf, c.attr_int(1), c.type_i32());
        b->append(inst);
        ceir::Operation* const tlas = ceir::rt::build_tlas_build(c, inst->result(0U), ceir::rt::type_tlas(c));
        b->append(tlas);
        ceir::Value* const g1 = mkidx(1);
        ceir::Value* const gg = mkidx(gx);
        ceir::Value*       ops[12];
        ops[0] = gg;
        ops[1] = g1;
        ops[2] = g1;
        ops[3] = tlas->result(0U);
        for (crd::u32 i = 0; i < nbind; ++i)
        {
            ceir::Value* const buf = mkbuf();
            ops[4U + i]            = buf;
            out_roots[i]           = c.resource_root(buf);
        }
        ceir::Operation* const rq = c.create_operation(c.intern_op("rt", "ray_query"),
                                                       crd::containers::ConstSpan<ceir::Value*>(ops, 4U + nbind), 0U,
                                                       ceir::TypeId{}, 0U);
        c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView(kernel)));
        c.set_attr(rq, "access", c.attr_string(crd::containers::StringView(access)));
        b->append(rq);
        cg::lower_region(c, *b, out_cmds);
    };

    // trace: grid=(1,1,1) (64 threads cover 8 rays); compact: grid=(1,1,1) local_size=1. Built ONCE (reused each iteration).
    crd::containers::Array<cg::LoweredCommand> tr_cmds(&alloc);
    crd::containers::Array<cg::LoweredCommand> co_cmds(&alloc);
    const ceir::Value*                         tr_roots[3] = {nullptr, nullptr, nullptr};
    const ceir::Value*                         co_roots[3] = {nullptr, nullptr, nullptr};
    build_prog("wavefront_trace", "rww", 1, 3U, tr_cmds, tr_roots);   // rays r, hit_flag w, hit_t w
    build_prog("wavefront_compact", "rww", 1, 3U, co_cmds, co_roots); // flags r, compacted w, count w

    // Host arrays (carried in-out across the submit+wait dispatches).
    crd::containers::Array<crd::u32> hit_flags(&alloc);
    crd::containers::Array<float>    hit_t(&alloc);
    crd::containers::Array<crd::u32> compacted(&alloc);
    crd::containers::Array<crd::u32> decisions(&alloc);
    crd::containers::Array<crd::u32> cont_flags(&alloc);
    crd::containers::Array<crd::u32> next_queue(&alloc);
    hit_flags.resize(nbuf, 0U);
    hit_t.resize(nbuf, 0.0F);
    compacted.resize(nbuf, 0xEEEEEEEEU);
    decisions.resize(nbuf, 0xEEEEEEEEU);
    cont_flags.resize(nbuf, 0U); // ⛔ host-ZEROED continuation flags: single-bounce writes none, so the 2nd compact -> 0
    next_queue.resize(nbuf, 0xEEEEEEEEU);
    crd::u32 hit_count  = 0U;
    crd::u32 next_count = 0U;

    // Oracle scene soup [ntri, v0..v2 per tri] (for trace + shade); the compact's AS is dead (unread).
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + 27U, 0.0);
    geo[0] = static_cast<crd::f64>(num_tris);
    for (int i = 0; i < 27; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> rays64(&alloc);
    rays64.resize(static_cast<crd::usize>(nbuf) * 6U, 0.0);
    for (crd::usize i = 0; i < rays64.size(); ++i) { rays64[i] = static_cast<crd::f64>(rays[i]); }
    crd::containers::Array<crd::f64> hf_ref(&alloc);
    crd::containers::Array<crd::f64> ht_ref(&alloc);
    crd::containers::Array<crd::f64> comp_ref(&alloc);
    crd::containers::Array<crd::f64> dec_ref(&alloc);
    crd::containers::Array<crd::f64> cont_ref(&alloc);
    crd::containers::Array<crd::f64> nq_ref(&alloc);
    crd::containers::Array<crd::f64> hc_ref(&alloc);
    crd::containers::Array<crd::f64> nc_ref(&alloc);
    hf_ref.resize(nbuf, 0.0);
    ht_ref.resize(nbuf, 0.0);
    comp_ref.resize(nbuf, 0.0);
    dec_ref.resize(nbuf, 0.0);
    cont_ref.resize(nbuf, 0.0);
    nq_ref.resize(nbuf, 0.0);
    hc_ref.resize(1U, 0.0);
    nc_ref.resize(1U, 0.0);

    RtGateState st;
    st.rt       = &rt;
    st.verts    = verts;
    st.ntris    = num_tris; // the scene is built ONCE on the first AccelBuild (guard) and reused by every program
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_build_scene;
    hooks.kernel_bytes   = &rt_gate_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_trace_dispatch;
    hooks.user           = &st;

    const auto exec = [&](const crd::containers::Array<cg::LoweredCommand>& cmds, const crd::u8* spv, crd::usize spv_sz,
                          const cg::RtHostBinding* binds, crd::u32 nb) {
        st.spirv      = spv;
        st.spirv_size = spv_sz;
        return cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                                      crd::containers::ConstSpan<cg::RtHostBinding>(binds, nb));
    };

    crd::u32 queue_count = nrays;      // the raygen: N host-filled camera rays
    int      iters       = 0;
    crd::u32 gpu_hash    = 0x9E3779B9U; // shared seed; identical fold on both sides
    crd::u32 oracle_hash = 0x9E3779B9U;
    while (queue_count > 0U)
    {
        // GPU: trace -> hit_flags + hit_t.
        const cg::RtHostBinding b_tr[3] = {{tr_roots[0], rays, nullptr, static_cast<crd::u64>(nbuf) * 6U * sizeof(float)},
                                           {tr_roots[1], nullptr, hit_flags.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {tr_roots[2], nullptr, hit_t.data(), static_cast<crd::u64>(nbuf) * sizeof(float)}};
        REQUIRE(exec(tr_cmds, spv_tr.data(), spv_tr.size(), b_tr, 3U) == cg::ExecuteError::None);
        // GPU: compact #1 (hit_flags -> compacted + hit_count).
        const cg::RtHostBinding b_c1[3] = {{co_roots[0], hit_flags.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[1], nullptr, compacted.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[2], nullptr, &hit_count, sizeof(crd::u32)}};
        REQUIRE(exec(co_cmds, spv_co.data(), spv_co.size(), b_c1, 3U) == cg::ExecuteError::None);
        // GPU: shade over the compacted hits — grid=hit_count (the COUNT-DRIVEN dispatch, rebuilt from the readback).
        crd::containers::Array<cg::LoweredCommand> sh_cmds(&alloc);
        const ceir::Value*                         sh_roots[4] = {nullptr, nullptr, nullptr, nullptr};
        build_prog("wavefront_shade", "rrrw", static_cast<crd::i64>(hit_count), 4U, sh_cmds, sh_roots);
        st.last_gx                      = 0U;
        const cg::RtHostBinding b_sh[4] = {{sh_roots[0], compacted.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {sh_roots[1], rays, nullptr, static_cast<crd::u64>(nbuf) * 6U * sizeof(float)},
                                           {sh_roots[2], hit_t.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(float)},
                                           {sh_roots[3], nullptr, decisions.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)}};
        REQUIRE(exec(sh_cmds, spv_sh.data(), spv_sh.size(), b_sh, 4U) == cg::ExecuteError::None);
        CHECK(st.last_gx == hit_count); // ⭐ the shade dispatch grid came from the compact's count readback (count-driven)
        // GPU: compact #2 (host-zeroed continuation flags -> next_count). Single-bounce: next_count == 0 by construction.
        const cg::RtHostBinding b_c2[3] = {{co_roots[0], cont_flags.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[1], nullptr, next_queue.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[2], nullptr, &next_count, sizeof(crd::u32)}};
        REQUIRE(exec(co_cmds, spv_co.data(), spv_co.size(), b_c2, 3U) == cg::ExecuteError::None);

        // ORACLE: mirror the SAME loop on the CPU (eval_cpu_kernel per dispatch, same inputs).
        kir::KernelBuffer bt[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                   {rays64.data(), static_cast<int>(rays64.size()), 0, 1},
                                   {hf_ref.data(), static_cast<int>(nbuf), 0, 2},
                                   {ht_ref.data(), static_cast<int>(nbuf), 0, 3}};
        kir::eval_cpu_kernel(kg_tr, ke_tr, bt, 4, 64U, &alloc, 1U);
        kir::KernelBuffer bc1[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                    {hf_ref.data(), static_cast<int>(nbuf), 0, 1},
                                    {comp_ref.data(), static_cast<int>(nbuf), 0, 2},
                                    {hc_ref.data(), 1, 0, 3}};
        kir::eval_cpu_kernel(kg_co, ke_co, bc1, 4, 1U, &alloc, 1U);
        const crd::u32 ohc = static_cast<crd::u32>(hc_ref[0]);
        kir::KernelBuffer bs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                   {comp_ref.data(), static_cast<int>(nbuf), 0, 1},
                                   {rays64.data(), static_cast<int>(rays64.size()), 0, 2},
                                   {ht_ref.data(), static_cast<int>(nbuf), 0, 3},
                                   {dec_ref.data(), static_cast<int>(nbuf), 0, 4}};
        kir::eval_cpu_kernel(kg_sh, ke_sh, bs, 5, 1U, &alloc, ohc > 0U ? ohc : 1U);
        kir::KernelBuffer bc2[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                    {cont_ref.data(), static_cast<int>(nbuf), 0, 1},
                                    {nq_ref.data(), static_cast<int>(nbuf), 0, 2},
                                    {nc_ref.data(), 1, 0, 3}};
        kir::eval_cpu_kernel(kg_co, ke_co, bc2, 4, 1U, &alloc, 1U);

        // HOST triple32 fold over DECISION INTS ONLY, [0..count-1], identical order both sides.
        gpu_hash    = rt_fold(gpu_hash, hit_count);
        oracle_hash = rt_fold(oracle_hash, ohc);
        for (crd::u32 j = 0; j < hit_count; ++j) { gpu_hash = rt_fold(gpu_hash, compacted[j]); }
        for (crd::u32 j = 0; j < ohc; ++j) { oracle_hash = rt_fold(oracle_hash, static_cast<crd::u32>(comp_ref[j])); }
        for (crd::u32 j = 0; j < hit_count; ++j) { gpu_hash = rt_fold(gpu_hash, decisions[j]); }
        for (crd::u32 j = 0; j < ohc; ++j) { oracle_hash = rt_fold(oracle_hash, static_cast<crd::u32>(dec_ref[j])); }
        gpu_hash    = rt_fold(gpu_hash, next_count);
        oracle_hash = rt_fold(oracle_hash, static_cast<crd::u32>(nc_ref[0]));
        CHECK(gpu_hash == oracle_hash); // ⭐ per-iteration decision-hash GPU == oracle

        // Direct GPU==oracle + the ANALYTIC pin (parity alone is blind to a symmetric authoring error).
        CHECK(hit_count == 4U);
        CHECK(ohc == 4U);
        for (crd::u32 j = 0; j < 4U; ++j)
        {
            INFO("slot " << j << ": gpu comp=" << compacted[j] << " dec=" << decisions[j] << " | oracle comp="
                         << static_cast<crd::u32>(comp_ref[j]) << " dec=" << static_cast<crd::u32>(dec_ref[j]));
            CHECK(compacted[j] == static_cast<crd::u32>(comp_ref[j]));
            CHECK(decisions[j] == static_cast<crd::u32>(dec_ref[j]));
        }
        CHECK(decisions[0] == 0U); // (4,0,0)   under the occluder -> SHADOWED
        CHECK(decisions[1] == 1U); // (-8,0,0)  clear -> LIT
        CHECK(decisions[2] == 0U); // (4,0,0.4) under the occluder -> SHADOWED
        CHECK(decisions[3] == 1U); // (-6,0,0)  clear -> LIT

        CHECK(next_count == 0U);  // ⭐ single-bounce: the compact#2 readback IS zero (termination value PINNED, not inferred)
        queue_count = next_count; // terminate via the compact#2 readback, NOT a hardcode
        ++iters;
    }
    CHECK(iters == 1);        // the loop body ran EXACTLY once (single-bounce)
    CHECK(queue_count == 0U); // and terminated via the count readback
}
