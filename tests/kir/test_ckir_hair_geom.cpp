// test_ckir_hair_geom.cpp — D-007 B18-d: hair strand GEOMETRY (crd::kir::hairgeom).
//
// The hair-mesh representation's whole promise is that strands need never be stored: a coarse layered bundle plus a (u,v) is
// enough to regenerate any strand exactly. The gates below are the claims that promise rests on — corner strands INTERPOLATE
// the bundle's vertex chain (a Catmull-Rom passes through its control points, unlike a B-spline), interior strands stay inside
// the bundle, tangents are unit and follow the curve, and the styling operator perturbs without destroying the base shape.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair_geom.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr int kLayers  = 4;
constexpr int kPoints  = 8;
constexpr int kStrands = 64;

constexpr double kPiG = crd::kir::hairgeom::kPi;

[[nodiscard]] double sq(double x) { return x * x; }

// A bundle: 4 stacked quad layers rising in +z, each shrinking slightly and drifting in +x — i.e. a tapered, leaning tuft.
void make_bundle(crd::containers::Array<double>& lay)
{
    lay.resize(static_cast<crd::usize>(kLayers) * 4U * 3U, 0.0);
    for (int i = 0; i < kLayers; ++i)
    {
        const double t     = static_cast<double>(i) / static_cast<double>(kLayers - 1);
        const double shrink = 1.0 - 0.4 * t;   // taper toward the tip
        const double drift  = 0.8 * t * t;     // lean in +x
        const double z      = 2.0 * t;
        const double corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int c = 0; c < 4; ++c)
        {
            const crd::usize o = static_cast<crd::usize>(i * 4 + c) * 3U;
            lay[o + 0U] = corner[c][0] * shrink + drift;
            lay[o + 1U] = corner[c][1] * shrink;
            lay[o + 2U] = z;
        }
    }
}

void gen_strands(crd::memory::IAllocator& alloc, const kir::hairgeom::StrandGenConfig& cfg,
                 crd::containers::Array<double>& lay, crd::containers::Array<double>& uv,
                 crd::containers::Array<double>& out)
{
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hairgeom::build_strand_gen_kernel(g, cfg);
    out.resize(static_cast<crd::usize>(kStrands) * static_cast<crd::usize>(cfg.points) * 6U, 0.0);
    kir::KernelBuffer b[3] = {{lay.data(), kLayers * 4 * 3, 0, 0},
                              {uv.data(), kStrands * 2, 0, 1},
                              {out.data(), kStrands * cfg.points * 6, 0, 2}};
    kir::eval_cpu_kernel(g, e, b, 3, e.local_size[0], &alloc, static_cast<crd::u32>(kStrands / 64));
}
} // namespace

