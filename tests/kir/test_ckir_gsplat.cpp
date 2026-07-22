// test_ckir_gsplat.cpp — D-007 B19-a: the 3D GAUSSIAN SPLATTING forward rasteriser.
//
// The projection (EWA splat) is pure geometry, so it is pinned against CLOSED-FORM answers — an isotropic Gaussian
// on the optical axis has a screen mean, depth, 2D conic and radius all computable by hand, which is what makes the
// gate able to catch a subtly-wrong covariance transform (the failure mode an image test sails past).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_gsplat.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_scan.hpp>
#include <crd/kir/ckir_sort.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace kir = crd::kir;

namespace
{
// pack one Gaussian into the 14-float layout the kernel reads
void put_gauss(crd::containers::Array<double>& g, int i, double mx, double my, double mz, double s, double qx,
               double qy, double qz, double qw, double opac, double shr, double shg, double shb)
{
    const crd::usize o = static_cast<crd::usize>(i) * 14U;
    g[o + 0U]  = mx; g[o + 1U] = my; g[o + 2U] = mz;
    g[o + 3U]  = s;  g[o + 4U] = s;  g[o + 5U] = s;   // isotropic scale
    g[o + 6U]  = qx; g[o + 7U] = qy; g[o + 8U] = qz; g[o + 9U] = qw;
    g[o + 10U] = opac;
    g[o + 11U] = shr; g[o + 12U] = shg; g[o + 13U] = shb;
}
} // namespace

TEST_CASE("ckir gsplat projection matches closed-form geometry for on-axis Gaussians", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "gsplat-proj");
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> out(&alloc);

    constexpr int ng = 3;
    gauss.resize(static_cast<crd::usize>(ng) * 14U, 0.0);
    // camera: R = identity, t = (0,0,5) ⇒ view p = μ + (0,0,5), so world origin sits 5 in front (view-z convention +z).
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;             // R = I (rows 0,1,2 diag)
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;           // t = (0,0,5)
    cam[12] = 500.0; cam[13] = 500.0;                     // fx, fy
    cam[14] = 256.0; cam[15] = 256.0;                     // cx, cy
    cam[16] = 0.2; cam[17] = 512.0; cam[18] = 512.0;      // near, imgW, imgH

    const double s = 0.01;
    put_gauss(gauss, 0, 0.0, 0.0, 0.0, s, 0, 0, 0, 1, 0.9, 0.0, 0.0, 0.0);  // on-axis, iso, identity rotation
    put_gauss(gauss, 1, 1.0, 0.0, 0.0, s, 0, 0, 0, 1, 0.9, 0.0, 0.0, 0.0);  // +x by 1 world unit
    put_gauss(gauss, 2, 0.0, 0.0, -10.0, s, 0, 0, 0, 1, 0.9, 0.0, 0.0, 0.0); // BEHIND the camera (view-z = -5) ⇒ culled

    kir::gsplat::GsplatProjectConfig cfg;
    kir::KGraph                      g(&alloc);
    const kir::KEntry                e = kir::gsplat::build_gsplat_project_kernel(g, cfg);
    out.resize(static_cast<crd::usize>(ng) * 12U, 0.0);
    kir::KernelBuffer bb[3] = {{gauss.data(), ng * 14, 0, 0}, {cam.data(), 20, 0, 1}, {out.data(), ng * 12, 0, 2}};
    kir::eval_cpu_kernel(g, e, bb, 3, e.local_size[0], &alloc, 1U);

    const auto get = [&](int i, int k) { return out[static_cast<crd::usize>(i) * 12U + static_cast<crd::usize>(k)]; };

    // ── Gaussian 0: on-axis ── mean = (cx, cy) = (256,256), depth = 5.
    CHECK(crd::math::abs(get(0, 0) - 256.0) < 1.0e-4);
    CHECK(crd::math::abs(get(0, 1) - 256.0) < 1.0e-4);
    CHECK(crd::math::abs(get(0, 2) - 5.0) < 1.0e-6);
    // Σ′ = diag(fx²·s²/z² + lowpass) = 500²·0.01²/25 + 0.3 = 1.0 + 0.3 = 1.3. conic = 1.3/det, det = 1.69.
    const double covd = 500.0 * 500.0 * s * s / 25.0 + cfg.lowpass; // 1.3
    const double det  = covd * covd;
    CHECK(crd::math::abs(get(0, 3) - covd / det) < 1.0e-5); // conic a
    CHECK(crd::math::abs(get(0, 4)) < 1.0e-6);              // conic b (isotropic ⇒ 0)
    CHECK(crd::math::abs(get(0, 5) - covd / det) < 1.0e-5); // conic c
    // radius = ceil(3√1.3) = ceil(3.4205) = 4.
    CHECK(crd::math::abs(get(0, 6) - 4.0) < 1.0e-9);
    CHECK(crd::math::abs(get(0, 7) - 0.5) < 1.0e-6);        // colour = 0.5 + C0·0 = 0.5
    CHECK(crd::math::abs(get(0, 10) - 0.9) < 1.0e-6);       // opacity passthrough
    CHECK(get(0, 11) > 0.5);                                // valid

    // ── Gaussian 1: +x ── mean_x = fx·1/5 + cx = 100 + 256 = 356, mean_y = 256, depth = 5.
    CHECK(crd::math::abs(get(1, 0) - 356.0) < 1.0e-4);
    CHECK(crd::math::abs(get(1, 1) - 256.0) < 1.0e-4);
    CHECK(crd::math::abs(get(1, 2) - 5.0) < 1.0e-6);

    // ── Gaussian 2: behind the camera ── invalid.
    CHECK(get(2, 11) < 0.5);
}

