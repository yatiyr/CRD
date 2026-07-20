#pragma once

// ckir_hair_geom.hpp — D-007 B18-d: HAIR STRAND GEOMETRY + the compressed strand G-buffer.
//
// Research: docs/research/2026-07-19-hair-fur-frontier-collection.md. ckir_hair.hpp is the fibre BCSDF, ckir_hair_scatter.hpp
// is inter-fibre transport; THIS file is where hair becomes geometry.
//
// Two ideas from the study drive the design:
//
//  1. HAIR MESHES (Bhokare/Montalvo/Diaz/Yuksel, SIGGRAPH 2024) — do NOT store strands. A groom is a coarse layered extrusion
//     mesh (bundles); each strand is identified only by its (u,v) inside a bundle face and is GENERATED on the fly by
//     interpolating through the bundle's layers. Storing 100K+ splines costs hundreds of megabytes and the bandwidth to match;
//     regenerating them per pass is dramatically cheaper. This also makes LOD trivial — generate fewer strands, not simplify
//     stored ones.
//
//  2. DEFERRED SOFTWARE RASTERIZATION (Lipp/Jarabo/Wimmer/Bode, 2026) — hair strands are ~1px wide, and a hardware rasterizer
//     dispatches 2x2 fragment quads, wasting up to 75% of lanes on helper invocations. So strands are rasterized in COMPUTE
//     (DDA lines) into a 64-bit atomicMin G-buffer and shaded DEFERRED, once per visible pixel instead of once per overdraw.
//     The 64-bit budget is the hard constraint that shapes everything: 24b depth | 16b octahedral tangent | 18b uvw | 6b AO.
//     ⭐ The trick that makes it fit: store the uvw STYLING COORDINATES, not the shading values — appearance is re-queried at
//     shade time from uvw, so an arbitrarily rich material costs 18 bits in the G-buffer.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_nodes.hpp>

