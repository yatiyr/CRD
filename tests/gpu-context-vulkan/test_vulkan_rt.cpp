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

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace gpu = crd::gpu;
namespace kir = crd::kir;

namespace
{
// The one-thread-per-ray inline-ray-query kernel: TLAS at binding 0, rays (6 floats each) at binding 1, out distance at 2.
kir::KEntry build_trace_kernel(kir::KGraph& g, int local_size)
{
    const kir::Shape sh1 = kir::make_shape({1});
    const auto       cf  = [&](double v) { return g.constant(v, sh1, kir::DType::F32); };
    const auto       cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, kir::DType::U32); };
    const int        as   = g.accel_struct_decl(0, 0);
    const int        rays = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int        out  = g.buffer_decl(kir::DType::F32, 0, 2, true);
    const int        mark = g.kernel_stmt_mark();
    const int        tid  = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(local_size))), g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int        base = g.binary(kir::KOp::Mul, tid, cu(6U));
    const auto       ld   = [&](crd::u32 k) { return g.buffer_load(rays, g.binary(kir::KOp::Add, base, cu(k))); };
    const int        t    = g.trace_ray_closest(as, ld(0), ld(1), ld(2), ld(3), ld(4), ld(5), cf(0.001), cf(1.0e30));
    g.stmt_buffer_store(out, tid, t);
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

    constexpr crd::u32 kNTris = 1U;
    constexpr crd::u32 kNRays = 4U;
    const int          local  = 64;

    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_trace_kernel(g, local);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_trace", &alloc);
    INFO(spv.error_message.c_str());
    REQUIRE(spv.ok);

    // The triangle (z=2) — GPU wants raw float3 verts; the CPU oracle wants [triCount, v0.xyz, v1.xyz, v2.xyz].
    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    const float rays[kNRays][6] = {
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},  // hit at t=2
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, -1.0F}, // miss
        {5.0F, 5.0F, 0.0F, 0.0F, 0.0F, 1.0F},  // miss (outside)
        {0.1F, 0.1F, 1.0F, 0.0F, 0.0F, 1.0F},  // hit at t=1
    };

    // ── CPU oracle (the reference) ──
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = static_cast<crd::f64>(kNTris);
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> rays64(&alloc);
    rays64.resize(static_cast<crd::usize>(kNRays) * 6U, 0.0);
    for (crd::u32 r = 0; r < kNRays; ++r)
    {
        for (int c = 0; c < 6; ++c) { rays64[static_cast<crd::usize>(r) * 6U + static_cast<crd::usize>(c)] = static_cast<crd::f64>(rays[r][c]); }
    }
    crd::containers::Array<crd::f64> ref(&alloc);
    ref.resize(kNRays, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {rays64.data(), static_cast<int>(rays64.size()), 0, 1}, {ref.data(), static_cast<int>(kNRays), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, static_cast<crd::u32>(local), &alloc, 1U);

    // ── GPU: build the AS + trace ──
    auto scene = rt.build_scene(verts, kNTris);
    REQUIRE(scene != nullptr);
    float gpu_t[kNRays] = {0.0F, 0.0F, 0.0F, 0.0F};
    gpu::VulkanRayTracingContext::Binding bind[2] = {
        {&rays[0][0], nullptr, static_cast<crd::u64>(kNRays) * 6U * sizeof(float), 1U}, // rays in at binding 1
        {nullptr, gpu_t, static_cast<crd::u64>(kNRays) * sizeof(float), 2U},            // distances out at binding 2
    };
    const crd::u32 groups = (kNRays + static_cast<crd::u32>(local) - 1U) / static_cast<crd::u32>(local);
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), groups));

    for (crd::u32 r = 0; r < kNRays; ++r)
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
    constexpr crd::u32 kN = 64U; // 8x8 grid of ground points
    crd::containers::Array<float> pos(&alloc);
    pos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    pos64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    for (crd::usize i = 0; i < pos.size(); ++i) { pos64[i] = static_cast<crd::f64>(pos[i]); }
    crd::containers::Array<crd::f64> refv(&alloc);
    refv.resize(kN, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {refv.data(), static_cast<int>(kN), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, scfg.local_size, &alloc, (kN + scfg.local_size - 1U) / scfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, 1U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_v(&alloc);
    gpu_v.resize(kN, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[2] = {
        {pos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U},
        {nullptr, gpu_v.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), (kN + scfg.local_size - 1U) / scfg.local_size));

    int lit = 0;
    int shadowed = 0;
    int mism = 0;
    for (crd::u32 p = 0; p < kN; ++p)
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
    constexpr crd::u32 kNTri = 2U;
    constexpr crd::u32 kN    = 64U;
    crd::containers::Array<float> pos(&alloc);
    crd::containers::Array<float> nrm(&alloc);
    pos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    nrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    geo.resize(1U + static_cast<crd::usize>(kNTri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(kNTri);
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    pos64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    for (crd::usize i = 0; i < pos.size(); ++i) { pos64[i] = static_cast<crd::f64>(pos[i]); nrm64[i] = static_cast<crd::f64>(nrm[i]); }
    crd::containers::Array<crd::f64> refv(&alloc);
    refv.resize(kN, 0.0);
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {refv.data(), static_cast<int>(kN), 0, 3}};
    kir::eval_cpu_kernel(g, e, bufs, 4, acfg.local_size, &alloc, (kN + acfg.local_size - 1U) / acfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, kNTri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_ao(&alloc);
    gpu_ao.resize(kN, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[3] = {
        {pos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U},
        {nrm.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 2U},
        {nullptr, gpu_ao.data(), static_cast<crd::u64>(kN) * sizeof(float), 3U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 3), (kN + acfg.local_size - 1U) / acfg.local_size));

    double worst   = 0.0;
    double sum_gpu = 0.0;
    double sum_ref = 0.0;
    double ao_min  = 1.0;
    double ao_max  = 0.0;
    for (crd::u32 p = 0; p < kN; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_ao[p]) - refv[p]);
        if (d > worst) { worst = d; }
        sum_gpu += static_cast<double>(gpu_ao[p]);
        sum_ref += refv[p];
        if (refv[p] < ao_min) { ao_min = refv[p]; }
        if (refv[p] > ao_max) { ao_max = refv[p]; }
    }
    INFO("RTAO: worst |GPU-ref|=" << worst << "  mean GPU=" << sum_gpu / kN << " ref=" << sum_ref / kN << "  AO range=[" << ao_min << ", " << ao_max << "]");
    CHECK(worst < 0.12);                                    // per-point AO matches (deterministic sampling; ≤~3/32 grazing-edge ray flips)
    CHECK(crd::math::abs(sum_gpu - sum_ref) / kN < 0.02);   // ...and the mean is tight (edge noise averages out)
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
    constexpr crd::u32 kNTri = 2U;
    constexpr crd::u32 kN    = 64U;
    crd::containers::Array<float> pos(&alloc);
    crd::containers::Array<float> nrm(&alloc);
    pos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    nrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    geo.resize(1U + static_cast<crd::usize>(kNTri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(kNTri);
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc), nrm64(&alloc), tn64(&alloc);
    pos64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    tn64.resize(6U, 0.0);
    for (crd::usize i = 0; i < pos.size(); ++i) { pos64[i] = static_cast<crd::f64>(pos[i]); nrm64[i] = static_cast<crd::f64>(nrm[i]); }
    for (int i = 0; i < 6; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    crd::containers::Array<crd::f64> refc(&alloc);
    refc.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, rcfg.local_size, &alloc, (kN + rcfg.local_size - 1U) / rcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, kNTri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[4] = {
        {pos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U},
        {nrm.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 2U},
        {tri_n, nullptr, 6U * sizeof(float), 3U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(kN) * 3U * sizeof(float), 4U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 4), (kN + rcfg.local_size - 1U) / rcfg.local_size));

    double worst   = 0.0;
    int    hit_col = 0;
    int    sky_col = 0;
    for (crd::u32 p = 0; p < kN * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
    }
    for (crd::u32 p = 0; p < kN; ++p) // classify by the blue channel: sky (~0.8) vs shaded ceiling (~0.2)
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
    constexpr crd::u32 kNTri = 2U;
    constexpr crd::u32 kN    = 64U;
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    geo.resize(1U + static_cast<crd::usize>(kNTri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(kNTri);
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc), nrm64(&alloc), tn64(&alloc);
    pos64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    tn64.resize(6U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 6; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    crd::containers::Array<crd::f64> refc(&alloc);
    refc.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, (kN + pcfg.local_size - 1U) / pcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, kNTri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[4] = {
        {ppos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U},
        {pnrm.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 2U},
        {tri_n, nullptr, 6U * sizeof(float), 3U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(kN) * 3U * sizeof(float), 4U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 4), (kN + pcfg.local_size - 1U) / pcfg.local_size));

    double worst = 0.0;
    double sumd  = 0.0;
    double lmin  = 10.0;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < kN * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
        sumd += d;
    }
    for (crd::u32 p = 0; p < kN; ++p) // luminance of the ref radiance, to check GI spatial variation
    {
        const double lum = 0.3 * refc[p * 3U] + 0.6 * refc[p * 3U + 1U] + 0.1 * refc[p * 3U + 2U];
        if (lum < lmin) { lmin = lum; }
        if (lum > lmax) { lmax = lum; }
    }
    INFO("pathtrace: worst |GPU-ref|=" << worst << "  mean|Δ|=" << sumd / (kN * 3U) << "  radiance lum range=[" << lmin << ", " << lmax << "]");
    CHECK(worst < 0.06);              // GPU path radiance == CPU path-tracer oracle (transcendental ULP + rare grazing flips)
    CHECK(sumd / (kN * 3U) < 0.004);  // ...the bulk matches tightly
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
    constexpr crd::u32 kNTri = 4U;
    constexpr crd::u32 kN    = 64U;
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    geo.resize(1U + static_cast<crd::usize>(kNTri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(kNTri);
    for (int i = 0; i < 36; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc), nrm64(&alloc), tn64(&alloc), refc(&alloc);
    pos64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    tn64.resize(12U, 0.0);
    refc.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 12; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, (kN + pcfg.local_size - 1U) / pcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, kNTri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[4] = {
        {ppos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U},
        {pnrm.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 2U},
        {tri_n, nullptr, 12U * sizeof(float), 3U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(kN) * 3U * sizeof(float), 4U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 4), (kN + pcfg.local_size - 1U) / pcfg.local_size));

    double worst = 0.0;
    double sumd  = 0.0;
    double lmin  = 1.0e30;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < kN * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
        sumd += d;
    }
    for (crd::u32 p = 0; p < kN; ++p)
    {
        const double v = refc[p * 3U];
        if (v < lmin) { lmin = v; }
        if (v > lmax) { lmax = v; }
    }
    INFO("nee/mis: worst |GPU-ref|=" << worst << "  mean|Δ|=" << sumd / (kN * 3U) << "  radiance range=[" << lmin << ", " << lmax << "]");
    CHECK(worst < 0.05);             // GPU MIS radiance == CPU oracle (transcendental ULP + rare grazing shadow-ray flips)
    CHECK(sumd / (kN * 3U) < 0.003); // ...the bulk matches tightly
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
    constexpr crd::u32 kNTri = 4U;
    constexpr crd::u32 kN    = 64U;
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    geo.resize(1U + static_cast<crd::usize>(kNTri) * 9U, 0.0);
    geo[0] = static_cast<crd::f64>(kNTri);
    for (int i = 0; i < 36; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    crd::containers::Array<crd::f64> pos64(&alloc), nrm64(&alloc), refc(&alloc);
    pos64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    nrm64.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    refc.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {refc.data(), static_cast<int>(refc.size()), 0, 3}};
    kir::eval_cpu_kernel(g, e, bufs, 4, rcfg.local_size, &alloc, (kN + rcfg.local_size - 1U) / rcfg.local_size);

    // ── GPU ──
    auto scene = rt.build_scene(verts, kNTri);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> gpu_c(&alloc);
    gpu_c.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[3] = {
        {ppos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U},
        {pnrm.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 2U},
        {nullptr, gpu_c.data(), static_cast<crd::u64>(kN) * 3U * sizeof(float), 3U},
    };
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                              crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 3), (kN + rcfg.local_size - 1U) / rcfg.local_size));

    double worst = 0.0;
    double sumd  = 0.0;
    double lmin  = 1.0e30;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < kN * 3U; ++p)
    {
        const double d = crd::math::abs(static_cast<double>(gpu_c[p]) - refc[p]);
        if (d > worst) { worst = d; }
        sumd += d;
    }
    for (crd::u32 p = 0; p < kN; ++p)
    {
        const double v = refc[p * 3U];
        if (v < lmin) { lmin = v; }
        if (v > lmax) { lmax = v; }
    }
    INFO("restir-di: worst |GPU-ref|=" << worst << "  mean|Δ|=" << sumd / (kN * 3U) << "  radiance range=[" << lmin << ", " << lmax << "]");
    CHECK(worst < 0.05);             // GPU ReSTIR radiance == CPU reservoir oracle (deterministic WRS; rare grazing shadow flips)
    CHECK(sumd / (kN * 3U) < 0.004); // ...the bulk matches tightly
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
    constexpr crd::u32 kW = 16U, kH = 16U, kN = kW * kH; // 256 pixels = 4×64 (exact workgroup cover, no bounds guard needed)

    // scene: JUST the area light (no occluder) — a fully-visible light so V=1 everywhere and the estimator variance is pure
    // area-sampling noise. That isolates what TEMPORAL reuse fixes (accumulating effective samples ⇒ crushing sampling noise);
    // the shadow-boundary variance an occluder adds is the SPATIAL-reuse story, exercised in the spatiotemporal test.
    const float verts[18] = {-1.5F, 3.0F, -1.5F, 1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F,
                             -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, 1.5F};
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    for (crd::u32 j = 0; j < kH; ++j)
    {
        for (crd::u32 i = 0; i < kW; ++i)
        {
            const crd::u32 p = j * kW + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(kW - 1U) * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(kH - 1U) * static_cast<float>(j);
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
    const crd::u64 posBytes = static_cast<crd::u64>(kN) * 3U * sizeof(float);
    const crd::u64 resBytes = static_cast<crd::u64>(kN) * static_cast<crd::u64>(kir::rt::kRestirReservoirStride) * sizeof(float);

    // ── converged REFERENCE: single-pass ReSTIR DI, 256 frames of M=16 (we proved this estimator unbiased vs NEE) ──
    crd::containers::Array<float> ref(&alloc);
    ref.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    {
        kir::rt::RestirDiConfig rc;
        rc.frames = 256U; rc.candidates = 16U; rc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_restir_di_kernel(g, rc);
        const auto spv = compile(e, g, "ref");
        B bind[3] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U}, {nullptr, ref.data(), posBytes, 3U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                  crd::containers::ConstSpan<B>(bind, 3), kN / 64U));
    }

    // ── single-pass BASELINE (no reuse): 1 frame of M=2 — the noisy per-frame image temporal reuse must beat ──
    crd::containers::Array<float> single(&alloc);
    single.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    {
        kir::rt::RestirDiConfig rc;
        rc.frames = 1U; rc.candidates = 2U; rc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_restir_di_kernel(g, rc);
        const auto spv = compile(e, g, "single");
        B bind[3] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U}, {nullptr, single.data(), posBytes, 3U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                  crd::containers::ConstSpan<B>(bind, 3), kN / 64U));
    }

    // ── TEMPORAL ping-pong: build the temporal + shade kernels once, dispatch F frames feeding rprev←rcur each frame ──
    kir::rt::RestirStConfig sc;
    sc.width = kW; sc.height = kH; sc.m_initial = 2U; sc.local_size = 64U;
    kir::KGraph gt(&alloc), gs(&alloc);
    const kir::KEntry et = kir::rt::build_restir_temporal_kernel(gt, sc);
    const kir::KEntry es = kir::rt::build_restir_shade_kernel(gs, sc);
    const auto tspv = compile(et, gt, "temporal");
    const auto sspv = compile(es, gs, "shade");

    crd::containers::Array<float> rprev(&alloc), rcur(&alloc), rad(&alloc), accum(&alloc), last(&alloc);
    rprev.resize(static_cast<crd::usize>(kN) * kir::rt::kRestirReservoirStride, 0.0F); // frame 0 history = empty (M=0)
    rcur.resize(rprev.size(), 0.0F);
    rad.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    accum.resize(rad.size(), 0.0F);
    last.resize(rad.size(), 0.0F);
    constexpr crd::u32 kF = 48U;
    for (crd::u32 f = 0; f < kF; ++f)
    {
        crd::u32 frame = f;
        B tb[5] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U},
                   {rprev.data(), nullptr, resBytes, 3U}, {nullptr, rcur.data(), resBytes, 4U},
                   {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()),
                                  crd::containers::ConstSpan<B>(tb, 5), kN / 64U));
        B sb[4] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U},
                   {rcur.data(), nullptr, resBytes, 3U}, {nullptr, rad.data(), posBytes, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()),
                                  crd::containers::ConstSpan<B>(sb, 4), kN / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { accum[i] += rad[i]; }
        for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rcur[i]; } // temporal feedback
        if (f == kF - 1U) { for (crd::usize i = 0; i < rad.size(); ++i) { last[i] = rad[i]; } }
    }
    crd::containers::Array<float> mean(&alloc);
    mean.resize(rad.size(), 0.0F);
    for (crd::usize i = 0; i < mean.size(); ++i) { mean[i] = accum[i] / static_cast<float>(kF); }

    const auto spatialMean = [&](const crd::containers::Array<float>& a) {
        double s = 0.0;
        for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); }
        return s / static_cast<double>(a.size());
    };
    const double refMean   = spatialMean(ref);
    const double tMean     = spatialMean(mean); // frame-averaged temporal image, spatially averaged
    const double errLast   = rms(last, ref);    // per-pixel noise of a single warmed-up frame (1-spp — inherently noisy)
    const double errSingle = rms(single, ref);  // per-pixel noise of single-pass RIS at the same M
    INFO("temporal ReSTIR: refMean=" << refMean << "  tMean=" << tMean << "  rms(warm-ref)=" << errLast << "  rms(single-ref)=" << errSingle);
    // UNBIASED: the spatial mean (bias shows as a systematic offset; per-pixel Monte-Carlo noise cancels in the spatial average).
    CHECK(refMean > 0.05);                        // the reference actually has lit direct illumination
    CHECK(crd::math::abs(tMean - refMean) / refMean < 0.03);
    // VARIANCE REDUCTION: a warmed-up temporal frame is markedly less noisy than single-pass RIS at the SAME per-frame budget —
    // the whole point of ReSTIR (the residual noise is what the denoiser then cleans up).
    CHECK(errLast < errSingle * 0.6);
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
    constexpr crd::u32 kW = 16U, kH = 16U, kN = kW * kH;

    // occluder (prims 0-1, 1×1 at y=2) + area light (prims 2-3, 3×3 at y=3).
    const float verts[36] = {
        -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, 0.5F,
        -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, 1.5F};
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    for (crd::u32 j = 0; j < kH; ++j)
    {
        for (crd::u32 i = 0; i < kW; ++i)
        {
            const crd::u32 p = j * kW + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(kW - 1U) * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(kH - 1U) * static_cast<float>(j);
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
    const auto spatialMean = [&](const crd::containers::Array<float>& a) {
        double s = 0.0;
        for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); }
        return s / static_cast<double>(a.size());
    };
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 posBytes = static_cast<crd::u64>(kN) * 3U * sizeof(float);
    const crd::u64 resBytes = static_cast<crd::u64>(kN) * static_cast<crd::u64>(kir::rt::kRestirReservoirStride) * sizeof(float);

    // converged reference (single-pass ReSTIR DI, 256×M16) + noisy single-pass baseline (1×M2), both WITH the occluder's shadow.
    crd::containers::Array<float> ref(&alloc), single(&alloc);
    ref.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    single.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    for (int pass = 0; pass < 2; ++pass)
    {
        kir::rt::RestirDiConfig rc;
        rc.frames = pass == 0 ? 256U : 1U; rc.candidates = pass == 0 ? 16U : 2U; rc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_restir_di_kernel(g, rc);
        const auto spv = compile(e, g, pass == 0 ? "ref" : "single");
        auto& dst = pass == 0 ? ref : single;
        B bind[3] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U}, {nullptr, dst.data(), posBytes, 3U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                  crd::containers::ConstSpan<B>(bind, 3), kN / 64U));
    }

    // build the 3 spatiotemporal passes once.
    kir::rt::RestirStConfig sc;
    sc.width = kW; sc.height = kH; sc.m_initial = 2U; sc.spatial_k = 5U; sc.spatial_radius = 3.0F; sc.local_size = 64U;
    kir::KGraph gt(&alloc), gsp(&alloc), gsh(&alloc);
    const kir::KEntry et  = kir::rt::build_restir_temporal_kernel(gt, sc);
    const kir::KEntry esp = kir::rt::build_restir_spatial_kernel(gsp, sc);
    const kir::KEntry esh = kir::rt::build_restir_shade_kernel(gsh, sc);
    const auto tspv = compile(et, gt, "st_temporal");
    const auto pspv = compile(esp, gsp, "st_spatial");
    const auto sspv = compile(esh, gsh, "st_shade");

    crd::containers::Array<float> rprev(&alloc), rtmp(&alloc), rspa(&alloc), rad(&alloc), accum(&alloc), last(&alloc);
    rprev.resize(static_cast<crd::usize>(kN) * kir::rt::kRestirReservoirStride, 0.0F);
    rtmp.resize(rprev.size(), 0.0F);
    rspa.resize(rprev.size(), 0.0F);
    rad.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    accum.resize(rad.size(), 0.0F);
    last.resize(rad.size(), 0.0F);
    constexpr crd::u32 kF = 48U;
    for (crd::u32 f = 0; f < kF; ++f)
    {
        crd::u32 frame = f;
        B tb[5] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U}, {rprev.data(), nullptr, resBytes, 3U}, {nullptr, rtmp.data(), resBytes, 4U}, {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 5), kN / 64U));
        B pb[5] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U}, {rtmp.data(), nullptr, resBytes, 3U}, {nullptr, rspa.data(), resBytes, 4U}, {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(pspv.spirv.data(), pspv.spirv.size()), crd::containers::ConstSpan<B>(pb, 5), kN / 64U));
        B sb[4] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U}, {rspa.data(), nullptr, resBytes, 3U}, {nullptr, rad.data(), posBytes, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), kN / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { accum[i] += rad[i]; }
        // Feed back the PRE-spatial (temporal) reservoir. Feeding the post-spatial reservoir back would compound spatially-borrowed
        // samples across frames and DARKEN the estimate (the classic ReSTIR bias that only GRIS / pairwise-MIS weights fix); the
        // clean-temporal-history choice keeps the pipeline provably unbiased. Spatial reuse then acts as a per-frame refinement.
        for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rtmp[i]; }
        if (f == kF - 1U) { for (crd::usize i = 0; i < rad.size(); ++i) { last[i] = rad[i]; } }
    }
    crd::containers::Array<float> mean(&alloc);
    mean.resize(rad.size(), 0.0F);
    for (crd::usize i = 0; i < mean.size(); ++i) { mean[i] = accum[i] / static_cast<float>(kF); }

    const double refMean   = spatialMean(ref);
    const double stMean    = spatialMean(mean);
    const double errLast   = rms(last, ref);
    const double errSingle = rms(single, ref);
    INFO("spatiotemporal ReSTIR: refMean=" << refMean << "  stMean=" << stMean << "  rms(warm-ref)=" << errLast << "  rms(single-ref)=" << errSingle);
    CHECK(refMean > 0.05);
    CHECK(crd::math::abs(stMean - refMean) / refMean < 0.04);  // UNBIASED — the spatial Z-normalisation keeps the estimator unbiased
    // Spatiotemporal beats single-pass RIS even across the shadow edge. The margin is bounded by the hard-shadow penumbra's
    // irreducible 1-spp visibility variance (binary V per pixel) — the residual a denoiser then resolves; the noise-dominated
    // (fully-visible) win is the 5.8× shown in RT-5b.
    CHECK(errLast < errSingle * 0.85);
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
    constexpr crd::u32 kN = 64U;
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F);
    const float rd[4][6] = {{0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // → instance 0 (t=2)
                            {2.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // → instance 1 (t=2)
                            {4.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // → instance 2 (t=2)
                            {6.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}};  // → nothing (miss)
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }

    // ── CPU oracle: the 3 instances pre-baked to world space (translate each local triangle) ──
    crd::containers::Array<crd::f64> geo(&alloc), rays64(&alloc), oref(&alloc);
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
    oref.resize(kN, 0.0);
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
    got.resize(kN, 0.0F);
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));

    INFO("instanced t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    for (int r = 0; r < 3; ++r) { CHECK(crd::math::abs(static_cast<double>(got[r]) - 2.0) < 1.0e-4); } // each instance hit at t=2
    CHECK(got[3] > 1.0e29);                                                                             // the gap ⇒ miss
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
    constexpr crd::u32 kN = 64U;
    const float lights[4 * 15] = {
        -3.0F, 3.0F, -3.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 6.0F, 6.0F, 6.0F,
         2.0F, 4.0F, -2.0F, 1.5F, 0.0F, 0.0F, 0.0F, 0.0F, 1.5F, 0.0F, -1.0F, 0.0F, 4.0F, 4.0F, 4.0F,
        -2.0F, 3.5F,  2.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 2.0F, 0.0F, -1.0F, 0.0F, 5.0F, 5.0F, 5.0F,
         2.5F, 5.0F,  2.5F, 2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 3.0F, 3.0F, 3.0F};
    const float dumb[9] = {-1.0F, -10.0F, -1.0F, 1.0F, -10.0F, -1.0F, 0.0F, -10.0F, 1.0F};
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    const crd::u64 posBytes = static_cast<crd::u64>(kN) * 3U * sizeof(float);

    crd::containers::Array<float> img(&alloc);
    img.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
        B bind[4] = {{ppos.data(), nullptr, posBytes, 1U}, {pnrm.data(), nullptr, posBytes, 2U}, {lp, nullptr, static_cast<crd::u64>(nl) * 15U * sizeof(float), 3U}, {nullptr, img.data(), posBytes, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<B>(bind, 4), kN / 64U));
        double mean = 0.0;
        for (crd::u32 p = 0; p < kN; ++p) { mean += static_cast<double>(img[p * 3U]); }
        return mean / static_cast<double>(kN);
    };
    crd::containers::Array<float> refImg(&alloc), uniImg(&alloc), powImg(&alloc);
    refImg.resize(img.size(), 0.0F); uniImg.resize(img.size(), 0.0F); powImg.resize(img.size(), 0.0F);
    const auto rmsv = [&](const crd::containers::Array<float>& a) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { const double d = static_cast<double>(a[i]) - static_cast<double>(refImg[i]); s += d * d; } return crd::math::sqrt(s / static_cast<double>(a.size())); };

    double sumPer = 0.0;
    for (crd::u32 l = 0; l < 4U; ++l) { sumPer += run(&lights[l * 15U], 1U, 2048U, false); }
    const double all = run(lights, 4U, 8192U, false);
    for (crd::usize i = 0; i < img.size(); ++i) { refImg[i] = img[i]; }   // converged reference (uniform, 8192 spp)
    const double allPow = run(lights, 4U, 8192U, true);
    // low-spp images for the variance comparison (equal budget):
    run(lights, 4U, 64U, false); for (crd::usize i = 0; i < img.size(); ++i) { uniImg[i] = img[i]; }
    run(lights, 4U, 64U, true);  for (crd::usize i = 0; i < img.size(); ++i) { powImg[i] = img[i]; }
    const double eUni = rmsv(uniImg), ePow = rmsv(powImg);
    INFO("many-lights: Σ per-light=" << sumPer << "  uniform=" << all << "  power=" << allPow << "  rms64 uniform=" << eUni << " power=" << ePow);
    CHECK(sumPer > 0.05);
    CHECK(crd::math::abs(all - sumPer) / sumPer < 0.03);    // uniform selection unbiased ⇒ == the true multi-light sum
    CHECK(crd::math::abs(allPow - sumPer) / sumPer < 0.03); // POWER selection is ALSO unbiased (correct CDF + pdf)
    CHECK(ePow < eUni);                                     // power sampling has LOWER variance (importance-samples brighter lights)
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
    constexpr crd::u32 kN = 64U, kNTri = 6U;
    // prims 0-1 ceiling (y=6, big), prims 2-3 light0 (y=3), prims 4-5 light1 (y=3).
    const float verts[kNTri * 9] = {
        -4,6,-4,  4,6,-4,  4,6,4,      -4,6,-4,  4,6,4,  -4,6,4,       // ceiling
        -2.5F,3,-0.5F, -1.5F,3,-0.5F, -1.5F,3,0.5F,  -2.5F,3,-0.5F, -1.5F,3,0.5F, -2.5F,3,0.5F, // light0
         1.5F,3,-0.5F,  2.5F,3,-0.5F,  2.5F,3,0.5F,   1.5F,3,-0.5F,  2.5F,3,0.5F,  1.5F,3,0.5F}; // light1
    const float tri_n[kNTri * 3] = {0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0, 0,-1,0};
    const float lights[2 * 15] = {
        -2.5F, 3.0F, -0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 8.0F, 8.0F, 8.0F,
         1.5F, 3.0F, -0.5F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -1.0F, 0.0F, 6.0F, 6.0F, 6.0F};
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
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
    auto scene = rt.build_scene(verts, kNTri);
    REQUIRE(scene != nullptr);
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pb = static_cast<crd::u64>(kN) * 3U * sizeof(float);

    const auto run = [&](crd::u32 spp, crd::u32 rr_start, crd::containers::Array<float>& outc) {
        kir::rt::PathTraceFullConfig pc;
        pc.samples = spp; pc.bounces = 4U; pc.rr_start = rr_start;
        pc.albedo[0] = 0.6F; pc.albedo[1] = 0.6F; pc.albedo[2] = 0.6F;
        pc.ntri = kNTri; pc.nlights = 2U; pc.light_prim0 = 2U; pc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_pathtrace_full_kernel(g, pc);
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ptfull", &alloc);
        REQUIRE(spv.ok);
        outc.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
        B bind[5] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, kNTri * 3U * sizeof(float), 3U}, {lights, nullptr, 2U * 15U * sizeof(float), 4U}, {nullptr, outc.data(), pb, 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<B>(bind, 5), kN / 64U));
        return e;
    };
    const auto smean = [&](const crd::containers::Array<float>& a) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); } return s / static_cast<double>(a.size()); };

    // ── GPU==oracle at low spp (RR on) ──
    crd::containers::Array<float> gpu16(&alloc);
    const kir::KEntry e16 = run(16U, 1U, gpu16);
    {
        kir::rt::PathTraceFullConfig pc; pc.samples = 16U; pc.bounces = 4U; pc.rr_start = 1U;
        pc.albedo[0] = 0.6F; pc.albedo[1] = 0.6F; pc.albedo[2] = 0.6F; pc.ntri = kNTri; pc.nlights = 2U; pc.light_prim0 = 2U; pc.local_size = 64U;
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::rt::build_pathtrace_full_kernel(g, pc);
        crd::containers::Array<crd::f64> geo(&alloc), pos64(&alloc), nrm64(&alloc), tn64(&alloc), lt64(&alloc), refc(&alloc);
        geo.resize(1U + static_cast<crd::usize>(kNTri) * 9U, 0.0);
        geo[0] = static_cast<crd::f64>(kNTri);
        for (crd::u32 i = 0; i < kNTri * 9U; ++i) { geo[i + 1U] = static_cast<crd::f64>(verts[i]); }
        pos64.resize(ppos.size(), 0.0); nrm64.resize(pnrm.size(), 0.0); tn64.resize(kNTri * 3U, 0.0); lt64.resize(2U * 15U, 0.0); refc.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
        for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
        for (crd::u32 i = 0; i < kNTri * 3U; ++i) { tn64[i] = static_cast<crd::f64>(tri_n[i]); }
        for (crd::u32 i = 0; i < 2U * 15U; ++i) { lt64[i] = static_cast<crd::f64>(lights[i]); }
        kir::KernelBuffer bufs[6] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {lt64.data(), static_cast<int>(lt64.size()), 0, 4}, {refc.data(), static_cast<int>(refc.size()), 0, 5}};
        kir::eval_cpu_kernel(g, e, bufs, 6, 64U, &alloc, 1U);
        double worst = 0.0;
        for (crd::u32 p = 0; p < kN * 3U; ++p) { worst = crd::math::max(worst, crd::math::abs(static_cast<double>(gpu16[p]) - refc[p])); }
        INFO("full-pt GPU==oracle worst=" << worst);
        CHECK(worst < 0.05); // GPU == CPU oracle (deterministic, incl. the RR hash decisions + emissive MIS)
    }

    // ── Russian roulette unbiasedness: RR-on mean == RR-off mean ──
    crd::containers::Array<float> rrOn(&alloc), rrOff(&alloc);
    run(256U, 1U, rrOn);    // RR active from bounce 1
    run(256U, 99U, rrOff);  // RR never fires (reference)
    const double mOn = smean(rrOn), mOff = smean(rrOff);
    double mxv = 0.0; crd::u32 mxi = 0;
    for (crd::u32 i = 0; i < rrOff.size(); ++i) { if (static_cast<double>(rrOff[i]) > mxv) { mxv = static_cast<double>(rrOff[i]); mxi = i; } }
    INFO("full-pt  RR-on mean=" << mOn << "  RR-off mean=" << mOff << "  max=" << mxv << " @elem " << mxi << "  pt0=[" << rrOff[0] << "," << rrOff[1] << "," << rrOff[2] << "]");
    CHECK(mOff > 0.05);                                  // the lights illuminate the floor (emissive + many-lights direct)
    CHECK(crd::math::abs(mOn - mOff) / mOff < 0.04);     // Russian roulette is UNBIASED (survivors ÷p keep the mean)
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
    constexpr crd::u32 kW = 16U, kH = 16U, kN = kW * kH;
    // AS = the ceiling ONLY (a 10×10 quad at y=4, normal down). The light is analytic (in the light buffer, not the AS), at y=3
    // facing UP ⇒ it lights the ceiling but not the floor. Floor up-rays hit the ceiling ⇒ pure 1-bounce indirect.
    const float verts[18] = {-5,4,-5,  5,4,-5,  5,4,5,   -5,4,-5,  5,4,5,  -5,4,5};
    const float tri_n[6]  = {0,-1,0, 0,-1,0};
    const float light[15] = {-2.0F, 3.0F, -2.0F, 4.0F, 0.0F, 0.0F, 0.0F, 0.0F, 4.0F, 0.0F, 1.0F, 0.0F, 10.0F, 10.0F, 10.0F};
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    for (crd::u32 j = 0; j < kH; ++j)
    {
        for (crd::u32 i = 0; i < kW; ++i)
        {
            const crd::u32 p = j * kW + i;
            ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(kW - 1U) * static_cast<float>(i);
            ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(kH - 1U) * static_cast<float>(j);
            pnrm[p * 3U + 1U] = 1.0F;
        }
    }
    auto scene = rt.build_scene(verts, 2U);
    REQUIRE(scene != nullptr);
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pb = static_cast<crd::u64>(kN) * 3U * sizeof(float);
    const crd::u64 rbytes = static_cast<crd::u64>(kN) * kir::rt::kRestirGiStride * sizeof(float);

    kir::rt::RestirGiConfig sc;
    sc.width = kW; sc.height = kH; sc.ntri = 2U; sc.nlights = 1U; sc.local_size = 64U;
    kir::KGraph gt(&alloc), gsh(&alloc);
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

    crd::containers::Array<float> rprev(&alloc), rcur(&alloc), rad(&alloc), accRef(&alloc), accR(&alloc), warm(&alloc), single(&alloc);
    rprev.resize(static_cast<crd::usize>(kN) * kir::rt::kRestirGiStride, 0.0F);
    rcur.resize(rprev.size(), 0.0F); rad.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    accRef.resize(rad.size(), 0.0F); accR.resize(rad.size(), 0.0F); warm.resize(rad.size(), 0.0F); single.resize(rad.size(), 0.0F);

    const auto frameStep = [&](crd::u32 f, bool feedback, crd::containers::Array<float>& outRad) {
        crd::u32 frame = f;
        B tb[8] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {light, nullptr, 15U * sizeof(float), 4U}, {rprev.data(), nullptr, rbytes, 5U}, {nullptr, rcur.data(), rbytes, 6U}, {&frame, nullptr, sizeof(crd::u32), 7U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 7), kN / 64U));
        B sb[4] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rcur.data(), nullptr, rbytes, 3U}, {nullptr, outRad.data(), pb, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), kN / 64U));
        if (feedback) { for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rcur[i]; } }
    };
    // reference: NO reuse (rprev stays 0) averaged over many frames.
    constexpr crd::u32 kRef = 96U;
    for (crd::u32 f = 0; f < kRef; ++f) { frameStep(f, false, rad); for (crd::usize i = 0; i < rad.size(); ++i) { accRef[i] += rad[i]; } if (f == 0) { for (crd::usize i = 0; i < rad.size(); ++i) { single[i] = rad[i]; } } }
    // ReSTIR GI: temporal reuse.
    for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = 0.0F; }
    constexpr crd::u32 kF = 64U;
    for (crd::u32 f = 0; f < kF; ++f) { frameStep(f, true, rad); for (crd::usize i = 0; i < rad.size(); ++i) { accR[i] += rad[i]; } if (f == kF - 1U) { for (crd::usize i = 0; i < rad.size(); ++i) { warm[i] = rad[i]; } } }

    const auto smean = [&](const crd::containers::Array<float>& a, double sc2) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); } return s / static_cast<double>(a.size()) * sc2; };
    const auto rms = [&](const crd::containers::Array<float>& a, const crd::containers::Array<float>& b, double sca) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { const double d = static_cast<double>(a[i]) * sca - static_cast<double>(b[i]); s += d * d; } return crd::math::sqrt(s / static_cast<double>(a.size())); };
    const double refMean = smean(accRef, 1.0 / kRef);
    const double rMean   = smean(accR, 1.0 / kF);
    // ref image (per-pixel) for rms:
    crd::containers::Array<float> refImg(&alloc); refImg.resize(rad.size(), 0.0F);
    for (crd::usize i = 0; i < refImg.size(); ++i) { refImg[i] = accRef[i] / static_cast<float>(kRef); }
    const double errWarm = rms(warm, refImg, 1.0), errSingle = rms(single, refImg, 1.0);
    INFO("ReSTIR GI  refMean=" << refMean << "  reuseMean=" << rMean << "  rms(warm)=" << errWarm << "  rms(single)=" << errSingle);
    CHECK(refMean > 0.01);                                 // the floor receives indirect light (pure GI, no direct)
    CHECK(crd::math::abs(rMean - refMean) / refMean < 0.05); // UNBIASED: temporal reuse converges to the brute-force indirect
    CHECK(errWarm < errSingle * 0.8);                      // VARIANCE REDUCTION vs the no-reuse single-sample estimate
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
    constexpr crd::u32 kW = 16U, kH = 16U, kN = kW * kH;
    const float verts[18] = {-5,4,-5, 5,4,-5, 5,4,5, -5,4,-5, 5,4,5, -5,4,5};
    const float tri_n[6]  = {0,-1,0, 0,-1,0};
    const float light[15] = {-2.0F, 3.0F, -2.0F, 4.0F, 0.0F, 0.0F, 0.0F, 0.0F, 4.0F, 0.0F, 1.0F, 0.0F, 10.0F, 10.0F, 10.0F};
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(kN) * 3U, 0.0F); pnrm.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    for (crd::u32 j = 0; j < kH; ++j) { for (crd::u32 i = 0; i < kW; ++i) { const crd::u32 p = j * kW + i; ppos[p * 3U + 0U] = -3.0F + 6.0F / static_cast<float>(kW - 1U) * static_cast<float>(i); ppos[p * 3U + 2U] = -3.0F + 6.0F / static_cast<float>(kH - 1U) * static_cast<float>(j); pnrm[p * 3U + 1U] = 1.0F; } }
    auto scene = rt.build_scene(verts, 2U);
    REQUIRE(scene != nullptr);
    using B = gpu::VulkanRayTracingContext::Binding;
    const crd::u64 pb = static_cast<crd::u64>(kN) * 3U * sizeof(float);
    const crd::u64 rbytes = static_cast<crd::u64>(kN) * kir::rt::kRestirGiStride * sizeof(float);

    kir::rt::RestirGiConfig sc; sc.width = kW; sc.height = kH; sc.ntri = 2U; sc.nlights = 1U; sc.spatial_k = 5U; sc.spatial_radius = 3.0F; sc.local_size = 64U;
    kir::KGraph gt(&alloc), gsp(&alloc), gsh(&alloc);
    const kir::KEntry et = kir::rt::build_restir_gi_temporal_kernel(gt, sc);
    const kir::KEntry ep = kir::rt::build_restir_gi_spatial_kernel(gsp, sc);
    const kir::KEntry es = kir::rt::build_restir_gi_shade_kernel(gsh, sc);
    const auto comp = [&](const kir::KEntry& e, kir::KGraph& g, const char* t) { kir::GlslKernel kern(&alloc); REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern)); auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), t, &alloc); INFO(spv.error_message.c_str()); REQUIRE(spv.ok); return spv; };
    const auto tspv = comp(et, gt, "gt"), pspv = comp(ep, gsp, "gp"), sspv = comp(es, gsh, "gs");

    crd::containers::Array<float> rprev(&alloc), rtmp(&alloc), rspa(&alloc), rad(&alloc), accRef(&alloc), accST(&alloc);
    rprev.resize(static_cast<crd::usize>(kN) * kir::rt::kRestirGiStride, 0.0F); rtmp.resize(rprev.size(), 0.0F); rspa.resize(rprev.size(), 0.0F);
    rad.resize(static_cast<crd::usize>(kN) * 3U, 0.0F); accRef.resize(rad.size(), 0.0F); accST.resize(rad.size(), 0.0F);
    const auto smean = [&](const crd::containers::Array<float>& a, double s2) { double s = 0.0; for (crd::usize i = 0; i < a.size(); ++i) { s += static_cast<double>(a[i]); } return s / static_cast<double>(a.size()) * s2; };

    // reference: temporal-only, NO feedback, averaged (brute-force indirect).
    constexpr crd::u32 kRef = 96U;
    for (crd::u32 f = 0; f < kRef; ++f)
    {
        crd::u32 frame = f;
        B tb[7] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {light, nullptr, 15U * sizeof(float), 4U}, {rprev.data(), nullptr, rbytes, 5U}, {nullptr, rtmp.data(), rbytes, 6U}, {&frame, nullptr, sizeof(crd::u32), 7U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 7), kN / 64U));
        B sb[4] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rtmp.data(), nullptr, rbytes, 3U}, {nullptr, rad.data(), pb, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), kN / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { accRef[i] += rad[i]; }
    }
    // spatiotemporal: temporal → spatial(Jacobian) → shade, feed back the pre-spatial reservoir.
    for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = 0.0F; }
    constexpr crd::u32 kF = 64U;
    for (crd::u32 f = 0; f < kF; ++f)
    {
        crd::u32 frame = f;
        B tb[7] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {light, nullptr, 15U * sizeof(float), 4U}, {rprev.data(), nullptr, rbytes, 5U}, {nullptr, rtmp.data(), rbytes, 6U}, {&frame, nullptr, sizeof(crd::u32), 7U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(tspv.spirv.data(), tspv.spirv.size()), crd::containers::ConstSpan<B>(tb, 7), kN / 64U));
        B pbnd[5] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rtmp.data(), nullptr, rbytes, 3U}, {nullptr, rspa.data(), rbytes, 4U}, {&frame, nullptr, sizeof(crd::u32), 5U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(pspv.spirv.data(), pspv.spirv.size()), crd::containers::ConstSpan<B>(pbnd, 5), kN / 64U));
        B sb[4] = {{ppos.data(), nullptr, pb, 1U}, {pnrm.data(), nullptr, pb, 2U}, {rspa.data(), nullptr, rbytes, 3U}, {nullptr, rad.data(), pb, 4U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(sspv.spirv.data(), sspv.spirv.size()), crd::containers::ConstSpan<B>(sb, 4), kN / 64U));
        for (crd::usize i = 0; i < rad.size(); ++i) { accST[i] += rad[i]; }
        for (crd::usize i = 0; i < rprev.size(); ++i) { rprev[i] = rtmp[i]; }
    }
    const double refMean = smean(accRef, 1.0 / kRef), stMean = smean(accST, 1.0 / kF);
    INFO("ReSTIR GI spatiotemporal  refMean=" << refMean << "  stMean=" << stMean);
    CHECK(refMean > 0.01);
    CHECK(crd::math::abs(stMean - refMean) / refMean < 0.06); // Jacobian spatial reconnection keeps the GI estimator unbiased
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

    constexpr crd::u32 kN = 64U;
    crd::containers::Array<float> rays(&alloc), got(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F);
    got.resize(kN, 0.0F);
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
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));

    crd::u32 frontHits = 0, backHits = 0;
    for (crd::u32 p = 0; p < kN; ++p)
    {
        const double t = static_cast<double>(got[p]);
        if (t > 0.9 && t < 1.1) { ++frontHits; }
        else if (t > 1.9 && t < 2.1) { ++backHits; }
    }
    INFO("OMM: interior rays=" << interior << "  front(opaque)=" << frontHits << "  back(passed-through)=" << backHits);
    CHECK(frontHits > 3);                       // some rays hit the OPAQUE micro-triangles (front)
    CHECK(backHits > 3);                        // some rays PASSED THROUGH the transparent micro-triangles to the back
    CHECK(frontHits + backHits >= interior - 2); // every interior ray resolved to front or back (nothing lost)
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
    constexpr crd::u32 kN = 4U;
    const float rd[kN][6] = {{0.2F,0.2F,0,0,0,1}, {0.2F,0.2F,0,0,0,-1}, {5,5,0,0,0,1}, {0.1F,0.1F,1,0,0,1}};
    crd::containers::Array<float> rays(&alloc), got(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F); got.resize(kN, 0.0F);
    for (crd::u32 i = 0; i < kN; ++i) { for (int c = 0; c < 6; ++c) { rays[i * 6U + c] = rd[i][c]; } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_rays_pipeline(*scene, crd::containers::ConstSpan<crd::u8>(rg.spirv.data(), rg.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(ms.spirv.data(), ms.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(ch.spirv.data(), ch.spirv.size()),
                                   crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), kN, 1U));
    INFO("RT pipeline" << (ser ? " (SER)" : "") << " t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4); // closest-hit t=2 via the pipeline
    CHECK(got[1] > 1.0e29);                                            // miss shader ⇒ 1e30
    CHECK(got[2] > 1.0e29);                                            // miss
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

    constexpr crd::u32 kN = 64U;
    crd::containers::Array<float> rays(&alloc), got(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F); got.resize(kN, 0.0F);
    const float rd[4][6] = {{0.2F,0.2F,0,0,0,1}, {0.2F,0.2F,0,0,0,-1}, {5,5,0,0,0,1}, {0.1F,0.1F,1,0,0,1}};
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));

    INFO("cluster-AS t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4); // cluster BLAS traverses like a normal BLAS
    CHECK(got[1] > 1.0e29);
    CHECK(got[2] > 1.0e29);
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
    constexpr crd::u32 kStrands = 4;
    constexpr crd::u32 kSegs    = 8;
    float              segs[kStrands * kSegs * 8];
    for (crd::u32 s = 0; s < kStrands; ++s)
    {
        for (crd::u32 j = 0; j < kSegs; ++j)
        {
            const float t0 = static_cast<float>(j) / static_cast<float>(kSegs);
            const float t1 = static_cast<float>(j + 1U) / static_cast<float>(kSegs);
            float*      g  = segs + (s * kSegs + j) * 8U;
            g[0] = static_cast<float>(s) * 0.3F; g[1] = t0 * 2.0F; g[2] = 0.0F; g[3] = 0.05F * (1.0F - t0) + 0.01F;
            g[4] = static_cast<float>(s) * 0.3F; g[5] = t1 * 2.0F; g[6] = 0.0F; g[7] = 0.05F * (1.0F - t1) + 0.01F;
        }
    }
    auto scene = rt.build_scene_curves(segs, kStrands * kSegs);
    std::printf("[Vulkan B18-f] curve BLAS over %u swept segments: %s   (native LSS on this adapter: %s)\n",
                kStrands * kSegs, scene != nullptr ? "built" : "FAILED",
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
    constexpr int kStr  = 6;
    constexpr int kSeg  = 6;
    constexpr int nseg  = kStr * kSeg;
    constexpr int nray  = 256;
    crd::containers::Array<float>  segf(&alloc);
    crd::containers::Array<double> segd(&alloc);
    segf.resize(uz(nseg * 8), 0.0F);
    segd.resize(uz(nseg * 8), 0.0);
    for (int s = 0; s < kStr; ++s)
    {
        const float x = -1.0F + 0.4F * static_cast<float>(s);
        for (int j = 0; j < kSeg; ++j)
        {
            const float t0 = static_cast<float>(j) / static_cast<float>(kSeg);
            const float t1 = static_cast<float>(j + 1) / static_cast<float>(kSeg);
            float*      q  = segf.data() + uz((s * kSeg + j) * 8);
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
    kir::KernelBuffer ob_[4] = {{as_stub.data(), 1, 0, 0},
                                {segd.data(), nseg * 8, 0, 1},
                                {rayd.data(), nray * 6, 0, 2},
                                {ref.data(), nray * 2, 0, 3}};
    kir::eval_cpu_kernel(g, e, ob_, 4, e.local_size[0], &alloc, static_cast<crd::u32>(nray / 64));

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

    constexpr double kTmax = 50.0; // a MISS returns tmax, so classify against it, not a fixed sentinel
    int    hits = 0;
    int    disagree = 0;
    int    cpu_only = 0; // oracle hit, device missed  => AABB too small / commit rejected
    int    gpu_only = 0; // device hit, oracle missed  => false positive in the shader
    double worst = 0.0;
    for (int i = 0; i < nray; ++i)
    {
        const bool ch = ref[uz(i * 2)] < kTmax - 0.5;
        const bool gh = static_cast<double>(outf[uz(i * 2)]) < kTmax - 0.5;
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
    kir::KGraph grg(&alloc), gch(&alloc), gms(&alloc);
    const kir::KEntry erg = kir::rt::build_rt_pipeline_raygen(grg, ser); // request the SER reorder hint iff the adapter has SER
    const kir::KEntry ech = kir::rt::build_rt_pipeline_closesthit(gch);
    const kir::KEntry ems = kir::rt::build_rt_pipeline_miss(gms);
    kir::GlslKernel krg(&alloc), kch(&alloc), kms(&alloc);
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
    constexpr crd::u32 kN = 4U;
    const float rd[kN][6] = {{0.2F,0.2F,0,0,0,1}, {0.2F,0.2F,0,0,0,-1}, {5,5,0,0,0,1}, {0.1F,0.1F,1,0,0,1}};
    crd::containers::Array<float> rays(&alloc), got(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F); got.resize(kN, 0.0F);
    for (crd::u32 i = 0; i < kN; ++i) { for (int c = 0; c < 6; ++c) { rays[i * 6U + c] = rd[i][c]; } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_rays_pipeline(*scene, crd::containers::ConstSpan<crd::u8>(crg.spirv.data(), crg.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cms.spirv.data(), cms.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cch.spirv.data(), cch.spirv.size()),
                                   crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), kN, 1U));
    INFO("CKIR RT pipeline" << (ser ? " (SER)" : "") << " t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4);
    CHECK(got[1] > 1.0e29);
    CHECK(got[2] > 1.0e29);
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
    kir::KGraph grg(&alloc), gch(&alloc), gms(&alloc), gah(&alloc);
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

    constexpr crd::u32 kN = 64U;
    crd::containers::Array<float> rays(&alloc), got(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F); got.resize(kN, 0.0F);
    crd::u32 interior = 0;
    for (crd::u32 j = 0; j < 8U; ++j) { for (crd::u32 i = 0; i < 8U; ++i) {
        const crd::u32 p = j * 8U + i; const float x = 0.15F + 1.7F / 7.0F * static_cast<float>(i); const float y = 0.15F + 1.7F / 7.0F * static_cast<float>(j);
        rays[p * 6U + 0U] = x; rays[p * 6U + 1U] = y; rays[p * 6U + 5U] = 1.0F; if (x + y < 1.9F) { ++interior; } } }
    gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_rays_pipeline(*scene, crd::containers::ConstSpan<crd::u8>(crg.spirv.data(), crg.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cms.spirv.data(), cms.spirv.size()),
                                   crd::containers::ConstSpan<crd::u8>(cch.spirv.data(), cch.spirv.size()),
                                   crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), kN, 1U,
                                   crd::containers::ConstSpan<crd::u8>(cah.spirv.data(), cah.spirv.size())));
    crd::u32 hit = 0, pass = 0;
    for (crd::u32 p = 0; p < kN; ++p) { if (static_cast<double>(got[p]) > 0.9 && static_cast<double>(got[p]) < 1.1) { ++hit; } else if (static_cast<double>(got[p]) > 1.0e29) { ++pass; } }
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
        constexpr crd::u32 kN = 64U;
        crd::containers::Array<float> rays(&alloc), got(&alloc);
        rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F); got.resize(kN, 0.0F);
        for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }
        gpu::VulkanRayTracingContext::Binding bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
        REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), crd::containers::ConstSpan<gpu::VulkanRayTracingContext::Binding>(bind, 2), 1U));
        INFO("scalable mode=" << mode << " fell_back=" << fell << " t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
        CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4); // both cluster + std BLAS give identical hits
        CHECK(got[1] > 1.0e29); CHECK(got[2] > 1.0e29);
        CHECK(crd::math::abs(static_cast<double>(got[3]) - 1.0) < 1.0e-4);
    }
}