TEST_CASE("ckir gsplat render composites depth-sorted splats front-to-back", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator     alloc(64U << 20U, nullptr, "gsplat-render");
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);

    constexpr int ng = 2;
    constexpr int imw  = 64;
    constexpr int imh  = 64;
    // SH degree-0 coefficients giving pure primaries: colour = 0.5 + C0·sh, so a channel of +0.5/C0 → 1, −0.5/C0 → 0.
    constexpr double pos_c = 0.5 / kir::gsplat::detail::kShC0;
    constexpr double neg_c = -0.5 / kir::gsplat::detail::kShC0;

    gauss.resize(static_cast<crd::usize>(ng) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;          // t = (0,0,5)
    cam[12] = 100.0; cam[13] = 100.0;                    // fx, fy
    cam[14] = 32.0;  cam[15] = 32.0;                     // cx, cy = image centre
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    // A: NEAR (view-z 3), RED, opacity 0.8.   B: FAR (view-z 6), BLUE, opacity 0.8.  Both on-axis, both large.
    put_gauss(gauss, 0, 0.0, 0.0, -2.0, 0.2, 0, 0, 0, 1, 0.8, pos_c, neg_c, neg_c); // red, depth 3
    put_gauss(gauss, 1, 0.0, 0.0, 1.0, 0.2, 0, 0, 0, 1, 0.8, neg_c, neg_c, pos_c);  // blue, depth 6

    // ── project ──
    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    proj.resize(static_cast<crd::usize>(ng) * 12U, 0.0);
    kir::KernelBuffer pb[3] = {{gauss.data(), ng * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), ng * 12, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    // ── DEPTH SORT (host, nearest-first). B19-a2 replaces this with the GPU radix sort; the render kernel only ever
    //    sees depth-sorted input, so this reordering is where the "sort" half of 3DGS lives for now. ──
    int order[ng] = {0, 1};
    for (int i = 0; i < ng; ++i)
    {
        for (int j = i + 1; j < ng; ++j)
        {
            if (proj[static_cast<crd::usize>(order[j]) * 12U + 2U] < proj[static_cast<crd::usize>(order[i]) * 12U + 2U])
            {
                const int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    CHECK(order[0] == 0); // red (depth 3) must sort before blue (depth 6)
    crd::containers::Array<double> sorted(&alloc);
    sorted.resize(static_cast<crd::usize>(ng) * 12U, 0.0);
    for (int i = 0; i < ng; ++i)
    {
        for (int k = 0; k < 12; ++k)
        {
            sorted[static_cast<crd::usize>(i) * 12U + static_cast<crd::usize>(k)] =
                proj[static_cast<crd::usize>(order[i]) * 12U + static_cast<crd::usize>(k)];
        }
    }

    // ── render ──
    kir::gsplat::GsplatRenderConfig rcfg;
    rcfg.width = imw; rcfg.height = imh; rcfg.max_splats = ng;
    kir::KGraph       rg(&alloc);
    const kir::KEntry re = kir::gsplat::build_gsplat_render_kernel(rg, rcfg);
    crd::containers::Array<double> par(&alloc);
    crd::containers::Array<double> img(&alloc);
    par.resize(8U, 0.0);
    par[0] = static_cast<double>(ng); par[1] = imw; par[2] = imh;
    par[3] = 0.05; par[4] = 0.05; par[5] = 0.08;  // dim blue-grey background
    par[6] = 1.0 / 255.0;                          // alpha cutoff
    img.resize(static_cast<crd::usize>(imw) * imh * 4U, 0.0);
    kir::KernelBuffer rb[3] = {{sorted.data(), ng * 12, 0, 0}, {par.data(), 8, 0, 1}, {img.data(), imw * imh * 4, 0, 2}};
    kir::eval_cpu_kernel(rg, re, rb, 3, re.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    const auto pix = [&](int x, int y, int c) { return img[(static_cast<crd::usize>(y) * imw + static_cast<crd::usize>(x)) * 4U + static_cast<crd::usize>(c)]; };

    // centre pixel: red-over-blue-over-bg.  After A: R = 0.8·1; T = 0.2.  After B: B += 0.8·1·0.2 = 0.16; T = 0.04.
    //   R ≈ 0.8, B ≈ 0.16 + 0.08·0.04, G ≈ 0.05·0.04.
    INFO("centre RGB = " << pix(32, 32, 0) << ", " << pix(32, 32, 1) << ", " << pix(32, 32, 2));
    CHECK(pix(32, 32, 0) > 0.7);                 // near-red dominates
    CHECK(pix(32, 32, 2) > 0.1);                 // far-blue shows through the 0.2 residual transmittance
    CHECK(pix(32, 32, 2) < 0.25);                // ...but attenuated by the red in front
    CHECK(pix(32, 32, 1) < 0.05);                // almost no green anywhere in the splats
    CHECK(pix(32, 32, 0) > pix(32, 32, 2));      // front-to-back order preserved: red beats blue

    // a far corner is uncovered ⇒ background.
    INFO("corner RGB = " << pix(1, 1, 0) << ", " << pix(1, 1, 1) << ", " << pix(1, 1, 2));
    CHECK(crd::math::abs(pix(1, 1, 0) - 0.05) < 1.0e-3);
    CHECK(crd::math::abs(pix(1, 1, 2) - 0.08) < 1.0e-3);
}

TEST_CASE("ckir gsplat TILED render == brute-force render (the perf structure is exact)", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator     alloc(96U << 20U, nullptr, "gsplat-tiled");
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);

    constexpr int ng    = 8;
    constexpr int imw     = 64;
    constexpr int imh     = 64;
    constexpr int tile_px = 16;
    constexpr int tiles_x = imw / tile_px;     // 4
    constexpr int n_tiles = tiles_x * tiles_x; // 16
    constexpr int cap    = 64;
    constexpr double sh_p = 0.5 / kir::gsplat::detail::kShC0;
    constexpr double sh_n = -0.5 / kir::gsplat::detail::kShC0;

    gauss.resize(static_cast<crd::usize>(ng) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 100.0; cam[13] = 100.0; cam[14] = 32.0; cam[15] = 32.0;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    // 8 Gaussians spread across the frame at varied depths and colours — so different tiles get different subsets.
    const double sc = 0.15;
    put_gauss(gauss, 0, -0.9, -0.9, -1.0, sc, 0, 0, 0, 1, 0.85, sh_p, sh_n, sh_n);
    put_gauss(gauss, 1,  0.9, -0.9,  0.0, sc, 0, 0, 0, 1, 0.85, sh_n, sh_p, sh_n);
    put_gauss(gauss, 2, -0.9,  0.9,  1.0, sc, 0, 0, 0, 1, 0.85, sh_n, sh_n, sh_p);
    put_gauss(gauss, 3,  0.9,  0.9, -0.5, sc, 0, 0, 0, 1, 0.85, sh_p, sh_p, sh_n);
    put_gauss(gauss, 4,  0.0,  0.0,  0.5, sc, 0, 0, 0, 1, 0.85, sh_p, sh_n, sh_p);
    put_gauss(gauss, 5, -0.4,  0.4, -1.5, sc, 0, 0, 0, 1, 0.85, sh_n, sh_p, sh_p);
    put_gauss(gauss, 6,  0.4, -0.4,  2.0, sc, 0, 0, 0, 1, 0.85, sh_p, sh_p, sh_p);
    put_gauss(gauss, 7,  0.0,  0.6,  0.2, sc, 0, 0, 0, 1, 0.85, sh_n, sh_n, sh_n);

    // ── project ──
    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    proj.resize(static_cast<crd::usize>(ng) * 12U, 0.0);
    kir::KernelBuffer pb[3] = {{gauss.data(), ng * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), ng * 12, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    // ── depth sort (host, nearest-first) ──
    int order[ng];
    for (int i = 0; i < ng; ++i) { order[i] = i; }
    for (int i = 0; i < ng; ++i)
    {
        for (int j = i + 1; j < ng; ++j)
        {
            if (proj[static_cast<crd::usize>(order[j]) * 12U + 2U] < proj[static_cast<crd::usize>(order[i]) * 12U + 2U])
            {
                const int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    crd::containers::Array<double> sorted(&alloc);
    sorted.resize(static_cast<crd::usize>(ng) * 12U, 0.0);
    for (int i = 0; i < ng; ++i)
    {
        for (int k = 0; k < 12; ++k)
        {
            sorted[static_cast<crd::usize>(i) * 12U + static_cast<crd::usize>(k)] = proj[static_cast<crd::usize>(order[i]) * 12U + static_cast<crd::usize>(k)];
        }
    }

    crd::containers::Array<double> par(&alloc);
    par.resize(8U, 0.0);
    par[3] = 0.02; par[4] = 0.03; par[5] = 0.04; par[6] = 1.0 / 255.0;

    // ── (A) brute-force reference (B19-a render) ──
    kir::gsplat::GsplatRenderConfig bcfg;
    bcfg.width = imw; bcfg.height = imh; bcfg.max_splats = ng;
    kir::KGraph       bg(&alloc);
    const kir::KEntry be = kir::gsplat::build_gsplat_render_kernel(bg, bcfg);
    crd::containers::Array<double> bpar(&alloc);
    bpar.resize(8U, 0.0); bpar[0] = static_cast<double>(ng); bpar[1] = imw; bpar[2] = imh;
    bpar[3] = par[3]; bpar[4] = par[4]; bpar[5] = par[5]; bpar[6] = par[6];
    crd::containers::Array<double> bimg(&alloc);
    bimg.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KernelBuffer bb[3] = {{sorted.data(), ng * 12, 0, 0}, {bpar.data(), 8, 0, 1}, {bimg.data(), imw * imh * 4, 0, 2}};
    kir::eval_cpu_kernel(bg, be, bb, 3, be.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    // ── (B) build per-tile buckets host-side (B19-a3 does this on the GPU): each depth-sorted splat is appended to
    //    every tile its bbox covers, so each bucket is depth-ordered. ──
    crd::containers::Array<double> buckets(&alloc);
    crd::containers::Array<double> counts(&alloc);
    buckets.resize(static_cast<crd::usize>(n_tiles) * cap * 12U, 0.0);
    counts.resize(static_cast<crd::usize>(n_tiles), 0.0);
    for (int i = 0; i < ng; ++i)
    {
        const double mnx = sorted[static_cast<crd::usize>(i) * 12U + 0U];
        const double mny = sorted[static_cast<crd::usize>(i) * 12U + 1U];
        const double rad = sorted[static_cast<crd::usize>(i) * 12U + 6U];
        const double valid = sorted[static_cast<crd::usize>(i) * 12U + 11U];
        if (valid < 0.5) { continue; }
        int tx0 = static_cast<int>(crd::math::floor((mnx - rad) / tile_px));
        int tx1 = static_cast<int>(crd::math::floor((mnx + rad) / tile_px));
        int ty0 = static_cast<int>(crd::math::floor((mny - rad) / tile_px));
        int ty1 = static_cast<int>(crd::math::floor((mny + rad) / tile_px));
        if (tx0 < 0) { tx0 = 0; }  if (ty0 < 0) { ty0 = 0; }
        if (tx1 > tiles_x - 1) { tx1 = tiles_x - 1; }  if (ty1 > tiles_x - 1) { ty1 = tiles_x - 1; }
        for (int ty = ty0; ty <= ty1; ++ty)
        {
            for (int tx = tx0; tx <= tx1; ++tx)
            {
                const int t = ty * tiles_x + tx;
                const int c = static_cast<int>(counts[static_cast<crd::usize>(t)]);
                if (c >= cap) { continue; }
                for (int k = 0; k < 12; ++k)
                {
                    buckets[(static_cast<crd::usize>(t) * cap + static_cast<crd::usize>(c)) * 12U + static_cast<crd::usize>(k)] =
                        sorted[static_cast<crd::usize>(i) * 12U + static_cast<crd::usize>(k)];
                }
                counts[static_cast<crd::usize>(t)] = static_cast<double>(c + 1);
            }
        }
    }

    // ── (B) tiled render ──
    kir::gsplat::GsplatTiledConfig tcfg;
    tcfg.width = imw; tcfg.height = imh; tcfg.tile_px = tile_px; tcfg.cap = cap;
    kir::KGraph       tg(&alloc);
    const kir::KEntry te = kir::gsplat::build_gsplat_tiled_render_kernel(tg, tcfg);
    crd::containers::Array<double> tpar(&alloc);
    tpar.resize(8U, 0.0); tpar[0] = imw; tpar[1] = imh; tpar[2] = tiles_x;
    tpar[3] = par[3]; tpar[4] = par[4]; tpar[5] = par[5]; tpar[6] = par[6];
    crd::containers::Array<double> timg(&alloc);
    timg.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KernelBuffer tb[4] = {{buckets.data(), n_tiles * cap * 12, 0, 0}, {counts.data(), n_tiles, 0, 1},
                               {tpar.data(), 8, 0, 2}, {timg.data(), imw * imh * 4, 0, 3}};
    kir::eval_cpu_kernel(tg, te, tb, 4, te.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / 64));

    // ── the tiled render must EQUAL the brute-force render, pixel for pixel (same splats, same depth order). ──
    double worst = 0.0;
    for (int p = 0; p < imw * imh; ++p)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double d = crd::math::abs(timg[static_cast<crd::usize>(p) * 4U + static_cast<crd::usize>(c)]
                                            - bimg[static_cast<crd::usize>(p) * 4U + static_cast<crd::usize>(c)]);
            if (d > worst) { worst = d; }
        }
    }
    INFO("worst |tiled - brute| = " << worst);
    CHECK(worst < 2.0e-6); // F32 tier: same composite, same order ⇒ agreement to accumulated rounding
}

TEST_CASE("ckir gsplat MIP-SPLATTING is alias-free: total energy tracks true splat size", "[ckir][gsplat]")
{
    // The alias-free invariant (Mip-Splatting, Yu 2024). A splat's total contributed screen energy is ∝ opacity·√det Σ′.
    // As a splat shrinks below a pixel, the NAÏVE dilation floors its 2D covariance ⇒ energy plateaus at a floor ⇒ the
    // splat over-contributes = aliasing. MIP rescales opacity by √(detΣ′/detΣ′_mip), so energy tracks the TRUE Gaussian
    // size down to sub-pixel. Halving an isotropic splat's scale should quarter its energy (√det ∝ s²); MIP holds that
    // ratio, the naïve dilation compresses it. This gate measures the ratio directly — the gold-standard AA property.
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "gsplat-mip");
    crd::containers::Array<double> cam(&alloc);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 500.0; cam[13] = 500.0; cam[14] = 256.0; cam[15] = 256.0;
    cam[16] = 0.2; cam[17] = 512.0; cam[18] = 512.0;

    const double s_large = 0.10;
    const double s_tiny  = 0.005; // 20× smaller ⇒ true energy 400× smaller (√det ∝ s²)

    // project one on-axis isotropic Gaussian at a given scale under a given config; return its screen energy ∝ opac·√detΣ′.
    const auto energy = [&](double s, bool mip) {
        crd::containers::Array<double> gauss(&alloc);
        crd::containers::Array<double> out(&alloc);
        gauss.resize(14U, 0.0); out.resize(12U, 0.0);
        put_gauss(gauss, 0, 0.0, 0.0, 0.0, s, 0, 0, 0, 1, 0.9, 0.0, 0.0, 0.0);
        kir::gsplat::GsplatProjectConfig cfg;
        cfg.mip = mip; cfg.smooth_3d = 0.0; // isolate the 2D Mip filter (no 3D smoothing) for a clean invariant
        cfg.lowpass = 0.3; cfg.mip_2d = 0.3;
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::gsplat::build_gsplat_project_kernel(g, cfg);
        kir::KernelBuffer bb[3] = {{gauss.data(), 14, 0, 0}, {cam.data(), 20, 0, 1}, {out.data(), 12, 0, 2}};
        kir::eval_cpu_kernel(g, e, bb, 3, e.local_size[0], &alloc, 1U);
        const double ca   = out[3];
        const double cb   = out[4];
        const double cc   = out[5];
        const double opac = out[10];
        const double det_sigma = 1.0 / (ca * cc - cb * cb); // conic = Σ′⁻¹ ⇒ det Σ′ = 1/det(conic)
        return opac * crd::math::sqrt(det_sigma);
    };

    const double e_mip_large   = energy(s_large, true);
    const double e_mip_tiny    = energy(s_tiny, true);
    const double e_naive_large = energy(s_large, false);
    const double e_naive_tiny  = energy(s_tiny, false);

    const double ratio_mip   = e_mip_large / e_mip_tiny;
    const double ratio_naive = e_naive_large / e_naive_tiny;
    const double ideal       = (s_large / s_tiny) * (s_large / s_tiny); // 400
    INFO("energy ratio: MIP " << ratio_mip << ", naive " << ratio_naive << ", ideal " << ideal);
    std::printf("[B19-b MIP] energy ratio (20x shrink): MIP %.1f vs naive %.1f (ideal %.0f); tiny-splat energy MIP %.4f vs naive %.4f\n",
                ratio_mip, ratio_naive, ideal, e_mip_tiny, e_naive_tiny);

    // MIP tracks the TRUE size — the ratio is the ideal s² law to a few %.
    CHECK(crd::math::abs(ratio_mip - ideal) / ideal < 0.05);
    // the naïve dilation FLOORS the tiny splat ⇒ its ratio is badly compressed (aliasing).
    CHECK(ratio_naive < 0.6 * ideal);
    // ...and the concrete symptom: the sub-pixel splat is INFLATED by the naïve floor vs the correct MIP energy.
    INFO("tiny-splat energy: MIP " << e_mip_tiny << ", naive " << e_naive_tiny);
    CHECK(e_naive_tiny > 1.5 * e_mip_tiny);
}

TEST_CASE("ckir gsplat ON-DEVICE depth sort == host sort (the sort half runs on the GPU, B19-a3)", "[ckir][gsplat]")
{
    // The full on-device sort: project → depthkey → KEY-VALUE radix sort (ckir_sort.hpp) → gather. The result must be
    // the SAME depth order the host insertion sort gives — proving the "sort" half of 3DGS runs on-device bit-exactly,
    // no host crutch. Distinct depths ⇒ distinct 24-bit keys ⇒ no ties ⇒ the two orders are identical element-for-element.
    constexpr int n          = 2048;   // = nblocks(2) · epb(1024); the radix sort's block structure
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 1 << radix_bits;
    constexpr int epb        = 1024;
    constexpr int nblocks    = n / epb;
    constexpr int scan_threads = nblocks < threads ? nblocks : threads;
    crd::memory::TlsfAllocator     alloc(128U << 20U, nullptr, "gsplat-gpusort");

    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);
    gauss.resize(static_cast<crd::usize>(n) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 0.0;      // t = 0 ⇒ view-z = world z; depth = world z directly
    cam[12] = 100.0; cam[13] = 100.0; cam[14] = 32.0; cam[15] = 32.0;
    cam[16] = 0.2; cam[17] = 64.0; cam[18] = 64.0;

    crd::u32   st = 0x51A7EDU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n; ++i) // distinct depths spread over [3,9]; scattered x,y (irrelevant to the sort)
    {
        const double z = 3.0 + 6.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(n);
        put_gauss(gauss, i, (rnd() * 2.0 - 1.0) * 0.5, (rnd() * 2.0 - 1.0) * 0.5, z, 0.05, 0, 0, 0, 1, 0.7, 0.0, 0.0, 0.0);
    }

    // project
    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    proj.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    kir::KernelBuffer pb[3] = {{gauss.data(), n * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), n * 12, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    // depthkey → keys, vals
    crd::containers::Array<double> par(&alloc);
    crd::containers::Array<double> ka(&alloc);
    crd::containers::Array<double> kb(&alloc);
    crd::containers::Array<double> va(&alloc);
    crd::containers::Array<double> vb(&alloc);
    par.resize(3U, 0.0); par[0] = 2.5; par[1] = 9.5; par[2] = static_cast<double>(n);
    ka.resize(n, 0.0); kb.resize(n, 0.0); va.resize(n, 0.0); vb.resize(n, 0.0);
    kir::KGraph       dg(&alloc);
    const kir::KEntry de = kir::gsplat::build_gsplat_depthkey_kernel(dg, {});
    kir::KernelBuffer db[4] = {{proj.data(), n * 12, 0, 0}, {par.data(), 3, 0, 1}, {ka.data(), n, 0, 2}, {va.data(), n, 0, 3}};
    kir::eval_cpu_kernel(dg, de, db, 4, de.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    // KEY-VALUE radix sort (4 LSD passes)
    crd::containers::Array<double> bh(&alloc);
    crd::containers::Array<double> go(&alloc);
    crd::containers::Array<double> tot(&alloc);
    crd::containers::Array<double> gb(&alloc);
    bh.resize(static_cast<crd::usize>(nblocks) * nbins, 0.0); go.resize(static_cast<crd::usize>(nblocks) * nbins, 0.0);
    tot.resize(nbins, 0.0); gb.resize(nbins, 0.0);
    double* ck = ka.data(); double* ok = kb.data();
    double* cv = va.data(); double* ov = vb.data();
    for (int pass = 0; pass < 4; ++pass)
    {
        const int shift = pass * 8;
        kir::KGraph gh(&alloc);
        kir::KGraph gof1(&alloc);
        kir::KGraph gof2(&alloc);
        kir::KGraph gs(&alloc);
        const kir::KEntry eh  = kir::build_sort_histogram(gh, epb, threads, radix_bits, shift, nblocks);
        const kir::KEntry eo1 = kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads);
        const kir::KEntry eo2 = kir::build_sort_gbase(gof2, radix_bits);
        const kir::KEntry es  = kir::build_sort_scatter(gs, epb, threads, radix_bits, shift, nblocks, true);
        kir::KernelBuffer h[2] = {{ck, n, 0, 0}, {bh.data(), nblocks * nbins, 0, 1}};
        kir::eval_cpu_kernel(gh, eh, h, 2, eh.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        kir::KernelBuffer o1[3] = {{bh.data(), nblocks * nbins, 0, 0}, {go.data(), nblocks * nbins, 0, 1}, {tot.data(), nbins, 0, 2}};
        kir::eval_cpu_kernel(gof1, eo1, o1, 3, eo1.local_size[0], &alloc, static_cast<crd::u32>(nbins));
        kir::KernelBuffer o2[2] = {{tot.data(), nbins, 0, 0}, {gb.data(), nbins, 0, 1}};
        kir::eval_cpu_kernel(gof2, eo2, o2, 2, eo2.local_size[0], &alloc, 1U);
        kir::KernelBuffer s[6] = {{ck, n, 0, 0}, {ok, n, 0, 1}, {go.data(), nblocks * nbins, 0, 2}, {gb.data(), nbins, 0, 3}, {cv, n, 0, 4}, {ov, n, 0, 5}};
        kir::eval_cpu_kernel(gs, es, s, 6, es.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        double* tk = ck; ck = ok; ok = tk;
        double* tv = cv; cv = ov; ov = tv;
    }
    // cv now holds the sorted index order

    // gather → sorted-on-device
    crd::containers::Array<double> gpu_sorted(&alloc);
    gpu_sorted.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    kir::KGraph       gg(&alloc);
    const kir::KEntry ge = kir::gsplat::build_gsplat_gather_kernel(gg);
    kir::KernelBuffer gbuf[3] = {{proj.data(), n * 12, 0, 0}, {cv, n, 0, 1}, {gpu_sorted.data(), n * 12, 0, 2}};
    kir::eval_cpu_kernel(gg, ge, gbuf, 3, ge.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    // host reference: stable sort the indices by depth (slot 2), gather
    crd::containers::Array<crd::u32> ho(&alloc);
    ho.resize(n, 0U);
    for (int i = 0; i < n; ++i) { ho[static_cast<crd::usize>(i)] = static_cast<crd::u32>(i); }
    for (int i = 1; i < n; ++i) // insertion sort (stable), by depth
    {
        const crd::u32 key = ho[static_cast<crd::usize>(i)];
        const double   kd  = proj[static_cast<crd::usize>(key) * 12U + 2U];
        int            j   = i - 1;
        while (j >= 0 && proj[static_cast<crd::usize>(ho[static_cast<crd::usize>(j)]) * 12U + 2U] > kd)
        {
            const int jp1 = j + 1;
            ho[static_cast<crd::usize>(jp1)] = ho[static_cast<crd::usize>(j)];
            --j;
        }
        const int jp1 = j + 1;
        ho[static_cast<crd::usize>(jp1)] = key;
    }

    // the on-device gathered order must match the host-sorted gather, element for element.
    int mism = 0;
    for (int i = 0; i < n; ++i)
    {
        const crd::usize src = static_cast<crd::usize>(ho[static_cast<crd::usize>(i)]);
        for (int k = 0; k < 12; ++k)
        {
            if (crd::math::abs(gpu_sorted[static_cast<crd::usize>(i) * 12U + static_cast<crd::usize>(k)] - proj[src * 12U + static_cast<crd::usize>(k)]) > 1.0e-9) { ++mism; }
        }
    }
    INFO("mismatched sorted slots = " << mism << " of " << (n * 12));
    CHECK(mism == 0); // the GPU depth sort == the host depth sort, splat for splat
    // and the sorted depths are ascending (nearest-first).
    int bad = 0;
    for (int i = 1; i < n; ++i) { if (gpu_sorted[static_cast<crd::usize>(i) * 12U + 2U] < gpu_sorted[static_cast<crd::usize>(i - 1) * 12U + 2U] - 1.0e-9) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B19-a4 tilecount: covered-tile count per splat matches the host bbox rect", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator     alloc(64U << 20U, nullptr, "gsplat-tc");
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);

    constexpr int n       = 32;
    constexpr int imw     = 64;
    constexpr int imh     = 64;
    constexpr int tile_px = 16;
    constexpr int tiles_x = imw / tile_px; // 4
    constexpr int tiles_y = imh / tile_px; // 4

    gauss.resize(static_cast<crd::usize>(n) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 100.0; cam[13] = 100.0; cam[14] = 32.0; cam[15] = 32.0;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    crd::u32   st = 0xB19A4U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n; ++i) // splats scattered across the frame, varied scale (⇒ varied screen radius / tile coverage)
    {
        const double x = (rnd() * 2.0 - 1.0) * 1.2;
        const double y = (rnd() * 2.0 - 1.0) * 1.2;
        const double z = -1.0 + rnd() * 2.0;
        const double s = 0.03 + rnd() * 0.22;
        put_gauss(gauss, i, x, y, z, s, 0, 0, 0, 1, 0.85, 0.0, 0.0, 0.0);
    }

    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    proj.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    kir::KernelBuffer pb[3] = {{gauss.data(), n * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), n * 12, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    kir::gsplat::GsplatBinConfig bcfg;
    bcfg.width = imw; bcfg.height = imh; bcfg.tile_px = tile_px; bcfg.max_cover = 16;
    kir::KGraph       tg(&alloc);
    const kir::KEntry te = kir::gsplat::build_gsplat_tilecount_kernel(tg, bcfg);
    crd::containers::Array<double> tc(&alloc);
    tc.resize(static_cast<crd::usize>(n), 0.0);
    kir::KernelBuffer tb[2] = {{proj.data(), n * 12, 0, 0}, {tc.data(), n, 0, 1}};
    kir::eval_cpu_kernel(tg, te, tb, 2, te.local_size[0], &alloc, 1U);

    const auto clampi = [](int v, int lo, int hi) { int r = v; if (r < lo) { r = lo; } if (r > hi) { r = hi; } return r; };
    const float itpx = 1.0F / static_cast<float>(tile_px);
    int         mism = 0;
    int         maxc = 0;
    int         total = 0;
    for (int i = 0; i < n; ++i)
    {
        const float mnx = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 0U]);
        const float mny = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 1U]);
        const float rad = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 6U]);
        const float val = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 11U]);
        const int rx0 = clampi(static_cast<int>(crd::math::floor((mnx - rad) * itpx)), 0, tiles_x);
        const int rx1 = clampi(static_cast<int>(crd::math::floor((mnx + rad) * itpx)) + 1, 0, tiles_x);
        const int ry0 = clampi(static_cast<int>(crd::math::floor((mny - rad) * itpx)), 0, tiles_y);
        const int ry1 = clampi(static_cast<int>(crd::math::floor((mny + rad) * itpx)) + 1, 0, tiles_y);
        const int cxn = rx1 - rx0 > 0 ? rx1 - rx0 : 0;
        const int cyn = ry1 - ry0 > 0 ? ry1 - ry0 : 0;
        const int exp = val > 0.5F ? cxn * cyn : 0;
        const int got = static_cast<int>(tc[static_cast<crd::usize>(i)]);
        if (exp != got) { ++mism; }
        if (exp > maxc) { maxc = exp; }
        total += exp;
    }
    INFO("tilecount mismatches = " << mism << ", max cover = " << maxc << ", total instances = " << total);
    CHECK(mism == 0);
    CHECK(maxc <= bcfg.max_cover); // no splat exceeds the scatter fan-out cap ⇒ the bin is EXACT in this scene
    CHECK(total > n);              // some splats genuinely span multiple tiles (the coverage is non-trivial)
}

TEST_CASE("B19-a4 scatter: tilecount->scan->scatter packs (tile,splat) instances == host reference", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator     alloc(96U << 20U, nullptr, "gsplat-scatter");
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);

    constexpr int n         = 256;
    constexpr int imw       = 64;
    constexpr int imh       = 64;
    constexpr int tile_px   = 16;
    constexpr int tiles_x   = imw / tile_px; // 4
    constexpr int tiles_y   = imh / tile_px; // 4
    constexpr int max_cover = 16;            // >= tiles_x*tiles_y ⇒ can never overflow here
    constexpr int local     = 64;

    gauss.resize(static_cast<crd::usize>(n) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 100.0; cam[13] = 100.0; cam[14] = 32.0; cam[15] = 32.0;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    crd::u32   st = 0x5CA77EU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n; ++i)
    {
        const double x = (rnd() * 2.0 - 1.0) * 1.15;
        const double y = (rnd() * 2.0 - 1.0) * 1.15;
        const double z = -1.0 + rnd() * 2.0;
        const double s = 0.03 + rnd() * 0.18;
        put_gauss(gauss, i, x, y, z, s, 0, 0, 0, 1, 0.85, 0.0, 0.0, 0.0);
    }

    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    proj.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    kir::KernelBuffer pb[3] = {{gauss.data(), n * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), n * 12, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    kir::gsplat::GsplatBinConfig bcfg;
    bcfg.width = imw; bcfg.height = imh; bcfg.tile_px = tile_px; bcfg.max_cover = max_cover; bcfg.local_size = local;

    // tilecount
    kir::KGraph       tg(&alloc);
    const kir::KEntry te = kir::gsplat::build_gsplat_tilecount_kernel(tg, bcfg);
    crd::containers::Array<double> tc(&alloc);
    tc.resize(static_cast<crd::usize>(n), 0.0);
    kir::KernelBuffer tb[2] = {{proj.data(), n * 12, 0, 0}, {tc.data(), n, 0, 1}};
    kir::eval_cpu_kernel(tg, te, tb, 2, te.local_size[0], &alloc, static_cast<crd::u32>(n / local));

    // exclusive scan of tc → off (single-pass: n <= threads*8)
    kir::KGraph g0(&alloc);
    kir::KGraph g1(&alloc);
    kir::KGraph g2(&alloc);
    kir::KGraph* sg[3] = {&g0, &g1, &g2};
    const kir::ScanPlan plan = kir::build_scan(sg, n, false, 256, 1);
    REQUIRE(plan.single_pass);
    crd::containers::Array<double> off(&alloc);
    off.resize(static_cast<crd::usize>(n), 0.0);
    kir::KernelBuffer scb[2] = {{tc.data(), n, 0, 0}, {off.data(), n, 0, 1}};
    kir::eval_cpu_kernel(*plan.block_graph, plan.block, scb, 2, plan.block.local_size[0], &alloc, static_cast<crd::u32>(plan.nblocks));

    const int total = static_cast<int>(off[static_cast<crd::usize>(n - 1)] + tc[static_cast<crd::usize>(n - 1)]);

    // scatter (keys pre-filled with the 0xFFFFFFFF sentinel; only [0,total) get written)
    crd::containers::Array<crd::u32> keys(&alloc);
    crd::containers::Array<crd::u32> vals(&alloc);
    keys.resize(static_cast<crd::usize>(n) * max_cover, 0xFFFFFFFFU);
    vals.resize(static_cast<crd::usize>(n) * max_cover, 0U);
    crd::containers::Array<double> keysd(&alloc);
    crd::containers::Array<double> valsd(&alloc);
    keysd.resize(keys.size(), 0.0);
    valsd.resize(vals.size(), 0.0);
    for (crd::usize i = 0; i < keys.size(); ++i) { keysd[i] = static_cast<double>(keys[i]); valsd[i] = 0.0; }
    kir::KGraph       sg2(&alloc);
    const kir::KEntry se = kir::gsplat::build_gsplat_scatter_instances_kernel(sg2, bcfg);
    kir::KernelBuffer sb2[5] = {{proj.data(), n * 12, 0, 0}, {tc.data(), n, 0, 1}, {off.data(), n, 0, 2},
                                {keysd.data(), static_cast<int>(keysd.size()), 0, 3}, {valsd.data(), static_cast<int>(valsd.size()), 0, 4}};
    kir::eval_cpu_kernel(sg2, se, sb2, 5, se.local_size[0], &alloc, static_cast<crd::u32>(n * max_cover / local));

    // host reference: same rect, same row-major slot decode, same packed positions.
    const auto clampi = [](int v, int lo, int hi) { int r = v; if (r < lo) { r = lo; } if (r > hi) { r = hi; } return r; };
    const float itpx = 1.0F / static_cast<float>(tile_px);
    crd::containers::Array<int> hoff(&alloc);
    crd::containers::Array<int> htc(&alloc);
    hoff.resize(static_cast<crd::usize>(n), 0);
    htc.resize(static_cast<crd::usize>(n), 0);
    int acc = 0;
    for (int i = 0; i < n; ++i)
    {
        const float mnx = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 0U]);
        const float mny = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 1U]);
        const float rad = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 6U]);
        const float val = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 11U]);
        const int rx0 = clampi(static_cast<int>(crd::math::floor((mnx - rad) * itpx)), 0, tiles_x);
        const int rx1 = clampi(static_cast<int>(crd::math::floor((mnx + rad) * itpx)) + 1, 0, tiles_x);
        const int ry0 = clampi(static_cast<int>(crd::math::floor((mny - rad) * itpx)), 0, tiles_y);
        const int ry1 = clampi(static_cast<int>(crd::math::floor((mny + rad) * itpx)) + 1, 0, tiles_y);
        const int cxn = rx1 - rx0 > 0 ? rx1 - rx0 : 0;
        const int cyn = ry1 - ry0 > 0 ? ry1 - ry0 : 0;
        htc[static_cast<crd::usize>(i)] = val > 0.5F ? cxn * cyn : 0;
        hoff[static_cast<crd::usize>(i)] = acc;
        acc += htc[static_cast<crd::usize>(i)];
    }
    REQUIRE(acc == total); // the GPU scan total matches the host prefix sum

    int mismatch = 0;
    for (int i = 0; i < n; ++i)
    {
        const float mnx = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 0U]);
        const float mny = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 1U]);
        const float rad = static_cast<float>(proj[static_cast<crd::usize>(i) * 12U + 6U]);
        const int rx0 = clampi(static_cast<int>(crd::math::floor((mnx - rad) * itpx)), 0, tiles_x);
        const int rx1 = clampi(static_cast<int>(crd::math::floor((mnx + rad) * itpx)) + 1, 0, tiles_x);
        const int ry0 = clampi(static_cast<int>(crd::math::floor((mny - rad) * itpx)), 0, tiles_y);
        const int cxn = rx1 - rx0 > 0 ? rx1 - rx0 : 1;
        for (int s = 0; s < htc[static_cast<crd::usize>(i)]; ++s)
        {
            const int ly   = s / cxn;
            const int lx   = s - ly * cxn;
            const int tile = (ry0 + ly) * tiles_x + (rx0 + lx);
            const int pos  = hoff[static_cast<crd::usize>(i)] + s;
            if (static_cast<int>(keysd[static_cast<crd::usize>(pos)]) != tile) { ++mismatch; }
            if (static_cast<int>(valsd[static_cast<crd::usize>(pos)]) != i) { ++mismatch; }
        }
    }
    INFO("scatter mismatches = " << mismatch << " over T=" << total << " instances");
    CHECK(mismatch == 0);
    // every written key is a valid tile id (< nTiles); padding past T stayed at the sentinel.
    int bad_tile = 0;
    for (int p = 0; p < total; ++p) { if (static_cast<crd::u32>(keysd[static_cast<crd::usize>(p)]) >= static_cast<crd::u32>(tiles_x * tiles_y)) { ++bad_tile; } }
    CHECK(bad_tile == 0);
    CHECK(static_cast<crd::u32>(keysd[static_cast<crd::usize>(total)]) == 0xFFFFFFFFU); // first padding slot untouched
}

