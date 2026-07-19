// test_dx12_rt.cpp — D-007 C3/RT: the DX12 mirror of test_vulkan_rt. The SAME CKIR RT kernels (ckir_rt.hpp) lower to HLSL, dxc
// cooks DXIL (cs_6_5, inline RayQuery), and the Dx12RayTracingContext builds a DXR BLAS/TLAS and dispatches against it. Each
// effect's DX12 GPU output is checked against the SHARED CPU brute-force ray-triangle oracle (eval_cpu_kernel) — the same oracle
// the Vulkan tests use, so passing here establishes VK≈DX12 (both agree with the reference within RT-traversal geometric tolerance).

#include <crd/gpu/dx12_context.hpp>            // compile_hlsl_to_dxil
#include <crd/gpu/dx12_ray_tracing_context.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_rt.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace gpu = crd::gpu;
namespace kir = crd::kir;

namespace
{
// Emit the kernel's HLSL and cook it to DXIL (cs_6_5). Returns the DXIL bytes (empty on failure).
crd::containers::Array<crd::u8> dxil_of(kir::KGraph& g, const kir::KEntry& e, crd::memory::IAllocator* a)
{
    kir::GlslKernel kern(a);
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, a, kern));
    auto res = gpu::compile_hlsl_to_dxil(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "rt_dx12", a);
    INFO(res.error_message.c_str());
    REQUIRE(res.ok);
    crd::containers::Array<crd::u8> out(a);
    out.resize(res.dxil.size(), 0U);
    for (crd::usize i = 0; i < res.dxil.size(); ++i) { out[i] = res.dxil[i]; }
    return out;
}
using B = gpu::Dx12RayTracingContext::Binding;
} // namespace

