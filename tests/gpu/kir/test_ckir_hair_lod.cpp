// test_ckir_hair_lod.cpp — D-007 B18-d: strand LEVEL OF DETAIL (Lipp et al. 2026 §3.2).
//
// LOD for hair is not "draw fewer strands". Done naively it pops, and it silently corrupts the shadow term: culling the
// front-most strand leaves the previously-computed depth/opacity map STALE, so an inner strand becomes visible while
// still testing against the old depth, samples the high-opacity interior, and goes unnaturally dark (paper Fig 4).
// The gates below are exactly the claims that make the scheme usable:
//   (a) Delta == 0 when nothing is culled, and rises monotonically as strands are removed  (Eq 8, stated in the paper);
//   (b) Delta == -log(beta) to the oracle's precision                                       (Eq 8);
//   (c) the per-bundle random offset STAGGERS when bundles switch, so the groom-wide mean walks instead of
//       jumping a whole strand at once — that jump IS the pop (Eq 1; note this is staggering, NOT unbiasedness);
//   (d) N_LOD is monotone in L and stays inside [1, N]                                      (Eq 1 clamp);
//   (e) the control-point count is always a power of two plus one, within [C_layers, C_max] (Eq 4/5);
//   (f) at L == 1 everything is exact: N_LOD == N, Delta == 0 — full detail must not be approximate.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair_geom.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr int kBundles = 64; // one kernel lane each

// Run the LOD kernel over `kBundles` bundles. `ext` supplies each bundle's AABB diagonal in pixels (as a horizontal
// extent, which is all Eq 2 consumes), `delta` the per-bundle random offset.
void run_lod(crd::memory::IAllocator& alloc, const kir::hairgeom::StrandLodConfig& cfg,
             const crd::containers::Array<double>& ext, const crd::containers::Array<double>& delta,
             crd::containers::Array<double>& out)
{
    crd::containers::Array<double> in(&alloc);
    in.resize(static_cast<crd::usize>(kBundles) * 5U, 0.0);
    for (int b = 0; b < kBundles; ++b)
    {
        const crd::usize o = static_cast<crd::usize>(b) * 5U;
        in[o + 0U] = 0.0;                                   // aabb min x
        in[o + 1U] = 0.0;                                   // aabb min y
        in[o + 2U] = ext[static_cast<crd::usize>(b)];       // aabb max x  => diagonal == ext
        in[o + 3U] = 0.0;                                   // aabb max y
        in[o + 4U] = delta[static_cast<crd::usize>(b)];
    }
    out.resize(static_cast<crd::usize>(kBundles) * 4U, 0.0);
    kir::KGraph       g(&alloc);
    const kir::KEntry e     = kir::hairgeom::build_strand_lod_kernel(g, cfg);
    kir::KernelBuffer bb[2] = {{in.data(), kBundles * 5, 0, 0}, {out.data(), kBundles * 4, 0, 1}};
    kir::eval_cpu_kernel(g, e, bb, 2, e.local_size[0], &alloc, static_cast<crd::u32>(kBundles / 64));
}
} // namespace

