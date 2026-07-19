#pragma once

// ckir_oit.hpp — B17-c: the EXACT-REFERENCE order-independent transparency, authored in portable CKIR compute.
//
// The A-buffer / per-pixel-fragment-list OIT (Carpenter 1984) is the GROUND TRUTH the approximate tiers (WBOIT B17-a,
// MBOIT B17-b) are measured against: every translucent fragment is stored, then a per-pixel pass SORTS the fragments by
// depth and composites them EXACTLY with the front-to-back `over` operator. Because that composite is pure f32 mul/add/sub
// on a DETERMINISTIC (sorted) fragment order, the result is BIT-EXACT across every backend and vs the CPU oracle — the
// exact reference is exact, not to-ULP (unlike WBOIT's f16 accumulation + divide).
//
// Two compute kernels, both portable (no atomics, no new IR ops — the fragment→slot mapping is static for a fixed
// per-pixel layer count, so the classic runtime atomic append is replaced by a deterministic slot = fragment index):
//
//   1. `build_abuffer_store` — one thread per FRAGMENT (`W*H*layers` threads). Fragment `tid` belongs to layer
//      `q = tid / (W*H)`; it stores that layer's (colour, coverage, depth) into node slot `tid`. Deferred fragment capture.
//   2. `build_abuffer_resolve` — one thread per PIXEL. Gathers the pixel's `layers` fragments (slot = pixel + q*W*H),
//      SORTS them ascending by depth (a fixed compare-exchange network, fully unrolled — the per-pixel sort every A-buffer
//      resolve performs), and composites front-to-back `C += T·aᵢ·cᵢ ; T *= (1-aᵢ)` over the background. Writes RGB per pixel.

#include <crd/kir/ckir.hpp>