// B18-d: strand generation from a hair mesh. The decisive gate is INTERPOLATION — a strand placed exactly at a bundle corner
// (u,v ∈ {0,1}²) must pass THROUGH that corner's vertex on every layer. That is true for Catmull-Rom and false for a
// B-spline/approximating curve, so it proves both the bilinear placement and the curve choice in one assertion. Without it, a
// groom silently shrinks inside its authored cage.
TEST_CASE("B18-d: hair-mesh strands interpolate the bundle corners and stay inside the cage", "[kir][hair][geom]")
{
    crd::memory::TlsfAllocator     alloc(128U << 20U);
    crd::containers::Array<double> lay(&alloc);
    crd::containers::Array<double> uv(&alloc);
    crd::containers::Array<double> out(&alloc);
    make_bundle(lay);

    kir::hairgeom::StrandGenConfig cfg;
    cfg.layers = kLayers;
    cfg.points = kPoints;

    // strand 0..3 sit exactly on the four corners; the rest scatter across the face
    uv.resize(static_cast<crd::usize>(kStrands) * 2U, 0.0);
    const double cuv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int c = 0; c < 4; ++c)
    {
        uv[static_cast<crd::usize>(c) * 2U + 0U] = cuv[c][0];
        uv[static_cast<crd::usize>(c) * 2U + 1U] = cuv[c][1];
    }
    crd::u32 s   = 7777U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 4; i < kStrands; ++i)
    {
        uv[static_cast<crd::usize>(i) * 2U + 0U] = rnd();
        uv[static_cast<crd::usize>(i) * 2U + 1U] = rnd();
    }
    gen_strands(alloc, cfg, lay, uv, out);

    const auto p = [&](int strand, int j, int c) {
        return out[(static_cast<crd::usize>(strand) * kPoints + static_cast<crd::usize>(j)) * 6U + static_cast<crd::usize>(c)];
    };
    const auto t = [&](int strand, int j, int c) {
        return out[(static_cast<crd::usize>(strand) * kPoints + static_cast<crd::usize>(j)) * 6U + 3U + static_cast<crd::usize>(c)];
    };

    // ⭐ INTERPOLATION: corner strand c must pass through vertex c of every layer. With kPoints=8 and kLayers=4, sample j
    // lands on layer i exactly when w = j/7 hits i/3 — i.e. j = 0 (layer 0) and j = 7 (layer 3); the endpoints are the
    // unambiguous ones, so assert those exactly.
    for (int c = 0; c < 4; ++c)
    {
        for (int lyr : {0, kLayers - 1})
        {
            const int        j = (lyr == 0) ? 0 : kPoints - 1;
            const crd::usize o = static_cast<crd::usize>(lyr * 4 + c) * 3U;
            INFO("corner c=" << c << " layer=" << lyr << " strand=(" << p(c, j, 0) << "," << p(c, j, 1) << "," << p(c, j, 2)
                             << ")  vertex=(" << lay[o] << "," << lay[o + 1U] << "," << lay[o + 2U] << ")");
            CHECK(p(c, j, 0) == Catch::Approx(lay[o + 0U]).margin(1.0e-5));
            CHECK(p(c, j, 1) == Catch::Approx(lay[o + 1U]).margin(1.0e-5));
            CHECK(p(c, j, 2) == Catch::Approx(lay[o + 2U]).margin(1.0e-5));
        }
    }

    // every strand: unit tangents, monotone rise in z (the bundle extrudes upward), and inside the cage in x/y
    for (int i = 0; i < kStrands; ++i)
    {
        double prev_z = -1.0e30;
        for (int j = 0; j < kPoints; ++j)
        {
            const double tl = crd::math::sqrt(t(i, j, 0) * t(i, j, 0) + t(i, j, 1) * t(i, j, 1) + t(i, j, 2) * t(i, j, 2));
            CHECK(tl == Catch::Approx(1.0).epsilon(1.0e-4)); // tangents are normalized — the BCSDF frame depends on it
            CHECK(p(i, j, 2) >= prev_z - 1.0e-6);            // strands rise along the extrusion
            prev_z = p(i, j, 2);
            CHECK(crd::math::abs(p(i, j, 0)) < 2.2);          // stays within the authored cage (max |x| = 1 + drift 0.8)
            CHECK(crd::math::abs(p(i, j, 1)) < 1.2);
        }
        CHECK(p(i, kPoints - 1, 2) > p(i, 0, 2) + 1.0); // root → tip actually spans the bundle height
    }

    // distinct (u,v) must give distinct strands — a collapse here would mean the bilinear placement was dropped
    double maxsep = 0.0;
    for (int i = 4; i < kStrands; ++i)
    {
        const double d = crd::math::abs(p(i, 0, 0) - p(4, 0, 0)) + crd::math::abs(p(i, 0, 1) - p(4, 0, 1));
        if (d > maxsep) { maxsep = d; }
    }
    INFO("max root separation across random (u,v) = " << maxsep);
    CHECK(maxsep > 0.5);
}

