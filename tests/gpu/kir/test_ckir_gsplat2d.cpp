// test_ckir_gsplat2d.cpp — D-007 B19-c: 2D GAUSSIAN SPLATTING (surfels, Huang et al. SIGGRAPH 2024).
//
// The surfel primitive is a flat oriented disk; rendering is an EXACT ray–surfel intersection (a 3x3 solve per pixel),
// which gives view-consistent splats AND a geometrically-meaningful depth+normal per pixel. The project kernel is pinned
// against a closed-form tangent frame; the render is pinned against the hand-solved intersection of a known ray with a
// known disk (facing + slanted), plus the front-to-back composite.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_gsplat2d.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace kir = crd::kir;

namespace
{
// pack one surfel into the 13-float layout: [mux muy muz · su sv · qx qy qz qw · opacity · shR shG shB]
void put_surfel(crd::containers::Array<double>& g, int i, double mx, double my, double mz, double su, double sv,
                double qx, double qy, double qz, double qw, double opac, double shr, double shg, double shb)
{
    const crd::usize o = static_cast<crd::usize>(i) * 13U;
    g[o + 0U] = mx; g[o + 1U] = my; g[o + 2U] = mz;
    g[o + 3U] = su; g[o + 4U] = sv;
    g[o + 5U] = qx; g[o + 6U] = qy; g[o + 7U] = qz; g[o + 8U] = qw;
    g[o + 9U] = opac; g[o + 10U] = shr; g[o + 11U] = shg; g[o + 12U] = shb;
}
} // namespace