namespace crd::kir::oit
{

// B17-b MBOIT: the 4-power-moment Peters-Klein HAMBURGER reconstruction, SCALAR form. (The vec4 `lighting::msm_hamburger`
// can't lower through the scalar-only compute-kernel emitter, so this transcribes the identical math on scalar moments.)
// Given the normalized power moments β₁..β₄ and a query depth `z ∈ [-1,1]`, returns the "fraction of light" (visibility)
// at `z` — 1 = fully in front of the absorbance mass, 0 = fully behind. Bias toward the uniform-distribution moments
// (0, 0.375, 0, 0.375) kills ill-conditioning. All ops scalar (Add/Sub/Mul/Div/Sqrt/Max/Clamp/Select/Cmp).
[[nodiscard]] inline int msm_hamburger_scalar(crd::kir::KGraph& g, int m1, int m2, int m3, int m4, int z, double moment_bias)
{
    namespace k       = crd::kir;
    const k::Shape sh = k::make_shape({1});
    const auto     cf = [&](double v) { return g.constant(v, sh, k::DType::F32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     dv  = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };

    const double bm = moment_bias;
    const int    b0 = add(mul(m1, cf(1.0 - bm)), cf(0.0));           // biased β₁ (bias moment 0.0)
    const int    b1 = add(mul(m2, cf(1.0 - bm)), cf(0.375 * bm));   // biased β₂ (bias moment 0.375)
    const int    b2 = add(mul(m3, cf(1.0 - bm)), cf(0.0));           // biased β₃
    const int    b3 = add(mul(m4, cf(1.0 - bm)), cf(0.375 * bm));   // biased β₄

    const int l21d11 = sub(b2, mul(b0, b1));         // b2 − b0·b1
    const int d11    = sub(b1, mul(b0, b0));         // b1 − b0²
    const int sqvar  = sub(b3, mul(b1, b1));         // b3 − b1²
    const int d22d11 = sub(mul(sqvar, d11), mul(l21d11, l21d11));
    const int invd11 = dv(cf(1.0), d11);
    const int l21    = mul(l21d11, invd11);
    const int invd22 = dv(d11, d22d11);

    const int z0 = z;                                // depth_bias = 0
    int       c1 = sub(z0, b0);
    int       c2 = sub(sub(mul(z0, z0), b1), mul(l21, c1));
    c1           = mul(c1, invd11);
    c2           = mul(c2, invd22);
    c1           = sub(c1, mul(l21, c2));
    const int c0 = sub(cf(1.0), add(mul(c1, b0), mul(c2, b1)));

    const int p  = dv(c1, c2);
    const int q  = dv(c0, c2);
    const int r  = g.unary(k::KOp::Sqrt, g.binary(k::KOp::Max, sub(mul(mul(p, p), cf(0.25)), q), cf(0.0)));
    const int z1 = sub(mul(g.unary(k::KOp::Neg, p), cf(0.5)), r);
    const int z2 = add(mul(g.unary(k::KOp::Neg, p), cf(0.5)), r);

    const int caseA = g.binary(k::KOp::CmpLt, z2, z0);
    const int caseB = g.binary(k::KOp::CmpLt, z1, z0);
    // sw = caseA ? (z1,z0,1,1) : caseB ? (z0,z1,0,1) : (0,0,0,0)
    const int sw0 = g.select(caseA, z1, g.select(caseB, z0, cf(0.0)));
    const int sw1 = g.select(caseA, z0, g.select(caseB, z1, cf(0.0)));
    const int sw2 = g.select(caseA, cf(1.0), g.select(caseB, cf(0.0), cf(0.0)));
    const int sw3 = g.select(caseA, cf(1.0), g.select(caseB, cf(1.0), cf(0.0)));

    const int quotient  = dv(add(sub(mul(sw0, z2), mul(b0, add(sw0, z2))), b1), mul(sub(z2, sw1), sub(z0, z1)));
    const int intensity = add(sw2, mul(sw3, quotient));
    const int clamped   = g.ternary(k::KOp::Clamp, intensity, cf(0.0), cf(1.0));
    return sub(cf(1.0), clamped); // visibility = 1 − shadow intensity
}

// B17-b (extension): the 6-POWER-MOMENT Hamburger reconstruction — lifts the hero tier to 3 depth masses. The Hamburger solve
// generalizes to a larger Cholesky (4×4 Hankel) + a CUBIC root-solve + a Gauss-Radau (Lagrange-weight) form factor: the
// lower Chebyshev-Markov bound on the CDF at query `z` is the Radau quadrature (fixed node `z` + the 3 roots) summed over the
// nodes strictly in front of `z`. Given normalized power moments β₁..β₆ and query `z ∈ [-1,1]`, returns the "fraction of
// light" (visibility). Exact when the depth distribution has ≤ 3 masses (the bias regularizes the rank-deficient Hankel).
// All ops scalar (Add/Sub/Mul/Div/Sqrt/Max/Min/Clamp/Acos/Cos/Select/Cmp).
[[nodiscard]] inline int msm_hamburger6_scalar(crd::kir::KGraph& g, int m1, int m2, int m3, int m4, int m5, int m6, int z,
                                               double moment_bias)
{
    namespace k       = crd::kir;
    const k::Shape sh = k::make_shape({1});
    const auto     cf = [&](double v) { return g.constant(v, sh, k::DType::F32); };
    const auto     add = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     sub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     mul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     dv  = [&](int a, int b) { return g.binary(k::KOp::Div, a, b); };
    const auto     sq  = [&](int a) { return mul(a, a); };
    const auto     sqrtp = [&](int a) { return g.unary(k::KOp::Sqrt, g.binary(k::KOp::Max, a, cf(0.0))); };

    const double bm = moment_bias;
    const int    om = cf(1.0 - bm);
    // bias toward the uniform-distribution moments on [-1,1] (0, 1/3, 0, 1/5, 0, 1/7) — regularizes the near-singular Hankel.
    const int b1 = mul(m1, om);
    const int b2 = add(mul(m2, om), cf(bm / 3.0));
    const int b3 = mul(m3, om);
    const int b4 = add(mul(m4, om), cf(bm / 5.0));
    const int b5 = mul(m5, om);
    const int b6 = add(mul(m6, om), cf(bm / 7.0));

    // Cholesky of the 4×4 Hankel M[i][j] = b_{i+j} (b0=1): M = L·Lᵀ (L00 = √M00 = 1, elided).
    const int l10 = b1, l20 = b2, l30 = b3;
    const int l11 = sqrtp(sub(b2, sq(l10)));
    const int l21 = dv(sub(b3, mul(l20, l10)), l11);
    const int l31 = dv(sub(b4, mul(l30, l10)), l11);
    const int l22 = sqrtp(sub(sub(b4, sq(l20)), sq(l21)));
    const int l32 = dv(sub(sub(b5, mul(l30, l20)), mul(l31, l21)), l22);
    const int l33 = sqrtp(sub(sub(sub(b6, sq(l30)), sq(l31)), sq(l32)));

    // Solve M·c = (1, z, z², z³): forward L·y = u, back Lᵀ·c = y.
    const int u1 = z, u2 = sq(z), u3 = mul(u2, z);
    const int y0 = cf(1.0); // u0 / l00
    const int y1 = dv(sub(u1, mul(l10, y0)), l11);
    const int y2 = dv(sub(sub(u2, mul(l20, y0)), mul(l21, y1)), l22);
    const int y3 = dv(sub(sub(sub(u3, mul(l30, y0)), mul(l31, y1)), mul(l32, y2)), l33);
    const int c3 = dv(y3, l33);
    const int c2 = dv(sub(y2, mul(l32, c3)), l22);
    const int c1 = dv(sub(sub(y1, mul(l21, c2)), mul(l31, c3)), l11);
    const int c0 = sub(sub(sub(y0, mul(l10, c1)), mul(l20, c2)), mul(l30, c3)); // /l00 = 1

    // Cubic c3·x³ + c2·x² + c1·x + c0 = 0 → monic → depressed t³ + p·t + q (3 real roots via the trigonometric method).
    const int aa = dv(c2, c3), bb = dv(c1, c3), cc = dv(c0, c3);
    const int a3 = dv(aa, cf(3.0));
    const int p  = sub(bb, mul(aa, a3));                                      // B − A²/3
    const int q  = add(sub(mul(cf(2.0), mul(a3, sq(a3))), mul(a3, bb)), cc);  // 2(A/3)³ − (A/3)B + C
    const int mm = mul(cf(2.0), sqrtp(dv(g.unary(k::KOp::Neg, p), cf(3.0)))); // 2√(−p/3)
    const int arg = mul(mul(cf(1.5), dv(q, p)), sqrtp(mul(cf(-3.0), dv(cf(1.0), p)))); // (3q/2p)·√(−3/p)
    const int th  = dv(g.unary(k::KOp::Acos, g.ternary(k::KOp::Clamp, arg, cf(-1.0), cf(1.0))), cf(3.0));
    const double tp = 2.0943951023931953; // 2π/3
    const int x1 = sub(mul(mm, g.unary(k::KOp::Cos, th)), a3);
    const int x2 = sub(mul(mm, g.unary(k::KOp::Cos, sub(th, cf(tp)))), a3);
    const int x3 = sub(mul(mm, g.unary(k::KOp::Cos, sub(th, cf(2.0 * tp)))), a3);

    // Gauss-Radau (nodes z,x1,x2,x3) Lagrange weight at root xi = ∫ Πⱼ≠ᵢ(x−nodeⱼ)dμ / Πⱼ≠ᵢ(xi−nodeⱼ), using β₀..β₃.
    const auto lag = [&](int xi, int xj, int xk) {
        const int e1 = add(add(z, xj), xk);                                  // z+xj+xk
        const int e2 = add(add(mul(z, xj), mul(z, xk)), mul(xj, xk));        // zxj+zxk+xjxk
        const int e3 = mul(z, mul(xj, xk));                                  // z·xj·xk
        const int nn = sub(add(sub(b3, mul(e1, b2)), mul(e2, b1)), e3);      // β₃ − e1·β₂ + e2·β₁ − e3
        const int dd = mul(mul(sub(xi, z), sub(xi, xj)), sub(xi, xk));
        return dv(nn, dd);
    };
    const int w1 = lag(x1, x2, x3);
    const int w2 = lag(x2, x1, x3);
    const int w3 = lag(x3, x1, x2);
    // F = Σ wᵢ over roots strictly in front of z (xi < z) — the lower CDF bound.
    const int f = add(add(g.select(g.binary(k::KOp::CmpLt, x1, z), w1, cf(0.0)),
                          g.select(g.binary(k::KOp::CmpLt, x2, z), w2, cf(0.0))),
                      g.select(g.binary(k::KOp::CmpLt, x3, z), w3, cf(0.0)));
    return sub(cf(1.0), g.ternary(k::KOp::Clamp, f, cf(0.0), cf(1.0))); // visibility = 1 − CDF bound
}

// The maximum per-pixel fragment count the resolve's unrolled sort network supports (a fixed compile-time bound on the
// bounded A-buffer depth).
inline constexpr crd::u32 kMaxAbufferLayers = 8U;

// A-buffer parameters. `layers` = the per-pixel fragment count (the bounded A-buffer depth — the resolve unrolls a
// compare-exchange network of this size). `bg` = the opaque background the sorted transparency composites over.
struct AbufferConfig
{
    crd::u32 width      = 32;
    crd::u32 height     = 32;
    crd::u32 layers     = 4;   // fragments per pixel (the sort-network / gather depth)
    crd::u32 local_size = 64;
    crd::u32 samples    = 256; // B17-c stochastic tier: sub-samples per pixel (== TAA frames accumulated)
    float    bg[3]      = {0.10F, 0.10F, 0.12F};
};

// BUILD kernel: one thread per fragment stores its layer's (r,g,b,a,depth) at node slot = fragment id. Buffers:
// scene (set0 b0, read — `layers*5` f32 = [r,g,b,a,depth] per layer), node_r/g/b/a/depth (b1..b5, read-write, `W*H*layers`
// each). Dispatch `ceil(W*H*layers / local_size)` workgroups.
[[nodiscard]] inline crd::kir::KEntry build_abuffer_store(crd::kir::KGraph& g, const AbufferConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    const int scene = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int nr    = g.buffer_decl(k::DType::F32, 0, 1, true);
    const int ng    = g.buffer_decl(k::DType::F32, 0, 2, true);
    const int nb    = g.buffer_decl(k::DType::F32, 0, 3, true);
    const int na    = g.buffer_decl(k::DType::F32, 0, 4, true);
    const int nd    = g.buffer_decl(k::DType::F32, 0, 5, true);

    const crd::u32 wh    = cfg.width * cfg.height;
    const crd::u32 total = wh * cfg.layers;

    const int mark = g.kernel_stmt_mark();
    const int tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                          g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, tid, cu(total)));

