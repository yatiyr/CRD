// test_ckir_lss.cpp — D-007 B18-f: LINEAR SWEPT SPHERES, the ray-traced strand primitive.
//
// This is geometry, so it can be pinned against CLOSED-FORM answers rather than tolerances. Every gate below is an
// intersection whose exact distance is known by hand, which is what makes them able to catch a subtly-wrong quadratic —
// the failure mode that a "looks about right" image test sails straight past.
//
//   (a) a ray down the axis of a CAPSULE hits the cap at exactly (dist - r);
//   (b) a perpendicular ray through the middle of a capsule hits at exactly (dist - r), independent of where along
//       the segment it crosses — that is the defining property of a SWEPT sphere and the thing a cone term gets wrong;
//   (c) a ray passing farther than the radius MISSES — no false positives, which matter more than false negatives here
//       because a spurious hit is an opaque black speck in mid-air;
//   (d) the tapered case: radius varies linearly along the segment, so a perpendicular ray at parameter u must hit at
//       exactly dist - (ra + u*(rb - ra));
//   (e) END CAPS are present — a ray aimed just past the segment end must still hit the hemisphere. Without caps a groom
//       shows pinholes exactly at the joints between segments, the most visible place they could possibly appear;
//   (f) the returned axial coordinate u is correct, since shading reads the tangent and strand-v from it;
//   (g) the AABB is CONSERVATIVE — it must contain the whole swept volume, or traversal never offers the shader the hit.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_lss.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr int kLanes = 64;

// Trace `nseg` segments with one ray per lane. Rays are [ox,oy,oz,dx,dy,dz]; segments [ax,ay,az,ra,bx,by,bz,rb].
// `rays`/`segs` are non-const because KernelBuffer holds a mutable pointer (buffers are read-write to the evaluator);
// they are only read here.
void trace(crd::memory::IAllocator& alloc, int nseg, crd::containers::Array<double>& rays,
           crd::containers::Array<double>& segs, crd::containers::Array<double>& out)
{
    kir::lss::LssTraceConfig cfg;
    cfg.segments = nseg;
    out.resize(static_cast<crd::usize>(kLanes) * 2U, 0.0);
    kir::KGraph       g(&alloc);
    const kir::KEntry e     = kir::lss::build_lss_trace_kernel(g, cfg);
    kir::KernelBuffer bb[3] = {{rays.data(), kLanes * 6, 0, 0},
                               {segs.data(), nseg * 8, 0, 1},
                               {out.data(), kLanes * 2, 0, 2}};
    kir::eval_cpu_kernel(g, e, bb, 3, e.local_size[0], &alloc, 1U);
}

void one_segment(crd::containers::Array<double>& segs, double ax, double ay, double az, double ra, double bx, double by,
                 double bz, double rb)
{
    segs.resize(8U, 0.0);
    segs[0U] = ax; segs[1U] = ay; segs[2U] = az; segs[3U] = ra;
    segs[4U] = bx; segs[5U] = by; segs[6U] = bz; segs[7U] = rb;
}

void set_ray(crd::containers::Array<double>& rays, int i, double ox, double oy, double oz, double dx, double dy, double dz)
{
    const crd::usize o = static_cast<crd::usize>(i) * 6U;
    rays[o + 0U] = ox; rays[o + 1U] = oy; rays[o + 2U] = oz;
    rays[o + 3U] = dx; rays[o + 4U] = dy; rays[o + 5U] = dz;
}
} // namespace