// B18-d: the procedural STYLING operator (Yuksel §3.4). Curl must (a) be a genuine perturbation — zero amplitude reproduces the
// base curve EXACTLY, so styling can never silently corrupt authored geometry — and (b) displace by about the requested
// amplitude, tapering toward the tip when asked.
TEST_CASE("B18-d: strand styling perturbs without destroying the base curve", "[kir][hair][geom]")
{
    crd::memory::TlsfAllocator     alloc(128U << 20U);
    crd::containers::Array<double> lay(&alloc);
    crd::containers::Array<double> uv(&alloc);
    crd::containers::Array<double> base(&alloc);
    crd::containers::Array<double> curled(&alloc);
    crd::containers::Array<double> zero_amp(&alloc);
    make_bundle(lay);
    uv.resize(static_cast<crd::usize>(kStrands) * 2U, 0.0);
    for (int i = 0; i < kStrands; ++i)
    {
        uv[static_cast<crd::usize>(i) * 2U + 0U] = 0.5;
        uv[static_cast<crd::usize>(i) * 2U + 1U] = 0.5;
    }

    kir::hairgeom::StrandGenConfig cfg;
    cfg.layers = kLayers;
    cfg.points = kPoints;
    gen_strands(alloc, cfg, lay, uv, base);

    kir::hairgeom::StrandGenConfig cz = cfg;
    cz.curl_amp = 0.0; // explicitly zero ⇒ must be bit-identical to the base path
    gen_strands(alloc, cz, lay, uv, zero_amp);

    kir::hairgeom::StrandGenConfig cc = cfg;
    cc.curl_amp  = 0.15;
    cc.curl_freq = 4.0;
    cc.taper     = 0.0; // curl fades to a straight tip
    gen_strands(alloc, cc, lay, uv, curled);

    const auto p = [](const crd::containers::Array<double>& a, int j, int c) {
        return a[static_cast<crd::usize>(j) * 6U + static_cast<crd::usize>(c)];
    };
    double maxdiff = 0.0;
    double maxcurl = 0.0;
    for (int j = 0; j < kPoints; ++j)
    {
        for (int c = 0; c < 3; ++c)
        {
            maxdiff = crd::math::abs(p(zero_amp, j, c) - p(base, j, c)) > maxdiff
                          ? crd::math::abs(p(zero_amp, j, c) - p(base, j, c)) : maxdiff;
        }
        const double d = crd::math::sqrt(sq(p(curled, j, 0) - p(base, j, 0)) + sq(p(curled, j, 1) - p(base, j, 1))
                                         + sq(p(curled, j, 2) - p(base, j, 2)));
        if (d > maxcurl) { maxcurl = d; }
    }
    INFO("zero-amplitude deviation = " << maxdiff << " (must be 0)  |  max curl displacement = " << maxcurl
                                       << " (requested amplitude 0.15)");
    CHECK(maxdiff == 0.0);            // (a) styling is strictly opt-in
    CHECK(maxcurl > 0.02);            // (b) it actually displaces ...
    CHECK(maxcurl < 0.25);            //     ... by about the requested amount, not wildly
    // taper=0 ⇒ the curl must fade toward the tip: the last sample deviates less than a mid-strand one
    const double tip = crd::math::abs(p(curled, kPoints - 1, 0) - p(base, kPoints - 1, 0));
    const double mid = crd::math::abs(p(curled, kPoints / 2, 0) - p(base, kPoints / 2, 0));
    INFO("tip deviation " << tip << " vs mid " << mid);
    CHECK(tip <= mid + 1.0e-6);
}