namespace crd::kir::hairgeom
{

inline constexpr double kPi = crd::kir::hair::kPi;

namespace detail
{
using crd::kir::hair::detail::add;
using crd::kir::hair::detail::dv;
using crd::kir::hair::detail::kf;
using crd::kir::hair::detail::mul;
using crd::kir::hair::detail::safe_sqrt;
using crd::kir::hair::detail::sq;
using crd::kir::hair::detail::sub;

struct V3
{
    int x = -1, y = -1, z = -1;
};
[[nodiscard]] inline V3 v3lerp(KGraph& g, const V3& a, const V3& b, int t)
{
    return {add(g, a.x, mul(g, sub(g, b.x, a.x), t)), add(g, a.y, mul(g, sub(g, b.y, a.y), t)),
            add(g, a.z, mul(g, sub(g, b.z, a.z), t))};
}
// Catmull-Rom through p1→p2 with neighbours p0/p3 (the curve form hair meshes and every strand system use: it INTERPOLATES its
// control points, so a strand provably passes through the bundle layers rather than merely being pulled toward them).
[[nodiscard]] inline V3 catmull_rom(KGraph& g, const V3& p0, const V3& p1, const V3& p2, const V3& p3, int t)
{
    const auto k  = [&](double v) { return kf(g, t, v); };
    const int  t2 = sq(g, t);
    const int  t3 = mul(g, t2, t);
    // 0.5·((2p1) + (−p0+p2)t + (2p0−5p1+4p2−p3)t² + (−p0+3p1−3p2+p3)t³)
    const auto comp = [&](int a0, int a1, int a2, int a3) {
        const int c0 = mul(g, k(2.0), a1);
        const int c1 = sub(g, a2, a0);
        const int c2 = sub(g, add(g, mul(g, k(2.0), a0), mul(g, k(4.0), a2)), add(g, mul(g, k(5.0), a1), a3));
        const int c3 = add(g, sub(g, mul(g, k(3.0), a1), mul(g, k(3.0), a2)), sub(g, a3, a0));
        return mul(g, k(0.5), add(g, add(g, c0, mul(g, c1, t)), add(g, mul(g, c2, t2), mul(g, c3, t3))));
    };
    return {comp(p0.x, p1.x, p2.x, p3.x), comp(p0.y, p1.y, p2.y, p3.y), comp(p0.z, p1.z, p2.z, p3.z)};
}
} // namespace detail

struct StrandGenConfig
{
    int layers = 4; // L — hair-mesh extrusion layers (root layer + extrusions)
    int points = 8; // control points generated along each strand
    // MULTI-BUNDLE + PER-STRAND STYLING. Both exist for the same reason: anything read from a CONFIG bakes into the
    //   emitted shader as a literal, so a groom that varies styling per tuft emitted one SHADER PER TUFT (measured: 903
    //   GLSL->SPIR-V compiles per frame, which made a GPU render compile-bound rather than compute-bound). Worse, every
    //   strand inside a bundle then shared ONE curl phase - 128 parallel copies of one curve, which rasterizes as a solid
    //   RIBBON: the "noodle" look. Moving both to buffer data fixes both at once: ONE dispatch for the whole groom, and
    //   every strand independently phased so a tuft reads as interleaved fibres instead of a swept surface.
    int  bundles          = 1;     // tufts in the layer buffer; a thread's bundle = tid / per_bundle
    int  per_bundle       = 0;     // strands per bundle (0 => single bundle, every strand reads bundle 0)
    bool per_strand_style = false; // true => strand buffer stride 6: [u, v, curl_amp, curl_freq, curl_phase, len_frac]
    // Procedural STYLING (Yuksel 2024 §3.4): applied in the strand's local frame so it follows an animating groom. A helical
    // offset is the canonical curl operator; Curly-Cue's Fourier machinery would drive these amplitudes for coiled hair.
    double curl_amp  = 0.0;
    double curl_freq = 6.0;
    double taper     = 1.0; // curl amplitude scale at the tip (1 = uniform, 0 = curl fades to a straight tip)
};

// Generate strands from a hair-mesh bundle. ONE THREAD PER STRAND — nothing about the strand is stored in memory; it exists
// only for the duration of this dispatch, which is the entire point of the hair-mesh representation.
//
// Per output sample the strand position is: bilinear interpolation of the bundle's 4 corner vertices at (u,v) WITHIN each
// layer, then a Catmull-Rom through those per-layer points along the strand. Tangents come from analytic finite differences of
// the same curve (needed for shading — the BCSDF frame is built from the tangent).
//
// Buffers: b0 layers (F32, L·4·3 = 4 corner vertices per layer, xyz) · b1 strands (F32, 2/strand = [u, v])
//          b2 out (F32, nstrand·points·6 = [pos.xyz, tangent.xyz])
[[nodiscard]] inline KEntry build_strand_gen_kernel(KGraph& g, const StrandGenConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const int   nlay = cfg.layers;
    const int   npt  = cfg.points;

    const int lay_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int str_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int nper   = cfg.per_bundle > 0 ? cfg.per_bundle : 0;
    const int stride = cfg.per_strand_style ? 6 : 2;
    const int total  = (nper > 0 ? cfg.bundles * nper : 0);

    const int mark = g.kernel_stmt_mark();
    // A real dispatch rounds up to workgroup granularity, so tail lanes run past the strand count. Guard them, or they
    // write control points into whatever slot their clamped address lands on.
    const int guard = (total > 0) ? g.stmt_if_begin(g.binary(KOp::CmpLt, tid, cu(static_cast<crd::u32>(total)))) : -1;
    const int sb    = g.binary(KOp::Mul, tid, cu(static_cast<crd::u32>(stride)));
    const int u     = g.buffer_load(str_b, sb);
    const int v     = g.buffer_load(str_b, g.binary(KOp::Add, sb, cu(1)));
    g.stmt_materialize(u);
    g.stmt_materialize(v);
    // Per-strand styling, frozen at top level: the sample loop below is a sibling block (the block-scope scar).
    int c_amp = -1;
    int c_frq = -1;
    int c_pha = -1;
    int c_len = -1;
    if (cfg.per_strand_style)
    {
        c_amp = g.buffer_load(str_b, g.binary(KOp::Add, sb, cu(2)));
        c_frq = g.buffer_load(str_b, g.binary(KOp::Add, sb, cu(3)));
        c_pha = g.buffer_load(str_b, g.binary(KOp::Add, sb, cu(4)));
        c_len = g.buffer_load(str_b, g.binary(KOp::Add, sb, cu(5)));
        g.stmt_materialize(c_amp);
        g.stmt_materialize(c_frq);
        g.stmt_materialize(c_pha);
        g.stmt_materialize(c_len);
    }
    const int ob = g.binary(KOp::Mul, tid, cu(static_cast<crd::u32>(npt * 6)));
    g.stmt_materialize(ob); // ⛔ pin the index base at top level — it is first used inside the sample loop (block-scope trap)

    // ── per-layer control point: bilinear blend of the 4 corner vertices at (u,v). All host-indexed, so no runtime indexing. ──
    // The tuft this strand belongs to. With per_bundle == 0 this stays absent and the loads are host-indexed exactly as
    // before; otherwise the layer buffer is indexed at a RUNTIME offset so one dispatch covers the entire groom.
    int lay_base = -1;
    if (nper > 0)
    {
        lay_base = g.binary(KOp::Mul, g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(nper))),
                            cu(static_cast<crd::u32>(nlay * 4 * 3)));
        g.stmt_materialize(lay_base);
    }

    V3 ctrl[16];
    for (int i = 0; i < nlay && i < 16; ++i)
    {
        const auto vert = [&](int c) {
            const int  base = (i * 4 + c) * 3;
            const auto at   = [&](int k) {
                return lay_base >= 0 ? g.binary(KOp::Add, lay_base, cu(static_cast<crd::u32>(base + k)))
                                     : cu(static_cast<crd::u32>(base + k));
            };
            return V3{g.buffer_load(lay_b, at(0)), g.buffer_load(lay_b, at(1)), g.buffer_load(lay_b, at(2))};
        };
        const V3 e0 = v3lerp(g, vert(0), vert(1), u); // bottom edge
        const V3 e1 = v3lerp(g, vert(3), vert(2), u); // top edge
        ctrl[i]     = v3lerp(g, e0, e1, v);
        g.stmt_materialize(ctrl[i].x);
        g.stmt_materialize(ctrl[i].y);
        g.stmt_materialize(ctrl[i].z);
    }

    // ── walk the strand, evaluating the Catmull-Rom through those control points ──
    const int loop = g.stmt_for_begin(cu(static_cast<crd::u32>(npt)));
    const int j    = g.kernel_loop_var(loop);
    const int w    = dv(g, g.cast(j, DType::F32), ks(static_cast<double>(npt - 1))); // w ∈ [0,1] along the strand
    g.stmt_materialize(w);
    // Global spline parameter. A per-strand length fraction ends the strand EARLY along the same curve, so tips within one
    // tuft land at different lengths - real hair has no flat cut, and a uniform one is instantly readable as CG.
    int s = mul(g, w, ks(static_cast<double>(nlay - 1)));
    if (cfg.per_strand_style) { s = mul(g, s, c_len); }

    // The active segment is selected branchlessly; every control point is HOST-indexed, so no runtime array indexing is needed.
    V3 pos{ks(0.0), ks(0.0), ks(0.0)};
    V3 tan{ks(0.0), ks(0.0), ks(1.0)};
    for (int i = 0; i < nlay - 1; ++i)
    {
        const int lt  = sub(g, s, ks(static_cast<double>(i)));
        const int act = (i == nlay - 2) ? g.binary(KOp::CmpGe, s, ks(static_cast<double>(i)))  // last segment also owns w == 1
                                     : g.binary(KOp::BitAnd, g.binary(KOp::CmpGe, s, ks(static_cast<double>(i))),
                                                g.binary(KOp::CmpLt, s, ks(static_cast<double>(i + 1))));
        const V3& p0 = ctrl[i > 0 ? i - 1 : 0];
        const V3& p1 = ctrl[i];
        const V3& p2 = ctrl[i + 1];
        const V3& p3 = ctrl[i + 2 < nlay ? i + 2 : nlay - 1];
        const V3  cp = catmull_rom(g, p0, p1, p2, p3, lt);
        // analytic-ish tangent by a small central difference of the SAME curve (cheap and stays consistent with the position)
        const int   h  = ks(1.0e-3);
        const V3    cf = catmull_rom(g, p0, p1, p2, p3, add(g, lt, h));
        const V3    cb = catmull_rom(g, p0, p1, p2, p3, sub(g, lt, h));
        const V3    td = {sub(g, cf.x, cb.x), sub(g, cf.y, cb.y), sub(g, cf.z, cb.z)};
        pos = {g.select(act, cp.x, pos.x), g.select(act, cp.y, pos.y), g.select(act, cp.z, pos.z)};
        tan = {g.select(act, td.x, tan.x), g.select(act, td.y, tan.y), g.select(act, td.z, tan.z)};
    }
    const int tl = g.binary(KOp::Max, safe_sqrt(g, add(g, add(g, sq(g, tan.x), sq(g, tan.y)), sq(g, tan.z))), ks(1.0e-9));
    tan = {dv(g, tan.x, tl), dv(g, tan.y, tl), dv(g, tan.z, tl)};
    g.stmt_materialize(tan.x);
    g.stmt_materialize(tan.y);
    g.stmt_materialize(tan.z);

    // ── styling: a helical curl in the strand's local frame, tapered along its length. Applied AFTER the base curve so it
    //    follows the animating groom (Yuksel §3.4), and left at amplitude 0 by default so the base geometry is exact. ──
    if (cfg.curl_amp > 0.0 || cfg.per_strand_style)
    {
        // any stable frame perpendicular to the tangent
        const int ax = g.select(g.binary(KOp::CmpLt, g.unary(KOp::Abs, tan.x), ks(0.9)), ks(1.0), ks(0.0));
        const V3  rf = {ax, sub(g, ks(1.0), ax), ks(0.0)};
        const int d  = add(g, add(g, mul(g, rf.x, tan.x), mul(g, rf.y, tan.y)), mul(g, rf.z, tan.z));
        V3        b1 = {sub(g, rf.x, mul(g, d, tan.x)), sub(g, rf.y, mul(g, d, tan.y)), sub(g, rf.z, mul(g, d, tan.z))};
        const int blen = g.binary(KOp::Max, safe_sqrt(g, add(g, add(g, sq(g, b1.x), sq(g, b1.y)), sq(g, b1.z))), ks(1.0e-9));
        b1 = {dv(g, b1.x, blen), dv(g, b1.y, blen), dv(g, b1.z, blen)};
        const V3 b2 = {sub(g, mul(g, tan.y, b1.z), mul(g, tan.z, b1.y)), sub(g, mul(g, tan.z, b1.x), mul(g, tan.x, b1.z)),
                       sub(g, mul(g, tan.x, b1.y), mul(g, tan.y, b1.x))};
        // THE PHASE IS PER STRAND. Without it every strand in a tuft coils in lockstep and the tuft is a swept surface -
        // a ribbon. Decorrelating the phase is what makes neighbouring fibres cross and interleave like real hair.
        const int ph   = cfg.per_strand_style ? add(g, mul(g, mul(g, c_frq, ks(2.0 * kPi)), w), c_pha)
                                              : mul(g, ks(cfg.curl_freq * 2.0 * kPi), w);
        const int amp0 = cfg.per_strand_style ? c_amp : ks(cfg.curl_amp);
        const int am   = mul(g, amp0, add(g, ks(1.0), mul(g, ks(cfg.taper - 1.0), w))); // taper toward the tip
        const int cx = mul(g, am, g.unary(KOp::Cos, ph));
        const int cy = mul(g, am, g.unary(KOp::Sin, ph));
        pos = {add(g, pos.x, add(g, mul(g, cx, b1.x), mul(g, cy, b2.x))),
               add(g, pos.y, add(g, mul(g, cx, b1.y), mul(g, cy, b2.y))),
               add(g, pos.z, add(g, mul(g, cx, b1.z), mul(g, cy, b2.z)))};
    }

    const int jo = g.binary(KOp::Add, ob, g.binary(KOp::Mul, j, cu(6)));
    g.stmt_buffer_store(out_b, jo, pos.x);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, jo, cu(1)), pos.y);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, jo, cu(2)), pos.z);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, jo, cu(3)), tan.x);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, jo, cu(4)), tan.y);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, jo, cu(5)), tan.z);
    g.stmt_for_end(loop);
    if (guard >= 0) { g.stmt_if_end(guard); }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ══════════ THE 64-BIT COMPRESSED STRAND G-BUFFER (Lipp et al. 2026 §3.1.2) ══════════