TEST_CASE("ckir LSS capsule hits at the closed-form distance", "[ckir][lss]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "lss-capsule");
    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> out(&alloc);
    rays.resize(static_cast<crd::usize>(kLanes) * 6U, 0.0);

    // a capsule along +x from (0,0,0) to (4,0,0), constant radius 0.5
    const double r = 0.5;
    one_segment(segs, 0.0, 0.0, 0.0, r, 4.0, 0.0, 0.0, r);

    // (b) PERPENDICULAR rays crossing at several points along the axis. For a swept SPHERE the surface is everywhere at
    //     distance r from the axis, so the hit distance must be (dist - r) at EVERY crossing point — invariant along the
    //     segment. A cone term with a sign error still passes at the midpoint and fails away from it, which is why this
    //     sweeps rather than testing one ray.
    for (int i = 0; i < 8; ++i)
    {
        const double x = 0.5 + 0.4 * static_cast<double>(i); // stay clear of the caps
        set_ray(rays, i, x, -3.0, 0.0, 0.0, 1.0, 0.0);       // fire +y from 3 units away
    }
    // (c) MISSES: same geometry but offset in z by more than the radius
    for (int i = 8; i < 12; ++i)
    {
        set_ray(rays, i, 2.0, -3.0, r + 0.05 + 0.1 * static_cast<double>(i - 8), 0.0, 1.0, 0.0);
    }
    // (a) down the axis from -x: the near cap is a sphere of radius r centred at the origin
    set_ray(rays, 12, -5.0, 0.0, 0.0, 1.0, 0.0, 0.0);
    // (e) just PAST the far end — must still hit the end-cap hemisphere
    set_ray(rays, 13, 4.0 + r * 0.5, -3.0, 0.0, 0.0, 1.0, 0.0);
    // and clearly beyond the cap: a genuine miss
    set_ray(rays, 14, 4.0 + r + 0.2, -3.0, 0.0, 0.0, 1.0, 0.0);

    trace(alloc, 1, rays, segs, out);
    const auto hit_t = [&](int i) { return out[static_cast<crd::usize>(i) * 2U + 0U]; };
    const auto hit_u = [&](int i) { return out[static_cast<crd::usize>(i) * 2U + 1U]; };

    for (int i = 0; i < 8; ++i)
    {
        const double want = 3.0 - r; // origin 3 units from the axis, surface r from it
        INFO("perpendicular ray " << i << ": t = " << hit_t(i) << " want " << want);
        CHECK(crd::math::abs(hit_t(i) - want) < 1.0e-6);
        // (f) the axial coordinate must equal the crossing point's parameter along the segment
        const double want_u = (0.5 + 0.4 * static_cast<double>(i)) / 4.0;
        INFO("  u = " << hit_u(i) << " want " << want_u);
        CHECK(crd::math::abs(hit_u(i) - want_u) < 1.0e-5);
    }
    for (int i = 8; i < 12; ++i)
    {
        INFO("ray " << i << " passes at z = " << (r + 0.05 + 0.1 * static_cast<double>(i - 8)) << ", must MISS");
        CHECK(hit_t(i) > 1.0e29); // a spurious hit here is an opaque speck floating in mid-air
    }
    INFO("axial ray: t = " << hit_t(12) << " want " << (5.0 - r));
    CHECK(crd::math::abs(hit_t(12) - (5.0 - r)) < 1.0e-6);
    INFO("end-cap ray: t = " << hit_t(13) << " (must hit the hemisphere, not vanish)");
    CHECK(hit_t(13) < 1.0e29);
    CHECK(hit_t(13) > 3.0 - r - 1.0e-6);
    INFO("beyond-cap ray: t = " << hit_t(14) << " (must miss)");
    CHECK(hit_t(14) > 1.0e29);
}