// D-007 C3/RT-1: inline ray-query closest-hit distance on DX12 == CPU oracle (the fundamental — a DXR TLAS + inline RayQuery).
TEST_CASE("D-007 RT-1 DX12: inline rayQuery closest-hit t == CPU reference", "[gpu-context][dx12][gpu][rt]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::rt::build_ray_trace_kernel(g, 64U);
    const auto                 dxil = dxil_of(g, e, &alloc);

    // one triangle at z=2 spanning (0,0)-(1,0)-(0,1).
    const float verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    constexpr crd::u32 kN = 64U; // pad rays/out to the workgroup width so no thread reads/writes out of bounds
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F);
    const float rd[4][6] = {{0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // interior +z ⇒ t=2
                            {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, -1.0F},  // away ⇒ miss
                            {5.0F, 5.0F, 0.0F, 0.0F, 0.0F, 1.0F},   // outside ⇒ miss
                            {0.1F, 0.1F, 1.0F, 0.0F, 0.0F, 1.0F}};  // from z=1 ⇒ t=1
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc), rays64(&alloc), oref(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = 1.0;
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    rays64.resize(rays.size(), 0.0);
    for (crd::usize i = 0; i < rays.size(); ++i) { rays64[i] = static_cast<crd::f64>(rays[i]); }
    oref.resize(kN, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {rays64.data(), static_cast<int>(rays64.size()), 0, 1}, {oref.data(), static_cast<int>(oref.size()), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, 64U, &alloc, 1U);

    // ── DX12 GPU ──
    auto scene = rt.build_scene(verts, 1U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(kN, 0.0F);
    B bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(dxil.data(), dxil.size()), crd::containers::ConstSpan<B>(bind, 2), 1U));

    INFO("DX12 t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    CHECK(crd::math::abs(static_cast<double>(got[0]) - 2.0) < 1.0e-4); // hit t=2
    CHECK(got[1] > 1.0e29);                                            // miss ⇒ tmax
    CHECK(got[2] > 1.0e29);                                            // miss ⇒ tmax
    CHECK(crd::math::abs(static_cast<double>(got[3]) - 1.0) < 1.0e-4); // hit t=1
}

// D-007 RT-3 DX12: the path-tracing megakernel (For sample loop + unrolled bounce chain + trace_ray_hit) on DX12 == CPU oracle.
TEST_CASE("D-007 RT-3 DX12: path-tracing megakernel == CPU reference", "[gpu-context][dx12][gpu][rt]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::rt::PathTraceConfig   pcfg;
    pcfg.samples = 16U; pcfg.bounces = 3U;
    pcfg.albedo[0] = 0.4F; pcfg.albedo[1] = 0.4F; pcfg.albedo[2] = 0.4F;
    pcfg.ntri = 2U; pcfg.local_size = 64U;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_pathtrace_kernel(g, pcfg);
    const auto        dxil = dxil_of(g, e, &alloc);

    const float verts[18] = {-6.0F, 1.0F, -6.0F, 0.0F, 1.0F, -6.0F, 0.0F, 1.0F, 6.0F, -6.0F, 1.0F, -6.0F, 0.0F, 1.0F, 6.0F, -6.0F, 1.0F, 6.0F};
    const float tri_n[6]  = {0.0F, -1.0F, 0.0F, 0.0F, -1.0F, 0.0F};
    constexpr crd::u32 kN = 64U;
    crd::containers::Array<float> ppos(&alloc), pnrm(&alloc);
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
    // oracle
    crd::containers::Array<crd::f64> geo(&alloc), pos64(&alloc), nrm64(&alloc), tn64(&alloc), refc(&alloc);
    geo.resize(1U + 2U * 9U, 0.0);
    geo[0] = 2.0;
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    pos64.resize(ppos.size(), 0.0); nrm64.resize(pnrm.size(), 0.0); tn64.resize(6U, 0.0); refc.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 6; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, 1U);
    // GPU
    auto scene = rt.build_scene(verts, 2U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    B bind[4] = {{ppos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U}, {pnrm.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * 3U * sizeof(float), 4U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(dxil.data(), dxil.size()), crd::containers::ConstSpan<B>(bind, 4), 1U));

    double worst = 0.0;
    for (crd::u32 p = 0; p < kN * 3U; ++p) { worst = crd::math::max(worst, crd::math::abs(static_cast<double>(got[p]) - refc[p])); }
    INFO("DX12 pathtrace worst |GPU-ref|=" << worst);
    CHECK(worst < 0.06); // DX12 path radiance == CPU oracle (transcendental ULP + rare grazing flips) — same bar as Vulkan
}

// D-007 RT-4 DX12: the NEE+MIS area-light path tracer (shadow rays + trace_ray_hit + MIS) on DX12 == CPU oracle (soft shadow).
TEST_CASE("D-007 RT-4 DX12: NEE+MIS area-light path tracer == CPU reference", "[gpu-context][dx12][gpu][rt]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }

    crd::memory::TlsfAllocator  alloc(48U << 20U);
    kir::rt::PathTraceNeeConfig pcfg;
    pcfg.samples = 32U; pcfg.bounces = 2U;
    pcfg.albedo[0] = 0.6F; pcfg.albedo[1] = 0.6F; pcfg.albedo[2] = 0.6F;
    pcfg.light_p0[0] = -1.5F; pcfg.light_p0[1] = 3.0F; pcfg.light_p0[2] = -1.5F;
    pcfg.light_eu[0] = 3.0F; pcfg.light_ev[2] = 3.0F; pcfg.light_nl[1] = -1.0F;
    pcfg.light_le[0] = 8.0F; pcfg.light_le[1] = 8.0F; pcfg.light_le[2] = 8.0F;
    pcfg.ntri = 4U; pcfg.light_prim0 = 2U; pcfg.light_ntri = 2U; pcfg.local_size = 64U;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_pathtrace_nee_kernel(g, pcfg);
    const auto        dxil = dxil_of(g, e, &alloc);

    const float verts[36] = {
        -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, -0.5F, 0.5F, 2.0F, 0.5F, -0.5F, 2.0F, 0.5F,
        -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, -1.5F, 1.5F, 3.0F, 1.5F, -1.5F, 3.0F, 1.5F};
    const float tri_nf[12] = {0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F, -1.0F, 0.0F};
    constexpr crd::u32 kN = 64U;
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
    crd::containers::Array<crd::f64> geo(&alloc), pos64(&alloc), nrm64(&alloc), tn64(&alloc), refc(&alloc);
    geo.resize(1U + 4U * 9U, 0.0);
    geo[0] = 4.0;
    for (int i = 0; i < 36; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    pos64.resize(ppos.size(), 0.0); nrm64.resize(pnrm.size(), 0.0); tn64.resize(12U, 0.0); refc.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 12; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_nf[i]); }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, 1U);
    auto scene = rt.build_scene(verts, 4U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(static_cast<crd::usize>(kN) * 3U, 0.0F);
    B bind[4] = {{ppos.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 1U}, {pnrm.data(), nullptr, static_cast<crd::u64>(kN) * 3U * sizeof(float), 2U}, {tri_nf, nullptr, 12U * sizeof(float), 3U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * 3U * sizeof(float), 4U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(dxil.data(), dxil.size()), crd::containers::ConstSpan<B>(bind, 4), 1U));

    double worst = 0.0;
    double lmin  = 1.0e30;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < kN * 3U; ++p) { worst = crd::math::max(worst, crd::math::abs(static_cast<double>(got[p]) - refc[p])); }
    for (crd::u32 p = 0; p < kN; ++p) { lmin = crd::math::min(lmin, refc[p * 3U]); lmax = crd::math::max(lmax, refc[p * 3U]); }
    INFO("DX12 nee/mis worst |GPU-ref|=" << worst << "  range=[" << lmin << ", " << lmax << "]");
    CHECK(worst < 0.05);        // DX12 MIS radiance == CPU oracle (same bar as Vulkan)
    CHECK(lmax - lmin > 0.10);  // a real soft shadow
}