// Hair is rasterized in COMPUTE and shaded DEFERRED, so all per-pixel shading data must fit in ONE atomicMin-able 64-bit word:
// depth must occupy the HIGH bits so that a plain integer min IS a depth test. The budget:
//
//     [63:40] depth   24b   ·  [39:24] tangent 16b (octahedral)  ·  [23:6] uvw 18b (6+6+6)  ·  [5:0] AO 6b
//
// ⭐ The move that makes an arbitrarily rich material fit in 18 bits: store the uvw STYLING COORDINATES, not shaded values.
// Appearance (albedo, roughness, tilt, melanin…) is RE-QUERIED from uvw at shade time — so the G-buffer cost is independent of
// how complex the hair material becomes. Position is likewise not stored; it is reconstructed from depth.
// ⚠ Deferred shading has no transparency, which matters enormously for hair (strands are thinner than a pixel). Lipp recovers
// it as a POST-PROCESS from a second conservative layer — that is B18-e, not this file.
struct StrandGBufferConfig
{
    double far_plane = 1000.0; // depth is normalised by this before quantisation
};

namespace detail
{
// Octahedral unit-vector encoding: the standard 2-value parameterisation of the sphere with near-uniform error, which is why
// 8+8 bits suffice for a shading tangent (worst-case angular error well under a degree).
inline void octa_encode(KGraph& g, const V3& n, int& ox, int& oy)
{
    const auto k = [&](double v) { return kf(g, n.x, v); };
    const int  l = g.binary(KOp::Max, add(g, add(g, g.unary(KOp::Abs, n.x), g.unary(KOp::Abs, n.y)), g.unary(KOp::Abs, n.z)), k(1.0e-9));
    const int  px = dv(g, n.x, l);
    const int  py = dv(g, n.y, l);
    const int  pz = dv(g, n.z, l);
    const int  sx = g.select(g.binary(KOp::CmpGe, px, k(0.0)), k(1.0), k(-1.0));
    const int  sy = g.select(g.binary(KOp::CmpGe, py, k(0.0)), k(1.0), k(-1.0));
    const int  lo = g.binary(KOp::CmpLt, pz, k(0.0)); // fold the lower hemisphere outward
    ox = g.select(lo, mul(g, sub(g, k(1.0), g.unary(KOp::Abs, py)), sx), px);
    oy = g.select(lo, mul(g, sub(g, k(1.0), g.unary(KOp::Abs, px)), sy), py);
}
inline V3 octa_decode(KGraph& g, int ex, int ey)
{
    const auto k  = [&](double v) { return kf(g, ex, v); };
    const int  z  = sub(g, k(1.0), add(g, g.unary(KOp::Abs, ex), g.unary(KOp::Abs, ey)));
    const int  lo = g.binary(KOp::CmpLt, z, k(0.0));
    const int  sx = g.select(g.binary(KOp::CmpGe, ex, k(0.0)), k(1.0), k(-1.0));
    const int  sy = g.select(g.binary(KOp::CmpGe, ey, k(0.0)), k(1.0), k(-1.0));
    const int  x  = g.select(lo, mul(g, sub(g, k(1.0), g.unary(KOp::Abs, ey)), sx), ex);
    const int  y  = g.select(lo, mul(g, sub(g, k(1.0), g.unary(KOp::Abs, ex)), sy), ey);
    const int  l  = g.binary(KOp::Max, safe_sqrt(g, add(g, add(g, sq(g, x), sq(g, y)), sq(g, z))), k(1.0e-9));
    return {dv(g, x, l), dv(g, y, l), dv(g, z, l)};
}
} // namespace detail