TEST_CASE("ckir 2dgs project matches closed-form tangent frame and view centre", "[ckir][gsplat2d]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "gsplat2d-proj");
    crd::containers::Array<double> surf(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> out(&alloc);

    constexpr int ns = 2;
    surf.resize(static_cast<crd::usize>(ns) * 13U, 0.0);
    // camera: R = identity, t = (0,0,5) ⇒ view = world + (0,0,5).
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 500.0; cam[13] = 500.0; cam[14] = 256.0; cam[15] = 256.0;
    cam[16] = 0.2; cam[17] = 512.0; cam[18] = 512.0;

    // surfel 0: identity rotation ⇒ t_u=(1,0,0), t_v=(0,1,0), normal=(0,0,1). su=0.3, sv=0.2.
    put_surfel(surf, 0, 0.0, 0.0, 0.0, 0.3, 0.2, 0, 0, 0, 1, 0.9, 0.0, 0.0, 0.0);
    // surfel 1: behind camera (view-z = -5) ⇒ invalid.
    put_surfel(surf, 1, 0.0, 0.0, -10.0, 0.3, 0.2, 0, 0, 0, 1, 0.9, 0.0, 0.0, 0.0);

    kir::gsplat::Gsplat2dProjectConfig cfg;
    cfg.count = static_cast<crd::u32>(ns); // REN-38: tail-thread guard
    kir::KGraph                        g(&alloc);
    const kir::KEntry                  e = kir::gsplat::build_gsplat2d_project_kernel(g, cfg);
    out.resize(static_cast<crd::usize>(ns) * 19U, 0.0);
    kir::KernelBuffer bb[3] = {{surf.data(), ns * 13, 0, 0}, {cam.data(), 20, 0, 1}, {out.data(), ns * 19, 0, 2}};
    kir::eval_cpu_kernel(g, e, bb, 3, e.local_size[0], &alloc, 1U);

    const auto get = [&](int i, int k) { return out[static_cast<crd::usize>(i) * 19U + static_cast<crd::usize>(k)]; };

    // surfel 0: view centre = (0,0,5); A = su·t_u = (0.3,0,0); B = sv·t_v = (0,0.2,0); N = (0,0,1); depth 5.
    CHECK(crd::math::abs(get(0, 0) - 0.0) < 1.0e-5);
    CHECK(crd::math::abs(get(0, 1) - 0.0) < 1.0e-5);
    CHECK(crd::math::abs(get(0, 2) - 5.0) < 1.0e-5);
    CHECK(crd::math::abs(get(0, 3) - 0.3) < 1.0e-5); // A.x
    CHECK(crd::math::abs(get(0, 4) - 0.0) < 1.0e-6); // A.y
    CHECK(crd::math::abs(get(0, 5) - 0.0) < 1.0e-6); // A.z
    CHECK(crd::math::abs(get(0, 6) - 0.0) < 1.0e-6); // B.x
    CHECK(crd::math::abs(get(0, 7) - 0.2) < 1.0e-5); // B.y
    CHECK(crd::math::abs(get(0, 8) - 0.0) < 1.0e-6); // B.z
    CHECK(crd::math::abs(get(0, 9) - 0.0) < 1.0e-6);  // N.x
    CHECK(crd::math::abs(get(0, 10) - 0.0) < 1.0e-6); // N.y
    CHECK(crd::math::abs(get(0, 11) - 1.0) < 1.0e-5); // N.z (faces +z)
    CHECK(crd::math::abs(get(0, 12) - 5.0) < 1.0e-5); // depth
    CHECK(crd::math::abs(get(0, 16) - 0.9) < 1.0e-6); // opacity
    CHECK(get(0, 18) > 0.5);                          // valid

    // surfel 1: invalid.
    CHECK(get(1, 18) < 0.5);
}

namespace
{
// project a scene of surfels, then render, returning the 8-float/pixel output. Depth-sorts on the host (nearest-first).
void project_and_render(crd::memory::TlsfAllocator& alloc, crd::containers::Array<double>& surf, int ns,
                        crd::containers::Array<double>& cam, int imw, int imh,
                        crd::containers::Array<double>& img)
{
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    pcfg.count = static_cast<crd::u32>(ns); // REN-38: tail-thread guard
    kir::KGraph                        pg(&alloc);
    const kir::KEntry                  pe = kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg);
    crd::containers::Array<double>     prep(&alloc);
    prep.resize(static_cast<crd::usize>(ns) * 19U, 0.0);
    kir::KernelBuffer pb[3] = {{surf.data(), ns * 13, 0, 0}, {cam.data(), 20, 0, 1}, {prep.data(), ns * 19, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    // depth sort (slot 12), nearest-first
    crd::containers::Array<int> ord(&alloc);
    ord.resize(static_cast<crd::usize>(ns), 0);
    for (int i = 0; i < ns; ++i) { ord[static_cast<crd::usize>(i)] = i; }
    for (int i = 1; i < ns; ++i)
    {
        const int    key = ord[static_cast<crd::usize>(i)];
        const double kd  = prep[static_cast<crd::usize>(key) * 19U + 12U];
        int          j   = i - 1;
        while (j >= 0 && prep[static_cast<crd::usize>(ord[static_cast<crd::usize>(j)]) * 19U + 12U] > kd)
        {
            const int jp1 = j + 1;
            ord[static_cast<crd::usize>(jp1)] = ord[static_cast<crd::usize>(j)];
            --j;
        }
        const int jp1 = j + 1;
        ord[static_cast<crd::usize>(jp1)] = key;
    }
    crd::containers::Array<double> sorted(&alloc);
    sorted.resize(static_cast<crd::usize>(ns) * 19U, 0.0);
    for (int i = 0; i < ns; ++i)
    {
        for (int k = 0; k < 19; ++k) { sorted[static_cast<crd::usize>(i) * 19U + static_cast<crd::usize>(k)] = prep[static_cast<crd::usize>(ord[static_cast<crd::usize>(i)]) * 19U + static_cast<crd::usize>(k)]; }
    }

    kir::gsplat::Gsplat2dRenderConfig rcfg;
    rcfg.width = imw; rcfg.height = imh; rcfg.max_splats = ns;
    kir::KGraph       rg(&alloc);
    const kir::KEntry re = kir::gsplat::build_gsplat2d_render_kernel(rg, rcfg);
    crd::containers::Array<double> par(&alloc);
    par.resize(5U, 0.0); par[0] = static_cast<double>(ns); par[1] = 0.0; par[2] = 0.0; par[3] = 0.0; par[4] = 1.0 / 255.0;
    img.resize(static_cast<crd::usize>(imw * imh) * 8U, 0.0);
    kir::KernelBuffer rb[4] = {{sorted.data(), ns * 19, 0, 0}, {cam.data(), 20, 0, 1}, {par.data(), 5, 0, 2}, {img.data(), imw * imh * 8, 0, 3}};
    kir::eval_cpu_kernel(rg, re, rb, 4, re.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / re.local_size[0]));
}
} // namespace

TEST_CASE("ckir 2dgs render: facing surfel gives closed-form depth and normal", "[ckir][gsplat2d]")
{
    crd::memory::TlsfAllocator     alloc(64U << 20U, nullptr, "gsplat2d-face");
    crd::containers::Array<double> surf(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> img(&alloc);

    constexpr int imw = 32;
    constexpr int imh = 32;
    const double  posl = 0.5 / kir::gsplat::detail::kShC0; // SH0 giving colour 1.0 in a channel

    surf.resize(13U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;                 // t = (0,0,5): a surfel at world 0 sits at view-z 5
    cam[12] = 50.0; cam[13] = 50.0; cam[14] = 16.5; cam[15] = 16.5;   // cx,cy aligned to pixel (16,16) centre; wide FOV
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    // a facing surfel (identity rotation, normal +z), red, opacity 0.99 — sized so its 3σ footprint clears the corners.
    put_surfel(surf, 0, 0.0, 0.0, 0.0, 0.6, 0.6, 0, 0, 0, 1, 0.99, posl, -posl, -posl);
    project_and_render(alloc, surf, 1, cam, imw, imh, img);

    const auto at = [&](int x, int y, int k) { return img[static_cast<crd::usize>(y * imw + x) * 8U + static_cast<crd::usize>(k)]; };
    // centre pixel (16,16): ray (0,0,1), intersection at the surfel centre, depth = view-z = 5.
    const double t_c   = at(16, 16, 3);
    const double dsum  = at(16, 16, 4);
    const double surface_depth = dsum / (1.0 - t_c);
    CHECK(crd::math::abs(surface_depth - 5.0) < 1.0e-3);
    // normal (alpha-weighted) points at +z.
    const double nz = at(16, 16, 7) / (1.0 - t_c);
    const double nx = at(16, 16, 5) / (1.0 - t_c);
    CHECK(crd::math::abs(nz - 1.0) < 1.0e-3);
    CHECK(crd::math::abs(nx) < 1.0e-3);
    // colour is red-dominant at the centre.
    CHECK(at(16, 16, 0) > 0.8);
    CHECK(at(16, 16, 0) > at(16, 16, 2) + 0.5);
    // a corner pixel is background (the surfel footprint is finite): transmittance approx 1.
    CHECK(at(0, 0, 3) > 0.99);
}

TEST_CASE("ckir 2dgs render: SLANTED surfel gives a per-pixel depth gradient (the geometry win)", "[ckir][gsplat2d]")
{
    crd::memory::TlsfAllocator     alloc(64U << 20U, nullptr, "gsplat2d-slant");
    crd::containers::Array<double> surf(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> img(&alloc);

    constexpr int imw = 32;
    constexpr int imh = 32;
    const double  fx  = 100.0;
    const double  cx  = 16.5;
    const double  ll  = 5.0;

    surf.resize(13U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = ll;
    cam[12] = fx; cam[13] = 100.0; cam[14] = cx; cam[15] = 16.5;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    // surfel rotated 45 deg about y: quaternion (0, sin22.5, 0, cos22.5), view normal (0.7071,0,0.7071).
    const double s225 = 0.3826834323650898;
    const double c225 = 0.9238795325112867;
    put_surfel(surf, 0, 0.0, 0.0, 0.0, 3.0, 3.0, 0.0, s225, 0.0, c225, 0.99, 0.0, 0.0, 0.0);
    project_and_render(alloc, surf, 1, cam, imw, imh, img);

    const auto at = [&](int x, int y, int k) { return img[static_cast<crd::usize>(y * imw + x) * 8U + static_cast<crd::usize>(k)]; };
    // exact plane depth: lambda = (vc.N)/(d.N), vc=(0,0,ll), N=(0.7071,0,0.7071), d=((px-cx)/fx, ., 1) => lambda = ll/(dx+1).
    const auto sd = [&](int x, int y) { const double t = at(x, y, 3); return at(x, y, 4) / (1.0 - t); };
    int checked = 0;
    for (int x = 12; x <= 20; ++x)
    {
        const double t = at(x, 16, 3);
        if (t > 0.9) { continue; } // only pixels the surfel actually covers
        const double dx  = (static_cast<double>(x) + 0.5 - cx) / fx;
        const double lam = ll / (dx + 1.0);
        CHECK(crd::math::abs(sd(x, 16) - lam) < 2.0e-2);
        ++checked;
    }
    CHECK(checked >= 5);       // several covered pixels validated against the exact plane
    // the depth genuinely varies across the surfel (a 3DGS ellipsoid gives a single flat depth): left is farther.
    CHECK(sd(13, 16) > sd(19, 16) + 0.1);
    // the normal is tilted: Nx approx 0.7071, not 0 (a facing surfel gives Nx=0).
    const double nx = at(16, 16, 5) / (1.0 - at(16, 16, 3));
    CHECK(crd::math::abs(nx - 0.7071) < 1.0e-2);
}

TEST_CASE("ckir 2dgs render: near surfel composites over far", "[ckir][gsplat2d]")
{
    crd::memory::TlsfAllocator     alloc(64U << 20U, nullptr, "gsplat2d-comp");
    crd::containers::Array<double> surf(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> img(&alloc);

    constexpr int imw = 32;
    constexpr int imh = 32;
    const double  posl = 0.5 / kir::gsplat::detail::kShC0;

    surf.resize(static_cast<crd::usize>(2) * 13U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 100.0; cam[13] = 100.0; cam[14] = 16.5; cam[15] = 16.5;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    // NEAR red (world z -2, view-z 3), FAR blue (world z +1, view-z 6), both facing, large, opaque.
    put_surfel(surf, 0, 0.0, 0.0, -2.0, 3.0, 3.0, 0, 0, 0, 1, 0.99, posl, -posl, -posl);
    put_surfel(surf, 1, 0.0, 0.0, 1.0, 3.0, 3.0, 0, 0, 0, 1, 0.99, -posl, -posl, posl);
    project_and_render(alloc, surf, 2, cam, imw, imh, img);

    const auto at = [&](int x, int y, int k) { return img[static_cast<crd::usize>(y * imw + x) * 8U + static_cast<crd::usize>(k)]; };
    // centre: near-red wins the composite.
    CHECK(at(16, 16, 0) > at(16, 16, 2) + 0.5);
    // and the resolved surface depth is the NEAR surface (approx 3), not the far one.
    const double sd = at(16, 16, 4) / (1.0 - at(16, 16, 3));
    CHECK(sd < 4.0);
}

namespace
{
// project 1 surfel, then relight-render with a directional light. Returns 4-float/pixel RGBA.
void project_and_relight(crd::memory::TlsfAllocator& alloc, crd::containers::Array<double>& surf, int ns,
                         crd::containers::Array<double>& cam, int imw, int imh, crd::containers::Array<double>& par,
                         crd::containers::Array<double>& img)
{
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    pcfg.count = static_cast<crd::u32>(ns); // REN-38: tail-thread guard
    kir::KGraph                        pg(&alloc);
    const kir::KEntry                  pe = kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg);
    crd::containers::Array<double>     prep(&alloc);
    prep.resize(static_cast<crd::usize>(ns) * 19U, 0.0);
    kir::KernelBuffer pb[3] = {{surf.data(), ns * 13, 0, 0}, {cam.data(), 20, 0, 1}, {prep.data(), ns * 19, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    kir::gsplat::Gsplat2dRelightConfig rcfg;
    rcfg.width = imw; rcfg.height = imh; rcfg.max_splats = ns;
    kir::KGraph       rg(&alloc);
    const kir::KEntry re = kir::gsplat::build_gsplat2d_relight_render_kernel(rg, rcfg);
    img.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KernelBuffer rb[4] = {{prep.data(), ns * 19, 0, 0}, {cam.data(), 20, 0, 1}, {par.data(), 13, 0, 2}, {img.data(), imw * imh * 4, 0, 3}};
    kir::eval_cpu_kernel(rg, re, rb, 4, re.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / re.local_size[0]));
}
} // namespace

TEST_CASE("ckir 2dgs RELIGHTABLE: a captured surfel responds to a moving directional light", "[ckir][gsplat2d]")
{
    crd::memory::TlsfAllocator     alloc(64U << 20U, nullptr, "gsplat2d-relight");
    crd::containers::Array<double> surf(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> img(&alloc);

    constexpr int imw = 32;
    constexpr int imh = 32;

    surf.resize(13U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 50.0; cam[13] = 50.0; cam[14] = 16.5; cam[15] = 16.5;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);
    // a facing surfel (normal +z), RED albedo, opaque
    put_surfel(surf, 0, 0.0, 0.0, 0.0, 0.6, 0.6, 0, 0, 0, 1, 0.99, 0.8, 0.2, 0.2);

    const auto at = [&](crd::containers::Array<double>& im, int x, int y, int c) { return im[static_cast<crd::usize>(y * imw + x) * 4U + static_cast<crd::usize>(c)]; };
    const auto par = [&](double lx, double ly, double lz, double amb, double rough) {
        crd::containers::Array<double> p(&alloc);
        p.resize(13U, 0.0);
        p[0] = 1.0; p[4] = lx; p[5] = ly; p[6] = lz; p[7] = 1.0; p[8] = 1.0; p[9] = 1.0; p[10] = amb; p[11] = rough; p[12] = 1.0 / 255.0;
        return p;
    };

    // (1) light from the camera side (view −z) ⇒ the front face is lit ⇒ bright, red-dominant.
    crd::containers::Array<double> plit = par(0.0, 0.0, -1.0, 0.05, 0.4);
    crd::containers::Array<double> lit(&alloc);
    project_and_relight(alloc, surf, 1, cam, imw, imh, plit, lit);
    const double lit_r = at(lit, 16, 16, 0);
    CHECK(lit_r > 0.6);                              // strongly lit
    CHECK(lit_r > at(lit, 16, 16, 2) + 0.1);         // red albedo shows through (the white specular narrows, not erases, the gap)

    // (2) MOVE the light behind the surface (view +z) ⇒ the front face is unlit ⇒ ambient only ⇒ dark.
    crd::containers::Array<double> pdark = par(0.0, 0.0, 1.0, 0.05, 0.4);
    crd::containers::Array<double> dark(&alloc);
    project_and_relight(alloc, surf, 1, cam, imw, imh, pdark, dark);
    CHECK(at(dark, 16, 16, 0) < 0.1);                // only ambient·albedo remains
    CHECK(lit_r > at(dark, 16, 16, 0) + 0.5);        // relighting genuinely changed the shading

    // (3) roughness controls the specular: a SHARP (low-roughness) surface has a brighter central highlight than a rough one.
    crd::containers::Array<double> psharp = par(0.0, 0.0, -1.0, 0.05, 0.08);
    crd::containers::Array<double> prough = par(0.0, 0.0, -1.0, 0.05, 0.9);
    crd::containers::Array<double> sharp(&alloc);
    crd::containers::Array<double> rough(&alloc);
    project_and_relight(alloc, surf, 1, cam, imw, imh, psharp, sharp);
    project_and_relight(alloc, surf, 1, cam, imw, imh, prough, rough);
    // the specular adds white energy to the green channel (albedo green is low), so a sharp highlight lifts G at the centre.
    CHECK(at(sharp, 16, 16, 1) > at(rough, 16, 16, 1) + 0.1);
}

TEST_CASE("ckir 2dgs StopThePop: per-pixel resort composites depth-crossing surfels in the RIGHT order", "[ckir][gsplat2d]")
{
    crd::memory::TlsfAllocator     alloc(96U << 20U, nullptr, "gsplat2d-resort");
    crd::containers::Array<double> surf(&alloc);
    crd::containers::Array<double> cam(&alloc);

    constexpr int imw = 32;
    constexpr int imh = 32;
    const double  posl = 0.5 / kir::gsplat::detail::kShC0;
    const double  s25  = 0.2164396139381; // sin(12.5°)
    const double  c25  = 0.9762960071199; // cos(12.5°)  (25° tilt about y)

    surf.resize(static_cast<crd::usize>(2) * 13U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 50.0; cam[13] = 50.0; cam[14] = 16.5; cam[15] = 16.5;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    // A: RED, tilted −25° about y (nearer on the LEFT), a hair nearer at centre (global order = A first).
    put_surfel(surf, 0, 0.0, 0.0, -0.01, 1.5, 1.5, 0.0, -s25, 0.0, c25, 0.85, posl, -posl, -posl);
    // B: BLUE, tilted +25° about y (nearer on the RIGHT), a hair farther at centre.
    put_surfel(surf, 1, 0.0, 0.0, 0.01, 1.5, 1.5, 0.0, s25, 0.0, c25, 0.85, -posl, -posl, posl);

    // project + sort by centre depth (nearest first) → A (z 4.99) before B (z 5.01)
    kir::gsplat::Gsplat2dProjectConfig pcfg;
    pcfg.count = 2U; // REN-38: tail-thread guard (two surfels in this fixture)
    kir::KGraph                        pg(&alloc);
    const kir::KEntry                  pe = kir::gsplat::build_gsplat2d_project_kernel(pg, pcfg);
    crd::containers::Array<double>     prep(&alloc);
    prep.resize(static_cast<crd::usize>(2) * 19U, 0.0);
    kir::KernelBuffer pb[3] = {{surf.data(), 2 * 13, 0, 0}, {cam.data(), 20, 0, 1}, {prep.data(), 2 * 19, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);
    // A already nearest by centre depth ⇒ input order is the global order.

    const auto at = [&](crd::containers::Array<double>& im, int x, int y, int c) { return im[static_cast<crd::usize>(y * imw + x) * 4U + static_cast<crd::usize>(c)]; };

    // BASE render (global centre-depth order, 8-float output) — composites A(red) first everywhere.
    kir::gsplat::Gsplat2dRenderConfig brc;
    brc.width = imw; brc.height = imh; brc.max_splats = 2;
    kir::KGraph       brg(&alloc);
    const kir::KEntry bre = kir::gsplat::build_gsplat2d_render_kernel(brg, brc);
    crd::containers::Array<double> bpar(&alloc);
    bpar.resize(5U, 0.0); bpar[0] = 2.0; bpar[4] = 1.0 / 255.0;
    crd::containers::Array<double> base8(&alloc);
    base8.resize(static_cast<crd::usize>(imw * imh) * 8U, 0.0);
    kir::KernelBuffer brb[4] = {{prep.data(), 2 * 19, 0, 0}, {cam.data(), 20, 0, 1}, {bpar.data(), 5, 0, 2}, {base8.data(), imw * imh * 8, 0, 3}};
    kir::eval_cpu_kernel(brg, bre, brb, 4, bre.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));
    const auto base = [&](int x, int y, int c) { return base8[static_cast<crd::usize>(y * imw + x) * 8U + static_cast<crd::usize>(c)]; };

    // RESORT render (per-pixel λ order, 4-float output + scratch)
    kir::gsplat::Gsplat2dResortConfig rrc;
    rrc.width = imw; rrc.height = imh; rrc.max_splats = 2;
    kir::KGraph       rrg(&alloc);
    const kir::KEntry rre = kir::gsplat::build_gsplat2d_resort_render_kernel(rrg, rrc);
    crd::containers::Array<double> rpar(&alloc);
    rpar.resize(5U, 0.0); rpar[0] = 2.0; rpar[4] = 1.0 / 255.0;
    crd::containers::Array<double> rimg(&alloc);
    crd::containers::Array<double> scr(&alloc);
    rimg.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    scr.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KernelBuffer rrb[5] = {{prep.data(), 2 * 19, 0, 0}, {cam.data(), 20, 0, 1}, {rpar.data(), 5, 0, 2}, {rimg.data(), imw * imh * 4, 0, 3}, {scr.data(), imw * imh * 4, 0, 4}};
    kir::eval_cpu_kernel(rrg, rre, rrb, 5, rre.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    // the BASE composites A (red) first everywhere ⇒ red dominates on BOTH sides.
    CHECK(base(10, 16, 0) > base(10, 16, 2));   // left: red
    CHECK(base(22, 16, 0) > base(22, 16, 2));   // right: STILL red (global order can't flip per pixel)

    // the RESORT composites the per-pixel-nearest surfel first ⇒ red on the LEFT (A nearer), blue on the RIGHT (B nearer).
    CHECK(at(rimg, 10, 16, 0) > at(rimg, 10, 16, 2));   // left: red (A nearer here)
    CHECK(at(rimg, 22, 16, 2) > at(rimg, 22, 16, 0));   // right: BLUE (B nearer here) — the pop is fixed
    // and the resort genuinely differs from the base at the crossing (right side).
    CHECK(crd::math::abs(at(rimg, 22, 16, 2) - base(22, 16, 2)) > 0.2);
}
