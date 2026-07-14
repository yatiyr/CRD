// test_ckir_atmosphere.cpp — D-007 B15-a: the physically-based atmosphere LUTs (ckir_atmosphere.hpp) on the CPU oracle. B15-a-1
// verifies the TRANSMITTANCE LUT — the fraction of sunlight surviving the path from an altitude/direction out to space. The
// checks are PHYSICAL, not just self-consistent: (1) T ∈ (0,1] everywhere; (2) at the ground looking straight up the whole-
// atmosphere path attenuates measurably AND blue is attenuated MORE than red (Rayleigh ∝ 1/λ⁴ — the reason the sky is blue and
// sunsets are red); (3) a grazing horizon path (much longer) transmits far less than straight up; (4) high in thin air the path
// is negligible so T → 1. Portability (GPU == oracle) rides in test_vulkan_context.

#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_atmosphere.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
crd::usize uz(int v) { return static_cast<crd::usize>(v); }
} // namespace

TEST_CASE("B15-a transmittance LUT: whole-atmosphere extinction with the Rayleigh blue>green>red ordering", "[kir][atmos]")
{
    crd::memory::TlsfAllocator   alloc(64U << 20U);
    kir::atmos::AtmosphereConfig cfg;
    cfg.tlut_w = 64; // the CPU oracle is a recursive interpreter (O(texels·steps·nodes)); a small LUT keeps the test fast —
    cfg.tlut_h = 16; // the extinction physics is per-texel identical at any resolution (production runs at the 256×64 default).
    const int                    w = cfg.tlut_w;
    const int                    ht = cfg.tlut_h;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::atmos::build_atmos_transmittance(g, cfg);

    crd::containers::Array<crd::f64> out(&alloc);
    out.resize(uz(w * ht * 3));
    kir::KernelBuffer bufs[1] = {{out.data(), w * ht * 3, 0, 0}};
    kir::eval_cpu_kernel(g, e, bufs, 1, e.local_size[0], &alloc, static_cast<crd::u32>(w * ht / 64));

    // texel (ix,iy): iy = altitude axis (0 = ground r=Rg, H−1 = top r=Rt); ix = μ axis (0 = straight up μ=1, W−1 = horizon μ=0).
    const auto tx = [&](int ix, int iy, int c) { return out[uz((iy * w + ix) * 3 + c)]; };

    // (1) every texel is a physical transmittance ∈ (0, 1].
    double lo = 2.0;
    double hi = -1.0;
    for (int i = 0; i < w * ht * 3; ++i) { lo = out[uz(i)] < lo ? out[uz(i)] : lo; hi = out[uz(i)] > hi ? out[uz(i)] : hi; }
    CHECK(lo > 0.0);
    CHECK(hi <= 1.0 + 1e-6);

    // (2) GROUND, straight up (ix=0, iy=0): the full ~100 km column attenuates, and blue is hit hardest (β_R,b ≈ 5.7·β_R,r).
    const double gr = tx(0, 0, 0);
    const double gg = tx(0, 0, 1);
    const double gb = tx(0, 0, 2);
    CHECK(gr < 1.0);                // measurable extinction
    CHECK(gr > gg);                 // red survives more than green
    CHECK(gg > gb);                 // green survives more than blue — the Rayleigh 1/λ⁴ ordering (blue sky / red sunset)
    CHECK(gr > 0.88);               // analytic ≈ 0.94 (τ_r ≈ 0.062)
    CHECK(gr < 0.97);
    CHECK(gb > 0.70);               // analytic ≈ 0.76 (τ_b ≈ 0.271)
    CHECK(gb < 0.82);

    // (3) GROUND, grazing the horizon (ix=W−1, μ→0): a vastly longer path ⇒ far less transmitted than straight up.
    CHECK(tx(w - 1, 0, 0) < gr);
    CHECK(tx(w - 1, 0, 2) < gb * 0.5); // blue at the horizon is heavily extinguished

    // (4) TOP of the atmosphere, straight up (ix=0, iy=H−1): negligible remaining path ⇒ T → 1.
    CHECK(tx(0, ht - 1, 0) > 0.999);
    CHECK(tx(0, ht - 1, 2) > 0.999);
}