    const int q    = g.binary(k::KOp::Div, tid, cu(wh)); // which layer this fragment belongs to
    const int base = umul(q, cu(5U));
    g.stmt_buffer_store(nr, tid, g.buffer_load(scene, uadd(base, cu(0U))));
    g.stmt_buffer_store(ng, tid, g.buffer_load(scene, uadd(base, cu(1U))));
    g.stmt_buffer_store(nb, tid, g.buffer_load(scene, uadd(base, cu(2U))));
    g.stmt_buffer_store(na, tid, g.buffer_load(scene, uadd(base, cu(3U))));
    g.stmt_buffer_store(nd, tid, g.buffer_load(scene, uadd(base, cu(4U))));
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// RESOLVE kernel: one thread per pixel gathers its `layers` fragments (slot = pixel + q*W*H), sorts ascending by depth via a
// fully-unrolled bubble compare-exchange network (carrying r,g,b,a with the depth key), and composites front-to-back over
// the background. Buffers: node_r/g/b/a/depth (set0 b0..b4, read), out (b5, read-write — `W*H*3` f32, interleaved RGB).
// Dispatch `ceil(W*H / local_size)` workgroups.
[[nodiscard]] inline crd::kir::KEntry build_abuffer_resolve(crd::kir::KGraph& g, const AbufferConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     fadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     fsub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     fmul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    const int nr  = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int ng  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nb  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int na  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int ndp = g.buffer_decl(k::DType::F32, 0, 4, false);
    const int out = g.buffer_decl(k::DType::F32, 0, 5, true);

    const crd::u32 wh = cfg.width * cfg.height;
    const crd::u32 Q  = cfg.layers;

    const int mark  = g.kernel_stmt_mark();
    const int pixel = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                           g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, pixel, cu(wh)));

    // Gather the pixel's `layers` fragments (slot = pixel + q*W*H).
    const crd::u32 nq = Q < kMaxAbufferLayers ? Q : kMaxAbufferLayers;
    int            dd[kMaxAbufferLayers] = {};
    int            rr[kMaxAbufferLayers] = {};
    int            gg[kMaxAbufferLayers] = {};
    int            bb[kMaxAbufferLayers] = {};
    int            aa[kMaxAbufferLayers] = {};
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int slot = uadd(pixel, cu(qi * wh));
        dd[qi]         = g.buffer_load(ndp, slot);
        rr[qi]         = g.buffer_load(nr, slot);
        gg[qi]         = g.buffer_load(ng, slot);
        bb[qi]         = g.buffer_load(nb, slot);
        aa[qi]         = g.buffer_load(na, slot);
    }

    // Bubble compare-exchange network: sort ascending by depth (front-to-back), carrying (r,g,b,a) with the key.
    for (crd::u32 i = 0; i + 1U < nq; ++i)
    {
        for (crd::u32 j = 0; j + 1U + i < nq; ++j)
        {
            const int sw = g.binary(k::KOp::CmpGt, dd[j], dd[j + 1U]); // swap if left is farther
            const auto cx = [&](int* v) {
                const int lo = g.select(sw, v[j + 1U], v[j]);
                const int hi = g.select(sw, v[j], v[j + 1U]);
                v[j]      = lo;
                v[j + 1U] = hi;
            };
            cx(dd); cx(rr); cx(gg); cx(bb); cx(aa);
        }
    }

    // Composite front-to-back: C += T·aᵢ·cᵢ ; T *= (1-aᵢ). Then over the background.
    int tT = cf(1.0);
    int cR = cf(0.0);
    int cG = cf(0.0);
    int cB = cf(0.0);
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int ta = fmul(tT, aa[qi]);       // T·aᵢ
        cR           = fadd(cR, fmul(ta, rr[qi]));
        cG           = fadd(cG, fmul(ta, gg[qi]));
        cB           = fadd(cB, fmul(ta, bb[qi]));
        tT           = fmul(tT, fsub(cf(1.0), aa[qi]));
    }
    const int outR = fadd(cR, fmul(tT, cf(static_cast<double>(cfg.bg[0]))));
    const int outG = fadd(cG, fmul(tT, cf(static_cast<double>(cfg.bg[1]))));
    const int outB = fadd(cB, fmul(tT, cf(static_cast<double>(cfg.bg[2]))));

    const int obase = umul(pixel, cu(3U));
    g.stmt_buffer_store(out, uadd(obase, cu(0U)), outR);
    g.stmt_buffer_store(out, uadd(obase, cu(1U)), outG);
    g.stmt_buffer_store(out, uadd(obase, cu(2U)), outB);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── B17-b: MOMENT-BASED OIT (Münstermann 2018) — the hero glass/foliage quality tier ─────────────────────────────────