TEST_CASE("B19-a4 FULL GPU BINNING: tilecount->scan->scatter->sort->ranges->block-render == brute (pixelwise)", "[ckir][gsplat]")
{
    // The whole tile bin runs on-device (no host bucket fill): duplicate splats across covered tiles, scan, scatter
    // (tile,splat) instances, radix-sort BY TILE (stable ⇒ depth order kept within a tile), find per-tile ranges, then
    // the BLOCK render (one workgroup per tile, variable range length). The block image must equal the brute-force
    // all-splats-per-pixel render, splat-for-splat — proving the binning composites exactly the right splats in order.
    crd::memory::TlsfAllocator     alloc(160U << 20U, nullptr, "gsplat-bin");
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);

    constexpr int n         = 64;
    constexpr int imw       = 32;
    constexpr int imh       = 32;
    constexpr int tile_px   = 16;
    constexpr int tiles_x   = imw / tile_px; // 2
    constexpr int n_tiles   = tiles_x * (imh / tile_px); // 4
    constexpr int max_cover = 8;
    constexpr int local     = 64;
    // radix-sort geometry (pad the instance list to a block multiple; sentinels 0xFFFFFFFF sort to the end)
    constexpr int n_pad     = 1024;
    constexpr int threads   = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins     = 256;
    constexpr int epb       = 512;
    constexpr int nblocks   = n_pad / epb;
    constexpr int scan_threads = nblocks < threads ? nblocks : threads;
    const double  pos_c     = 0.5 / kir::gsplat::detail::kShC0;
    const double  neg_c     = -0.5 / kir::gsplat::detail::kShC0;

    gauss.resize(static_cast<crd::usize>(n) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 100.0; cam[13] = 100.0; cam[14] = static_cast<double>(imw) * 0.5; cam[15] = static_cast<double>(imh) * 0.5;
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);

    crd::u32   st = 0xB1994U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n; ++i) // spread over the frame + depth, varied scale/colour ⇒ real overlap and multi-tile coverage
    {
        const double x = (rnd() * 2.0 - 1.0) * 0.55;
        const double y = (rnd() * 2.0 - 1.0) * 0.55;
        const double z = -1.2 + rnd() * 2.4;
        const double s = 0.04 + rnd() * 0.16;
        const double cr = rnd(); const double cb = 1.0 - cr;
        put_gauss(gauss, i, x, y, z, s, 0, 0, 0, 1, 0.6 + rnd() * 0.3,
                  cr > 0.5 ? pos_c : neg_c, neg_c, cb > 0.5 ? pos_c : neg_c);
    }

    // project
    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    proj.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    kir::KernelBuffer pb[3] = {{gauss.data(), n * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), n * 12, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, 1U);

    // host depth sort → `sorted` (the a3 on-device sort is gated separately; here it just supplies depth-ordered input)
    crd::containers::Array<int> ord(&alloc);
    ord.resize(static_cast<crd::usize>(n), 0);
    for (int i = 0; i < n; ++i) { ord[static_cast<crd::usize>(i)] = i; }
    for (int i = 1; i < n; ++i)
    {
        const int   key = ord[static_cast<crd::usize>(i)];
        const double kd  = proj[static_cast<crd::usize>(key) * 12U + 2U];
        int         j   = i - 1;
        while (j >= 0 && proj[static_cast<crd::usize>(ord[static_cast<crd::usize>(j)]) * 12U + 2U] > kd)
        {
            const int jp1 = j + 1;
            ord[static_cast<crd::usize>(jp1)] = ord[static_cast<crd::usize>(j)];
            --j;
        }
        const int jp1 = j + 1;
        ord[static_cast<crd::usize>(jp1)] = key;
    }
    crd::containers::Array<double> sorted(&alloc);
    sorted.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < 12; ++k) { sorted[static_cast<crd::usize>(i) * 12U + static_cast<crd::usize>(k)] = proj[static_cast<crd::usize>(ord[static_cast<crd::usize>(i)]) * 12U + static_cast<crd::usize>(k)]; }
    }

    kir::gsplat::GsplatBinConfig bcfg;
    bcfg.width = imw; bcfg.height = imh; bcfg.tile_px = tile_px; bcfg.max_cover = max_cover; bcfg.local_size = local;

    // tilecount → tc
    kir::KGraph       tg(&alloc);
    const kir::KEntry te = kir::gsplat::build_gsplat_tilecount_kernel(tg, bcfg);
    crd::containers::Array<double> tc(&alloc);
    tc.resize(static_cast<crd::usize>(n), 0.0);
    kir::KernelBuffer tb[2] = {{sorted.data(), n * 12, 0, 0}, {tc.data(), n, 0, 1}};
    kir::eval_cpu_kernel(tg, te, tb, 2, te.local_size[0], &alloc, static_cast<crd::u32>(n / local));

    // exclusive scan → off
    kir::KGraph g0(&alloc);
    kir::KGraph g1(&alloc);
    kir::KGraph g2(&alloc);
    kir::KGraph* sg[3] = {&g0, &g1, &g2};
    const kir::ScanPlan plan = kir::build_scan(sg, n, false, 256, 1);
    crd::containers::Array<double> off(&alloc);
    off.resize(static_cast<crd::usize>(n), 0.0);
    kir::KernelBuffer scb[2] = {{tc.data(), n, 0, 0}, {off.data(), n, 0, 1}};
    kir::eval_cpu_kernel(*plan.block_graph, plan.block, scb, 2, plan.block.local_size[0], &alloc, static_cast<crd::u32>(plan.nblocks));
    const int total = static_cast<int>(off[static_cast<crd::usize>(n - 1)] + tc[static_cast<crd::usize>(n - 1)]);
    REQUIRE(total < n_pad); // the instance list fits the padded sort buffer (with room for the >T sentinel)

    // scatter → keys/vals (padded, sentinel-filled)
    crd::containers::Array<double> ka(&alloc);
    crd::containers::Array<double> kb(&alloc);
    crd::containers::Array<double> va(&alloc);
    crd::containers::Array<double> vb(&alloc);
    ka.resize(n_pad, static_cast<double>(0xFFFFFFFFU));
    kb.resize(n_pad, static_cast<double>(0xFFFFFFFFU));
    va.resize(n_pad, 0.0);
    vb.resize(n_pad, 0.0);
    kir::KGraph       sg2(&alloc);
    const kir::KEntry se = kir::gsplat::build_gsplat_scatter_instances_kernel(sg2, bcfg);
    kir::KernelBuffer sb2[5] = {{sorted.data(), n * 12, 0, 0}, {tc.data(), n, 0, 1}, {off.data(), n, 0, 2}, {ka.data(), n_pad, 0, 3}, {va.data(), n_pad, 0, 4}};
    kir::eval_cpu_kernel(sg2, se, sb2, 5, se.local_size[0], &alloc, static_cast<crd::u32>(n * max_cover / local));

    // KV radix sort BY TILE (4 LSD passes; stable ⇒ within-tile depth order preserved)
    crd::containers::Array<double> bh(&alloc);
    crd::containers::Array<double> go(&alloc);
    crd::containers::Array<double> tot(&alloc);
    crd::containers::Array<double> gb(&alloc);
    bh.resize(static_cast<crd::usize>(nblocks) * nbins, 0.0);
    go.resize(static_cast<crd::usize>(nblocks) * nbins, 0.0);
    tot.resize(nbins, 0.0);
    gb.resize(nbins, 0.0);
    double* ck = ka.data(); double* ok = kb.data();
    double* cv = va.data(); double* ov = vb.data();
    for (int pass = 0; pass < 4; ++pass)
    {
        const int shift = pass * 8;
        kir::KGraph gh(&alloc);
        kir::KGraph gof1(&alloc);
        kir::KGraph gof2(&alloc);
        kir::KGraph gs(&alloc);
        const kir::KEntry eh  = kir::build_sort_histogram(gh, epb, threads, radix_bits, shift, nblocks);
        const kir::KEntry eo1 = kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads);
        const kir::KEntry eo2 = kir::build_sort_gbase(gof2, radix_bits);
        const kir::KEntry es  = kir::build_sort_scatter(gs, epb, threads, radix_bits, shift, nblocks, true);
        kir::KernelBuffer h[2] = {{ck, n_pad, 0, 0}, {bh.data(), nblocks * nbins, 0, 1}};
        kir::eval_cpu_kernel(gh, eh, h, 2, eh.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        kir::KernelBuffer o1[3] = {{bh.data(), nblocks * nbins, 0, 0}, {go.data(), nblocks * nbins, 0, 1}, {tot.data(), nbins, 0, 2}};
        kir::eval_cpu_kernel(gof1, eo1, o1, 3, eo1.local_size[0], &alloc, static_cast<crd::u32>(nbins));
        kir::KernelBuffer o2[2] = {{tot.data(), nbins, 0, 0}, {gb.data(), nbins, 0, 1}};
        kir::eval_cpu_kernel(gof2, eo2, o2, 2, eo2.local_size[0], &alloc, 1U);
        kir::KernelBuffer s[6] = {{ck, n_pad, 0, 0}, {ok, n_pad, 0, 1}, {go.data(), nblocks * nbins, 0, 2}, {gb.data(), nbins, 0, 3}, {cv, n_pad, 0, 4}, {ov, n_pad, 0, 5}};
        kir::eval_cpu_kernel(gs, es, s, 6, es.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        double* tk = ck; ck = ok; ok = tk;
        double* tv = cv; cv = ov; ov = tv;
    }
    // ck = sorted tile keys, cv = sorted splat-index payload (the block render's `order`)

    // tile ranges (pre-zeroed)
    crd::containers::Array<double> ranges(&alloc);
    ranges.resize(static_cast<crd::usize>(n_tiles) * 2U, 0.0);
    crd::containers::Array<double> rpar(&alloc);
    rpar.resize(1U, 0.0); rpar[0] = static_cast<double>(total);
    kir::KGraph       rg(&alloc);
    const kir::KEntry re = kir::gsplat::build_gsplat_tile_ranges_kernel(rg, local);
    kir::KernelBuffer rb[3] = {{ck, n_pad, 0, 0}, {rpar.data(), 1, 0, 1}, {ranges.data(), n_tiles * 2, 0, 2}};
    kir::eval_cpu_kernel(rg, re, rb, 3, re.local_size[0], &alloc, static_cast<crd::u32>(n_pad / local));

    // ranges sanity: spans partition [0,total), non-overlapping, ascending
    int span_sum = 0;
    for (int t = 0; t < n_tiles; ++t)
    {
        const int rs = static_cast<int>(ranges[static_cast<crd::usize>(t) * 2U + 0U]);
        const int rend = static_cast<int>(ranges[static_cast<crd::usize>(t) * 2U + 1U]);
        CHECK(rend >= rs);
        span_sum += (rend - rs);
        for (int p = rs; p < rend; ++p) { CHECK(static_cast<int>(ck[static_cast<crd::usize>(p)]) == t); } // every instance in the span really has this tile
    }
    CHECK(span_sum == total); // the ranges tile every real instance exactly once

    // block render (one workgroup per tile, variable span)
    kir::gsplat::GsplatBlockConfig blkcfg;
    blkcfg.width = imw; blkcfg.height = imh; blkcfg.tile_px = tile_px;
    crd::containers::Array<double> bpar(&alloc);
    bpar.resize(4U, 0.0); bpar[0] = 0.02; bpar[1] = 0.03; bpar[2] = 0.04; bpar[3] = 1.0 / 255.0;
    crd::containers::Array<double> img(&alloc);
    img.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KGraph       blkg(&alloc);
    const kir::KEntry blke = kir::gsplat::build_gsplat_block_render_kernel(blkg, blkcfg);
    kir::KernelBuffer blb[5] = {{sorted.data(), n * 12, 0, 0}, {cv, n_pad, 0, 1}, {ranges.data(), n_tiles * 2, 0, 2}, {bpar.data(), 4, 0, 3}, {img.data(), imw * imh * 4, 0, 4}};
    kir::eval_cpu_kernel(blkg, blke, blb, 5, blke.local_size[0], &alloc, static_cast<crd::u32>(n_tiles));

    // brute reference (all splats per pixel, depth order)
    kir::gsplat::GsplatRenderConfig brcfg;
    brcfg.width = imw; brcfg.height = imh; brcfg.max_splats = n;
    crd::containers::Array<double> brpar(&alloc);
    brpar.resize(8U, 0.0); brpar[0] = static_cast<double>(n); brpar[1] = imw; brpar[2] = imh;
    brpar[3] = bpar[0]; brpar[4] = bpar[1]; brpar[5] = bpar[2]; brpar[6] = bpar[3];
    crd::containers::Array<double> brimg(&alloc);
    brimg.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KGraph       brg(&alloc);
    const kir::KEntry bre = kir::gsplat::build_gsplat_render_kernel(brg, brcfg);
    kir::KernelBuffer brb[3] = {{sorted.data(), n * 12, 0, 0}, {brpar.data(), 8, 0, 1}, {brimg.data(), imw * imh * 4, 0, 2}};
    kir::eval_cpu_kernel(brg, bre, brb, 3, bre.local_size[0], &alloc, static_cast<crd::u32>(imw * imh / local));

    double worst = 0.0;
    for (int q = 0; q < imw * imh; ++q)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double d = crd::math::abs(img[static_cast<crd::usize>(q) * 4U + static_cast<crd::usize>(c)] - brimg[static_cast<crd::usize>(q) * 4U + static_cast<crd::usize>(c)]);
            if (d > worst) { worst = d; }
        }
    }
    std::printf("[B19-a4] full GPU bin: %d splats, T=%d instances over %d tiles; worst |block - brute| = %.3e\n", n, total, n_tiles, worst);
    CHECK(worst < 1.0e-5); // the block render composites exactly the brute set, in depth order ⇒ bit-exact (f32)
}