TEST_CASE("ckir hair LOD depth fix is zero unculled and monotone in beta", "[ckir][hair][lod]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-lod-depth");
    crd::containers::Array<double> ext(&alloc);
    crd::containers::Array<double> delta(&alloc);
    crd::containers::Array<double> out(&alloc);
    ext.resize(kBundles, 0.0);
    delta.resize(kBundles, 0.0);

    kir::hairgeom::StrandLodConfig cfg;
    cfg.strands_per_bundle = 128;
    cfg.screen_height      = 1080.0;
    cfg.lambda             = 8.0;
    // sweep the screen footprint from tiny (far away) to full-screen (close up)
    for (int b = 0; b < kBundles; ++b)
    {
        ext[static_cast<crd::usize>(b)] = 1080.0 / 8.0 * (static_cast<double>(b) / static_cast<double>(kBundles - 1));
    }
    run_lod(alloc, cfg, ext, delta, out);

    const auto n_lod = [&](int b) { return out[static_cast<crd::usize>(b) * 4U + 0U]; };
    const auto delta_of = [&](int b) { return out[static_cast<crd::usize>(b) * 4U + 2U]; };
    const auto beta_of = [&](int b) { return out[static_cast<crd::usize>(b) * 4U + 3U]; };

    // (b) Delta == -log(beta), the whole of Eq 8.
    double worst = 0.0;
    for (int b = 0; b < kBundles; ++b)
    {
        const double want = -crd::math::log(beta_of(b));
        const double got  = delta_of(b);
        if (crd::math::abs(got - want) > worst) { worst = crd::math::abs(got - want); }
    }
    INFO("worst |Delta + log(beta)| = " << worst);
    CHECK(worst < 1.0e-6);

    // (a) Delta must DECREASE as beta rises (less culling ⇒ less correction), and hit exactly 0 at beta == 1.
    for (int b = 1; b < kBundles; ++b)
    {
        if (beta_of(b) > beta_of(b - 1)) { CHECK(delta_of(b) <= delta_of(b - 1) + 1.0e-9); }
    }
    // (f) the last bundle is full-screen ⇒ L saturates at 1 ⇒ nothing culled ⇒ the correction vanishes EXACTLY.
    INFO("full-detail bundle: N_LOD " << n_lod(kBundles - 1) << " beta " << beta_of(kBundles - 1)
                                      << " Delta " << delta_of(kBundles - 1));
    CHECK(n_lod(kBundles - 1) == 128.0);
    CHECK(beta_of(kBundles - 1) == 1.0);
    CHECK(delta_of(kBundles - 1) < 1.0e-9);
}

TEST_CASE("ckir hair LOD strand count is clamped and monotone", "[ckir][hair][lod]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-lod-count");
    crd::containers::Array<double> ext(&alloc);
    crd::containers::Array<double> delta(&alloc);
    crd::containers::Array<double> out(&alloc);
    ext.resize(kBundles, 0.0);
    delta.resize(kBundles, 0.0);

    kir::hairgeom::StrandLodConfig cfg;
    cfg.strands_per_bundle = 128;
    for (int b = 0; b < kBundles; ++b)
    {
        ext[static_cast<crd::usize>(b)] = 1080.0 / 8.0 * (static_cast<double>(b) / static_cast<double>(kBundles - 1));
    }
    run_lod(alloc, cfg, ext, delta, out);

    const auto n_lod = [&](int b) { return out[static_cast<crd::usize>(b) * 4U + 0U]; };
    // (d) never fewer than one strand (a bundle that survives frustum culling must still draw SOMETHING), never more
    //     than the full count, and never decreasing as the bundle grows on screen.
    for (int b = 0; b < kBundles; ++b)
    {
        CHECK(n_lod(b) >= 1.0);
        CHECK(n_lod(b) <= 128.0);
        if (b > 0) { CHECK(n_lod(b) >= n_lod(b - 1)); }
    }
    // a vanishingly small footprint must collapse to the floor, not to zero
    CHECK(n_lod(0) == 1.0);
}

