// test_dx12_rt.cpp — D-007 C3/RT: the DX12 mirror of test_vulkan_rt. The SAME CKIR RT kernels (ckir_rt.hpp) lower to HLSL, dxc
// cooks DXIL (cs_6_5, inline RayQuery), and the Dx12RayTracingContext builds a DXR BLAS/TLAS and dispatches against it. Each
// effect's DX12 GPU output is checked against the SHARED CPU brute-force ray-triangle oracle (eval_cpu_kernel) — the same oracle
// the Vulkan tests use, so passing here establishes VK≈DX12 (both agree with the reference within RT-traversal geometric tolerance).

#include <crd/gpu/dx12_context.hpp>            // compile_hlsl_to_dxil
#include <crd/gpu/dx12_ray_tracing_context.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // CEIR-19c: load the AUTHORED rt_witness.ckir (ckir_read)
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_rt.hpp>

// CEIR-19c: the ceir.rt->gpu execution bridge under test — an authored ceir.rt program lowered + driven onto this
// Dx12RayTracingContext through the caller hooks (crd-ceir-gpu names no backend). The DXR twin of the Vulkan bridge gate.
#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/ceir/rt.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream> // CEIR-19c: read the committed .ckir asset

namespace gpu  = crd::gpu;
namespace kir  = crd::kir;
namespace ceir = crd::ceir;      // CEIR-19c
namespace cg   = crd::ceir::gpu; // CEIR-19c

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

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
    constexpr crd::u32 k_n = 64U; // pad rays/out to the workgroup width so no thread reads/writes out of bounds
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(k_n) * 6U, 0.0F);
    const float rd[4][6] = {{0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},   // interior +z ⇒ t=2
                            {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, -1.0F},  // away ⇒ miss
                            {5.0F, 5.0F, 0.0F, 0.0F, 0.0F, 1.0F},   // outside ⇒ miss
                            {0.1F, 0.1F, 1.0F, 0.0F, 0.0F, 1.0F}};  // from z=1 ⇒ t=1
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }

    // ── CPU oracle ──
    crd::containers::Array<crd::f64> geo(&alloc);
    crd::containers::Array<crd::f64> rays64(&alloc);
    crd::containers::Array<crd::f64> oref(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = 1.0;
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    rays64.resize(rays.size(), 0.0);
    for (crd::usize i = 0; i < rays.size(); ++i) { rays64[i] = static_cast<crd::f64>(rays[i]); }
    oref.resize(k_n, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {rays64.data(), static_cast<int>(rays64.size()), 0, 1}, {oref.data(), static_cast<int>(oref.size()), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, 64U, &alloc, 1U);

    // ── DX12 GPU ──
    auto scene = rt.build_scene(verts, 1U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(k_n, 0.0F);
    B bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(k_n) * sizeof(float), 2U}};
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
    constexpr crd::u32 k_n = 64U;
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(k_n) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(k_n) * 3U, 0.0F);
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
    crd::containers::Array<crd::f64> geo(&alloc);
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    crd::containers::Array<crd::f64> tn64(&alloc);
    crd::containers::Array<crd::f64> refc(&alloc);
    geo.resize(1U + 2U * 9U, 0.0);
    geo[0] = 2.0;
    for (int i = 0; i < 18; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    pos64.resize(ppos.size(), 0.0); nrm64.resize(pnrm.size(), 0.0); tn64.resize(6U, 0.0); refc.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 6; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_n[i]); }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, 1U);
    // GPU
    auto scene = rt.build_scene(verts, 2U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(static_cast<crd::usize>(k_n) * 3U, 0.0F);
    B bind[4] = {{ppos.data(), nullptr, static_cast<crd::u64>(k_n) * 3U * sizeof(float), 1U}, {pnrm.data(), nullptr, static_cast<crd::u64>(k_n) * 3U * sizeof(float), 2U}, {tri_n, nullptr, 6U * sizeof(float), 3U}, {nullptr, got.data(), static_cast<crd::u64>(k_n) * 3U * sizeof(float), 4U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(dxil.data(), dxil.size()), crd::containers::ConstSpan<B>(bind, 4), 1U));

    double worst = 0.0;
    for (crd::u32 p = 0; p < k_n * 3U; ++p) { worst = crd::math::max(worst, crd::math::abs(static_cast<double>(got[p]) - refc[p])); }
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
    constexpr crd::u32 k_n = 64U;
    crd::containers::Array<float> ppos(&alloc);
    crd::containers::Array<float> pnrm(&alloc);
    ppos.resize(static_cast<crd::usize>(k_n) * 3U, 0.0F);
    pnrm.resize(static_cast<crd::usize>(k_n) * 3U, 0.0F);
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
    crd::containers::Array<crd::f64> geo(&alloc);
    crd::containers::Array<crd::f64> pos64(&alloc);
    crd::containers::Array<crd::f64> nrm64(&alloc);
    crd::containers::Array<crd::f64> tn64(&alloc);
    crd::containers::Array<crd::f64> refc(&alloc);
    geo.resize(1U + 4U * 9U, 0.0);
    geo[0] = 4.0;
    for (int i = 0; i < 36; ++i) { geo[static_cast<crd::usize>(i) + 1U] = static_cast<crd::f64>(verts[i]); }
    pos64.resize(ppos.size(), 0.0); nrm64.resize(pnrm.size(), 0.0); tn64.resize(12U, 0.0); refc.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    for (crd::usize i = 0; i < ppos.size(); ++i) { pos64[i] = static_cast<crd::f64>(ppos[i]); nrm64[i] = static_cast<crd::f64>(pnrm[i]); }
    for (int i = 0; i < 12; ++i) { tn64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(tri_nf[i]); }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos64.data(), static_cast<int>(pos64.size()), 0, 1}, {nrm64.data(), static_cast<int>(nrm64.size()), 0, 2}, {tn64.data(), static_cast<int>(tn64.size()), 0, 3}, {refc.data(), static_cast<int>(refc.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, pcfg.local_size, &alloc, 1U);
    auto scene = rt.build_scene(verts, 4U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(static_cast<crd::usize>(k_n) * 3U, 0.0F);
    B bind[4] = {{ppos.data(), nullptr, static_cast<crd::u64>(k_n) * 3U * sizeof(float), 1U}, {pnrm.data(), nullptr, static_cast<crd::u64>(k_n) * 3U * sizeof(float), 2U}, {tri_nf, nullptr, 12U * sizeof(float), 3U}, {nullptr, got.data(), static_cast<crd::u64>(k_n) * 3U * sizeof(float), 4U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(dxil.data(), dxil.size()), crd::containers::ConstSpan<B>(bind, 4), 1U));

    double worst = 0.0;
    double lmin  = 1.0e30;
    double lmax  = 0.0;
    for (crd::u32 p = 0; p < k_n * 3U; ++p) { worst = crd::math::max(worst, crd::math::abs(static_cast<double>(got[p]) - refc[p])); }
    for (crd::u32 p = 0; p < k_n; ++p) { lmin = crd::math::min(lmin, refc[p * 3U]); lmax = crd::math::max(lmax, refc[p * 3U]); }
    INFO("DX12 nee/mis worst |GPU-ref|=" << worst << "  range=[" << lmin << ", " << lmax << "]");
    // WARP (software; GitHub CI has no GPU) diverges from the fp64 oracle by ~0.05 on this MIS radiance (observed worst
    // 0.0524, radiance range ~[0.18, 1.22]); relax the bar on WARP ONLY (a real dispatch error is O(0.1..1)), keep the
    // tight 0.05 hardware bar.
    CHECK(worst < (crd::gpu::dx12_default_adapter_is_software() ? 0.20 : 0.05)); // DX12 MIS radiance == CPU oracle
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
    constexpr crd::u32 k_n = 64U;
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(k_n) * 6U, 0.0F);
    const float rd[4][6] = {{0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}, {2.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}, {4.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}, {6.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F}};
    for (int r = 0; r < 4; ++r) { for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + c] = rd[r][c]; } }

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
    oref.resize(k_n, 0.0);
    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {rays64.data(), static_cast<int>(rays64.size()), 0, 1}, {oref.data(), static_cast<int>(oref.size()), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, 64U, &alloc, 1U);

    auto scene = rt.build_scene_instanced(verts, 1U, transforms, 3U);
    REQUIRE(scene != nullptr);
    crd::containers::Array<float> got(&alloc);
    got.resize(k_n, 0.0F);
    B bind[2] = {{rays.data(), nullptr, rays.size() * sizeof(float), 1U}, {nullptr, got.data(), static_cast<crd::u64>(k_n) * sizeof(float), 2U}};
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
    kir::KGraph grg(&alloc);
    kir::KGraph gch(&alloc);
    kir::KGraph gms(&alloc);
    const kir::KEntry erg = kir::rt::build_rt_pipeline_raygen(grg, /*use_ser=*/false);
    const kir::KEntry ech = kir::rt::build_rt_pipeline_closesthit(gch);
    const kir::KEntry ems = kir::rt::build_rt_pipeline_miss(gms);
    kir::GlslKernel krg(&alloc);
    kir::GlslKernel kch(&alloc);
    kir::GlslKernel kms(&alloc);
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

