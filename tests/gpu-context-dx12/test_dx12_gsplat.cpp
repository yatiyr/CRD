// test_dx12_gsplat.cpp — D-007 B19 CROSS-BACKEND: the Gaussian-splatting kernel family lowers to HLSL and runs on
// DirectX 12, bit-comparably to the CPU oracle (and thus to Vulkan). Representative set covering the distinctive
// constructs: the 2DGS ray-surfel render (Cramer solve + For composite), the relightable PBR render (GGX), and the
// differentiable backward (the training gradient reduction). If any HLSL emitter lags GLSL, these catch it.

#include <crd/gpu/dx12_compute_context.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_gsplat.hpp>
#include <crd/kir/ckir_gsplat2d.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <ckir_kernel_dispatch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace g   = crd::gpu;
namespace kir = crd::kir;

namespace
{
using uzt = crd::usize;
[[nodiscard]] uzt uz(int v) { return static_cast<uzt>(v); }

// pack one 2DGS surfel (13 floats), project it via the CPU oracle into the 19-float prepared form.
void oracle_project(crd::memory::TlsfAllocator& alloc, crd::containers::Array<double>& surf, int ns,
                    crd::containers::Array<double>& cam, crd::containers::Array<double>& prep)
{
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    kir::KGraph                        pg(&alloc);
    const kir::KEntry                  pe = kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg);
    prep.resize(uz(ns) * 19U, 0.0);
    kir::KernelBuffer pb[3] = {{surf.data(), ns * 13, 0, 0}, {cam.data(), 20, 0, 1}, {prep.data(), ns * 19, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);
}
} // namespace

