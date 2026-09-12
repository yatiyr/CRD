#pragma once

// ckir_lss.hpp — D-007 B18-f: LINEAR SWEPT SPHERES, the ray-traced strand primitive.
//
// A hair strand is a chain of segments, and the right ray-tracing primitive for a segment is a sphere swept linearly along
// it — a ROUND CONE (capsule when both radii are equal). That is exactly what VK_NV_ray_tracing_linear_swept_spheres
// exposes in hardware. It is Blackwell-only silicon; this machine is Ada (RTX 4070 Ti SUPER, verified: it advertises
// VK_NV_cluster_acceleration_structure and VK_NV_ray_tracing_invocation_reorder but NOT the LSS extension).
//
// So the portable path is not a consolation prize here — it is THE path, and it has to be first class:
//   · the AS holds one procedural AABB per segment;
//   · the shader intersects the round cone ANALYTICALLY (this file);
//   · where the hardware does have LSS, the same scene description drives the native primitive instead.
//
// ⭐ WHY ANALYTIC AND NOT TESSELLATED. A tessellated tube costs 8-24 triangles per segment; a 170K-strand groom at 30
//    segments is 5M segments, so tessellation is 40M+ triangles — the AS alone would be gigabytes, and the silhouette
//    would still be faceted. The analytic form is 1 AABB + ~40 FLOPs and the silhouette is exact at every distance.
//
// GEOMETRY. Segment from a (radius ra) to b (radius rb). Writing d = b − a, the swept surface is the set of points at
// distance r(t) = ra + t·(rb − ra) from a + t·d for t ∈ [0,1], plus the two end caps. Substituting the ray o + s·w and
// clearing the square root gives a QUADRATIC in s whose coefficients are the ones below; the conical side is valid only
// where the hit's projection onto the axis lands in [0,1], and the caps are handled as two ray-sphere tests. This is the
// standard round-cone form (Quilez), specialised for the hair case where rb ≤ ra (a strand tapers to its tip).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair.hpp>