// Pack a strand fragment into the 64-bit layout and immediately unpack it — the round-trip a deferred pass actually performs.
// Buffers: b0 in (F32, 8/lane = [depth, tan.xyz, u, v, w, ao])
//          b1 out (F32, 9/lane = [depth', tan'.xyz, u', v', w', ao', depthBits])   — depthBits is the 24-bit integer, exactly
//          representable in f32's 24-bit mantissa, so the test can verify that an integer compare on it IS a depth test.
[[nodiscard]] inline KEntry build_strand_gbuffer_kernel(KGraph& g, const StrandGBufferConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int in_b  = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int  mark = g.kernel_stmt_mark();
    const int  ib   = g.binary(KOp::Mul, tid, cu(8));
    const auto ld   = [&](int k) {
        const int v = g.buffer_load(in_b, g.binary(KOp::Add, ib, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int depth = ld(0);
    const V3  tan{ld(1), ld(2), ld(3)};
    const int uu = ld(4);
    const int vv = ld(5);
    const int ww = ld(6);
    const int ao = ld(7);

    const auto sat = [&](int x) { return g.binary(KOp::Min, g.binary(KOp::Max, x, ks(0.0)), ks(1.0)); };
    // quantise → dequantise. roundEven matches the CPU oracle's nearbyint (the emitters lower Round that way).
    const auto quant = [&](int x, double levels) {
        const int q = g.unary(KOp::Round, mul(g, sat(x), ks(levels)));
        return q;
    };

    // depth: 24 bits, normalised by the far plane. Occupies the HIGH bits ⇒ integer min == depth test.
    const int dbits = quant(dv(g, depth, ks(cfg.far_plane)), 16777215.0);
    const int drt   = mul(g, dv(g, dbits, ks(16777215.0)), ks(cfg.far_plane));
    // tangent: octahedral, 8 bits per component (range [−1,1] ⇒ map through [0,1])
    int ex = -1;
    int ey = -1;
    octa_encode(g, tan, ex, ey);
    const int qx = quant(mul(g, add(g, ex, ks(1.0)), ks(0.5)), 255.0);
    const int qy = quant(mul(g, add(g, ey, ks(1.0)), ks(0.5)), 255.0);
    const V3  trt = octa_decode(g, sub(g, mul(g, dv(g, qx, ks(255.0)), ks(2.0)), ks(1.0)),
                                sub(g, mul(g, dv(g, qy, ks(255.0)), ks(2.0)), ks(1.0)));
    // uvw styling coordinates: 6 bits each (appearance is re-queried from these at shade time)
    const int qu = quant(uu, 63.0);
    const int qv = quant(vv, 63.0);
    const int qw = quant(ww, 63.0);
    // ambient occlusion: 6 bits
    const int qa = quant(ao, 63.0);

    const int ob = g.binary(KOp::Mul, tid, cu(9));
    const auto st = [&](int k, int val) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(static_cast<crd::u32>(k))), val); };
    st(0, drt);
    st(1, trt.x); st(2, trt.y); st(3, trt.z);
    st(4, dv(g, qu, ks(63.0))); st(5, dv(g, qv, ks(63.0))); st(6, dv(g, qw, ks(63.0)));
    st(7, dv(g, qa, ks(63.0)));
    st(8, dbits);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ══════════ B18-e — THE COMPOSITING FILTER (Lipp et al. 2026 §3.3.1) ══════════
// Deferred hair shading resolves ONE strand per pixel, but strands are thinner than a pixel: the result is severe aliasing and
// visibly disconnected strands. Lipp's answer is not MSAA (which costs linearly in samples) but a RECONSTRUCTION filter that
// exploits what hair actually is — locally a 1-D curve. Two properties make it work:
//
//   · ANISOTROPY. The kernel is an ELLIPSE aligned to the screen-space tangent: wide ALONG the strand (σ∥, so gaps between
//     rasterized fragments of the same strand are bridged — this is the "reconnection"), narrow ACROSS it (σ⊥, so neighbouring
//     strands are never smeared into each other). An isotropic blur cannot do both at once; that is the whole point.
//   · BILATERAL GUARDS. A colour-similarity term plus a hard depth rejection stop the filter bleeding across silhouettes or
//     between strands at different depths.
//
// Result in the paper: 1 spp + this filter ≈ MSAA-8 quality at ~44% of MSAA-8's cost, and it beats the neural upscalers at
// preserving flyaway strands (which are exactly the high-frequency detail those methods erase).
//
//   w_PQ = exp(−d∥²/σ∥² − d⊥²/σ⊥²) · exp(−‖C_P − C_Q‖²/σ_c²),  d∥ = d·T̂,  d⊥ = ‖d − d∥T̂‖
struct HairFilterConfig
{
    int    width        = 640;
    int    height       = 520;
    int    radius       = 5;       // (2r+1)² window — the paper uses r = 5
    double sigma_par    = 4.0;     // ALONG the strand: large ⇒ bridges gaps, reconnecting a broken strand
    double sigma_perp   = 1.0;     // ACROSS the strand: small ⇒ neighbouring strands stay distinct
    double sigma_color  = 0.9;     // colour similarity (paper: 0.9)
    double depth_reject = 1.45e-3; // hard depth cut (paper's threshold) — never blend across a silhouette
};

// Buffers: b0 colour (F32, 3/px) · b1 screen-space tangent (F32, 2/px, unit) · b2 depth (F32, 1/px)
//          b3 out (F32, 4/px = [rgb, weightSum]) — the weight sum is written so tests can verify the partition of unity.
[[nodiscard]] inline KEntry build_hair_filter_kernel(KGraph& g, const HairFilterConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const int   rad = cfg.radius;
    const int   dim = 2 * rad + 1;

    const int col_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int tan_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int dep_b = g.buffer_decl(DType::F32, 0, 2, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 3, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    // ⛔ BOUNDS GUARD. A real dispatch rounds up to workgroup granularity, so the tail workgroup ALWAYS launches lanes past
    //    the pixel count. Without this every one of them runs the full gather and RMW-accumulates into a clamped address —
    //    silently corrupting the last pixel. Never let a kernel's correctness depend on width*height being a multiple of 64.
    const int guard = g.stmt_if_begin(g.binary(KOp::CmpLt, tid, cu(static_cast<crd::u32>(cfg.width * cfg.height))));
    const int px    = g.binary(KOp::Mod, tid, cu(static_cast<crd::u32>(cfg.width)));
    const int py   = g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(cfg.width)));
    const int fpx  = g.cast(px, DType::F32);
    const int fpy  = g.cast(py, DType::F32);
    g.stmt_materialize(fpx);
    g.stmt_materialize(fpy);

    const int c3   = g.binary(KOp::Mul, tid, cu(3));
    const int t2   = g.binary(KOp::Mul, tid, cu(2));
    const int ob   = g.binary(KOp::Mul, tid, cu(4));
    g.stmt_materialize(c3);
    g.stmt_materialize(t2);
    g.stmt_materialize(ob);
    const int cr = g.buffer_load(col_b, c3);
    const int cg = g.buffer_load(col_b, g.binary(KOp::Add, c3, cu(1)));
    const int cb = g.buffer_load(col_b, g.binary(KOp::Add, c3, cu(2)));
    const int tx = g.buffer_load(tan_b, t2);
    const int ty = g.buffer_load(tan_b, g.binary(KOp::Add, t2, cu(1)));
    const int dp = g.buffer_load(dep_b, tid);
    g.stmt_materialize(cr); g.stmt_materialize(cg); g.stmt_materialize(cb);
    g.stmt_materialize(tx); g.stmt_materialize(ty); g.stmt_materialize(dp);

    for (int k = 0; k < 4; ++k) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(static_cast<crd::u32>(k))), ks(0.0)); }

    const int loop = g.stmt_for_begin(cu(static_cast<crd::u32>(dim * dim)));
    const int iv   = g.kernel_loop_var(loop);
    const int dxi  = sub(g, g.cast(g.binary(KOp::Mod, iv, cu(static_cast<crd::u32>(dim))), DType::F32), ks(static_cast<double>(rad)));
    const int dyi  = sub(g, g.cast(g.binary(KOp::Div, iv, cu(static_cast<crd::u32>(dim))), DType::F32), ks(static_cast<double>(rad)));
    const int qx   = add(g, fpx, dxi);
    const int qy   = add(g, fpy, dyi);
    // in-bounds mask (clamp the address, zero the weight) — never wrap around a row edge
    const int inb  = g.binary(KOp::BitAnd,
                             g.binary(KOp::BitAnd, g.binary(KOp::CmpGe, qx, ks(0.0)),
                                      g.binary(KOp::CmpLt, qx, ks(static_cast<double>(cfg.width)))),
                             g.binary(KOp::BitAnd, g.binary(KOp::CmpGe, qy, ks(0.0)),
                                      g.binary(KOp::CmpLt, qy, ks(static_cast<double>(cfg.height)))));
    const int cqx = g.binary(KOp::Min, g.binary(KOp::Max, qx, ks(0.0)), ks(static_cast<double>(cfg.width - 1)));
    const int cqy = g.binary(KOp::Min, g.binary(KOp::Max, qy, ks(0.0)), ks(static_cast<double>(cfg.height - 1)));
    const int qi  = g.cast(add(g, mul(g, cqy, ks(static_cast<double>(cfg.width))), cqx), DType::U32);
    g.stmt_materialize(qi);
    const int q3  = g.binary(KOp::Mul, qi, cu(3));
    const int qr  = g.buffer_load(col_b, q3);
    const int qg  = g.buffer_load(col_b, g.binary(KOp::Add, q3, cu(1)));
    const int qb2 = g.buffer_load(col_b, g.binary(KOp::Add, q3, cu(2)));
    const int qd  = g.buffer_load(dep_b, qi);
    g.stmt_materialize(qr); g.stmt_materialize(qg); g.stmt_materialize(qb2); g.stmt_materialize(qd);

    // ELLIPTICAL spatial term, oriented by the screen-space tangent
    const int dpar  = add(g, mul(g, dxi, tx), mul(g, dyi, ty));                  // d∥ = d·T̂
    const int dperp = sub(g, mul(g, dxi, g.unary(KOp::Neg, ty)), g.unary(KOp::Neg, mul(g, dyi, tx))); // d⊥ = d·T̂⊥
    const int wsp   = g.unary(KOp::Exp, g.unary(KOp::Neg,
                                                add(g, dv(g, sq(g, dpar), ks(cfg.sigma_par * cfg.sigma_par)),
                                                    dv(g, sq(g, dperp), ks(cfg.sigma_perp * cfg.sigma_perp)))));
    // COLOUR-similarity term (edge preserving)
    const int cd2 = add(g, add(g, sq(g, sub(g, qr, cr)), sq(g, sub(g, qg, cg))), sq(g, sub(g, qb2, cb)));
    const int wcl = g.unary(KOp::Exp, g.unary(KOp::Neg, dv(g, cd2, ks(cfg.sigma_color * cfg.sigma_color))));
    // HARD depth rejection — never blend across a silhouette or between strands at different depths
    const int okz = g.binary(KOp::CmpLt, g.unary(KOp::Abs, sub(g, qd, dp)), ks(cfg.depth_reject));
    int       w   = mul(g, wsp, wcl);
    w             = g.select(g.binary(KOp::BitAnd, inb, okz), w, ks(0.0));

    const auto oi = [&](int k) { return g.binary(KOp::Add, ob, cu(static_cast<crd::u32>(k))); };
    g.stmt_buffer_store(out_b, oi(0), add(g, g.buffer_load(out_b, oi(0)), mul(g, w, qr)));
    g.stmt_buffer_store(out_b, oi(1), add(g, g.buffer_load(out_b, oi(1)), mul(g, w, qg)));
    g.stmt_buffer_store(out_b, oi(2), add(g, g.buffer_load(out_b, oi(2)), mul(g, w, qb2)));
    g.stmt_buffer_store(out_b, oi(3), add(g, g.buffer_load(out_b, oi(3)), w));
    g.stmt_for_end(loop);

    // normalise (⛔ freeze the raw sums first — inline loads would re-read slots we are about to overwrite)
    const int sr = g.buffer_load(out_b, oi(0));
    const int sg = g.buffer_load(out_b, oi(1));
    const int sb = g.buffer_load(out_b, oi(2));
    const int sw = g.buffer_load(out_b, oi(3));
    g.stmt_materialize(sr); g.stmt_materialize(sg); g.stmt_materialize(sb); g.stmt_materialize(sw);
    const int nw = g.binary(KOp::Max, sw, ks(1.0e-8));
    g.stmt_buffer_store(out_b, oi(0), dv(g, sr, nw));
    g.stmt_buffer_store(out_b, oi(1), dv(g, sg, nw));
    g.stmt_buffer_store(out_b, oi(2), dv(g, sb, nw));
    g.stmt_buffer_store(out_b, oi(3), sw);
    g.stmt_if_end(guard);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::hairgeom