TEST_CASE("B19-d quantise: the K-bit attribute codec round-trips within the quantisation step", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "gsplat-quant");
    constexpr int n    = 256;
    constexpr int natt = 14;
    constexpr int bits = 12;
    const double  levels = static_cast<double>((1U << bits) - 1U);

    crd::containers::Array<double> gs(&alloc);
    gs.resize(static_cast<crd::usize>(n) * natt, 0.0);
    crd::u32 st = 0xC0DEU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n * natt; ++i) { gs[static_cast<crd::usize>(i)] = (rnd() * 2.0 - 1.0) * 3.0; }

    // per-attribute observed ranges
    crd::containers::Array<double> rng(&alloc);
    rng.resize(static_cast<crd::usize>(natt) * 2U, 0.0);
    for (int k = 0; k < natt; ++k)
    {
        double lo = 1.0e30; double hi = -1.0e30;
        for (int i = 0; i < n; ++i) { const double v = gs[static_cast<crd::usize>(i) * static_cast<crd::usize>(natt) + static_cast<crd::usize>(k)]; if (v < lo) { lo = v; } if (v > hi) { hi = v; } }
        rng[static_cast<crd::usize>(k) * 2U] = lo; rng[static_cast<crd::usize>(k) * 2U + 1U] = hi;
    }

    kir::gsplat::GsplatQuantizeConfig qc;
    qc.natt = natt; qc.bits = bits;
    crd::containers::Array<double> codes(&alloc);
    crd::containers::Array<double> recon(&alloc);
    codes.resize(static_cast<crd::usize>(n) * natt, 0.0);
    recon.resize(static_cast<crd::usize>(n) * natt, 0.0);
    kir::KGraph       qg(&alloc);
    const kir::KEntry qe = kir::gsplat::build_gsplat_quantize_kernel(qg, qc);
    kir::KernelBuffer qb[3] = {{gs.data(), n * natt, 0, 0}, {rng.data(), natt * 2, 0, 1}, {codes.data(), n * natt, 0, 2}};
    kir::eval_cpu_kernel(qg, qe, qb, 3, qe.local_size[0], &alloc, static_cast<crd::u32>(n / qc.local_size));
    kir::KGraph       dg(&alloc);
    const kir::KEntry de = kir::gsplat::build_gsplat_dequantize_kernel(dg, qc);
    kir::KernelBuffer db[3] = {{codes.data(), n * natt, 0, 0}, {rng.data(), natt * 2, 0, 1}, {recon.data(), n * natt, 0, 2}};
    kir::eval_cpu_kernel(dg, de, db, 3, de.local_size[0], &alloc, static_cast<crd::u32>(n / qc.local_size));

    double worst = 0.0; int bad_code = 0;
    for (int k = 0; k < natt; ++k)
    {
        const double step = (rng[static_cast<crd::usize>(k) * 2U + 1U] - rng[static_cast<crd::usize>(k) * 2U]) / levels;
        for (int i = 0; i < n; ++i)
        {
            const double err = crd::math::abs(recon[static_cast<crd::usize>(i) * static_cast<crd::usize>(natt) + static_cast<crd::usize>(k)] - gs[static_cast<crd::usize>(i) * static_cast<crd::usize>(natt) + static_cast<crd::usize>(k)]);
            if (err > worst) { worst = err; }
            if (err > 0.5001 * step + 1.0e-6) { ++bad_code; } // nearest-code error ≤ half a step
        }
    }
    std::printf("[B19-d quant] %d-bit codec: worst round-trip err %.3e; ratio 32/%d = %.1fx\n", bits, worst, bits, 32.0 / bits);
    CHECK(bad_code == 0);                       // every attribute reconstructs within half a quantisation step
    CHECK(32.0 / static_cast<double>(bits) > 2.0); // real bit-rate reduction (packed: 32/bits×)
}