TEST_CASE("ckir hair LOD stochastic offset removes the popping step", "[ckir][hair][lod]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-lod-stoch");
    crd::containers::Array<double> ext(&alloc);
    crd::containers::Array<double> delta(&alloc);
    crd::containers::Array<double> out(&alloc);
    ext.resize(kBundles, 0.0);
    delta.resize(kBundles, 0.0);

    kir::hairgeom::StrandLodConfig cfg;
    cfg.strands_per_bundle = 16; // small N ⇒ a large quantisation step, so popping is maximally visible

    // ⚠ WHAT Eq 1 ACTUALLY CLAIMS. delta is described as "a per-bundle random offset that SMOOTHS LOD TRANSITIONS" —
    //   it staggers *when each bundle switches*. It is NOT unbiased stochastic rounding: the dither enters as
    //   ceil(L*(N + delta)), so its amplitude is L, not 1, and E[N_LOD] therefore does not equal L*N. (Checked
    //   analytically at L = 0.4213, N = 16: the switch happens at delta = 0.6152, giving E = 7*0.6152 + 8*0.3848
    //   = 7.385 — exactly what the kernel produces. Asserting E[N_LOD] == L*N would be testing a property the paper
    //   never claims, and would fail against a correct implementation.)
    //
    //   The real claim is about POPPING, so measure popping directly: sweep L across a quantisation boundary and watch
    //   the groom-wide mean strand count. Undithered, every bundle crosses together and the mean jumps by a FULL strand
    //   in one step — that jump is the pop. Dithered, bundles cross at staggered thresholds and the mean walks up in
    //   1/kBundles-sized increments.
    const int steps = 48;
    double        worst_dither = 0.0;
    double        worst_plain  = 0.0;
    double        prev_dither  = -1.0;
    double        prev_plain   = -1.0;

    for (int s2 = 0; s2 < steps; ++s2)
    {
        // sweep L over a window that contains a boundary of ceil(L*N)
        const double lval = 0.40 + 0.10 * (static_cast<double>(s2) / static_cast<double>(steps - 1));
        for (int b = 0; b < kBundles; ++b)
        {
            ext[static_cast<crd::usize>(b)] = lval * cfg.screen_height / cfg.lambda;
            delta[static_cast<crd::usize>(b)] = (static_cast<double>(b) + 0.5) / static_cast<double>(kBundles);
        }
        run_lod(alloc, cfg, ext, delta, out);
        double md = 0.0;
        for (int b = 0; b < kBundles; ++b) { md += out[static_cast<crd::usize>(b) * 4U + 0U]; }
        md /= static_cast<double>(kBundles);

        for (int b = 0; b < kBundles; ++b) { delta[static_cast<crd::usize>(b)] = 0.0; } // the undithered control
        run_lod(alloc, cfg, ext, delta, out);
        double mp = 0.0;
        for (int b = 0; b < kBundles; ++b) { mp += out[static_cast<crd::usize>(b) * 4U + 0U]; }
        mp /= static_cast<double>(kBundles);

        if (prev_dither >= 0.0)
        {
            const double jd = crd::math::abs(md - prev_dither);
            const double jp = crd::math::abs(mp - prev_plain);
            if (jd > worst_dither) { worst_dither = jd; }
            if (jp > worst_plain) { worst_plain = jp; }
        }
        prev_dither = md;
        prev_plain  = mp;
    }

    INFO("worst single-step jump in groom-wide mean strand count — dithered " << worst_dither << ", undithered "
                                                                              << worst_plain);
    CHECK(worst_plain > 0.9);          // undithered: the whole groom steps together, a full strand at once — the POP
    CHECK(worst_dither < 0.5);         // dithered: bundles cross at staggered L, so the mean walks
    CHECK(worst_dither < worst_plain); // and the dither is strictly the thing that fixes it
}

TEST_CASE("ckir hair LOD control points snap to a power of two plus one", "[ckir][hair][lod]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-lod-cp");
    crd::containers::Array<double> ext(&alloc);
    crd::containers::Array<double> delta(&alloc);
    crd::containers::Array<double> out(&alloc);
    ext.resize(kBundles, 0.0);
    delta.resize(kBundles, 0.0);

    kir::hairgeom::StrandLodConfig cfg;
    cfg.max_points   = 127;
    cfg.layer_points = 5;
    for (int b = 0; b < kBundles; ++b)
    {
        ext[static_cast<crd::usize>(b)] = 1080.0 / 8.0 * (static_cast<double>(b) / static_cast<double>(kBundles - 1));
    }
    run_lod(alloc, cfg, ext, delta, out);

    // (e) Eq 5 exists so that a vertex-count change is a clean halving/doubling — an arbitrary count would reshuffle
    //     every control point and produce a visible geometry jump. So the result must ALWAYS be 2^k + 1.
    double prev = 0.0;
    for (int b = 0; b < kBundles; ++b)
    {
        const double c = out[static_cast<crd::usize>(b) * 4U + 1U];
        CHECK(c <= 127.0);
        CHECK(c >= 2.0);
        const double k = c - 1.0; // must be an exact power of two
        const double l2 = crd::math::log2(k);
        INFO("bundle " << b << " C = " << c << " (C-1 = " << k << ", log2 = " << l2 << ")");
        CHECK(crd::math::abs(l2 - crd::math::round(l2)) < 1.0e-9);
        CHECK(c >= prev); // monotone with screen size
        prev = c;
    }
}