TEST_CASE("B15-a multiple-scattering LUT: positive, finite bluish fill that grows toward the ground and the sunlit sky", "[kir][atmos]")
{
    crd::memory::TlsfAllocator   alloc(128U << 20U);
    kir::atmos::AtmosphereConfig cfg;
    cfg.tlut_w   = 64; // small LUTs keep the recursive CPU-oracle reference fast; physics is resolution-independent.
    cfg.tlut_h   = 16;
    cfg.mslut_res = 16;
    const int                    tw  = cfg.tlut_w;
    const int                    th  = cfg.tlut_h;
    const int                    res = cfg.mslut_res;

    // 1) the transmittance LUT (the multiscatter kernel samples it for the sun-transmittance at each march step).
    kir::KGraph       gt(&alloc);
    const kir::KEntry et = kir::atmos::build_atmos_transmittance(gt, cfg);
    crd::containers::Array<crd::f64> tlut(&alloc);
    tlut.resize(uz(tw * th * 3));
    kir::KernelBuffer tb[1] = {{tlut.data(), tw * th * 3, 0, 0}};
    kir::eval_cpu_kernel(gt, et, tb, 1, et.local_size[0], &alloc, static_cast<crd::u32>(tw * th / 64));

    // 2) the multiple-scattering LUT.
    kir::KGraph       gm(&alloc);
    const kir::KEntry em = kir::atmos::build_atmos_multiscatter(gm, cfg);
    crd::containers::Array<crd::f64> ms(&alloc);
    ms.resize(uz(res * res * 3));
    kir::KernelBuffer mb[2] = {{tlut.data(), tw * th * 3, 0, 0}, {ms.data(), res * res * 3, 0, 1}};
    kir::eval_cpu_kernel(gm, em, mb, 2, em.local_size[0], &alloc, static_cast<crd::u32>(res * res / 64));

    // texel (ix,iy): ix = μ_sun axis (0 = sun below horizon, res−1 = sun overhead); iy = altitude (0 = ground, res−1 = top).
    const auto mx = [&](int ix, int iy, int c) { return ms[uz((iy * res + ix) * 3 + c)]; };

    // (1) the multiscatter fill is a POSITIVE, FINITE radiance everywhere (the geometric series converged: f_ms < 1).
    double lo = 1e30;
    double hi = -1e30;
    for (int i = 0; i < res * res * 3; ++i) { lo = ms[uz(i)] < lo ? ms[uz(i)] : lo; hi = ms[uz(i)] > hi ? ms[uz(i)] : hi; }
    CHECK(lo >= 0.0);
    CHECK(hi < 10.0); // bounded — no runaway from the 1/(1−f_ms) series

    // (2) with the sun overhead (ix=res−1), the ground has MORE air below it to multiply-scatter than the top ⇒ larger fill.
    CHECK(mx(res - 1, 0, 2) > mx(res - 1, res - 1, 2));

    // (3) the multiscatter fill is BLUISH — Rayleigh (∝1/λ⁴) dominates multiple scattering, so blue > red at the sunlit ground.
    CHECK(mx(res - 1, 0, 2) > mx(res - 1, 0, 0));

    // (4) more sunlight enters with the sun UP than DOWN ⇒ a brighter multiscatter fill overhead than when the sun is below.
    CHECK(mx(res - 1, 0, 2) > mx(0, 0, 2));
}