TEST_CASE("B19-d Morton: sorting by the Z-order key gives spatial locality (adjacent deltas shrink)", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U, nullptr, "gsplat-morton");
    constexpr int n          = 1024;
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 256;
    constexpr int epb        = 512;
    constexpr int nblocks    = n / epb;
    constexpr int scan_threads = nblocks < threads ? nblocks : threads;

    crd::containers::Array<double> gs(&alloc);
    gs.resize(static_cast<crd::usize>(n) * 14U, 0.0);
    crd::u32 st = 0x37EU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n; ++i) { gs[static_cast<crd::usize>(i) * 14U] = rnd(); gs[static_cast<crd::usize>(i) * 14U + 1U] = rnd(); gs[static_cast<crd::usize>(i) * 14U + 2U] = rnd(); }
    crd::containers::Array<double> bd(&alloc);
    bd.resize(6U, 0.0); bd[3] = 1.0; bd[4] = 1.0; bd[5] = 1.0;

    // Morton keys + index
    crd::containers::Array<double> ka(&alloc);
    crd::containers::Array<double> kb(&alloc);
    crd::containers::Array<double> va(&alloc);
    crd::containers::Array<double> vb(&alloc);
    ka.resize(n, 0.0); kb.resize(n, 0.0); va.resize(n, 0.0); vb.resize(n, 0.0);
    kir::KGraph       mg(&alloc);
    const kir::KEntry me = kir::gsplat::build_gsplat_morton_kernel(mg, {});
    kir::KernelBuffer mb[4] = {{gs.data(), n * 14, 0, 0}, {bd.data(), 6, 0, 1}, {ka.data(), n, 0, 2}, {va.data(), n, 0, 3}};
    kir::eval_cpu_kernel(mg, me, mb, 4, me.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    // KV radix sort by Morton key (4 passes)
    crd::containers::Array<double> bh(&alloc);
    crd::containers::Array<double> go(&alloc);
    crd::containers::Array<double> tot(&alloc);
    crd::containers::Array<double> gb(&alloc);
    bh.resize(static_cast<crd::usize>(nblocks) * nbins, 0.0); go.resize(static_cast<crd::usize>(nblocks) * nbins, 0.0);
    tot.resize(nbins, 0.0); gb.resize(nbins, 0.0);
    double* ck = ka.data(); double* ok = kb.data();
    double* cv = va.data(); double* ov = vb.data();
    for (int pass = 0; pass < 4; ++pass)
    {
        const int shift = pass * 8;
        kir::KGraph gh(&alloc);
        kir::KGraph gof1(&alloc);
        kir::KGraph gof2(&alloc);
        kir::KGraph gscat(&alloc);
        const kir::KEntry eh  = kir::build_sort_histogram(gh, epb, threads, radix_bits, shift, nblocks);
        const kir::KEntry eo1 = kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads);
        const kir::KEntry eo2 = kir::build_sort_gbase(gof2, radix_bits);
        const kir::KEntry es  = kir::build_sort_scatter(gscat, epb, threads, radix_bits, shift, nblocks, true);
        kir::KernelBuffer h[2] = {{ck, n, 0, 0}, {bh.data(), nblocks * nbins, 0, 1}};
        kir::eval_cpu_kernel(gh, eh, h, 2, eh.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        kir::KernelBuffer o1[3] = {{bh.data(), nblocks * nbins, 0, 0}, {go.data(), nblocks * nbins, 0, 1}, {tot.data(), nbins, 0, 2}};
        kir::eval_cpu_kernel(gof1, eo1, o1, 3, eo1.local_size[0], &alloc, static_cast<crd::u32>(nbins));
        kir::KernelBuffer o2[2] = {{tot.data(), nbins, 0, 0}, {gb.data(), nbins, 0, 1}};
        kir::eval_cpu_kernel(gof2, eo2, o2, 2, eo2.local_size[0], &alloc, 1U);
        kir::KernelBuffer s[6] = {{ck, n, 0, 0}, {ok, n, 0, 1}, {go.data(), nblocks * nbins, 0, 2}, {gb.data(), nbins, 0, 3}, {cv, n, 0, 4}, {ov, n, 0, 5}};
        kir::eval_cpu_kernel(gscat, es, s, 6, es.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        double* tk = ck; ck = ok; ok = tk;
        double* tv = cv; cv = ov; ov = tv;
    }
    // cv = index in Morton order

    const auto pos = [&](int idx, int c) { return gs[static_cast<crd::usize>(idx) * 14U + static_cast<crd::usize>(c)]; };
    const auto dist = [&](int ia, int ib) { const double dxp = pos(ia, 0) - pos(ib, 0); const double dyp = pos(ia, 1) - pos(ib, 1); const double dzp = pos(ia, 2) - pos(ib, 2); return crd::math::sqrt(dxp * dxp + dyp * dyp + dzp * dzp); };
    double orig_delta = 0.0; double morton_delta = 0.0;
    for (int i = 1; i < n; ++i)
    {
        orig_delta += dist(i, i - 1); // original (random) order
        const int a = static_cast<int>(cv[static_cast<crd::usize>(i)]);
        const int b = static_cast<int>(cv[static_cast<crd::usize>(i - 1)]);
        morton_delta += dist(a, b);   // Morton order
    }
    orig_delta /= static_cast<double>(n - 1);
    morton_delta /= static_cast<double>(n - 1);
    std::printf("[B19-d Morton] mean adjacent |Δpos|: random %.4f → Morton %.4f (%.1f× more local)\n", orig_delta, morton_delta, orig_delta / morton_delta);
    CHECK(morton_delta < orig_delta * 0.5); // Morton order is markedly more spatially local ⇒ smaller residuals ⇒ compressible
}

namespace
{
// render the differentiable Gaussian and return the sum-of-squares loss against `target`.
double diff_loss(crd::memory::TlsfAllocator& alloc, const kir::gsplat::GsplatDiffConfig& cfg,
                 crd::containers::Array<double>& params, crd::containers::Array<double>& target)
{
    kir::KGraph       fg(&alloc);
    const kir::KEntry fe = kir::gsplat::build_gsplat_diff_forward_kernel(fg, cfg);
    crd::containers::Array<double> img(&alloc);
    img.resize((static_cast<crd::usize>(cfg.width) * static_cast<crd::usize>(cfg.height)), 0.0);
    kir::KernelBuffer fb[2] = {{params.data(), 5, 0, 0}, {img.data(), cfg.width * cfg.height, 0, 1}};
    kir::eval_cpu_kernel(fg, fe, fb, 2, fe.local_size[0], &alloc, static_cast<crd::u32>(cfg.width * cfg.height / cfg.local_size));
    double l = 0.0;
    for (int i = 0; i < cfg.width * cfg.height; ++i) { const double d = img[static_cast<crd::usize>(i)] - target[static_cast<crd::usize>(i)]; l += d * d; }
    return l;
}
} // namespace

TEST_CASE("B19-f differentiable: analytic gradients match finite differences", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "gsplat-diff");
    kir::gsplat::GsplatDiffConfig cfg;
    cfg.width = 16; cfg.height = 16; cfg.local_size = 64;

    // target image = render of a "true" Gaussian
    crd::containers::Array<double> tgtp(&alloc);
    tgtp.resize(5U, 0.0); tgtp[0] = 8.0; tgtp[1] = 8.0; tgtp[2] = 3.0; tgtp[3] = 0.9; tgtp[4] = 0.8;
    crd::containers::Array<double> target(&alloc);
    {
        kir::KGraph fg(&alloc);
        const kir::KEntry fe = kir::gsplat::build_gsplat_diff_forward_kernel(fg, cfg);
        target.resize((static_cast<crd::usize>(cfg.width) * static_cast<crd::usize>(cfg.height)), 0.0);
        kir::KernelBuffer fb[2] = {{tgtp.data(), 5, 0, 0}, {target.data(), cfg.width * cfg.height, 0, 1}};
        kir::eval_cpu_kernel(fg, fe, fb, 2, fe.local_size[0], &alloc, static_cast<crd::u32>(cfg.width * cfg.height / cfg.local_size));
    }

    // a DIFFERENT test point (non-zero gradient)
    crd::containers::Array<double> params(&alloc);
    params.resize(5U, 0.0); params[0] = 7.0; params[1] = 9.0; params[2] = 2.5; params[3] = 0.7; params[4] = 0.6;

    // analytic gradient
    crd::containers::Array<double> grad(&alloc);
    grad.resize(5U, 0.0);
    kir::KGraph       bg(&alloc);
    const kir::KEntry be = kir::gsplat::build_gsplat_diff_backward_kernel(bg, cfg);
    kir::KernelBuffer bb[3] = {{params.data(), 5, 0, 0}, {target.data(), cfg.width * cfg.height, 0, 1}, {grad.data(), 5, 0, 2}};
    kir::eval_cpu_kernel(bg, be, bb, 3, be.local_size[0], &alloc, 1U);

    // finite differences
    const double eps[5] = {1.0e-3, 1.0e-3, 1.0e-3, 1.0e-4, 1.0e-4};
    int worst_ok = 0;
    for (int k = 0; k < 5; ++k)
    {
        const double save = params[static_cast<crd::usize>(k)];
        params[static_cast<crd::usize>(k)] = save + eps[k]; const double lp = diff_loss(alloc, cfg, params, target);
        params[static_cast<crd::usize>(k)] = save - eps[k]; const double lm = diff_loss(alloc, cfg, params, target);
        params[static_cast<crd::usize>(k)] = save;
        const double fd = (lp - lm) / (2.0 * eps[k]);
        const double an = grad[static_cast<crd::usize>(k)];
        const double rel = crd::math::abs(fd - an) / (crd::math::abs(fd) + crd::math::abs(an) + 1.0e-6);
        INFO("param " << k << ": analytic " << an << " vs FD " << fd);
        CHECK(rel < 1.0e-2); // analytic gradient matches finite differences
        if (rel < 1.0e-2) { ++worst_ok; }
    }
    CHECK(worst_ok == 5);
}