TEST_CASE("D-007 B19 cross-backend: 2DGS render + relight DISPATCH on DX12 == CPU oracle", "[dx12][compute][gpu][gsplat2d]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    constexpr int imw = 32;
    constexpr int imh = 32;
    constexpr int ns  = 12;

    // scene (doubles for the oracle)
    crd::containers::Array<double> surf(&alloc);
    crd::containers::Array<double> cam(&alloc);
    surf.resize(uz(ns) * 13U, 0.0);
    crd::u32 st = 0xDC12U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < ns; ++i)
    {
        double* q = surf.data() + uz(i) * 13U;
        q[0] = (rnd() * 2.0 - 1.0) * 0.6; q[1] = (rnd() * 2.0 - 1.0) * 0.6; q[2] = -0.5 + rnd();
        q[3] = 0.3 + rnd() * 0.3; q[4] = 0.3 + rnd() * 0.3;
        const double a1 = rnd() * 6.2831853; const double a2 = rnd() * 6.2831853; const double u1 = rnd();
        q[5] = crd::math::sqrt(1.0 - u1) * crd::math::sin(a1); q[6] = crd::math::sqrt(1.0 - u1) * crd::math::cos(a1);
        q[7] = crd::math::sqrt(u1) * crd::math::sin(a2); q[8] = crd::math::sqrt(u1) * crd::math::cos(a2);
        q[9] = 0.85; q[10] = rnd(); q[11] = rnd(); q[12] = rnd();
    }
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0; cam[11] = 5.0;
    cam[12] = 50.0; cam[13] = 50.0; cam[14] = 16.0; cam[15] = 16.0;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    crd::containers::Array<double> prep(&alloc);
    oracle_project(alloc, surf, ns, cam, prep);
    // (a single-workgroup scene ⇒ centre-depth order irrelevant; feed prep directly)

    crd::containers::Array<float> prep32(&alloc);
    crd::containers::Array<float> cam32(&alloc);
    prep32.resize(uz(ns) * 19U, 0.0F);
    cam32.resize(20U, 0.0F);
    for (int i = 0; i < ns * 19; ++i) { prep32[uz(i)] = static_cast<float>(prep[uz(i)]); }
    for (int i = 0; i < 20; ++i) { cam32[uz(i)] = static_cast<float>(cam[uz(i)]); }

    // ── base render ──
    kir::gsplat::Gsplat2dRenderConfig rc;
    rc.width = imw; rc.height = imh; rc.max_splats = ns;
    kir::KGraph rg(&alloc);
    const kir::KEntry re = kir::gsplat::build_gsplat2d_render_kernel(rg, rc);
    crd::containers::Array<double> rpar(&alloc);
    rpar.resize(5U, 0.0); rpar[0] = static_cast<double>(ns); rpar[4] = 1.0 / 255.0;
    crd::containers::Array<double> rref(&alloc);
    rref.resize(uz(imw * imh) * 8U, 0.0);
    kir::KernelBuffer rb[4] = {{prep.data(), ns * 19, 0, 0}, {cam.data(), 20, 0, 1}, {rpar.data(), 5, 0, 2}, {rref.data(), imw * imh * 8, 0, 3}};
    kir::eval_cpu_kernel(rg, re, rb, 4, re.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    kir::GlslKernel rk(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(rg, re, &alloc, rk));
    auto rpipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(rk.source), 4, 0U);
    REQUIRE(rpipe != nullptr);
    crd::containers::Array<float> rpar32(&alloc);
    crd::containers::Array<float> rimg32(&alloc);
    rpar32.resize(5U, 0.0F); rpar32[0] = static_cast<float>(ns); rpar32[4] = 1.0F / 255.0F;
    rimg32.resize(uz(imw * imh) * 8U, 0.0F);
    { float* hb[4] = {prep32.data(), cam32.data(), rpar32.data(), rimg32.data()}; const int ln[4] = {ns * 19, 20, 5, imw * imh * 8}; crd::kir_test::dispatch_kernel_1wg(ctx, *rpipe, hb, ln, 4, static_cast<crd::u32>((imw * imh + 63) / 64)); }
    float worst_r = 0.0F;
    for (int i = 0; i < imw * imh * 8; ++i) { const float d = crd::math::abs(rimg32[uz(i)] - static_cast<float>(rref[uz(i)])); if (d > worst_r) { worst_r = d; } }
    CHECK(worst_r < 2.0e-3F); // 2DGS render on DX12 == oracle

    // ── relight render ──
    kir::gsplat::Gsplat2dRelightConfig lc;
    lc.width = imw; lc.height = imh; lc.max_splats = ns;
    kir::KGraph lg(&alloc);
    const kir::KEntry le = kir::gsplat::build_gsplat2d_relight_render_kernel(lg, lc);
    crd::containers::Array<double> lpar(&alloc);
    lpar.resize(13U, 0.0); lpar[0] = static_cast<double>(ns);
    lpar[4] = 0.3; lpar[5] = 0.4; lpar[6] = -1.0; lpar[7] = 1.0; lpar[8] = 0.95; lpar[9] = 0.9; lpar[10] = 0.05; lpar[11] = 0.35; lpar[12] = 1.0 / 255.0;
    crd::containers::Array<double> lref(&alloc);
    lref.resize(uz(imw * imh) * 4U, 0.0);
    kir::KernelBuffer lb[4] = {{prep.data(), ns * 19, 0, 0}, {cam.data(), 20, 0, 1}, {lpar.data(), 13, 0, 2}, {lref.data(), imw * imh * 4, 0, 3}};
    kir::eval_cpu_kernel(lg, le, lb, 4, le.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    kir::GlslKernel lk(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(lg, le, &alloc, lk));
    auto lpipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(lk.source), 4, 0U);
    REQUIRE(lpipe != nullptr);
    crd::containers::Array<float> lpar32(&alloc);
    crd::containers::Array<float> limg32(&alloc);
    lpar32.resize(13U, 0.0F); for (int i = 0; i < 13; ++i) { lpar32[uz(i)] = static_cast<float>(lpar[uz(i)]); }
    limg32.resize(uz(imw * imh) * 4U, 0.0F);
    { float* hb[4] = {prep32.data(), cam32.data(), lpar32.data(), limg32.data()}; const int ln[4] = {ns * 19, 20, 13, imw * imh * 4}; crd::kir_test::dispatch_kernel_1wg(ctx, *lpipe, hb, ln, 4, static_cast<crd::u32>((imw * imh + 63) / 64)); }
    float worst_l = 0.0F;
    for (int i = 0; i < imw * imh * 4; ++i) { const float d = crd::math::abs(limg32[uz(i)] - static_cast<float>(lref[uz(i)])); if (d > worst_l) { worst_l = d; } }
    std::printf("[B19 DX12] 2DGS render worst %.3e; relight worst %.3e\n", worst_r, worst_l);
    CHECK(worst_l < 2.0e-3F); // relightable PBR render on DX12 == oracle
}