//
// The moment-based transmittance estimate: instead of storing/sorting every fragment (A-buffer) or a crude fixed depth
// weight (WBOIT), capture the low-order POWER MOMENTS of the absorbance-over-depth distribution and reconstruct each
// fragment's transmittance `T(zᵢ)` from them via the SAME 4-power-moment Hamburger solve as moment shadow maps
// (`lighting::msm_hamburger`, Peters-Klein 2015 — MBOIT is its direct heir). Per fragment: absorbance `aᵢ = -ln(1-αᵢ)`
// (so `exp(-Σaⱼ) = Π(1-αⱼ)` exactly); warp depth `w = 2d-1 ∈ [-1,1]`; moments `b_k = Σ aᵢ·wᵢ^k` (k=0..4). Normalized
// `β = (b₁,b₂,b₃,b₄)/b₀` feed `msm_hamburger`, which returns the "fraction of light" at `w` — its complement is the
// fraction of absorbance IN FRONT, so `T(zᵢ) = exp(-b₀·(1 - vis(wᵢ)))`. Composite `Σ T(zᵢ)·αᵢ·cᵢ + exp(-b₀)·background`.
// Division + ln/exp ⇒ a TO-ULP tier (validated to tolerance vs `eval_cpu_kernel`, and its error vs the exact A-buffer
// scores the reconstruction). Reuses the SHARED deferred fragment store (`build_abuffer_store`).
//
// RESOLVE kernel: one thread per pixel gathers its `layers` fragments (slot = pixel + q*W*H, from `build_abuffer_store`),
// computes the per-pixel moments, then per-fragment transmittance + composite. Buffers: node_r/g/b/a/depth (set0 b0..b4,
// read), out (b5, read-write — W*H*3 f32 interleaved RGB).
[[nodiscard]] inline crd::kir::KEntry build_mboit_resolve(crd::kir::KGraph& g, const AbufferConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     fadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     fsub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     fmul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    const int nr  = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int ng  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nb  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int na  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int ndp = g.buffer_decl(k::DType::F32, 0, 4, false);
    const int out = g.buffer_decl(k::DType::F32, 0, 5, true);

    const crd::u32 wh = cfg.width * cfg.height;
    const crd::u32 nq = cfg.layers < kMaxAbufferLayers ? cfg.layers : kMaxAbufferLayers;

    const int mark  = g.kernel_stmt_mark();
    const int pixel = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                           g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, pixel, cu(wh)));

    int rr[kMaxAbufferLayers] = {};
    int gg[kMaxAbufferLayers] = {};
    int bb[kMaxAbufferLayers] = {};
    int aa[kMaxAbufferLayers] = {};
    int ww[kMaxAbufferLayers]   = {}; // warped depth 2d-1
    int absb[kMaxAbufferLayers] = {}; // absorbance -ln(1-a)
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int slot = uadd(pixel, cu(qi * wh));
        rr[qi]         = g.buffer_load(nr, slot);
        gg[qi]         = g.buffer_load(ng, slot);
        bb[qi]         = g.buffer_load(nb, slot);
        aa[qi]         = g.buffer_load(na, slot);
        const int d    = g.buffer_load(ndp, slot);
        ww[qi]         = fsub(fmul(cf(2.0), d), cf(1.0)); // 2d-1 -> [-1,1]
        // absorbance = -ln(1 - min(a, 0.9999)) (clamp keeps ln finite for near-opaque fragments)
        const int ac   = g.binary(k::KOp::Min, aa[qi], cf(0.9999));
        absb[qi]       = g.unary(k::KOp::Neg, g.unary(k::KOp::Log, fsub(cf(1.0), ac)));
    }

    // Power moments of the absorbance-over-warped-depth distribution: b_k = Σ absᵢ·wᵢ^k.
    int b0 = cf(0.0);
    int m1 = cf(0.0);
    int m2 = cf(0.0);
    int m3 = cf(0.0);
    int m4 = cf(0.0);
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int w2 = fmul(ww[qi], ww[qi]);
        const int w3 = fmul(w2, ww[qi]);
        const int w4 = fmul(w2, w2);
        b0           = fadd(b0, absb[qi]);
        m1           = fadd(m1, fmul(absb[qi], ww[qi]));
        m2           = fadd(m2, fmul(absb[qi], w2));
        m3           = fadd(m3, fmul(absb[qi], w3));
        m4           = fadd(m4, fmul(absb[qi], w4));
    }
    const int invb0 = g.binary(k::KOp::Div, cf(1.0), g.binary(k::KOp::Max, b0, cf(1.0e-9)));
    const int beta1 = fmul(m1, invb0);
    const int beta2 = fmul(m2, invb0);
    const int beta3 = fmul(m3, invb0);
    const int beta4 = fmul(m4, invb0);

    // Composite: Σ T(zᵢ)·αᵢ·cᵢ + exp(-b₀)·background, T(zᵢ) = exp(-b₀·(1 - vis(wᵢ))).
    int cR = cf(0.0);
    int cG = cf(0.0);
    int cB = cf(0.0);
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int vis    = msm_hamburger_scalar(g, beta1, beta2, beta3, beta4, ww[qi], 6.0e-4); // fraction of light at wᵢ
        const int shadow = fsub(cf(1.0), vis);                                                   // fraction of absorbance in front (CDF at wᵢ)
        const int t      = g.unary(k::KOp::Exp, g.unary(k::KOp::Neg, fmul(b0, shadow)));          // T(zᵢ) = exp(-b₀·G(wᵢ))
        const int ta     = fmul(t, aa[qi]);
        cR               = fadd(cR, fmul(ta, rr[qi]));
        cG               = fadd(cG, fmul(ta, gg[qi]));
        cB               = fadd(cB, fmul(ta, bb[qi]));
    }
    const int tTotal = g.unary(k::KOp::Exp, g.unary(k::KOp::Neg, b0)); // = Π(1-αⱼ)
    const int outR   = fadd(cR, fmul(tTotal, cf(static_cast<double>(cfg.bg[0]))));
    const int outG   = fadd(cG, fmul(tTotal, cf(static_cast<double>(cfg.bg[1]))));
    const int outB   = fadd(cB, fmul(tTotal, cf(static_cast<double>(cfg.bg[2]))));

    const int obase = umul(pixel, cu(3U));
    g.stmt_buffer_store(out, uadd(obase, cu(0U)), outR);
    g.stmt_buffer_store(out, uadd(obase, cu(1U)), outG);
    g.stmt_buffer_store(out, uadd(obase, cu(2U)), outB);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// B17-b (extension) RESOLVE kernel: like `build_mboit_resolve` but with 6 POWER MOMENTS + the cubic `msm_hamburger6_scalar`