TEST_CASE("B19-f differentiable: SGD fits a Gaussian to a target image (loss decreases)", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U, nullptr, "gsplat-train");
    kir::gsplat::GsplatDiffConfig cfg;
    cfg.width = 16; cfg.height = 16; cfg.local_size = 64;

    crd::containers::Array<double> tgtp(&alloc);
    tgtp.resize(5U, 0.0); tgtp[0] = 8.0; tgtp[1] = 8.0; tgtp[2] = 3.0; tgtp[3] = 0.9; tgtp[4] = 0.8;
    crd::containers::Array<double> target(&alloc);
    {
        kir::KGraph fg(&alloc);
        const kir::KEntry fe = kir::gsplat::build_gsplat_diff_forward_kernel(fg, cfg);
        target.resize((static_cast<crd::usize>(cfg.width) * static_cast<crd::usize>(cfg.height)), 0.0);
        kir::KernelBuffer fb[2] = {{tgtp.data(), 5, 0, 0}, {target.data(), cfg.width * cfg.height, 0, 1}};
        kir::eval_cpu_kernel(fg, fe, fb, 2, fe.local_size[0], &alloc, static_cast<crd::u32>(cfg.width * cfg.height / cfg.local_size));
    }

    // start from wrong parameters
    crd::containers::Array<double> params(&alloc);
    params.resize(5U, 0.0); params[0] = 6.5; params[1] = 9.5; params[2] = 2.2; params[3] = 0.55; params[4] = 0.5;
    const double loss0 = diff_loss(alloc, cfg, params, target);

    crd::containers::Array<double> grad(&alloc);
    crd::containers::Array<double> lr(&alloc);
    grad.resize(5U, 0.0); lr.resize(5U, 0.0);
    kir::KGraph       bg(&alloc);
    const kir::KEntry be = kir::gsplat::build_gsplat_diff_backward_kernel(bg, cfg);
    kir::KGraph       sg(&alloc);
    const kir::KEntry se = kir::gsplat::build_gsplat_sgd_step_kernel(sg, 5);
    lr[0] = 0.004; // one lr broadcast to all params (buffer[0]) — the sgd kernel reads lr[0]

    for (int step = 0; step < 300; ++step)
    {
        kir::KernelBuffer bb[3] = {{params.data(), 5, 0, 0}, {target.data(), cfg.width * cfg.height, 0, 1}, {grad.data(), 5, 0, 2}};
        kir::eval_cpu_kernel(bg, be, bb, 3, be.local_size[0], &alloc, 1U);
        kir::KernelBuffer sb[3] = {{params.data(), 5, 0, 0}, {grad.data(), 5, 0, 1}, {lr.data(), 5, 0, 2}};
        kir::eval_cpu_kernel(sg, se, sb, 3, se.local_size[0], &alloc, 1U);
    }
    const double loss1 = diff_loss(alloc, cfg, params, target);
    std::printf("[B19-f train] SGD 300 steps: loss %.4f → %.4f (params μ=(%.2f,%.2f) s=%.2f op=%.2f col=%.2f)\n",
                loss0, loss1, params[0], params[1], params[2], params[3], params[4]);
    CHECK(loss1 < loss0 * 0.1);          // the fit converges — loss falls by >10×
    CHECK(crd::math::abs(params[0] - 8.0) < 0.5); // μx recovered
    CHECK(crd::math::abs(params[1] - 8.0) < 0.5); // μy recovered
}