// D-007 B18-f DX12: PROCEDURAL CURVE traversal — the same CKIR TraceRayCurves statement the Vulkan gate runs, lowered to
// HLSL and executed against a DXR procedural-AABB BLAS. This is the load-bearing PORTABILITY claim of the strand tier:
// Vulkan can lean on VK_NV_ray_tracing_linear_swept_spheres where the adapter has it, but DXR has NO swept-sphere
// primitive at any tier, so the analytic intersector in CKIR is the ONLY thing making a strand tier exist here. If the
// two backends disagree, the intersector is not portable and the tier is a Vulkan feature wearing a portable name.
//
// Geometry, rays, seed and tolerance are IDENTICAL to the Vulkan gate on purpose — both are held against the same CPU
// oracle, so agreement here plus agreement there establishes VK == DX12 without a direct device-to-device comparison.
TEST_CASE("D-007 B18-f DX12: CKIR TraceRayCurves on DXR procedural AABBs == CPU oracle", "[gpu-context][dx12][gpu][rt]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }

    crd::memory::TlsfAllocator alloc(64U << 20U);
    const auto                 uz = [](int v) { return static_cast<crd::usize>(v); };

    // ── a small groom: strands of tapering segments, spread so most rays hit something ──
    constexpr int k_str = 6;
    constexpr int k_seg = 6;
    constexpr int nseg = k_str * k_seg;
    constexpr int nray = 256;
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
    crd::u32   st  = 0x18F00D01U; // the SAME seed as the Vulkan gate — identical rays, identical oracle
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
    // ⚠ DX12 buffer bindings live at u1.. (u0 unused, TLAS is the root SRV t0), so the segment/ray/out bindings are
    //   1/2/3 exactly as on Vulkan — the SAME KGraph shape, which is the point.
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
    const int  mark = g.kernel_stmt_mark();
    const int  rb   = g.binary(kir::KOp::Mul, tid, cu(6));
    const auto rl   = [&](int k) {
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
    kir::KernelBuffer oracle_bufs[4] = {{as_stub.data(), 1, 0, 0},
                                {segd.data(), nseg * 8, 0, 1},
                                {rayd.data(), nray * 6, 0, 2},
                                {ref.data(), nray * 2, 0, 3}};
    kir::eval_cpu_kernel(g, e, oracle_bufs, 4, e.local_size[0], &alloc, static_cast<crd::u32>(nray / 64));

    // ── then the device ──
    const auto dxil = dxil_of(g, e, &alloc);
    auto       scene = rt.build_scene_curves(segf.data(), static_cast<crd::u32>(nseg));
    REQUIRE(scene != nullptr);

    crd::containers::Array<float> outf(&alloc);
    outf.resize(uz(nray * 2), 0.0F);
    B bind[3] = {{segf.data(), nullptr, static_cast<crd::u64>(nseg) * 8U * sizeof(float), 1U},
                 {rayf.data(), nullptr, static_cast<crd::u64>(nray) * 6U * sizeof(float), 2U},
                 {nullptr, outf.data(), static_cast<crd::u64>(nray) * 2U * sizeof(float), 3U}};
    REQUIRE(rt.trace_dispatch(*scene, crd::containers::ConstSpan<crd::u8>(dxil.data(), dxil.size()),
                              crd::containers::ConstSpan<B>(bind, 3), static_cast<crd::u32>(nray / 64)));

    constexpr double k_tmax = 50.0; // a MISS returns tmax, so classify against it, not a fixed sentinel
    int    hits = 0;
    int    disagree = 0;
    int    cpu_only = 0; // oracle hit, device missed  => AABB too small / commit rejected
    int    gpu_only = 0; // device hit, oracle missed  => false positive in the intersector
    double worst = 0.0;
    for (int i = 0; i < nray; ++i)
    {
        const bool ch = ref[uz(i * 2)] < k_tmax - 0.5;
        const bool gh = static_cast<double>(outf[uz(i * 2)]) < k_tmax - 0.5;
        if (ch != gh)
        {
            ++disagree;
            if (ch) { ++cpu_only; } else { ++gpu_only; }
            continue;
        }
        if (!ch) { continue; }
        ++hits;
        const double dt = crd::math::abs(static_cast<double>(outf[uz(i * 2)]) - ref[uz(i * 2)]);
        const double du = crd::math::abs(static_cast<double>(outf[uz(i * 2 + 1)]) - ref[uz(i * 2 + 1)]);
        if (dt > worst) { worst = dt; }
        if (du > worst) { worst = du; }
    }
    INFO("DX12 curve traversal: " << nray << " rays over " << nseg << " segments, " << hits << " hits, maxabs = " << worst
                                  << ", disagreements = " << disagree << " (oracle-only " << cpu_only << ", device-only "
                                  << gpu_only << ")");
    CHECK(hits > 32);     // the groom must actually be hit, or the comparison proves nothing
    CHECK(disagree == 0); // hardware traversal must find exactly the hits the brute-force oracle finds
    CHECK(worst < 1.0e-3);
}

// ── CEIR-19c STAGE-1 DEVICE GATE (DX12 twin of the Vulkan bridge gate). ───────────────────────────────────────────────
// An AUTHORED ceir.rt program (blas_build -> instance_populate -> tlas_build -> ray_query) lowered (lower_region) + DRIVEN
// onto this Dx12RayTracingContext through execute_rt_lowered's caller HOOKS. The ray_query's kernel is the committed
// assets/ckir/rt_witness.ckir (mandate #1: the .ckir is the source; NO C++ builder) → HLSL → DXIL (cs_6_5, inline RayQuery).
// GPU prim-id + hit/miss BIT-EXACT vs eval_cpu_kernel's TraceRayHit oracle (SAME loaded graph) + KNOWN ANALYTIC values (the
// cf(node) symmetric-bug guard). Passing here + on Vulkan establishes VK≈DX12 for the ceir.rt->gpu bridge.
namespace
{
struct RtGateStateDx
{
    gpu::Dx12RayTracingContext*       rt        = nullptr;
    const float*                      verts     = nullptr;
    crd::u32                          ntris     = 0U;
    std::unique_ptr<gpu::Dx12RtScene> scene;
    const crd::u8*                    dxil      = nullptr;
    crd::usize                        dxil_size = 0U;
    crd::u32                          last_gx   = 0U; // CEIR-19c stage 2: the last dispatch grid.x — pins the count-driven dispatch
};
// CEIR-19c stage 2: the deterministic triple32 (Wellons) fold — the HOST-side decision-hash (decision ints only, no t-float).
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
cg::RtSceneHandle rt_gate_dx_build_scene(const ceir::Operation* /*accel_op*/, void* user)
{
    auto* s = static_cast<RtGateStateDx*>(user);
    if (s->scene == nullptr) { s->scene = s->rt->build_scene(s->verts, s->ntris); } // fused: build once on the first AccelBuild
    return (s->scene != nullptr) ? cg::RtSceneHandle{1U} : cg::RtSceneHandle{0U};
}
crd::containers::ConstSpan<crd::u8> rt_gate_dx_kernel_bytes(const ceir::Operation* /*ray_query*/, void* user)
{
    auto* s = static_cast<RtGateStateDx*>(user);
    return crd::containers::ConstSpan<crd::u8>(s->dxil, s->dxil_size);
}
bool rt_gate_dx_trace_dispatch(cg::RtSceneHandle tlas, crd::containers::ConstSpan<crd::u8> bytes,
                               crd::containers::ConstSpan<cg::RtHostBinding> binds, crd::u32 gx, crd::u32 /*gy*/,
                               crd::u32 /*gz*/, void* user)
{
    auto* s = static_cast<RtGateStateDx*>(user);
    if (tlas == 0U || s->scene == nullptr) { return false; }
    s->last_gx = gx; // pin: the harness CHECKs the shade dispatch's gx == the compact's hit_count readback
    gpu::Dx12RayTracingContext::Binding vb[8];
    const crd::u32                      n = static_cast<crd::u32>(binds.size() < 8U ? binds.size() : 8U);
    for (crd::u32 i = 0; i < n; ++i)
    {
        vb[i].upload   = binds[i].upload;
        vb[i].readback = binds[i].readback;
        vb[i].bytes    = binds[i].bytes;
        vb[i].binding  = i + 1U; // the TLAS is implicit @ descriptor 0; SSBOs follow at 1,2,3 (REN-38-A9)
    }
    return s->rt->trace_dispatch(*s->scene, bytes, crd::containers::ConstSpan<gpu::Dx12RayTracingContext::Binding>(vb, n), gx);
}
} // namespace

TEST_CASE("CEIR-19c DX12: the ceir.rt->gpu bridge (execute_rt_lowered) traces the authored rt_witness.ckir == oracle + analytic",
          "[gpu-context][dx12][gpu][rt][ceir19c]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }

    crd::memory::TlsfAllocator alloc(16U << 20U);

    // 1. Load + compile the AUTHORED rt_witness.ckir → HLSL → DXIL (cs_6_5).
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
    const crd::containers::Array<crd::u8> dxil = dxil_of(kg, ke, &alloc); // emit HLSL + compile_hlsl_to_dxil (cs_6_5)
    REQUIRE(dxil.size() > 0U);

    // 2. The trivial scene: 1 axis-aligned tri @ z=2; 64 rays (one workgroup — the witness has no tail guard).
    constexpr crd::u32 num_tris = 1U;
    constexpr crd::u32 nrays    = 64U;
    const float        verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    crd::containers::Array<float> rays(&alloc);
    rays.resize(static_cast<crd::usize>(nrays) * 6U, 0.0F);
    const float seed[4][6] = {
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 1.0F},  // hit @ t=2, prim 0
        {0.2F, 0.2F, 0.0F, 0.0F, 0.0F, -1.0F}, // miss
        {5.0F, 5.0F, 0.0F, 0.0F, 0.0F, 1.0F},  // miss
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
            rays[r * 6U + 2U] = 100.0F;
            rays[r * 6U + 5U] = 1.0F;
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
    ceir::Value* const g1      = mkidx(1);
    ceir::Value*       rq_ops[7] = {g1, g1, g1, tlas->result(0U), rays_v, hit_t_v, hit_p_v};
    ceir::Operation* const rq   = c.create_operation(c.intern_op("rt", "ray_query"),
                                                     crd::containers::ConstSpan<ceir::Value*>(rq_ops, 7U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("rt_witness")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rww")));
    body->append(rq);

    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    // 4. Drive the lowered list through execute_rt_lowered's hooks onto the real DX12 RT context.
    RtGateStateDx st;
    st.rt        = &rt;
    st.verts     = verts;
    st.ntris     = num_tris;
    st.dxil      = dxil.data();
    st.dxil_size = dxil.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_dx_build_scene;
    hooks.kernel_bytes   = &rt_gate_dx_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_dx_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[3];
    binds[0] = {c.resource_root(rays_v), rays.data(), nullptr, static_cast<crd::u64>(nrays) * 6U * sizeof(float)};
    binds[1] = {c.resource_root(hit_t_v), nullptr, gpu_t.data(), static_cast<crd::u64>(nrays) * sizeof(float)};
    binds[2] = {c.resource_root(hit_p_v), nullptr, gpu_prim.data(), static_cast<crd::u64>(nrays) * sizeof(crd::u32)};
    const cg::ExecuteError err =
        cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                               crd::containers::ConstSpan<cg::RtHostBinding>(binds, 3U));
    REQUIRE(err == cg::ExecuteError::None);

    // 5. The oracle: eval_cpu_kernel on the SAME loaded graph, AS-slot = triangle soup [ntri, v0..v2].
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
        CHECK(gpu_hit == cpu_hit);
        CHECK(gpu_prim[r] == static_cast<crd::u32>(ref_p[r]));
        if (cpu_hit) { CHECK(crd::math::abs(static_cast<double>(gpu_t[r]) - ref_t[r]) < 1.0e-3); ++checked_hits; }
    }
    CHECK(checked_hits >= 2U);

    // 7. ANALYTIC pins (MANDATORY — parity alone is blind to a symmetric authoring error; the cf(node) scar).
    CHECK(crd::math::abs(static_cast<double>(gpu_t[0]) - 2.0) < 1.0e-3);
    CHECK(gpu_prim[0] == 0U);
    CHECK(gpu_prim[1] == 0xFFFFFFFFU);
    CHECK(gpu_prim[2] == 0xFFFFFFFFU);
    CHECK(crd::math::abs(static_cast<double>(gpu_t[3]) - 1.0) < 1.0e-3);
    CHECK(gpu_prim[3] == 0U);
}

// ── CEIR-19c STAGE 2 (DX12 mirror): the 3 wavefront kernels + the full host-loop, dispatched through execute_rt_lowered over
// the Dx12RayTracingContext. Structurally identical to the Vulkan [ceir19c] gates (same scenes/rays/oracle/analytic); only the
// RT surface + the HLSL->DXIL compile differ. Establishes VK≈DX12 for the whole wavefront.
TEST_CASE("CEIR-19c DX12: the authored serial-compact kernel compacts hit-flags == oracle + analytic",
          "[gpu-context][dx12][gpu][rt][ceir19c]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }
    crd::memory::TlsfAllocator alloc(16U << 20U);
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
    const crd::containers::Array<crd::u8> dxil = dxil_of(kg, ke, &alloc);
    REQUIRE(dxil.size() > 0U);

    constexpr crd::u32               k_n        = 8U;
    const crd::u32                   flags[k_n] = {1U, 0U, 1U, 1U, 0U, 0U, 1U, 0U};
    crd::containers::Array<crd::u32> gpu_comp(&alloc);
    crd::containers::Array<crd::u32> gpu_count(&alloc);
    gpu_comp.resize(k_n, 0xEEEEEEEEU);
    gpu_count.resize(1U, 0U);

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
    ceir::Value* const flags_v   = mkbuf();
    ceir::Value* const comp_v    = mkbuf();
    ceir::Value* const count_v   = mkbuf();
    ceir::Value* const g1        = mkidx(1);
    ceir::Value*       rq_ops[7] = {g1, g1, g1, tlas->result(0U), flags_v, comp_v, count_v};
    ceir::Operation* const rq =
        c.create_operation(c.intern_op("rt", "ray_query"), crd::containers::ConstSpan<ceir::Value*>(rq_ops, 7U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("wavefront_compact")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rww")));
    body->append(rq);
    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    const float   verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
    RtGateStateDx st;
    st.rt        = &rt;
    st.verts     = verts;
    st.ntris     = 1U;
    st.dxil      = dxil.data();
    st.dxil_size = dxil.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_dx_build_scene;
    hooks.kernel_bytes   = &rt_gate_dx_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_dx_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[3];
    binds[0] = {c.resource_root(flags_v), &flags[0], nullptr, static_cast<crd::u64>(k_n) * sizeof(crd::u32)};
    binds[1] = {c.resource_root(comp_v), nullptr, gpu_comp.data(), static_cast<crd::u64>(k_n) * sizeof(crd::u32)};
    binds[2] = {c.resource_root(count_v), nullptr, gpu_count.data(), sizeof(crd::u32)};
    REQUIRE(cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                                   crd::containers::ConstSpan<cg::RtHostBinding>(binds, 3U))
            == cg::ExecuteError::None);

    crd::containers::Array<crd::f64> geo(&alloc);
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
    const crd::u32 expect_comp[4] = {0U, 2U, 3U, 6U};
    CHECK(gpu_count[0] == 4U);
    CHECK(static_cast<crd::u32>(count_ref[0]) == 4U);
    for (crd::u32 j = 0; j < 4U; ++j)
    {
        CHECK(gpu_comp[j] == expect_comp[j]);
        CHECK(gpu_comp[j] == static_cast<crd::u32>(comp_ref[j]));
    }
}

TEST_CASE("CEIR-19c DX12: the authored wavefront trace kernel writes hit-flags == oracle + analytic",
          "[gpu-context][dx12][gpu][rt][ceir19c]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }
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
    const crd::containers::Array<crd::u8> dxil = dxil_of(kg, ke, &alloc);
    REQUIRE(dxil.size() > 0U);

    constexpr crd::u32            num_tris = 1U;
    constexpr crd::u32            nrays    = 64U;
    const float                   verts[9] = {0.0F, 0.0F, 2.0F, 1.0F, 0.0F, 2.0F, 0.0F, 1.0F, 2.0F};
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
    ceir::Value* const rays_v    = mkbuf();
    ceir::Value* const flag_v    = mkbuf();
    ceir::Value* const t_v       = mkbuf();
    ceir::Value* const g1        = mkidx(1);
    ceir::Value*       rq_ops[7] = {g1, g1, g1, tlas->result(0U), rays_v, flag_v, t_v};
    ceir::Operation* const rq =
        c.create_operation(c.intern_op("rt", "ray_query"), crd::containers::ConstSpan<ceir::Value*>(rq_ops, 7U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("wavefront_trace")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rww")));
    body->append(rq);
    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    RtGateStateDx st;
    st.rt        = &rt;
    st.verts     = verts;
    st.ntris     = num_tris;
    st.dxil      = dxil.data();
    st.dxil_size = dxil.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_dx_build_scene;
    hooks.kernel_bytes   = &rt_gate_dx_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_dx_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[3];
    binds[0] = {c.resource_root(rays_v), rays.data(), nullptr, static_cast<crd::u64>(nrays) * 6U * sizeof(float)};
    binds[1] = {c.resource_root(flag_v), nullptr, gpu_flag.data(), static_cast<crd::u64>(nrays) * sizeof(crd::u32)};
    binds[2] = {c.resource_root(t_v), nullptr, gpu_t.data(), static_cast<crd::u64>(nrays) * sizeof(float)};
    REQUIRE(cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                                   crd::containers::ConstSpan<cg::RtHostBinding>(binds, 3U))
            == cg::ExecuteError::None);

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
    for (crd::u32 r = 0; r < nrays; ++r)
    {
        CHECK(gpu_flag[r] == static_cast<crd::u32>(flag_ref[r]));
        CHECK((gpu_flag[r] == 0U || gpu_flag[r] == 1U));
    }
    CHECK(gpu_flag[0] == 1U);
    CHECK(gpu_flag[1] == 0U);
    CHECK(gpu_flag[2] == 0U);
    CHECK(gpu_flag[3] == 1U);
    CHECK(crd::math::abs(static_cast<double>(gpu_t[0]) - 2.0) < 1.0e-3);
    CHECK(crd::math::abs(static_cast<double>(gpu_t[3]) - 1.0) < 1.0e-3);
}

TEST_CASE("CEIR-19c DX12: the authored wavefront shade kernel writes lit/shadowed == oracle + analytic",
          "[gpu-context][dx12][gpu][rt][ceir19c]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }
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
    const crd::containers::Array<crd::u8> dxil = dxil_of(kg, ke, &alloc);
    REQUIRE(dxil.size() > 0U);

    const float                      verts[9]     = {-1.0F, 4.0F, -1.0F, 1.0F, 4.0F, -1.0F, 0.0F, 4.0F, 1.0F};
    const crd::u32                   compacted[2] = {0U, 1U};
    const float                      rays_in[12]  = {0.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F, 3.0F, 10.0F, 0.0F, 0.0F, -1.0F, 0.0F};
    const float                      hitt_in[2]   = {10.0F, 10.0F};
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
    ceir::Value* const comp_v    = mkbuf();
    ceir::Value* const rays_v    = mkbuf();
    ceir::Value* const hitt_v    = mkbuf();
    ceir::Value* const dec_v     = mkbuf();
    ceir::Value* const g2        = mkidx(2);
    ceir::Value* const g1        = mkidx(1);
    ceir::Value*       rq_ops[8] = {g2, g1, g1, tlas->result(0U), comp_v, rays_v, hitt_v, dec_v};
    ceir::Operation* const rq =
        c.create_operation(c.intern_op("rt", "ray_query"), crd::containers::ConstSpan<ceir::Value*>(rq_ops, 8U), 0U, ceir::TypeId{}, 0U);
    c.set_attr(rq, "kernel", c.attr_symbol(crd::containers::StringView("wavefront_shade")));
    c.set_attr(rq, "access", c.attr_string(crd::containers::StringView("rrrw")));
    body->append(rq);
    crd::containers::Array<cg::LoweredCommand> cmds(&alloc);
    cg::lower_region(c, *body, cmds);

    RtGateStateDx st;
    st.rt        = &rt;
    st.verts     = verts;
    st.ntris     = 1U;
    st.dxil      = dxil.data();
    st.dxil_size = dxil.size();
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_dx_build_scene;
    hooks.kernel_bytes   = &rt_gate_dx_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_dx_trace_dispatch;
    hooks.user           = &st;
    cg::RtHostBinding binds[4];
    binds[0] = {c.resource_root(comp_v), &compacted[0], nullptr, 2U * sizeof(crd::u32)};
    binds[1] = {c.resource_root(rays_v), &rays_in[0], nullptr, 12U * sizeof(float)};
    binds[2] = {c.resource_root(hitt_v), &hitt_in[0], nullptr, 2U * sizeof(float)};
    binds[3] = {c.resource_root(dec_v), nullptr, gpu_dec.data(), 2U * sizeof(crd::u32)};
    REQUIRE(cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                                   crd::containers::ConstSpan<cg::RtHostBinding>(binds, 4U))
            == cg::ExecuteError::None);

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
    kir::eval_cpu_kernel(kg, ke, bufs, 5, 1U, &alloc, 2U);
    CHECK(gpu_dec[0] == static_cast<crd::u32>(dec_ref[0]));
    CHECK(gpu_dec[1] == static_cast<crd::u32>(dec_ref[1]));
    CHECK(gpu_dec[0] == 0U);
    CHECK(gpu_dec[1] == 1U);
}

// ── CEIR-19c STAGE 2 (DX12): the §134 WAVEFRONT HOST-LOOP — the DX12 twin of the Vulkan culmination. Same host loop, scene,
// oracle mirror, per-iter triple32 decision-hash, and termination-via-readback; only the RT surface + DXIL compile differ.
TEST_CASE("CEIR-19c DX12: the AUTHORED wavefront host-loop (trace->compact->shade) == oracle + analytic",
          "[gpu-context][dx12][gpu][rt][ceir19c]")
{
    gpu::Dx12RayTracingContext rt;
    if (!rt.valid()) { WARN("no D3D12 DXR-1.1 device available; skipping"); return; }
    crd::memory::TlsfAllocator alloc(64U << 20U);

    kir::KGraph                     kg_tr(&alloc);
    kir::KGraph                     kg_co(&alloc);
    kir::KGraph                     kg_sh(&alloc);
    kir::KEntry                     ke_tr;
    kir::KEntry                     ke_co;
    kir::KEntry                     ke_sh;
    crd::containers::Array<crd::u8> dx_tr(&alloc);
    crd::containers::Array<crd::u8> dx_co(&alloc);
    crd::containers::Array<crd::u8> dx_sh(&alloc);
    const auto load_kernel = [&](const char* path, kir::KGraph& kg, kir::KEntry& ke, crd::containers::Array<crd::u8>& dx) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&alloc);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke).ok);
        dx = dxil_of(kg, ke, &alloc);
        REQUIRE(dx.size() > 0U);
    };
    load_kernel(CRD_REPO_DIR "/assets/ckir/wavefront_trace.ckir", kg_tr, ke_tr, dx_tr);
    load_kernel(CRD_REPO_DIR "/assets/ckir/wavefront_compact.ckir", kg_co, ke_co, dx_co);
    load_kernel(CRD_REPO_DIR "/assets/ckir/wavefront_shade.ckir", kg_sh, ke_sh, dx_sh);

    const float verts[27] = {-10.0F, 0.0F, -10.0F, 10.0F, 0.0F, -10.0F, 10.0F, 0.0F, 10.0F,
                             -10.0F, 0.0F, -10.0F, 10.0F, 0.0F, 10.0F,  -10.0F, 0.0F, 10.0F,
                             1.0F,   4.0F, -1.0F,  3.0F,  4.0F, -1.0F,  2.0F,   4.0F, 1.0F};
    constexpr crd::u32 num_tris    = 3U;
    constexpr crd::u32 nrays       = 8U;
    constexpr crd::u32 nbuf        = 64U;
    const float        seed[nrays * 6] = {4.0F,  20.0F, 0.0F, 0.0F, -1.0F, 0.0F, -8.0F, 20.0F, 0.0F, 0.0F, -1.0F, 0.0F,
                                          4.0F,  20.0F, 0.4F, 0.0F, -1.0F, 0.0F, -6.0F, 20.0F, 0.0F, 0.0F, -1.0F, 0.0F,
                                          15.0F, 20.0F, 0.0F, 0.0F, -1.0F, 0.0F, 16.0F, 20.0F, 0.0F, 0.0F, -1.0F, 0.0F,
                                          17.0F, 20.0F, 0.0F, 0.0F, -1.0F, 0.0F, 18.0F, 20.0F, 0.0F, 0.0F, -1.0F, 0.0F};
    float rays[nbuf * 6] = {};
    for (crd::u32 r = 0; r < nbuf; ++r)
    {
        if (r < nrays)
        {
            for (int col = 0; col < 6; ++col) { rays[r * 6U + static_cast<crd::u32>(col)] = seed[r * 6U + static_cast<crd::u32>(col)]; }
        }
        else
        {
            rays[r * 6U + 0U] = 100.0F;
            rays[r * 6U + 1U] = 100.0F;
            rays[r * 6U + 2U] = 100.0F;
            rays[r * 6U + 5U] = 1.0F;
        }
    }

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
    crd::containers::Array<cg::LoweredCommand> tr_cmds(&alloc);
    crd::containers::Array<cg::LoweredCommand> co_cmds(&alloc);
    const ceir::Value*                         tr_roots[3] = {nullptr, nullptr, nullptr};
    const ceir::Value*                         co_roots[3] = {nullptr, nullptr, nullptr};
    build_prog("wavefront_trace", "rww", 1, 3U, tr_cmds, tr_roots);
    build_prog("wavefront_compact", "rww", 1, 3U, co_cmds, co_roots);

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
    cont_flags.resize(nbuf, 0U);
    next_queue.resize(nbuf, 0xEEEEEEEEU);
    crd::u32 hit_count  = 0U;
    crd::u32 next_count = 0U;

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

    RtGateStateDx st;
    st.rt    = &rt;
    st.verts = verts;
    st.ntris = num_tris;
    cg::RtHooks hooks;
    hooks.build_scene    = &rt_gate_dx_build_scene;
    hooks.kernel_bytes   = &rt_gate_dx_kernel_bytes;
    hooks.trace_dispatch = &rt_gate_dx_trace_dispatch;
    hooks.user           = &st;
    const auto exec = [&](const crd::containers::Array<cg::LoweredCommand>& cmds, const crd::u8* dx, crd::usize dx_sz,
                          const cg::RtHostBinding* binds, crd::u32 nb) {
        st.dxil      = dx;
        st.dxil_size = dx_sz;
        return cg::execute_rt_lowered(c, crd::containers::ConstSpan<cg::LoweredCommand>(cmds.data(), cmds.size()), hooks,
                                      crd::containers::ConstSpan<cg::RtHostBinding>(binds, nb));
    };

    crd::u32 queue_count = nrays;
    int      iters       = 0;
    crd::u32 gpu_hash    = 0x9E3779B9U;
    crd::u32 oracle_hash = 0x9E3779B9U;
    while (queue_count > 0U)
    {
        const cg::RtHostBinding b_tr[3] = {{tr_roots[0], rays, nullptr, static_cast<crd::u64>(nbuf) * 6U * sizeof(float)},
                                           {tr_roots[1], nullptr, hit_flags.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {tr_roots[2], nullptr, hit_t.data(), static_cast<crd::u64>(nbuf) * sizeof(float)}};
        REQUIRE(exec(tr_cmds, dx_tr.data(), dx_tr.size(), b_tr, 3U) == cg::ExecuteError::None);
        const cg::RtHostBinding b_c1[3] = {{co_roots[0], hit_flags.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[1], nullptr, compacted.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[2], nullptr, &hit_count, sizeof(crd::u32)}};
        REQUIRE(exec(co_cmds, dx_co.data(), dx_co.size(), b_c1, 3U) == cg::ExecuteError::None);
        crd::containers::Array<cg::LoweredCommand> sh_cmds(&alloc);
        const ceir::Value*                         sh_roots[4] = {nullptr, nullptr, nullptr, nullptr};
        build_prog("wavefront_shade", "rrrw", static_cast<crd::i64>(hit_count), 4U, sh_cmds, sh_roots);
        st.last_gx                      = 0U;
        const cg::RtHostBinding b_sh[4] = {{sh_roots[0], compacted.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {sh_roots[1], rays, nullptr, static_cast<crd::u64>(nbuf) * 6U * sizeof(float)},
                                           {sh_roots[2], hit_t.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(float)},
                                           {sh_roots[3], nullptr, decisions.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)}};
        REQUIRE(exec(sh_cmds, dx_sh.data(), dx_sh.size(), b_sh, 4U) == cg::ExecuteError::None);
        CHECK(st.last_gx == hit_count);
        const cg::RtHostBinding b_c2[3] = {{co_roots[0], cont_flags.data(), nullptr, static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[1], nullptr, next_queue.data(), static_cast<crd::u64>(nbuf) * sizeof(crd::u32)},
                                           {co_roots[2], nullptr, &next_count, sizeof(crd::u32)}};
        REQUIRE(exec(co_cmds, dx_co.data(), dx_co.size(), b_c2, 3U) == cg::ExecuteError::None);

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

        gpu_hash    = rt_fold(gpu_hash, hit_count);
        oracle_hash = rt_fold(oracle_hash, ohc);
        for (crd::u32 j = 0; j < hit_count; ++j) { gpu_hash = rt_fold(gpu_hash, compacted[j]); }
        for (crd::u32 j = 0; j < ohc; ++j) { oracle_hash = rt_fold(oracle_hash, static_cast<crd::u32>(comp_ref[j])); }
        for (crd::u32 j = 0; j < hit_count; ++j) { gpu_hash = rt_fold(gpu_hash, decisions[j]); }
        for (crd::u32 j = 0; j < ohc; ++j) { oracle_hash = rt_fold(oracle_hash, static_cast<crd::u32>(dec_ref[j])); }
        gpu_hash    = rt_fold(gpu_hash, next_count);
        oracle_hash = rt_fold(oracle_hash, static_cast<crd::u32>(nc_ref[0]));
        CHECK(gpu_hash == oracle_hash);

        CHECK(hit_count == 4U);
        CHECK(ohc == 4U);
        for (crd::u32 j = 0; j < 4U; ++j)
        {
            CHECK(compacted[j] == static_cast<crd::u32>(comp_ref[j]));
            CHECK(decisions[j] == static_cast<crd::u32>(dec_ref[j]));
        }
        CHECK(decisions[0] == 0U);
        CHECK(decisions[1] == 1U);
        CHECK(decisions[2] == 0U);
        CHECK(decisions[3] == 1U);

        CHECK(next_count == 0U); // ⭐ single-bounce: the compact#2 readback IS zero (termination value PINNED, not inferred)
        queue_count = next_count;
        ++iters;
    }
    CHECK(iters == 1);
    CHECK(queue_count == 0U);
}