TEST_CASE("D-007 B19-f cross-backend: differentiable forward + backward DISPATCH on DX12 == CPU oracle", "[dx12][compute][gpu][gsplat]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    g::Dx12ComputeContext      ctx(&alloc);
    if (!ctx.valid()) { WARN("no D3D12 device available; skipping"); return; }

    kir::gsplat::GsplatDiffConfig cfg;
    cfg.width = 32; cfg.height = 32; cfg.local_size = 64;
    const int np = cfg.width * cfg.height;

    crd::containers::Array<double> tgtp(&alloc);
    tgtp.resize(5U, 0.0); tgtp[0] = 16.0; tgtp[1] = 16.0; tgtp[2] = 5.0; tgtp[3] = 0.9; tgtp[4] = 0.8;
    crd::containers::Array<double> params(&alloc);
    params.resize(5U, 0.0); params[0] = 14.0; params[1] = 18.0; params[2] = 4.0; params[3] = 0.6; params[4] = 0.55;

    // target + oracle grad
    kir::KGraph fg(&alloc);
    const kir::KEntry fe = kir::gsplat::build_gsplat_diff_forward_kernel(fg, cfg);
    crd::containers::Array<double> target(&alloc); target.resize(uz(np), 0.0);
    { kir::KernelBuffer fb[2] = {{tgtp.data(), 5, 0, 0}, {target.data(), np, 0, 1}}; kir::eval_cpu_kernel(fg, fe, fb, 2, fe.local_size[0], &alloc, static_cast<crd::u32>(np / cfg.local_size)); }
    kir::KGraph bg(&alloc);
    const kir::KEntry be = kir::gsplat::build_gsplat_diff_backward_kernel(bg, cfg);
    crd::containers::Array<double> gref(&alloc); gref.resize(5U, 0.0);
    { kir::KernelBuffer bb[3] = {{params.data(), 5, 0, 0}, {target.data(), np, 0, 1}, {gref.data(), 5, 0, 2}}; kir::eval_cpu_kernel(bg, be, bb, 3, be.local_size[0], &alloc, 1U); }

    // DX12 backward
    kir::GlslKernel bk(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(bg, be, &alloc, bk));
    auto bpipe = ctx.create_pipeline_from_hlsl(crd::containers::to_view(bk.source), 3, 0U);
    REQUIRE(bpipe != nullptr);
    crd::containers::Array<float> p32(&alloc); crd::containers::Array<float> t32(&alloc); crd::containers::Array<float> g32(&alloc);
    p32.resize(5U, 0.0F); for (int i = 0; i < 5; ++i) { p32[uz(i)] = static_cast<float>(params[uz(i)]); }
    t32.resize(uz(np), 0.0F); for (int i = 0; i < np; ++i) { t32[uz(i)] = static_cast<float>(target[uz(i)]); }
    g32.resize(5U, 0.0F);
    { float* hb[3] = {p32.data(), t32.data(), g32.data()}; const int ln[3] = {5, np, 5}; crd::kir_test::dispatch_kernel_1wg(ctx, *bpipe, hb, ln, 3, 1U); }

    float worst = 0.0F;
    for (int k = 0; k < 5; ++k) { const float rel = crd::math::abs(g32[uz(k)] - static_cast<float>(gref[uz(k)])) / (crd::math::abs(static_cast<float>(gref[uz(k)])) + 1.0e-4F); if (rel > worst) { worst = rel; } }
    std::printf("[B19-f DX12] training backward gradient worst rel = %.3e\n", worst);
    CHECK(worst < 1.0e-3F); // the training gradient on DX12 == oracle (Vulkan-matched)
}