namespace crd::kir::lss
{

namespace detail
{
using crd::kir::hair::detail::add;
using crd::kir::hair::detail::dv;
using crd::kir::hair::detail::mul;
using crd::kir::hair::detail::safe_sqrt;
using crd::kir::hair::detail::sq;
using crd::kir::hair::detail::sub;

struct V3
{
    int x = -1, y = -1, z = -1;
};
[[nodiscard]] inline int dot3(KGraph& g, const V3& a, const V3& b)
{
    return add(g, add(g, mul(g, a.x, b.x), mul(g, a.y, b.y)), mul(g, a.z, b.z));
}
[[nodiscard]] inline V3 sub3(KGraph& g, const V3& a, const V3& b)
{
    return {sub(g, a.x, b.x), sub(g, a.y, b.y), sub(g, a.z, b.z)};
}
[[nodiscard]] inline V3 mad3(KGraph& g, const V3& a, const V3& b, int s)
{
    return {add(g, a.x, mul(g, b.x, s)), add(g, a.y, mul(g, b.y, s)), add(g, a.z, mul(g, b.z, s))};
}
} // namespace detail

// Analytic ray / round-cone intersection.
//   ro, rd    the ray (rd need not be normalised; the returned t is in rd units)
//   pa, pb    segment endpoints;  ra, rb  their radii
//   tmax      current closest hit — anything beyond is rejected
// Writes the hit distance into `t_out` (or leaves it at tmax on a miss) and the axial parameter into `u_out`, which is what
// the shader needs for shading: it gives the tangent, the strand-space v coordinate, and the interpolated radius.
//
// ⚠ BRANCHLESS BY CONSTRUCTION. Every rejection is a select, never an early-out. A divergent early-out inside a ray-query
//   candidate loop serialises the whole subgroup, and on the CPU oracle it would also break the statement-tier lockstep
//   model. The cost is that both the cone and the two caps are always evaluated; that is the right trade at 40 FLOPs.
inline void lss_intersect(KGraph& g, const detail::V3& ro, const detail::V3& rd, const detail::V3& pa, const detail::V3& pb,
                          int ra, int rb, int tmax, int& t_out, int& u_out)
{
    using namespace detail;
    const auto ks = [&](double v) { return crd::kir::hair::detail::kf(g, ra, v); };

    // ⛔ THE REFERENCE FORM ASSUMES A UNIT DIRECTION. m2 = dot(ba, d) and the k-coefficients are dimensionally wrong for
    //    an unnormalised ray, so normalise here and scale the result back to RAY units at the end. Getting this wrong is
    //    silent: it still returns plausible distances, just for the wrong surface.
    const int rlen  = g.binary(KOp::Max, safe_sqrt(g, dot3(g, rd, rd)), ks(1.0e-20));
    const int rinv  = dv(g, ks(1.0), rlen);
    const V3  d     = {mul(g, rd.x, rinv), mul(g, rd.y, rinv), mul(g, rd.z, rinv)};
    const int tminu = mul(g, ks(1.0e-5), rlen); // the epsilon, expressed in unit-direction units
    const int tmaxu = mul(g, tmax, rlen);

    // ⛔⛔ RE-ORIGIN THE RAY AT THE SEGMENT. The k-coefficients recover a term of order m0·ra² by subtracting
    //     quantities of order |ro−pa|². For a REALISTIC 68 µm fibre viewed from ~1 unit away those differ by EIGHT
    //     orders of magnitude, so in f32 the radius sits below the cancellation noise of the very terms carrying it,
    //     and the solve commits hits that are not on the surface. Measured on the GPU before this: the hit sat ~6×
    //     the fibre radius off-axis, and mean |h| came out 0.92 where a cylinder must give 0.5 — which is what printed
    //     as beading along every strand. Sliding the origin down the ray costs one dot product and fixes it exactly
    //     (measured after: |roff| = 5.01e-5 against a mean radius of 5.0e-5; mean |h| = 0.4996).
    //
    //     ⚠ It is invisible at PLACEHOLDER thickness — a fat strand keeps the term inside f32's range — so "it looked
    //       fine before" is not evidence here. The defect arrived with correctness, not with the change that exposed it.
    const V3  ba  = sub3(g, pb, pa);
    const int tsh = dot3(g, sub3(g, pa, ro), d);
    const V3  roL = mad3(g, ro, d, tsh);
    const V3  oa  = sub3(g, roL, pa);
    const V3  ob  = sub3(g, roL, pb);
    const int rr = sub(g, ra, rb);
    const int m0 = dot3(g, ba, ba);
    const int m1 = dot3(g, ba, oa);
    const int m2 = dot3(g, ba, d);
    const int m3 = dot3(g, d, oa);
    const int m5 = dot3(g, oa, oa);
    const int m6 = dot3(g, ob, d);
    const int m7 = dot3(g, ob, ob);

    // ⛔ d2 = m0 − rr², NOT m0 + rr². The sign, the k-coefficient scaling, and the span bound below were all wrong in the
    //    first version; the cone branch then reported hits in empty space (measured: 118 of 132 hits spurious, every one
    //    from this branch). All four defects are invisible for a capsule, where rr == 0.
    const int d2 = sub(g, m0, sq(g, rr));

    // ── CONICAL SIDE (Quilez round-cone) ────────────────────────────────────────────────────────────────────────────
    const int k2 = sub(g, d2, sq(g, m2));
    const int k1 = add(g, sub(g, mul(g, d2, m3), mul(g, m1, m2)), mul(g, mul(g, m2, rr), ra));
    const int k0 = sub(g, add(g, sub(g, mul(g, d2, m5), sq(g, m1)), mul(g, mul(g, mul(g, m1, rr), ra), ks(2.0))),
                       mul(g, m0, sq(g, ra)));
    const int h  = sub(g, sq(g, k1), mul(g, k0, k2));
    const int sh = safe_sqrt(g, g.binary(KOp::Max, h, ks(0.0)));
    const int k2s = g.select(g.binary(KOp::CmpLt, g.unary(KOp::Abs, k2), ks(1.0e-20)), ks(1.0e-20), k2);
    const int tcL = dv(g, sub(g, g.unary(KOp::Neg, sh), k1), k2s);       // local to the shifted origin
    const int tc  = add(g, tcL, tsh);                                    // ...and back to the true ray parameter
    const int yc  = add(g, sub(g, m1, mul(g, ra, rr)), mul(g, tcL, m2)); // y uses the LOCAL t, matching m1/m2
    const int cone_ok = g.binary(KOp::BitAnd,
                                 g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, h, ks(0.0)),
                                          g.binary(KOp::CmpGt, tc, tminu)),
                                 g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, yc, ks(0.0)),
                                          g.binary(KOp::CmpLt, yc, d2))); // span is [0, d2], NOT [0, m0]
    const int uc = dv(g, yc, g.binary(KOp::Max, d2, ks(1.0e-20)));

    // ── END CAPS ────────────────────────────────────────────────────────────────────────────────────────────────────
    // Two ray-sphere tests, in the unit-direction frame so they share the cone's parameterisation. Without caps a ray
    // grazing a segment end misses entirely, which shows as pinholes exactly at the joints between segments.
    const int h1 = add(g, sub(g, sq(g, m3), m5), sq(g, ra));
    const int h2 = add(g, sub(g, sq(g, m6), m7), sq(g, rb));
    const int t1 = add(g, sub(g, g.unary(KOp::Neg, m3), safe_sqrt(g, g.binary(KOp::Max, h1, ks(0.0)))), tsh);
    const int t2 = add(g, sub(g, g.unary(KOp::Neg, m6), safe_sqrt(g, g.binary(KOp::Max, h2, ks(0.0)))), tsh);
    const int ok1 = g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, h1, ks(0.0)), g.binary(KOp::CmpGt, t1, tminu));
    const int ok2 = g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, h2, ks(0.0)), g.binary(KOp::CmpGt, t2, tminu));

    // ── RESOLVE: nearest valid of {cone, cap a, cap b}, still under tmax ─────────────────────────────────────────────
    int best    = tmaxu;
    int bu      = ks(0.0);
    int hit_any = -1;
    const auto take = [&](int ok, int t, int u) {
        const int win = g.binary(KOp::BitAnd, ok, g.binary(KOp::CmpLt, t, best));
        bu            = g.select(win, u, bu);
        best          = g.select(win, t, best);
        hit_any       = (hit_any < 0) ? win : g.binary(KOp::BitOr, hit_any, win);
    };
    // candidates are measured from the SHIFTED origin, so add tsh back to reach the true ray parameter
    take(cone_ok, tc, uc);
    take(ok1, t1, ks(0.0)); // cap a sits at the segment root
    take(ok2, t2, ks(1.0)); // cap b at the tip

    // ⛔ ON A MISS, RETURN THE CALLER'S `tmax` VERBATIM — do NOT recover it as `tmax·rlen·(1/rlen)`. That round trip is
    //    not the identity in f32, so a miss came back a hair BELOW the bound it was given. Callers sweep segments with
    //    `bu = select(t < best, u, bu); best = t;`, so a miss testing as "nearer" CLOBBERED the accumulated u with this
    //    function's miss value of 0 — a shaded strand would take the root's tangent and radius at an arbitrary point
    //    along its length. It also let t drift ~100 ulp over an 8-segment sweep. Both backends and the CPU oracle each
    //    rounded the round trip differently, which is how it surfaced: GPU u == 0 where the oracle had a real u.
    t_out = g.select(hit_any, mul(g, best, rinv), tmax); // back to RAY units, exact on a miss
    u_out = bu;
}