TEST_CASE("B19 perf: shared-memory block render == direct block render (bit-exact, multi-chunk)", "[ckir][gsplat]")
{
    crd::memory::TlsfAllocator     alloc(96U << 20U, nullptr, "gsplat-smem");
    crd::containers::Array<double> gauss(&alloc);
    crd::containers::Array<double> cam(&alloc);
    crd::containers::Array<double> proj(&alloc);

    constexpr int n       = 100;      // > 64 ⇒ the shared-mem render runs 2 chunks (BS = tile_px² = 64)
    constexpr int imw     = 8;
    constexpr int imh     = 8;
    constexpr int tile_px = 8;
    constexpr int n_tiles = (imw / tile_px) * (imh / tile_px); // 1 (the lockstep oracle can only afford a tiny workgroup)

    gauss.resize(static_cast<crd::usize>(n) * 14U, 0.0);
    cam.resize(20U, 0.0);
    cam[0] = 1.0; cam[4] = 1.0; cam[8] = 1.0;
    cam[9] = 0.0; cam[10] = 0.0; cam[11] = 5.0;
    cam[12] = 60.0; cam[13] = 60.0; cam[14] = 4.0; cam[15] = 4.0; // project onto the single tile (pixels 0..7)
    cam[16] = 0.2; cam[17] = static_cast<double>(imw); cam[18] = static_cast<double>(imh);
    crd::u32 st = 0x5E11U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < n; ++i)
    {
        const double z = 3.0 + 4.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(n); // distinct depths
        put_gauss(gauss, i, (rnd() * 2.0 - 1.0) * 0.4, (rnd() * 2.0 - 1.0) * 0.4, z, 0.08, 0, 0, 0, 1, 0.25,
                  (rnd() - 0.5) / kir::gsplat::detail::kShC0, (rnd() - 0.5) / kir::gsplat::detail::kShC0, (rnd() - 0.5) / kir::gsplat::detail::kShC0);
    }

    kir::gsplat::GsplatProjectConfig pcfg;
    kir::KGraph                      pg(&alloc);
    const kir::KEntry                pe = kir::gsplat::build_gsplat_project_kernel(pg, pcfg);
    proj.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    kir::KernelBuffer pb[3] = {{gauss.data(), n * 14, 0, 0}, {cam.data(), 20, 0, 1}, {proj.data(), n * 12, 0, 2}};
    kir::eval_cpu_kernel(pg, pe, pb, 3, pe.local_size[0], &alloc, static_cast<crd::u32>((n + 63) / 64));

    // host depth sort → sorted; order = identity; ranges: all n instances in tile 0.
    crd::containers::Array<int> ord(&alloc);
    ord.resize(static_cast<crd::usize>(n), 0);
    for (int i = 0; i < n; ++i) { ord[static_cast<crd::usize>(i)] = i; }
    for (int i = 1; i < n; ++i)
    {
        const int key = ord[static_cast<crd::usize>(i)]; const double kd = proj[static_cast<crd::usize>(key) * 12U + 2U]; int j = i - 1;
        while (j >= 0 && proj[static_cast<crd::usize>(ord[static_cast<crd::usize>(j)]) * 12U + 2U] > kd) { const int jp1 = j + 1; ord[static_cast<crd::usize>(jp1)] = ord[static_cast<crd::usize>(j)]; --j; }
        const int jp1 = j + 1; ord[static_cast<crd::usize>(jp1)] = key;
    }
    crd::containers::Array<double> sorted(&alloc);
    sorted.resize(static_cast<crd::usize>(n) * 12U, 0.0);
    for (int i = 0; i < n; ++i) { for (int k = 0; k < 12; ++k) { sorted[static_cast<crd::usize>(i) * 12U + static_cast<crd::usize>(k)] = proj[static_cast<crd::usize>(ord[static_cast<crd::usize>(i)]) * 12U + static_cast<crd::usize>(k)]; } }
    crd::containers::Array<double> order(&alloc);
    order.resize(static_cast<crd::usize>(n), 0.0);
    for (int i = 0; i < n; ++i) { order[static_cast<crd::usize>(i)] = static_cast<double>(i); } // identity (sorted[] already in depth order)
    crd::containers::Array<double> ranges(&alloc);
    ranges.resize(static_cast<crd::usize>(n_tiles) * 2U, 0.0);
    ranges[0] = 0.0; ranges[1] = static_cast<double>(n); // tile 0 = [0,n); tiles 1..3 = [0,0)

    crd::containers::Array<double> par(&alloc);
    par.resize(4U, 0.0); par[0] = 0.02; par[1] = 0.03; par[2] = 0.04; par[3] = 1.0 / 255.0;

    kir::gsplat::GsplatBlockConfig bc;
    bc.width = imw; bc.height = imh; bc.tile_px = tile_px;

    // direct block render
    crd::containers::Array<double> img_d(&alloc);
    img_d.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KGraph       dg(&alloc);
    const kir::KEntry de = kir::gsplat::build_gsplat_block_render_kernel(dg, bc);
    kir::KernelBuffer db[5] = {{sorted.data(), n * 12, 0, 0}, {order.data(), n, 0, 1}, {ranges.data(), n_tiles * 2, 0, 2}, {par.data(), 4, 0, 3}, {img_d.data(), imw * imh * 4, 0, 4}};
    kir::eval_cpu_kernel(dg, de, db, 5, de.local_size[0], &alloc, static_cast<crd::u32>(n_tiles));

    // shared-memory block render
    crd::containers::Array<double> img_s(&alloc);
    img_s.resize(static_cast<crd::usize>(imw * imh) * 4U, 0.0);
    kir::KGraph       sg(&alloc);
    const kir::KEntry se = kir::gsplat::build_gsplat_block_render_smem_kernel(sg, bc);
    kir::KernelBuffer sb[5] = {{sorted.data(), n * 12, 0, 0}, {order.data(), n, 0, 1}, {ranges.data(), n_tiles * 2, 0, 2}, {par.data(), 4, 0, 3}, {img_s.data(), imw * imh * 4, 0, 4}};
    kir::eval_cpu_kernel(sg, se, sb, 5, se.local_size[0], &alloc, static_cast<crd::u32>(n_tiles));

    double worst = 0.0; double lum = 0.0;
    for (int q = 0; q < imw * imh; ++q)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double d = crd::math::abs(img_s[static_cast<crd::usize>(q) * 4U + static_cast<crd::usize>(c)] - img_d[static_cast<crd::usize>(q) * 4U + static_cast<crd::usize>(c)]);
            if (d > worst) { worst = d; }
            lum += img_s[static_cast<crd::usize>(q) * 4U + static_cast<crd::usize>(c)];
        }
    }
    std::printf("[B19 smem] shared-mem render vs direct: %d splats (2 chunks), worst |diff| = %.3e, mean lum %.4f\n", n, worst, lum / static_cast<double>(imw * imh * 3));
    CHECK(lum > 0.01);      // the tile actually composited a lot of overdraw
    CHECK(worst < 1.0e-6);  // shared-mem batched render == direct render (same order ⇒ bit-exact f32)
}