TEST_CASE("ckir LSS tapered segment interpolates its radius linearly", "[ckir][lss]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "lss-taper");
    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> out(&alloc);
    rays.resize(static_cast<crd::usize>(kLanes) * 6U, 0.0);

    // a strand tapering from 0.4 at the root to 0.1 at the tip — the real hair case
    const double ra = 0.4;
    const double rb = 0.1;
    one_segment(segs, 0.0, 0.0, 0.0, ra, 4.0, 0.0, 0.0, rb);

    // (d) perpendicular rays at increasing u; the hit must track the LINEARLY interpolated radius
    for (int i = 0; i < 10; ++i)
    {
        const double u = 0.08 + 0.084 * static_cast<double>(i);
        set_ray(rays, i, u * 4.0, -3.0, 0.0, 0.0, 1.0, 0.0);
    }
    trace(alloc, 1, rays, segs, out);

    // ⭐ THE EXACT EXPECTATION, not an approximation. The ray travels along +y at fixed x, and in that plane the cone
    //   surface is the TANGENT LINE to the two end circles — tilted by the half-angle alpha where sin(alpha) = (ra-rb)/L.
    //   The perpendicular distance from the axis is r(u), but the distance measured ALONG THE RAY is r(u)/cos(alpha).
    //   An earlier version of this gate used r(u) and hid the difference under a 2e-4 tolerance; that slack was exactly
    //   wide enough to also hide a genuinely wrong cone formulation. Closed-form and tight, or it is not a gate.
    const double sin_a = (ra - rb) / 4.0;
    const double cos_a = crd::math::sqrt(1.0 - sin_a * sin_a);
    double       worst = 0.0;
    for (int i = 0; i < 10; ++i)
    {
        const double u    = 0.08 + 0.084 * static_cast<double>(i);
        const double rad  = ra + u * (rb - ra);
        const double want = 3.0 - rad / cos_a;
        const double got  = out[static_cast<crd::usize>(i) * 2U + 0U];
        INFO("u = " << u << " radius " << rad << ": t = " << got << " want " << want);
        CHECK(crd::math::abs(got - want) < 2.0e-5);
        if (crd::math::abs(got - want) > worst) { worst = crd::math::abs(got - want); }
    }
    INFO("worst taper deviation " << worst);
    // ⭐ THE TAPER MUST BE MONOTONE. If it were not, a strand's silhouette would wobble along its length — the single
    //    most obvious artifact a hair renderer can have, and invisible in any single-ray test.
    double prev = -1.0;
    for (int i = 0; i < 10; ++i)
    {
        const double got = out[static_cast<crd::usize>(i) * 2U + 0U];
        if (i > 0) { CHECK(got > prev); } // thinner further along ⇒ the surface is farther ⇒ t grows
        prev = got;
    }
}

TEST_CASE("ckir LSS picks the nearest of several segments", "[ckir][lss]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "lss-multi");
    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> out(&alloc);
    rays.resize(static_cast<crd::usize>(kLanes) * 6U, 0.0);

    // four parallel strands stacked in y at increasing distance from the ray origin
    const int    nseg = 4;
    const double r    = 0.2;
    segs.resize(static_cast<crd::usize>(nseg) * 8U, 0.0);
    for (int s = 0; s < nseg; ++s)
    {
        const crd::usize o = static_cast<crd::usize>(s) * 8U;
        const double     y = 1.0 + static_cast<double>(s);
        segs[o + 0U] = -2.0; segs[o + 1U] = y; segs[o + 2U] = 0.0; segs[o + 3U] = r;
        segs[o + 4U] =  2.0; segs[o + 5U] = y; segs[o + 6U] = 0.0; segs[o + 7U] = r;
    }
    // fire +y from below: must always find the FIRST strand, never one behind it
    for (int i = 0; i < 6; ++i) { set_ray(rays, i, -1.0 + 0.4 * static_cast<double>(i), 0.0, 0.0, 0.0, 1.0, 0.0); }
    trace(alloc, nseg, rays, segs, out);

    for (int i = 0; i < 6; ++i)
    {
        const double got = out[static_cast<crd::usize>(i) * 2U + 0U];
        INFO("ray " << i << ": t = " << got << " want " << (1.0 - r) << " (the NEAREST strand, not one behind)");
        CHECK(crd::math::abs(got - (1.0 - r)) < 1.0e-6);
    }
}