TEST_CASE("B15-a sky-view LUT: a blue zenith, a sun-side glow, and a bright horizon (single + multiple scattering)", "[kir][atmos]")
{
    crd::memory::TlsfAllocator   alloc(256U << 20U);
    kir::atmos::AtmosphereConfig cfg;
    cfg.tlut_w    = 64; // small input LUTs (the recursive CPU-oracle reference is O(texels·steps·nodes)) — the kernel logic +
    cfg.tlut_h    = 16; // portability are per-texel identical at any resolution; production runs the full-size defaults.
    cfg.mslut_res = 16;
    cfg.skyview_w = 32;
    cfg.skyview_h = 16;
    cfg.sky_steps = 12;
    const int                    tw  = cfg.tlut_w;
    const int                    th  = cfg.tlut_h;
    const int                    res = cfg.mslut_res;
    const int                    sw  = cfg.skyview_w;
    const int                    sh  = cfg.skyview_h;

    // build the two input LUTs first.
    kir::KGraph       gt(&alloc);
    const kir::KEntry et = kir::atmos::build_atmos_transmittance(gt, cfg);
    crd::containers::Array<crd::f64> tlut(&alloc);
    tlut.resize(uz(tw * th * 3));
    kir::KernelBuffer tb[1] = {{tlut.data(), tw * th * 3, 0, 0}};
    kir::eval_cpu_kernel(gt, et, tb, 1, et.local_size[0], &alloc, static_cast<crd::u32>(tw * th / 64));

    kir::KGraph       gm(&alloc);
    const kir::KEntry em = kir::atmos::build_atmos_multiscatter(gm, cfg);
    crd::containers::Array<crd::f64> ms(&alloc);
    ms.resize(uz(res * res * 3));
    kir::KernelBuffer mb[2] = {{tlut.data(), tw * th * 3, 0, 0}, {ms.data(), res * res * 3, 0, 1}};
    kir::eval_cpu_kernel(gm, em, mb, 2, em.local_size[0], &alloc, static_cast<crd::u32>(res * res / 64));

    // the sky-view LUT.
    kir::KGraph       gs(&alloc);
    const kir::KEntry es = kir::atmos::build_atmos_skyview(gs, cfg);
    crd::containers::Array<crd::f64> sv(&alloc);
    sv.resize(uz(sw * sh * 3));
    kir::KernelBuffer sb[3] = {{tlut.data(), tw * th * 3, 0, 0}, {ms.data(), res * res * 3, 0, 1}, {sv.data(), sw * sh * 3, 0, 2}};
    kir::eval_cpu_kernel(gs, es, sb, 3, es.local_size[0], &alloc, static_cast<crd::u32>(sw * sh / 64));

    // texel (ix,iy): ix = azimuth relative to the sun (ix=sw/2 ≈ toward the sun, ix=0 ≈ away); iy = view zenith (0 ≈ straight up).
    const auto sx  = [&](int ix, int iy, int c) { return sv[uz((iy * sw + ix) * 3 + c)]; };
    const auto lum = [&](int ix, int iy) { return sx(ix, iy, 0) + sx(ix, iy, 1) + sx(ix, iy, 2); };
    const int  isun = sw / 2; // azimuth toward the sun (φ≈0)
    const int  iaway = 0;     // azimuth away from the sun (φ≈−π)
    const int  izen = 0;      // straight up
    const int  ihor = sh / 2; // ~horizon
    const int  isunz = static_cast<int>(0.386 * static_cast<double>(sh)); // the sun's zenith band (θ ≈ acos(0.35) ≈ 69.5°)

    // (1) the sky radiance is non-negative and finite everywhere.
    double lo = 1e30;
    double hi = -1e30;
    for (int i = 0; i < sw * sh * 3; ++i) { lo = sv[uz(i)] < lo ? sv[uz(i)] : lo; hi = sv[uz(i)] > hi ? sv[uz(i)] : hi; }
    CHECK(lo >= 0.0);
    CHECK(hi < 100.0);

    // (2) the ZENITH is BLUE — Rayleigh scatters blue most, so looking straight up blue dominates red.
    CHECK(sx(isun, izen, 2) > sx(isun, izen, 0));

    // (3) the sky GLOWS toward the sun — Mie forward scattering + the phase peak make the sun-side brighter than the opposite.
    CHECK(lum(isun, isunz) > lum(iaway, isunz));

    // (4) the HORIZON is brighter than the ZENITH — the long grazing path scatters far more in-scattered light.
    CHECK(lum(isun, ihor) > lum(isun, izen));
}