// reconstruction — the hero tier lifted to 3-mass depth complexity. Same buffers/dispatch as the 4-moment resolve.
[[nodiscard]] inline crd::kir::KEntry build_mboit6_resolve(crd::kir::KGraph& g, const AbufferConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     fadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     fsub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     fmul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    const int nr  = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int ng  = g.buffer_decl(k::DType::F32, 0, 1, false);
    const int nb  = g.buffer_decl(k::DType::F32, 0, 2, false);
    const int na  = g.buffer_decl(k::DType::F32, 0, 3, false);
    const int ndp = g.buffer_decl(k::DType::F32, 0, 4, false);
    const int out = g.buffer_decl(k::DType::F32, 0, 5, true);

    const crd::u32 wh = cfg.width * cfg.height;
    const crd::u32 nq = cfg.layers < kMaxAbufferLayers ? cfg.layers : kMaxAbufferLayers;

    const int mark  = g.kernel_stmt_mark();
    const int pixel = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                           g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, pixel, cu(wh)));

    int rr[kMaxAbufferLayers] = {};
    int gg[kMaxAbufferLayers] = {};
    int bb[kMaxAbufferLayers] = {};
    int aa[kMaxAbufferLayers] = {};
    int ww[kMaxAbufferLayers]   = {};
    int absb[kMaxAbufferLayers] = {};
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int slot = uadd(pixel, cu(qi * wh));
        rr[qi]         = g.buffer_load(nr, slot);
        gg[qi]         = g.buffer_load(ng, slot);
        bb[qi]         = g.buffer_load(nb, slot);
        aa[qi]         = g.buffer_load(na, slot);
        const int d    = g.buffer_load(ndp, slot);
        ww[qi]         = fsub(fmul(cf(2.0), d), cf(1.0));
        const int ac   = g.binary(k::KOp::Min, aa[qi], cf(0.9999));
        absb[qi]       = g.unary(k::KOp::Neg, g.unary(k::KOp::Log, fsub(cf(1.0), ac)));
    }

    int b0 = cf(0.0), m1 = cf(0.0), m2 = cf(0.0), m3 = cf(0.0), m4 = cf(0.0), m5 = cf(0.0), m6 = cf(0.0);
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int w2 = fmul(ww[qi], ww[qi]);
        const int w3 = fmul(w2, ww[qi]);
        const int w4 = fmul(w2, w2);
        const int w5 = fmul(w4, ww[qi]);
        const int w6 = fmul(w3, w3);
        b0           = fadd(b0, absb[qi]);
        m1           = fadd(m1, fmul(absb[qi], ww[qi]));
        m2           = fadd(m2, fmul(absb[qi], w2));
        m3           = fadd(m3, fmul(absb[qi], w3));
        m4           = fadd(m4, fmul(absb[qi], w4));
        m5           = fadd(m5, fmul(absb[qi], w5));
        m6           = fadd(m6, fmul(absb[qi], w6));
    }
    const int invb0 = g.binary(k::KOp::Div, cf(1.0), g.binary(k::KOp::Max, b0, cf(1.0e-9)));
    const int beta1 = fmul(m1, invb0);
    const int beta2 = fmul(m2, invb0);
    const int beta3 = fmul(m3, invb0);
    const int beta4 = fmul(m4, invb0);
    const int beta5 = fmul(m5, invb0);
    const int beta6 = fmul(m6, invb0);

    int cR = cf(0.0), cG = cf(0.0), cB = cf(0.0);
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int vis    = msm_hamburger6_scalar(g, beta1, beta2, beta3, beta4, beta5, beta6, ww[qi], 1.0e-2);
        const int shadow = fsub(cf(1.0), vis);
        const int t      = g.unary(k::KOp::Exp, g.unary(k::KOp::Neg, fmul(b0, shadow)));
        const int ta     = fmul(t, aa[qi]);
        cR               = fadd(cR, fmul(ta, rr[qi]));
        cG               = fadd(cG, fmul(ta, gg[qi]));
        cB               = fadd(cB, fmul(ta, bb[qi]));
    }
    const int tTotal = g.unary(k::KOp::Exp, g.unary(k::KOp::Neg, b0));
    const int outR   = fadd(cR, fmul(tTotal, cf(static_cast<double>(cfg.bg[0]))));
    const int outG   = fadd(cG, fmul(tTotal, cf(static_cast<double>(cfg.bg[1]))));
    const int outB   = fadd(cB, fmul(tTotal, cf(static_cast<double>(cfg.bg[2]))));

    const int obase = umul(pixel, cu(3U));
    g.stmt_buffer_store(out, uadd(obase, cu(0U)), outR);
    g.stmt_buffer_store(out, uadd(obase, cu(1U)), outG);
    g.stmt_buffer_store(out, uadd(obase, cu(2U)), outB);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// B17-c (scalable): the ATOMIC LINKED-LIST A-buffer BUILD (Carpenter 1984, GPU form) — the DEPLOYABLE fragment capture, not