TEST_CASE("ckir LSS AABB is conservative", "[ckir][lss]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "lss-aabb");
    crd::containers::Array<double> in(&alloc);
    crd::containers::Array<double> out(&alloc);

    kir::lss::LssAabbConfig cfg;
    cfg.segments = kLanes;
    in.resize(static_cast<crd::usize>(kLanes) * 8U, 0.0);
    crd::u32   st  = 0x5EED1234U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int s = 0; s < kLanes; ++s)
    {
        const crd::usize o = static_cast<crd::usize>(s) * 8U;
        for (int k = 0; k < 3; ++k) { in[o + static_cast<crd::usize>(k)] = rnd() * 4.0 - 2.0; }
        in[o + 3U] = 0.02 + rnd() * 0.3;
        for (int k = 0; k < 3; ++k) { in[o + 4U + static_cast<crd::usize>(k)] = rnd() * 4.0 - 2.0; }
        in[o + 7U] = 0.02 + rnd() * 0.3;
    }
    out.resize(static_cast<crd::usize>(kLanes) * 6U, 0.0);
    kir::KGraph       g(&alloc);
    const kir::KEntry e     = kir::lss::build_lss_aabb_kernel(g, cfg);
    kir::KernelBuffer bb[2] = {{in.data(), kLanes * 8, 0, 0}, {out.data(), kLanes * 6, 0, 1}};
    kir::eval_cpu_kernel(g, e, bb, 2, e.local_size[0], &alloc, 1U);

    // ⭐ CONSERVATIVE OR NOTHING. Traversal only offers the shader primitives whose AABB the ray entered, so a box that
    //    clips the swept volume silently DROPS hits — and a dropped hit in a groom reads as a hole, not as an error.
    //    Sample the swept surface densely and require every point inside the box.
    for (int s = 0; s < kLanes; ++s)
    {
        const crd::usize i = static_cast<crd::usize>(s) * 8U;
        const crd::usize o = static_cast<crd::usize>(s) * 6U;
        const double     ax = in[i + 0U];
        const double     ay = in[i + 1U];
        const double     az = in[i + 2U];
        const double     ra = in[i + 3U];
        const double     bx = in[i + 4U];
        const double     by = in[i + 5U];
        const double     bz = in[i + 6U];
        const double     rb = in[i + 7U];
        for (int k = 0; k <= 16; ++k)
        {
            const double t   = static_cast<double>(k) / 16.0;
            const double cx  = ax + (bx - ax) * t;
            const double cy  = ay + (by - ay) * t;
            const double cz  = az + (bz - az) * t;
            const double rad = ra + (rb - ra) * t;
            // the extreme points of the swept sphere at this station, along each axis
            CHECK(cx - rad >= out[o + 0U] - 1.0e-9);
            CHECK(cy - rad >= out[o + 1U] - 1.0e-9);
            CHECK(cz - rad >= out[o + 2U] - 1.0e-9);
            CHECK(cx + rad <= out[o + 3U] + 1.0e-9);
            CHECK(cy + rad <= out[o + 4U] + 1.0e-9);
            CHECK(cz + rad <= out[o + 5U] + 1.0e-9);
        }
    }
}

TEST_CASE("ckir LSS tapered segment rejects rays that pass outside it", "[ckir][lss]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "lss-taper-miss");
    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> out(&alloc);
    rays.resize(static_cast<crd::usize>(kLanes) * 6U, 0.0);

    // ⛔ THE GATE THAT WAS MISSING, and the bug it exists to catch.
    //   The round-cone axial coordinate is y = m1 - ra*rr + t*m2 (rr = ra - rb). The -ra*rr term was absent, so the
    //   [0, m0] span test admitted roots lying OFF the segment and the intersector reported hits in EMPTY SPACE.
    //   It is invisible for a capsule (rr == 0), and the existing taper gates only fired rays that HIT — so nothing
    //   caught it until hardware, where the correct AABB culled the phantom hits and it presented as the DEVICE
    //   missing 63% of rays. A tapered MISS is the one configuration that exposes it directly.
    const double ra = 0.40;
    const double rb = 0.05; // a strong taper ⇒ a large ra*rr, so the omitted term is large
    one_segment(segs, 0.0, 0.0, 0.0, ra, 4.0, 0.0, 0.0, rb);

    // Rays perpendicular to the axis, offset in z by MORE than the local radius at their crossing point. Every one of
    // these must miss: the swept surface is nowhere within reach.
    int n = 0;
    for (int i = 0; i < 12; ++i)
    {
        const double u   = 0.05 + 0.08 * static_cast<double>(i);
        const double rad = ra + u * (rb - ra);
        set_ray(rays, n++, u * 4.0, -3.0, rad + 0.05 + 0.03 * static_cast<double>(i), 0.0, 1.0, 0.0);
    }
    // ...and rays aimed well BEYOND both ends along the axis direction, which must also miss.
    for (int i = 0; i < 6; ++i)
    {
        set_ray(rays, n++, 4.0 + ra + 0.3 + 0.2 * static_cast<double>(i), -3.0, 0.0, 0.0, 1.0, 0.0);
        set_ray(rays, n++, -(ra + 0.3 + 0.2 * static_cast<double>(i)), -3.0, 0.0, 0.0, 1.0, 0.0);
    }
    trace(alloc, 1, rays, segs, out);

    for (int i = 0; i < n; ++i)
    {
        const double t = out[static_cast<crd::usize>(i) * 2U + 0U];
        INFO("ray " << i << " must MISS the tapered segment, got t = " << t);
        CHECK(t > 1.0e29);
    }

    // And the complement: rays that DO hit must still hit, so the fix cannot have been a blanket rejection.
    for (int i = 0; i < 10; ++i)
    {
        const double u = 0.08 + 0.084 * static_cast<double>(i);
        set_ray(rays, i, u * 4.0, -3.0, 0.0, 0.0, 1.0, 0.0);
    }
    trace(alloc, 1, rays, segs, out);
    for (int i = 0; i < 10; ++i)
    {
        const double u    = 0.08 + 0.084 * static_cast<double>(i);
        const double sa   = (ra - rb) / 4.0;
        const double want = 3.0 - (ra + u * (rb - ra)) / crd::math::sqrt(1.0 - sa * sa); // exact: the cone is tilted
        const double t    = out[static_cast<crd::usize>(i) * 2U + 0U];
        INFO("hit ray " << i << ": t = " << t << " want " << want);
        CHECK(crd::math::abs(t - want) < 2.0e-5);
    }
}