// B18-d: the 64-bit compressed strand G-buffer. Deferred hair shading is only viable if every shading input survives this
// round trip, so the gates are the quantisation budgets themselves:
//   depth 24b over the far plane, tangent 16b octahedral, uvw 6b each, AO 6b — and, decisively, DEPTH MUST ORDER: the packed
//   24-bit depth field is monotone in true depth, which is what lets a plain integer atomicMin double as the depth test.
TEST_CASE("B18-d: the 64-bit strand G-buffer round-trips within budget and orders by depth", "[kir][hair][geom][gbuffer]")
{
    crd::memory::TlsfAllocator     alloc(128U << 20U);
    constexpr int                  k_n = 64;
    constexpr double               k_far = 1000.0;
    crd::containers::Array<double> in(&alloc);
    crd::containers::Array<double> out(&alloc);
    in.resize(static_cast<crd::usize>(k_n) * 8U, 0.0);

    crd::u32 s   = 24680U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    crd::containers::Array<double> tx(&alloc);
    crd::containers::Array<double> ty(&alloc);
    crd::containers::Array<double> tz(&alloc);
    tx.resize(k_n, 0.0); ty.resize(k_n, 0.0); tz.resize(k_n, 0.0);
    for (int i = 0; i < k_n; ++i)
    {
        // random UNIT tangents covering the whole sphere (octahedral encoding must handle both hemispheres)
        const double z   = rnd() * 2.0 - 1.0;
        const double phi = rnd() * 2.0 * kPiG;
        const double r   = crd::math::sqrt(crd::math::abs(1.0 - z * z));
        const crd::usize o = static_cast<crd::usize>(i) * 8U;
        in[o + 0U] = (static_cast<double>(i) + 0.5) / static_cast<double>(k_n) * k_far; // strictly increasing depth
        in[o + 1U] = r * crd::math::cos(phi);
        in[o + 2U] = r * crd::math::sin(phi);
        in[o + 3U] = z;
        in[o + 4U] = rnd();
        in[o + 5U] = rnd();
        in[o + 6U] = rnd();
        in[o + 7U] = rnd();
        tx[static_cast<crd::usize>(i)] = in[o + 1U];
        ty[static_cast<crd::usize>(i)] = in[o + 2U];
        tz[static_cast<crd::usize>(i)] = in[o + 3U];
    }

    kir::hairgeom::StrandGBufferConfig gc;
    gc.far_plane = k_far;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hairgeom::build_strand_gbuffer_kernel(g, gc);
    out.resize(static_cast<crd::usize>(k_n) * 9U, 0.0);
    kir::KernelBuffer b[2] = {{in.data(), k_n * 8, 0, 0}, {out.data(), k_n * 9, 0, 1}};
    kir::eval_cpu_kernel(g, e, b, 2, e.local_size[0], &alloc, 1U);

    const auto out_at = [&](int i, int c) { return out[static_cast<crd::usize>(i) * 9U + static_cast<crd::usize>(c)]; };
    double worst_depth = 0.0;
    double worst_ang = 0.0;
    double worst_uvw = 0.0;
    double worst_ao = 0.0;
    double prev_bits   = -1.0;
    for (int i = 0; i < k_n; ++i)
    {
        const crd::usize o = static_cast<crd::usize>(i) * 8U;
        worst_depth = crd::math::abs(out_at(i, 0) - in[o + 0U]) > worst_depth ? crd::math::abs(out_at(i, 0) - in[o + 0U]) : worst_depth;
        // tangent error as an ANGLE (what actually matters for shading), via the dot product of unit vectors
        const double d   = out_at(i, 1) * tx[static_cast<crd::usize>(i)] + out_at(i, 2) * ty[static_cast<crd::usize>(i)]
                         + out_at(i, 3) * tz[static_cast<crd::usize>(i)];
        const double ang = crd::math::acos(crd::math::clamp(d, -1.0, 1.0));
        if (ang > worst_ang) { worst_ang = ang; }
        for (int c = 0; c < 3; ++c)
        {
            const double e2 = crd::math::abs(out_at(i, 4 + c) - in[o + 4U + static_cast<crd::usize>(c)]);
            if (e2 > worst_uvw) { worst_uvw = e2; }
        }
        const double ea = crd::math::abs(out_at(i, 7) - in[o + 7U]);
        if (ea > worst_ao) { worst_ao = ea; }
        // ⭐ depth bits must be strictly monotone — this is what makes an integer atomicMin a valid depth test
        CHECK(out_at(i, 8) > prev_bits);
        prev_bits = out_at(i, 8);
    }
    INFO("round-trip: depth " << worst_depth << " (budget " << k_far / 16777216.0 << ")  tangent angle " << worst_ang
                              << " rad  uvw " << worst_uvw << " (budget " << 0.5 / 63.0 << ")  ao " << worst_ao);
    CHECK(worst_depth < k_far / 16777216.0 * 2.0); // 24-bit depth over the far plane
    CHECK(worst_ang < 0.02);                      // 16-bit octahedral tangent ⇒ ~1 degree worst case
    CHECK(worst_uvw <= 0.5 / 63.0 + 1.0e-6);      // 6 bits per styling coordinate
    CHECK(worst_ao <= 0.5 / 63.0 + 1.0e-6);       // 6-bit AO
}