// the static-slot reference. One thread per FRAGMENT: grab a unique node from the pool via `atomicAdd(counter,1)`, store its
// (colour, coverage, depth), then push it onto the pixel's list via `atomicExchange(head[pixel], slot)` (setting next = old
// head). Handles arbitrary per-pixel overdraw with no advance layer count — the whole point vs the static-slot build. The
// list ORDER is race-nondeterministic; the resolve SORTS ⇒ the final image is deterministic (validate that, not the list).
// Buffers: scene (b0, read — layers*5 = [r,g,b,a,depth]) · counter (b1, rw, u32, pre-0) · head (b2, rw, u32, pre-EMPTY) ·
// node_next (b3, rw, u32) · node_r/g/b/a/depth (b4..b8, rw). Dispatch `ceil(W*H*layers / local_size)` (one per fragment).
inline constexpr crd::u32 kAbufferEmpty = 0xFFFFFFFFU; // empty head / end-of-list sentinel

[[nodiscard]] inline crd::kir::KEntry build_abuffer_atomic_build(crd::kir::KGraph& g, const AbufferConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    // Gold A-buffer layout: ONE interleaved node pool (r,g,b,a,depth per node — stride 5) + a parallel u32 `next` array,
    // exactly the AAA `struct Node{ vec4 col; float z; uint next; }` pool factored into CKIR's single-dtype buffers. Five
    // bindings (was nine), under the descriptor cap and matching how real linked-list OIT stores fragments.
    const int scene    = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int counter  = g.buffer_decl(k::DType::U32, 0, 1, true);
    const int head     = g.buffer_decl(k::DType::U32, 0, 2, true);
    const int nnext    = g.buffer_decl(k::DType::U32, 0, 3, true);
    const int nodedata = g.buffer_decl(k::DType::F32, 0, 4, true); // stride 5: [r,g,b,a,depth] per node

    const crd::u32 wh    = cfg.width * cfg.height;
    const crd::u32 total = wh * cfg.layers; // maxNodes

    const int mark = g.kernel_stmt_mark();
    const int tid  = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                          g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, tid, cu(total)));

    const int q     = g.binary(k::KOp::Div, tid, cu(wh));
    const int pixel = g.binary(k::KOp::Sub, tid, umul(q, cu(wh)));
    const int base  = umul(q, cu(5U));
    const int rv    = g.buffer_load(scene, uadd(base, cu(0U)));
    const int gv    = g.buffer_load(scene, uadd(base, cu(1U)));
    const int bv    = g.buffer_load(scene, uadd(base, cu(2U)));
    const int av    = g.buffer_load(scene, uadd(base, cu(3U)));
    const int dv    = g.buffer_load(scene, uadd(base, cu(4U)));

    const int slot  = g.atomic_add_fetch(counter, cu(0U), cu(1U)); // unique node id (RETURNS the old counter)
    const int ok    = g.stmt_if_begin(g.binary(k::KOp::CmpLt, slot, cu(total)));
    const int nbase = umul(slot, cu(5U));
    g.stmt_buffer_store(nodedata, uadd(nbase, cu(0U)), rv);
    g.stmt_buffer_store(nodedata, uadd(nbase, cu(1U)), gv);
    g.stmt_buffer_store(nodedata, uadd(nbase, cu(2U)), bv);
    g.stmt_buffer_store(nodedata, uadd(nbase, cu(3U)), av);
    g.stmt_buffer_store(nodedata, uadd(nbase, cu(4U)), dv);
    const int prev = g.atomic_exchange(head, pixel, slot); // push onto the pixel's list (RETURNS the old head)
    g.stmt_buffer_store(nnext, slot, prev);
    g.stmt_if_end(ok);
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// B17-c (scalable) RESOLVE: one thread per pixel WALKS its linked list (head → next → …, up to `layers` deep), gathers the
// fragments, SORTS ascending by depth (the same fixed compare-exchange network as the static resolve), and composites
// front-to-back over the background — the deterministic image the race-built list yields. Buffers: head (b0, u32, read) ·
// node_next (b1, u32, read) · node_r/g/b/a/depth (b2..b6, read) · out (b7, rw — W*H*3 interleaved RGB).
[[nodiscard]] inline crd::kir::KEntry build_abuffer_atomic_resolve(crd::kir::KGraph& g, const AbufferConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     fadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     fsub = [&](int a, int b) { return g.binary(k::KOp::Sub, a, b); };
    const auto     fmul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    const int head     = g.buffer_decl(k::DType::U32, 0, 0, false);
    const int nnext    = g.buffer_decl(k::DType::U32, 0, 1, false);
    const int nodedata = g.buffer_decl(k::DType::F32, 0, 2, false); // stride 5: [r,g,b,a,depth] per node
    const int out      = g.buffer_decl(k::DType::F32, 0, 3, true);

    const crd::u32 wh    = cfg.width * cfg.height;
    const crd::u32 total = wh * cfg.layers;
    const crd::u32 nq    = cfg.layers < kMaxAbufferLayers ? cfg.layers : kMaxAbufferLayers;

    const int mark  = g.kernel_stmt_mark();
    const int pixel = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                           g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, pixel, cu(wh)));

    const int  empty  = cu(kAbufferEmpty);
    const int  maxidx = cu(total - 1U);
    const auto clampi = [&](int n) { return g.binary(k::KOp::Min, n, maxidx); }; // keep the load index in-bounds
    // Walk the list: node[0]=head[pixel]; node[qi]=next[node[qi-1]] (guarded — EMPTY once the list ends).
    int node[kMaxAbufferLayers] = {};
    node[0] = g.buffer_load(head, pixel);
    for (crd::u32 qi = 1; qi < nq; ++qi)
    {
        const int valid = g.binary(k::KOp::CmpNe, node[qi - 1], empty);
        node[qi]        = g.select(valid, g.buffer_load(nnext, clampi(node[qi - 1])), empty);
    }
    // Gather (r,g,b,a,depth); an EMPTY node contributes nothing (alpha 0, depth 2 ⇒ sorts to the back).
    int dd[kMaxAbufferLayers] = {}, rr[kMaxAbufferLayers] = {}, gg[kMaxAbufferLayers] = {};
    int bb[kMaxAbufferLayers] = {}, aa[kMaxAbufferLayers] = {};
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int v  = g.binary(k::KOp::CmpNe, node[qi], empty);
        const int nb = umul(clampi(node[qi]), cu(5U)); // node base in the stride-5 pool
        rr[qi]       = g.select(v, g.buffer_load(nodedata, uadd(nb, cu(0U))), cf(0.0));
        gg[qi]       = g.select(v, g.buffer_load(nodedata, uadd(nb, cu(1U))), cf(0.0));
        bb[qi]       = g.select(v, g.buffer_load(nodedata, uadd(nb, cu(2U))), cf(0.0));
        aa[qi]       = g.select(v, g.buffer_load(nodedata, uadd(nb, cu(3U))), cf(0.0));
        dd[qi]       = g.select(v, g.buffer_load(nodedata, uadd(nb, cu(4U))), cf(2.0));
    }
    // Sort ascending by depth (front-to-back), then composite `C += T·aᵢ·cᵢ ; T *= (1-aᵢ)` over the background.
    for (crd::u32 i = 0; i + 1U < nq; ++i)
    {
        for (crd::u32 j = 0; j + 1U + i < nq; ++j)
        {
            const int sw = g.binary(k::KOp::CmpGt, dd[j], dd[j + 1U]);
            const auto cx = [&](int* vv) { const int lo = g.select(sw, vv[j + 1U], vv[j]); const int hi = g.select(sw, vv[j], vv[j + 1U]); vv[j] = lo; vv[j + 1U] = hi; };
            cx(dd); cx(rr); cx(gg); cx(bb); cx(aa);
        }
    }
    int tT = cf(1.0), cR = cf(0.0), cG = cf(0.0), cB = cf(0.0);
    for (crd::u32 qi = 0; qi < nq; ++qi)
    {
        const int ta = fmul(tT, aa[qi]);
        cR           = fadd(cR, fmul(ta, rr[qi]));
        cG           = fadd(cG, fmul(ta, gg[qi]));
        cB           = fadd(cB, fmul(ta, bb[qi]));
        tT           = fmul(tT, fsub(cf(1.0), aa[qi]));
    }
    const int obase = umul(pixel, cu(3U));
    g.stmt_buffer_store(out, uadd(obase, cu(0U)), fadd(cR, fmul(tT, cf(static_cast<double>(cfg.bg[0])))));
    g.stmt_buffer_store(out, uadd(obase, cu(1U)), fadd(cG, fmul(tT, cf(static_cast<double>(cfg.bg[1])))));
    g.stmt_buffer_store(out, uadd(obase, cu(2U)), fadd(cB, fmul(tT, cf(static_cast<double>(cfg.bg[2])))));
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── B17-c (scalable) STOCHASTIC TRANSPARENCY (Enderton et al. 2010) ─────────────────────────────────────────────────────
// The cheap UNBOUNDED-depth tier: no per-pixel list, no sort, no moment budget. Each of S sub-samples keeps the NEAREST
// fragment that stochastically COVERS it (a deterministic hash < αᵢ is the screen-door test); averaging the sub-samples is
// an UNBIASED Monte-Carlo estimate of the exact `over` composite — because P(fragment i is the nearest cover of a sample) =
// αᵢ·Π_{j nearer}(1-αⱼ), whose expectation is exactly the front-to-back `over`. So E[stochastic] == the exact A-buffer, and
// the estimate CONVERGES to it as S grows (~1/√S). The noise is the cost; in a real renderer ONE sample runs per frame and
// TAA accumulates across frames (S == frames). Crucially the RNG is a DETERMINISTIC integer hash of (pixel, fragment, sample)
// ⇒ the "random" result is BIT-IDENTICAL across every backend and vs the CPU oracle — a portable, reproducible stochastic
// tier (TAA history stays consistent). Buffers: scene (b0, read — layers*5 f32) · out (b1, read-write — W*H*3 f32 RGB).
[[nodiscard]] inline crd::kir::KEntry build_stochastic_resolve(crd::kir::KGraph& g, const AbufferConfig& cfg)
{
    namespace k        = crd::kir;
    const k::Shape sh1 = k::make_shape({1});
    const auto     cf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    const auto     cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, k::DType::U32); };
    const auto     uadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     umul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };
    const auto     uxor = [&](int a, int b) { return g.binary(k::KOp::BitXor, a, b); };
    const auto     ushr = [&](int a, int b) { return g.binary(k::KOp::Shr, a, b); };
    const auto     fadd = [&](int a, int b) { return g.binary(k::KOp::Add, a, b); };
    const auto     fmul = [&](int a, int b) { return g.binary(k::KOp::Mul, a, b); };

    // triple32 (Wellons) integer hash of a combined (pixel,fragment,sample) seed → a uniform float in [0,1). Deterministic ⇒
    // the same bits on every backend. Uses the top 24 bits so float(h>>8) is EXACT (≤2^24) ⇒ no rounding, bit-identical.
    const auto hash01 = [&](int seed) {
        int h = seed;
        h = uxor(h, ushr(h, cu(17U)));
        h = umul(h, cu(0xED5AD4BBU));
        h = uxor(h, ushr(h, cu(11U)));
        h = umul(h, cu(0xAC4C1B51U));
        h = uxor(h, ushr(h, cu(15U)));
        h = umul(h, cu(0x31848BABU));
        h = uxor(h, ushr(h, cu(14U)));
        return fmul(g.cast(ushr(h, cu(8U)), k::DType::F32), cf(1.0 / 16777216.0));
    };

    const int scene = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int out   = g.buffer_decl(k::DType::F32, 0, 1, true);

    const crd::u32 wh = cfg.width * cfg.height;
    const crd::u32 L  = cfg.layers;
    const crd::u32 S  = cfg.samples;

    const int mark  = g.kernel_stmt_mark();
    const int pixel = uadd(umul(g.builtin(k::KBuiltin::WorkgroupIndex), cu(cfg.local_size)),
                           g.builtin(k::KBuiltin::LocalInvocationIndex));
    const int guard = g.stmt_if_begin(g.binary(k::KOp::CmpLt, pixel, cu(wh)));
    const int obase = umul(pixel, cu(3U));

    g.stmt_buffer_store(out, uadd(obase, cu(0U)), cf(0.0)); // accumulator ← 0 (one thread per pixel ⇒ plain RMW, no atomics)
    g.stmt_buffer_store(out, uadd(obase, cu(1U)), cf(0.0));
    g.stmt_buffer_store(out, uadd(obase, cu(2U)), cf(0.0));

    const int floop = g.stmt_for_begin(cu(S)); // S sub-samples (== TAA frames); the body is emitted ONCE, run S times
    const int s     = g.kernel_loop_var(floop);
    const int sseed = umul(uadd(s, cu(1U)), cu(0x9E3779B9U)); // decorrelate the sample stream
    // per-sample: keep the NEAREST fragment that stochastically covers this sample (L unrolled — small). No-cover ⇒ background.
    int bestD = cf(2.0), bestR = cf(static_cast<double>(cfg.bg[0])), bestG = cf(static_cast<double>(cfg.bg[1])),
        bestB = cf(static_cast<double>(cfg.bg[2]));
    for (crd::u32 q = 0; q < L; ++q)
    {
        const int fb  = umul(cu(q), cu(5U));
        const int rv  = g.buffer_load(scene, uadd(fb, cu(0U)));
        const int gv  = g.buffer_load(scene, uadd(fb, cu(1U)));
        const int bv  = g.buffer_load(scene, uadd(fb, cu(2U)));
        const int av  = g.buffer_load(scene, uadd(fb, cu(3U)));
        const int dv  = g.buffer_load(scene, uadd(fb, cu(4U)));
        const int rnd  = hash01(uxor(umul(pixel, cu(0x632BE5ABU)), uadd(sseed, umul(cu(q + 1U), cu(0x85157AF5U)))));
        const int cov  = g.binary(k::KOp::CmpLt, rnd, av);      // screen-door coverage: hash < alpha
        const int near = g.binary(k::KOp::CmpLt, dv, bestD);    // and strictly nearer than the current best
        // take = cov && near, expressed as a nested select (cov ? (near ? new : keep) : keep) — no bool constant needed
        bestD = g.select(cov, g.select(near, dv, bestD), bestD);
        bestR = g.select(cov, g.select(near, rv, bestR), bestR);
        bestG = g.select(cov, g.select(near, gv, bestG), bestG);
        bestB = g.select(cov, g.select(near, bv, bestB), bestB);
    }
    g.stmt_buffer_store(out, uadd(obase, cu(0U)), fadd(g.buffer_load(out, uadd(obase, cu(0U))), bestR)); // accumulate
    g.stmt_buffer_store(out, uadd(obase, cu(1U)), fadd(g.buffer_load(out, uadd(obase, cu(1U))), bestG));
    g.stmt_buffer_store(out, uadd(obase, cu(2U)), fadd(g.buffer_load(out, uadd(obase, cu(2U))), bestB));
    g.stmt_for_end(floop);

    const int invS = cf(1.0 / static_cast<double>(S)); // mean over samples ⇒ the unbiased `over` estimate
    g.stmt_buffer_store(out, uadd(obase, cu(0U)), fmul(g.buffer_load(out, uadd(obase, cu(0U))), invS));
    g.stmt_buffer_store(out, uadd(obase, cu(1U)), fmul(g.buffer_load(out, uadd(obase, cu(1U))), invS));
    g.stmt_buffer_store(out, uadd(obase, cu(2U)), fmul(g.buffer_load(out, uadd(obase, cu(2U))), invS));
    g.stmt_if_end(guard);

    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = cfg.local_size;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::oit