// D-007 B18-f: the RT STRAND TIER — hardware traversal feeding the B18-a hair BCSDF.
//
// The traversal gates above prove WHERE a ray hits a strand. This one proves the hit is SHADEABLE, which is a different
// and much easier thing to get quietly wrong: the BCSDF lives in the fibre frame, and building that frame from a curve
// hit means recovering the tangent from the winning segment, the azimuthal offset h from the ray's miss distance, and an
// orthonormal basis anchored so that phi_o = 0. Any of those can be plausible-looking and wrong, and the result is not a
// crash but hair that is lit incorrectly — exactly the class of defect an image comparison rationalises away.
//
// So the gate is CONSTRUCTIVE, not tolerance-based: a configuration whose frame is known by hand, checked against a
// direct evaluation of the same BCSDF at the hand-computed angles.
//
//   segment: (0,0,0) -> (4,0,0), constant radius 0.5   =>  T = +x
//   ray:     from (2, -3, z0) along +y                 =>  wo = -y,  binormal = T x wo = -z,  h = -z0 / 0.5
//   frame:   Y = wo (already perpendicular to tangent) => wo_f = (0,1,0), phi_o = 0 by construction
//   light:   (0,0,-1)                                  =>  Z = T x Y = -z, so wi_f = (0,0,1), phi_i = pi/2
//
// If the frame construction drifts, f disagrees with the direct evaluation and this fails; a tolerance test against a
// stored number could not tell a frame bug from a BCSDF change.
TEST_CASE("ckir RT strand tier shades a curve hit with the hair BCSDF in the correct fibre frame", "[ckir][lss][hair]")
{
    crd::memory::TlsfAllocator     alloc(96U << 20U, nullptr, "rt-hair-shade");
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> out(&alloc);
    crd::containers::Array<double> as_stub(&alloc);

    constexpr int nray = 64;
    constexpr double strand_r = 0.5;
    constexpr double ray_tmax = 50.0;

    segs.resize(8U, 0.0);
    segs[0U] = 0.0; segs[1U] = 0.0; segs[2U] = 0.0; segs[3U] = strand_r;
    segs[4U] = 4.0; segs[5U] = 0.0; segs[6U] = 0.0; segs[7U] = strand_r;

    // rays 0..7 hit at a spread of azimuthal offsets z0; rays 8.. miss entirely (offset beyond the radius)
    rays.resize(static_cast<crd::usize>(nray) * 6U, 0.0);
    const auto z0_of = [](int i) { return -0.4 + 0.1 * static_cast<double>(i); }; // -0.4 .. +0.3, all inside r
    for (int i = 0; i < nray; ++i)
    {
        const crd::usize o = static_cast<crd::usize>(i) * 6U;
        rays[o + 0U] = 2.0;
        rays[o + 1U] = -3.0;
        rays[o + 2U] = (i < 8) ? z0_of(i) : (strand_r + 0.25 + 0.05 * static_cast<double>(i));
        rays[o + 3U] = 0.0; rays[o + 4U] = 1.0; rays[o + 5U] = 0.0;
    }

    kir::lss::RtHairShadeConfig cfg;
    cfg.rays     = nray;
    cfg.tmax     = ray_tmax;
    cfg.light[0] = 0.0; cfg.light[1] = 0.0; cfg.light[2] = -1.0;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::lss::build_rt_hair_shade_kernel(g, cfg);
    out.resize(static_cast<crd::usize>(nray) * 2U, 0.0);
    as_stub.resize(1U, 0.0);
    kir::KernelBuffer bb[4] = {{as_stub.data(), 1, 0, 0},
                               {segs.data(), static_cast<int>(segs.size()), 0, 1},
                               {rays.data(), nray * 6, 0, 2},
                               {out.data(), nray * 2, 0, 3}};
    kir::eval_cpu_kernel(g, e, bb, 4, e.local_size[0], &alloc, static_cast<crd::u32>(nray / 64));

    // ── the independent reference: the SAME BCSDF, at angles from a SECOND, plain-C++ frame construction ──
    // The frame is built here in host double-precision vector maths — an implementation that shares no code with the IR
    // node version under test. Feeding the angles in through a buffer (rather than deriving them inside the graph) keeps
    // the reference free of any IR arithmetic that could fail the same way as the thing being checked.
    // Layout, 7 scalars per lane: [sin_to, cos_to, phi_o, sin_ti, cos_ti, phi_i, h].
    crd::containers::Array<double> refv(&alloc);
    crd::containers::Array<double> aref(&alloc);
    refv.resize(64U, 0.0);
    aref.resize(64U * 7U, 0.0);
    {
        const double pa[3] = {segs[0U], segs[1U], segs[2U]};
        const double pb[3] = {segs[4U], segs[5U], segs[6U]};
        const auto   dot3h = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        const auto   nrm3h = [](double* v) {
            const double l = crd::math::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            v[0] /= l; v[1] /= l; v[2] /= l;
        };
        const auto crs3h = [](const double* a, const double* b, double* o) {
            o[0] = a[1] * b[2] - a[2] * b[1];
            o[1] = a[2] * b[0] - a[0] * b[2];
            o[2] = a[0] * b[1] - a[1] * b[0];
        };
        double tangent[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        nrm3h(tangent);
        for (int i = 0; i < 8; ++i)
        {
            const crd::usize o  = static_cast<crd::usize>(i) * 6U;
            const double     ro[3] = {rays[o + 0U], rays[o + 1U], rays[o + 2U]};
            double           wo[3] = {-rays[o + 3U], -rays[o + 4U], -rays[o + 5U]};
            nrm3h(wo);
            const double wi[3] = {cfg.light[0], cfg.light[1], cfg.light[2]};

            double bn[3];
            crs3h(tangent, wo, bn);
            nrm3h(bn);
            const double oa[3] = {ro[0] - pa[0], ro[1] - pa[1], ro[2] - pa[2]};
            const double h     = dot3h(oa, bn) / strand_r; // constant radius here, so no u interpolation needed

            const double wot   = dot3h(wo, tangent);
            double       frame_y[3]  = {wo[0] - tangent[0] * wot, wo[1] - tangent[1] * wot, wo[2] - tangent[2] * wot};
            nrm3h(frame_y);
            double frame_z[3];
            crs3h(tangent, frame_y, frame_z);

            const double woy = dot3h(wo, frame_y);
            const double woz = dot3h(wo, frame_z);
            const double wit = dot3h(wi, tangent);
            const double wiy = dot3h(wi, frame_y);
            const double wiz = dot3h(wi, frame_z);
            const crd::usize a = static_cast<crd::usize>(i) * 7U;
            aref[a + 0U] = wot;
            aref[a + 1U] = crd::math::sqrt(1.0 - wot * wot);
            aref[a + 2U] = crd::math::atan2(woz, woy);
            aref[a + 3U] = wit;
            aref[a + 4U] = crd::math::sqrt(1.0 - wit * wit);
            aref[a + 5U] = crd::math::atan2(wiz, wiy);
            aref[a + 6U] = h;
        }

        kir::KGraph      gr(&alloc);
        const kir::Shape shu = kir::make_shape({1});
        const auto       cu  = [&](crd::u32 v) { return gr.constant(static_cast<double>(v), shu, kir::DType::U32); };
        const auto       ks  = [&](double v) { return gr.constant(v, shu, kir::DType::F32); };
        const int        ab  = gr.buffer_decl(kir::DType::F32, 0, 0, false);
        const int        ob  = gr.buffer_decl(kir::DType::F32, 0, 1, true);
        const int        tid = gr.binary(kir::KOp::Add, gr.binary(kir::KOp::Mul, gr.builtin(kir::KBuiltin::WorkgroupIndex), cu(64)),
                                  gr.builtin(kir::KBuiltin::LocalInvocationIndex));
        const int  mark = gr.kernel_stmt_mark();
        const int  base = gr.binary(kir::KOp::Mul, tid, cu(7));
        const auto al   = [&](int k) {
            const int v = gr.buffer_load(ab, gr.binary(kir::KOp::Add, base, cu(static_cast<crd::u32>(k))));
            gr.stmt_materialize(v);
            return v;
        };
        const int f = kir::hair::hair_bcsdf_eval_angles(gr, al(0), al(1), al(2), al(3), al(4), al(5), al(6), ks(cfg.eta),
                                                        ks(cfg.sigma_a), ks(cfg.beta_m), ks(cfg.beta_n), ks(cfg.alpha_deg));
        gr.stmt_buffer_store(ob, tid, f);
        kir::KEntry er;
        er.stage             = kir::KStage::Compute;
        er.local_size[0]     = 64;
        er.kernel_body_begin = mark;
        er.kernel_body_count = gr.stmt_count() - mark;
        kir::KernelBuffer rbb[2] = {{aref.data(), 64 * 7, 0, 0}, {refv.data(), 64, 0, 1}};
        kir::eval_cpu_kernel(gr, er, rbb, 2, 64U, &alloc, 1U);
    }

    int distinct = 0;
    for (int i = 0; i < 8; ++i)
    {
        const double f = out[static_cast<crd::usize>(i) * 2U + 0U];
        const double t = out[static_cast<crd::usize>(i) * 2U + 1U];
        INFO("ray " << i << " (z0 = " << z0_of(i) << "): f = " << f << " want " << refv[static_cast<crd::usize>(i)]
                    << ", t = " << t);
        CHECK(t < ray_tmax - 1.0);   // it must actually hit the strand
        CHECK(f > 0.0);           // a BCSDF value is strictly positive where the fibre is lit
        // ⚠ F32 tolerance: both sides run the f32 statement tier, and the BCSDF is a long chain (Bessel, logistic,
        //   Fresnel), so this is an accumulated-rounding bound, not a "close enough" fudge.
        CHECK(crd::math::abs(f - refv[static_cast<crd::usize>(i)]) < 2.0e-5);
        if (i > 0 && crd::math::abs(f - out[static_cast<crd::usize>(i - 1) * 2U + 0U]) > 1.0e-7) { ++distinct; }
    }
    // h must actually REACH the BCSDF: if the frame code computed a constant h (say, always 0) every lane would return
    // the same value and every check above would still pass.
    CHECK(distinct >= 6);

    for (int i = 8; i < 16; ++i)
    {
        INFO("miss ray " << i);
        CHECK(out[static_cast<crd::usize>(i) * 2U + 1U] > ray_tmax - 1.0e-2); // a miss returns tmax
        CHECK(out[static_cast<crd::usize>(i) * 2U + 0U] == 0.0);           // ...and contributes no radiance
    }
}