// The conservative AABB of one swept segment — what the procedural BLAS stores, one per segment.
// Must be conservative: an AABB that clips the primitive drops hits the traversal never even offers the shader.
struct LssAabbConfig
{
    int  segments = 64;   // one lane per segment
    bool pad_eps  = true; // widen by a small epsilon so a tangential ray is never lost to floating-point at the boundary
};

// Buffers: b0 in (F32, 8/segment = [ax, ay, az, ra, bx, by, bz, rb]) · b1 out (F32, 6/segment = [minxyz, maxxyz])
[[nodiscard]] inline KEntry build_lss_aabb_kernel(KGraph& g, const LssAabbConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int in_b  = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark  = g.kernel_stmt_mark();
    const int guard = g.stmt_if_begin(g.binary(KOp::CmpLt, tid, cu(static_cast<crd::u32>(cfg.segments))));
    const int ib    = g.binary(KOp::Mul, tid, cu(8));
    const auto ld   = [&](int k) {
        const int v = g.buffer_load(in_b, g.binary(KOp::Add, ib, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int ax = ld(0);
    const int ay = ld(1);
    const int az = ld(2);
    const int ra = ld(3);
    const int bx = ld(4);
    const int by = ld(5);
    const int bz = ld(6);
    const int rb = ld(7);

    const int eps = ks(cfg.pad_eps ? 1.0e-5 : 0.0);
    const int ob  = g.binary(KOp::Mul, tid, cu(6));
    const auto emit = [&](int k, int lo_a, int r_a, int lo_b, int r_b, bool is_min) {
        // each end contributes its own sphere, so the bound is min/max of (centre -+ radius) over BOTH ends
        const int e0 = is_min ? sub(g, lo_a, r_a) : add(g, lo_a, r_a);
        const int e1 = is_min ? sub(g, lo_b, r_b) : add(g, lo_b, r_b);
        int       v  = is_min ? g.binary(KOp::Min, e0, e1) : g.binary(KOp::Max, e0, e1);
        v            = is_min ? sub(g, v, eps) : add(g, v, eps);
        g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(static_cast<crd::u32>(k))), v);
    };
    emit(0, ax, ra, bx, rb, true);
    emit(1, ay, ra, by, rb, true);
    emit(2, az, ra, bz, rb, true);
    emit(3, ax, ra, bx, rb, false);
    emit(4, ay, ra, by, rb, false);
    emit(5, az, ra, bz, rb, false);
    g.stmt_if_end(guard);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// One thread per ray: intersect against a FIXED set of swept segments, returning the nearest hit distance and its axial
// coordinate. This is the CPU-verifiable core of the strand tier; on the device the same maths runs inside the ray-query
// candidate loop, with the BLAS supplying which segments to test rather than a host-side sweep.
struct LssTraceConfig
{
    int    segments = 8;     // segments tested per ray (host-unrolled — this is the reference form)
    double tmax     = 1.0e30;
};

// Buffers: b0 rays (F32, 6/ray) · b1 segments (F32, 8/segment) · b2 out (F32, 2/ray = [t, u])
[[nodiscard]] inline KEntry build_lss_trace_kernel(KGraph& g, const LssTraceConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int ray_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int seg_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int rb_seg  = g.binary(KOp::Mul, tid, cu(6));
    const auto rl  = [&](int k) {
        const int v = g.buffer_load(ray_b, g.binary(KOp::Add, rb_seg, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const V3 ro{rl(0), rl(1), rl(2)};
    const V3 rd{rl(3), rl(4), rl(5)};

    int best = ks(cfg.tmax);
    int bu   = ks(0.0);
    for (int s = 0; s < cfg.segments; ++s)
    {
        const auto sl = [&](int k) {
            const int v = g.buffer_load(seg_b, cu(static_cast<crd::u32>(s * 8 + k)));
            g.stmt_materialize(v);
            return v;
        };
        const V3  pa{sl(0), sl(1), sl(2)};
        const int ra = sl(3);
        const V3  pb{sl(4), sl(5), sl(6)};
        const int rb2 = sl(7);
        int       t   = -1;
        int       u   = -1;
        lss_intersect(g, ro, rd, pa, pb, ra, rb2, best, t, u);
        // lss_intersect already rejects anything at or beyond `best`, so the running minimum is just its output
        bu   = g.select(g.binary(KOp::CmpLt, t, best), u, bu);
        best = t;
        g.stmt_materialize(best);
        g.stmt_materialize(bu);
    }

    const int ob = g.binary(KOp::Mul, tid, cu(2));
    g.stmt_buffer_store(out_b, ob, best);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(1)), bu);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── B18-f: the RT STRAND TIER — hardware traversal feeding the B18-a hair BCSDF. ──────────────────────────────────────
//
// This is what the whole slice is for. Everything above builds the traversal; this shades it. One TraceRayCurves per
// pixel against the procedural curve BLAS, and every hit is shaded with the SAME energy-conserving Chiang BCSDF the
// raster tier uses — not a stand-in, not a lambert placeholder. The rasteriser and the ray tracer disagreeing about
// what a hair fibre looks like would make the strand tier a second material, and there is only one material.
//
// ⭐ WHY THE HIT NEEDS `prim`. The BCSDF is defined in the FIBRE FRAME: θ is measured from the plane perpendicular to
//    the strand tangent, φ around it. The tangent is a property of the segment that was hit, so shading is impossible
//    from (t, u) alone — hence RtCurveHit::prim, and hence this kernel re-reads the winning segment from the same
//    buffer the AS was built from.
//
// THE OFFSET h. The BCSDF's γo = asin(h) is the azimuthal position where the ray enters the fibre, h ∈ [−1, 1] in units
// of the fibre radius. Measured directly as the SIGNED perpendicular miss distance of the ray from the strand axis:
// project (o − a) onto the axis-perpendicular direction the ray travels sideways along, i.e. normalize(cross(T, w)).
// This is exact by construction rather than reconstructed from the surface normal, which degenerates at grazing angles.
struct RtHairShadeConfig
{
    int    rays      = 4096;    // one per thread
    double tmax      = 50.0;
    double eta       = 1.55;    // index of refraction of the cortex (Marschner)
    double beta_m    = 0.25;    // longitudinal roughness
    double beta_n    = 0.30;    // azimuthal roughness
    double alpha_deg = 2.0;     // cuticle scale tilt
    double sigma_a   = 0.20;    // absorption (monochrome here; the coloured path takes a vec3)
    double light[3]  = {0.0, 0.0, -1.0}; // direction TO the light, world space, expected normalised
};

// Buffers: b0 AS · b1 segments (F32, 8/segment) · b2 rays (F32, 6/ray) · b3 out (F32, 2/ray = [f, t])
// `f` is the BCSDF value for the (light, view) pair at the hit — 0 on a miss, which is also what the oracle produces,
// so a miss and a black fibre are distinguished by `t` rather than by the radiance.
[[nodiscard]] inline KEntry build_rt_hair_shade_kernel(KGraph& g, const RtHairShadeConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int as    = g.accel_struct_decl(0, 0);
    const int seg_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int ray_b = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 3, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int  mark = g.kernel_stmt_mark();
    const int  rb_seg  = g.binary(KOp::Mul, tid, cu(6));
    const auto rl   = [&](int k) {
        const int v = g.buffer_load(ray_b, g.binary(KOp::Add, rb_seg, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const V3 ro{rl(0), rl(1), rl(2)};
    const V3 rd{rl(3), rl(4), rl(5)};

    // ⛔ Do NOT stmt_materialize the hit results. TraceRayCurves ALREADY materialises all three (the emitters mark them
    //    temped, the oracle allocates their slots), and materialising again allocates a SECOND slot that nothing ever
    //    writes — so the value silently reads back as 0. That is how this first presented: t == 0 on every lane, which
    //    made every ray look like a hit at the near plane and defeated the miss mask.
    const auto hit = g.trace_ray_curves(as, seg_b, ro.x, ro.y, ro.z, rd.x, rd.y, rd.z, ks(1.0e-4), ks(cfg.tmax));

    // ⛔ A MISS carries prim = 0xFFFFFFFF, which would index far out of bounds. Clamp the index to segment 0 and mask
    //    the RESULT instead — a branch here would diverge per lane for no benefit, and an out-of-bounds load on the
    //    device is silent corruption rather than a fault.
    const int miss = g.binary(KOp::CmpGe, hit.t, ks(cfg.tmax - 1.0e-3));
    const int sidx = g.select(miss, cu(0), g.binary(KOp::Mul, hit.prim, cu(8)));
    const auto sl  = [&](int k) {
        const int v = g.buffer_load(seg_b, g.binary(KOp::Add, sidx, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const V3  pa{sl(0), sl(1), sl(2)};
    const int ra = sl(3);
    const V3  pb{sl(4), sl(5), sl(6)};
    const int rb2 = sl(7);

    // fibre tangent T (unit) and the interpolated radius at the hit's axial coordinate
    const V3  ba   = sub3(g, pb, pa);
    const int blen = g.binary(KOp::Max, safe_sqrt(g, dot3(g, ba, ba)), ks(1.0e-20));
    const int binv = dv(g, ks(1.0), blen);
    const V3  tang{mul(g, ba.x, binv), mul(g, ba.y, binv), mul(g, ba.z, binv)};
    const int rad = g.binary(KOp::Max, add(g, ra, mul(g, sub(g, rb2, ra), hit.u)), ks(1.0e-20));

    // view direction ωo = −rd (unit), and the light direction ωi
    const int rlen = g.binary(KOp::Max, safe_sqrt(g, dot3(g, rd, rd)), ks(1.0e-20));
    const int rinv = dv(g, ks(1.0), rlen);
    const V3  wo{g.unary(KOp::Neg, mul(g, rd.x, rinv)), g.unary(KOp::Neg, mul(g, rd.y, rinv)),
                g.unary(KOp::Neg, mul(g, rd.z, rinv))};
    const V3  wi{ks(cfg.light[0]), ks(cfg.light[1]), ks(cfg.light[2])};

    // h = signed perpendicular miss distance / radius, measured along normalize(cross(T, ωo))
    const V3  cr0{sub(g, mul(g, tang.y, wo.z), mul(g, tang.z, wo.y)), sub(g, mul(g, tang.z, wo.x), mul(g, tang.x, wo.z)),
                 sub(g, mul(g, tang.x, wo.y), mul(g, tang.y, wo.x))};
    const int clen = g.binary(KOp::Max, safe_sqrt(g, dot3(g, cr0, cr0)), ks(1.0e-20));
    const int cinv = dv(g, ks(1.0), clen);
    const V3  bn{mul(g, cr0.x, cinv), mul(g, cr0.y, cinv), mul(g, cr0.z, cinv)}; // binormal, unit, ⊥ T and ⊥ ωo
    const V3  oa = sub3(g, ro, pa);
    const int h  = g.binary(KOp::Max, g.binary(KOp::Min, dv(g, dot3(g, oa, bn), rad), ks(1.0)), ks(-1.0));

    // FIBRE FRAME: x = T, y = the axis-perpendicular part of ωo, z = T × y. Anchoring y to ωo makes φo ≡ 0, which is
    // the rotated frame the azimuthal term is written in, and makes cosθo fall out exactly as |ωo⊥| with no sqrt.
    const int wo_t = dot3(g, wo, tang);
    const V3  wop{sub(g, wo.x, mul(g, tang.x, wo_t)), sub(g, wo.y, mul(g, tang.y, wo_t)), sub(g, wo.z, mul(g, tang.z, wo_t))};
    const int plen = g.binary(KOp::Max, safe_sqrt(g, dot3(g, wop, wop)), ks(1.0e-20));
    const int pinv = dv(g, ks(1.0), plen);
    const V3  fy{mul(g, wop.x, pinv), mul(g, wop.y, pinv), mul(g, wop.z, pinv)};
    const V3  fz{sub(g, mul(g, tang.y, fy.z), mul(g, tang.z, fy.y)), sub(g, mul(g, tang.z, fy.x), mul(g, tang.x, fy.z)),
               sub(g, mul(g, tang.x, fy.y), mul(g, tang.y, fy.x))};

    // ⛔ SCALAR ANGLES, NOT hair_bcsdf_eval's vec3 wrapper. The statement-tier evaluator (eval_cpu_kernel) is SCALAR:
    //    Vec3/VecComp/Swizzle have no case there, so they fall through to apply_ternary/apply_unary and evaluate to
    //    garbage SILENTLY. The vec3 wrapper is for the RASTER fragment path, where vectors are native. The scalar-angle
    //    core exists for exactly this tier — use it. (This cost an hour: h was provably correct and the frame vectors
    //    were provably correct, but the vec3 carrying them into the BCSDF collapsed its z component to 0, so φ was 0
    //    instead of π/2 and the result came out symmetric in h — physically plausible, and wrong.)
    const int sin_to = wo_t;
    const int cos_to = plen;   // |ωo⊥| — exact, and non-negative by construction
    const int phi_o  = ks(0.0); // ≡ 0: Y was BUILT as ωo's perpendicular part, so ωo has no Z component
    const int sin_ti = dot3(g, wi, tang);
    const int cos_ti = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_ti)));
    const int phi_i  = g.binary(KOp::Atan2, dot3(g, wi, fz), dot3(g, wi, fy));

    const int f  = hair::hair_bcsdf_eval_angles(g, sin_to, cos_to, phi_o, sin_ti, cos_ti, phi_i, h, ks(cfg.eta),
                                               ks(cfg.sigma_a), ks(cfg.beta_m), ks(cfg.beta_n), ks(cfg.alpha_deg));
    const int fm = g.select(miss, ks(0.0), f);

    const int ob = g.binary(KOp::Mul, tid, cu(2));
    g.stmt_buffer_store(out_b, ob, fm);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(1)), hit.t);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::lss