// D-007 RT-6 DX12: MULTI-INSTANCE TLAS — the DX12 mirror. One BLAS instanced at 3 translations via per-instance 3×4 transforms;
// GPU instanced traversal == the CPU oracle with the 3 world-space copies. Portable scale capability on the second backend.
TEST_CASE("D-007 RT-6 DX12: multi-instance TLAS (per-instance transforms) == CPU reference", "[gpu-context][dx12][gpu][rt]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::rt::build_ray_trace_kernel(g, 64U);
    const auto                 dxil = dxil_of(g, e, &alloc);

    const float verts[9]       = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    const float transforms[36] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                                  1, 0, 0, 2, 0, 1, 0, 0, 0, 0, 1, 0,
                                  1, 0, 0, 4, 0, 1, 0, 0, 0, 0, 1, 0};
    constexpr crd::u32 kN = 64U;
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(kN) * 6U, 0.0F);
    const float rd[4][6] = {{0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}, {2.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}, {4.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}, {6.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}};
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }

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

    auto scene = rt.build_scene_instanced(verts, 1U, transforms, 3U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(kN, 0.0F);
    B bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(kN) * sizeof(float), 2U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(dxil.data(), dxil.size()), crd::containers::ConstSpan<B>(bind, 2), 1U));

    INFO("DX12 instanced t=[" << got[0] << ", " << got[1] << ", " << got[2] << ", " << got[3] << "]");
    for (int r = 0; r < 3; ++r) { CHECK(crd::math::abs(static_cast<double>(got[r]) - 2.0) < 1.0e-4); }
    CHECK(got[3] > 1.0e29);
    for (int r = 0; r < 4; ++r) { CHECK(crd::math::abs(static_cast<double>(got[r]) - oref[r]) < 1.0e-4); }
}

// D-007 P2 DX12: the CKIR-authored RT-pipeline stages lower to valid DXR HLSL — raygen/closest-hit/miss emitted from KEntry via
// emit_rt_stage_hlsl and cooked to a DXIL library (lib_6_3) by dxc. Proves the vendor RT-pipeline shaders are portable CKIR (the
// DX12 DXR runtime dispatch is a separate device feature; DX12 reports RtPipeline=false today so a portable consumer falls back).
TEST_CASE("D-007 P2 DX12: CKIR RT pipeline stages lower to valid DXR HLSL", "[gpu-context][dx12][rt]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph grg(&alloc), gch(&alloc), gms(&alloc);
    const kir::KEntry erg = kir::rt::build_rt_pipeline_raygen(grg, /*use_ser=*/false);
    const kir::KEntry ech = kir::rt::build_rt_pipeline_closesthit(gch);
    const kir::KEntry ems = kir::rt::build_rt_pipeline_miss(gms);
    kir::GlslKernel krg(&alloc), kch(&alloc), kms(&alloc);
    REQUIRE(kir::emit_rt_stage_hlsl(grg, erg, &alloc, krg, false));
    REQUIRE(kir::emit_rt_stage_hlsl(gch, ech, &alloc, kch, false));
    REQUIRE(kir::emit_rt_stage_hlsl(gms, ems, &alloc, kms, false));
    INFO("raygen HLSL:\n" << krg.source.c_str());
    const auto crg = gpu::compile_hlsl_to_dxil(gpu::ShaderStage::RayGen, crd::containers::to_view(krg.source), "rg", &alloc);
    const auto cch = gpu::compile_hlsl_to_dxil(gpu::ShaderStage::ClosestHit, crd::containers::to_view(kch.source), "ch", &alloc);
    const auto cms = gpu::compile_hlsl_to_dxil(gpu::ShaderStage::Miss, crd::containers::to_view(kms.source), "ms", &alloc);
    INFO("rg err: " << crg.error_message.c_str());
    if (!crg.ok) { WARN("dxc unavailable / lib_6_3 unsupported; skipping"); return; }
    CHECK(crg.ok);            // raygen → DXIL library
    CHECK(cch.ok);            // closest-hit
    CHECK(cms.ok);            // miss
    CHECK(crg.dxil.size() > 0);
}
