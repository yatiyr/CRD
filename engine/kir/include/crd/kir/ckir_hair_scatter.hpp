#pragma once

// ckir_hair_scatter.hpp — D-007 B18-c: HAIR/FUR MULTIPLE SCATTERING (three tiers) + self-shadowing.
//
// Research: docs/research/2026-07-19-hair-fur-frontier-collection.md. ckir_hair.hpp owns the SINGLE-FIBRE BCSDF; this header
// owns everything that happens BETWEEN fibres. The load-bearing structural fact from the literature study:
//
//        *** every multiple-scattering tier is an INTEGRAL OF THE SINGLE-FIBRE BCSDF ***
//
//   · Zinke 2008 dual scattering: ā_f / ā_b are front/back hemisphere averages of f_s (Eq 6 / Eq 12).
//   · Hu 2026 volumetric MS:      Albedo(ωi) = ∫ S dωo, and the volume's PHASE FUNCTION is literally S normalised.
//
// So ONE precomputed LUT over our own BCSDF feeds BOTH tiers — nothing is re-authored, and any improvement to the fibre model
// (medulla, Huang microfacet) propagates into the scattering tiers for free. That LUT is what this file builds first.
//
// TIERS (all three land in B18-c; see the dossier for why each exists):
//   1. `build_hair_scatter_lut_kernel`  — the shared moment LUT (this file's foundation).
//   2. Zinke 2008 dual scattering        — the fast classic tier; closed-form from the LUT.
//   3. Hu 2026 volumetric approximation  — the GOLD tier that matches path tracing and fixes dual scattering's curly-hair failure.
//   + Yuksel 2008 deep opacity maps      — the self-shadow transmittance both tiers consume.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_nodes.hpp>

namespace crd::kir::hairms
{

inline constexpr double kPi = crd::kir::hair::kPi;

// The moment LUT layout: 5 floats per θd bin.
//   [0] ā_f  average FORWARD  scattering attenuation  (Zinke Eq 6)   — also Hu's Albedo = ā_f + ā_b
//   [1] ā_b  average BACKWARD scattering attenuation  (Zinke Eq 12)
//   [2] β̄f² average forward longitudinal VARIANCE     (Zinke Eq 8 accumulates these along the shadow path)
//   [3] Δ̄_b average backward longitudinal SHIFT       (Zinke Eq 15)
//   [4] σ̄_b² average backward longitudinal VARIANCE   (Zinke Eq 15)
inline constexpr int kLutStride = 5;

struct HairScatterLutConfig
{
    int n_theta_d = 64; // LUT bins over θd (one KERNEL LANE each ⇒ keep a multiple of 64)
    int n_h       = 4;  // fibre-offset (near-field h) samples to average over
    int n_theta_o = 24; // outgoing longitudinal samples
    int n_phi_o   = 48; // outgoing azimuthal samples
    // the single-fibre model the moments are taken FROM (defaults match ckir_hair.hpp)
    double eta       = 1.55;
    double sigma_a   = 0.2;
    double beta_m    = 0.3;
    double beta_n    = 0.3;
    double alpha_deg = 2.0;
    double fur_kappa = 0.0; // >0 ⇒ the medulla lobe participates in the scattering moments too
    double fur_sigma = 2.0;
    double fur_albedo = 0.9;
    double fur_g      = 0.0;
    double fur_beta_s = 0.8;
};

// Build the shared scattering-moment LUT. ONE THREAD PER θd BIN; each thread numerically integrates the single-fibre BCSDF over
// the outgoing sphere (and averages over the fibre offset h, since our BCSDF is Chiang's NEAR-FIELD form), splitting the result
// into the FORWARD and BACKWARD azimuthal hemispheres and accumulating their longitudinal moments.
//
// Measure: ∫ f·cos²θo dθo dφo — identical to the white-furnace gate in tests/kir/test_ckir_hair.cpp, which is what makes
// ā_f + ā_b == the fibre's directional albedo an exactly checkable invariant (and the test asserts precisely that).
//
// Buffers: out (b0, F32, n_theta_d·5). No inputs — the fibre parameters are baked from the config.
[[nodiscard]] inline KEntry build_hair_scatter_lut_kernel(KGraph& g, const HairScatterLutConfig& cfg)
{
    using namespace crd::kir::hair::detail; // kf/mul/add/sub/dv/sq/safe_sqrt
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };

    const int out_b = g.buffer_decl(DType::F32, 0, 0, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const auto ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    // θd for this lane (the inclination of the incident/direct illumination)
    const double dtd     = kPi / static_cast<double>(cfg.n_theta_d);
    const int    theta_d = add(g, ks(-0.5 * kPi), mul(g, add(g, g.cast(tid, DType::F32), ks(0.5)), ks(dtd)));
    g.stmt_materialize(theta_d);
    const int sin_ti = g.unary(KOp::Sin, theta_d);
    const int cos_ti = g.unary(KOp::Cos, theta_d);
    g.stmt_materialize(sin_ti);
    g.stmt_materialize(cos_ti);

    const int base = g.binary(KOp::Mul, tid, cu(static_cast<crd::u32>(kLutStride)));
    for (int k = 0; k < kLutStride; ++k) { g.stmt_buffer_store(out_b, g.binary(KOp::Add, base, cu(static_cast<crd::u32>(k))), ks(0.0)); }

    // ONE flattened loop over (h, θo, φo) — CKIR For carries no loop register, and a flat index avoids nesting entirely.
    const int    nt    = cfg.n_theta_o;
    const int    np    = cfg.n_phi_o;
    const int    nh    = cfg.n_h;
    const double dth   = kPi / static_cast<double>(nt);
    const double dph   = 2.0 * kPi / static_cast<double>(np);
    const double wscale = dth * dph / static_cast<double>(nh); // the ∫ dθo dφo measure, averaged over h

    const int floop = g.stmt_for_begin(cu(static_cast<crd::u32>(nh * nt * np)));
    const int iv    = g.kernel_loop_var(floop);
    const int ntnp  = cu(static_cast<crd::u32>(nt * np));
    const int ih    = g.binary(KOp::Div, iv, ntnp);
    const int rem   = g.binary(KOp::Mod, iv, ntnp);
    const int it    = g.binary(KOp::Div, rem, cu(static_cast<crd::u32>(np)));
    const int ip    = g.binary(KOp::Mod, rem, cu(static_cast<crd::u32>(np)));

    const int hoff  = add(g, ks(-1.0), mul(g, add(g, g.cast(ih, DType::F32), ks(0.5)), ks(2.0 / static_cast<double>(nh))));
    const int th_o  = add(g, ks(-0.5 * kPi), mul(g, add(g, g.cast(it, DType::F32), ks(0.5)), ks(dth)));
    const int ph_o  = add(g, ks(-kPi), mul(g, add(g, g.cast(ip, DType::F32), ks(0.5)), ks(dph)));
    g.stmt_materialize(hoff);
    g.stmt_materialize(th_o);
    g.stmt_materialize(ph_o);
    const int sin_to = g.unary(KOp::Sin, th_o);
    const int cos_to = g.unary(KOp::Cos, th_o);
    g.stmt_materialize(sin_to);
    g.stmt_materialize(cos_to);

    // the single-fibre BCSDF (incident azimuth 0; scalar σₐ ⇒ the whole cone rides the scalar compute emitter)
    const int f = crd::kir::hair::hair_bcsdf_eval_angles(g, sin_ti, cos_ti, ks(0.0), sin_to, cos_to, ph_o, hoff, ks(cfg.eta),
                                                         ks(cfg.sigma_a), ks(cfg.beta_m), ks(cfg.beta_n), ks(cfg.alpha_deg),
                                                         cfg.fur_kappa, cfg.fur_sigma, cfg.fur_albedo, cfg.fur_g, cfg.fur_beta_s);
    const int fw = mul(g, f, mul(g, sq(g, cos_to), ks(wscale))); // f · cos²θo · dθdφ/Nh
    g.stmt_materialize(fw);

    // ⛔ FORWARD is the FAR azimuthal half (|φo| > π/2), NOT the near one. Zinke's "forward scattering" means light CONTINUING
    // ONWARD past the fibre, and our own azimuthal model says exactly where that lands: hair_np centres lobe p at
    // Φ(p) = 2p·γt − 2γo + p·π, so the R lobe peaks at φo ≈ φi (light returned TOWARD the source = BACKWARD) while the strong
    // TT lobe peaks at φo ≈ φi ± π (light passed THROUGH = FORWARD). Getting this backwards silently swaps ā_f and ā_b and
    // inverts dual scattering's whole global term; the `ā_f > ā_b` assertion in the tests is what pins it down.
    // Longitudinal deflection Δ = θo + θd is 0 on the specular cone, so its moments ARE the spread statistics Zinke needs.
    const int fwd = g.binary(KOp::CmpGt, g.unary(KOp::Abs, ph_o), ks(0.5 * kPi));
    const int dl  = add(g, th_o, theta_d);
    const int zero = ks(0.0);
    const int f_f = g.select(fwd, fw, zero);
    const int f_b = g.select(fwd, zero, fw);
    const auto idx = [&](int k) { return g.binary(KOp::Add, base, cu(static_cast<crd::u32>(k))); };
    g.stmt_buffer_store(out_b, idx(0), add(g, g.buffer_load(out_b, idx(0)), f_f));                        // ā_f
    g.stmt_buffer_store(out_b, idx(1), add(g, g.buffer_load(out_b, idx(1)), f_b));                        // ā_b
    g.stmt_buffer_store(out_b, idx(2), add(g, g.buffer_load(out_b, idx(2)), mul(g, f_f, sq(g, dl))));     // Σ f·Δ²  (forward)
    g.stmt_buffer_store(out_b, idx(3), add(g, g.buffer_load(out_b, idx(3)), mul(g, f_b, dl)));            // Σ f·Δ   (backward)
    g.stmt_buffer_store(out_b, idx(4), add(g, g.buffer_load(out_b, idx(4)), mul(g, f_b, sq(g, dl))));     // Σ f·Δ²  (backward)
    g.stmt_for_end(floop);

    // Normalise the raw sums into the moments the tiers consume.
    // ⛔ READ-AFTER-WRITE HAZARD: buffer loads are INLINE in the compute emitter (re-read at every use, which is what makes
    // them correct across barriers). So an expression that loads slot k is re-evaluated when it is finally emitted — and if an
    // EARLIER store in this same sequence already overwrote slot k, it silently reads the mutated value. That is exactly what
    // happened here: σ̄_b² depends on Δ̄_b, whose load of slot 3 was clobbered by the store of Δ̄_b itself, collapsing σ̄_b² to 0
    // (and with it the whole backscattering lobe). MATERIALIZE the raw sums first so every moment is computed from frozen
    // pre-store temps.
    const int raw_af  = g.buffer_load(out_b, idx(0));
    const int raw_ab  = g.buffer_load(out_b, idx(1));
    const int raw_m2f = g.buffer_load(out_b, idx(2));
    const int raw_m1b = g.buffer_load(out_b, idx(3));
    const int raw_m2b = g.buffer_load(out_b, idx(4));
    g.stmt_materialize(raw_af);
    g.stmt_materialize(raw_ab);
    g.stmt_materialize(raw_m2f);
    g.stmt_materialize(raw_m1b);
    g.stmt_materialize(raw_m2b);
    const int safe_af = g.binary(KOp::Max, raw_af, ks(1.0e-9));
    const int safe_ab = g.binary(KOp::Max, raw_ab, ks(1.0e-9));
    const int bf2 = dv(g, raw_m2f, safe_af);                                            // β̄f² = E[Δ²] about the specular cone
    const int db  = dv(g, raw_m1b, safe_ab);                                            // Δ̄_b = E[Δ]
    const int sb2 = g.binary(KOp::Max, sub(g, dv(g, raw_m2b, safe_ab), sq(g, db)), ks(0.0)); // σ̄_b² = E[Δ²] − E[Δ]²
    g.stmt_buffer_store(out_b, idx(2), bf2);
    g.stmt_buffer_store(out_b, idx(3), db);
    g.stmt_buffer_store(out_b, idx(4), sb2);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ══════════ TIER 1 — DUAL SCATTERING (Zinke/Yuksel/Weber/Keyser, SIGGRAPH 2008) ══════════
// Ψ = Ψ^G·(1 + Ψ^L)  (Eq 3): light reaching a fibre is split into GLOBAL multiple scattering (how much survives the walk
// through the groom toward the shading point) and LOCAL multiple scattering (what bounces around in the immediate
// neighbourhood). Its whole efficiency rests on one simplification: along the SHADOW PATH only FORWARD scattering matters, so
// the global term is a product of average forward attenuations — and every one of those averages is a LUT lookup above.
//
// ⚠ Known failure modes (from the dossier — this is why the Hu-2026 volumetric tier also ships): the local-similarity
// assumption breaks on curly hair, it considers only R/TT, assumes infinite strands behind the shading point, and yields a
// characteristically "dry" look. Prefer this tier for cost, the volumetric tier for fidelity.
namespace detail
{
using crd::kir::hair::detail::add;
using crd::kir::hair::detail::dv;
using crd::kir::hair::detail::kf;
using crd::kir::hair::detail::mul;
using crd::kir::hair::detail::safe_sqrt;
using crd::kir::hair::detail::sq;
using crd::kir::hair::detail::sub;

// Zinke's g(a, b): a UNIT-AREA Gaussian in variable `a` with zero mean and VARIANCE `b` (paper §2.2 notation).
[[nodiscard]] inline int gauss_var(KGraph& g, int x, int v)
{
    const auto k  = [&](double c) { return kf(g, x, c); };
    const int  vv = g.binary(KOp::Max, v, k(1.0e-6)); // n=0 ⇒ variance 0 ⇒ the paper's delta; clamp to a very narrow Gaussian
    return dv(g, g.unary(KOp::Exp, g.unary(KOp::Neg, dv(g, sq(g, x), mul(g, k(2.0), vv)))),
              safe_sqrt(g, mul(g, k(2.0 * kPi), vv)));
}

// FORWARD azimuthal indicator, matching the LUT's convention EXACTLY (the far half-cone, |Δφ| > π/2, is where TT lands —
// see the long note on the LUT's `fwd`). s̃ = 1/π on the selected half, 0 on the other.
[[nodiscard]] inline int azim_sel(KGraph& g, int dphi, bool forward)
{
    const auto k  = [&](double c) { return kf(g, dphi, c); };
    int        w  = sub(g, dphi, mul(g, k(2.0 * kPi), g.unary(KOp::Round, dv(g, dphi, k(2.0 * kPi))))); // wrap to (−π, π]
    const int  far = g.binary(KOp::CmpGt, g.unary(KOp::Abs, w), k(0.5 * kPi));
    return forward ? g.select(far, k(1.0 / kPi), k(0.0)) : g.select(far, k(0.0), k(1.0 / kPi));
}
} // namespace detail

struct DualScatterConfig
{
    double d_f = 0.7; // forward density factor (Zinke: 0.6–0.8 realistic for human hair; the paper uses 0.7 throughout)
    double d_b = 0.7; // backward density factor (approximated as equal to d_f)
};

// Evaluate dual scattering's two terms from the moment LUT.
//   psi_g  = Ψ^G = T_f·S_f              — global: what survives the walk to the shading point (Eq 4/5/7)
//   f_back = (2/cosθ)·Ā_b·S̄_b           — local backscattering BCSDF (Eq 10/11/13/14/15)
// The caller combines them as Ψ·f_s = Ψ^G·(f_s + d_b·f_back), since Ψ^L·f_s ≈ d_b·f_back (Eq 9).
//
// `n_strands` is the number of fibres crossed along the shadow path (that is what the deep-opacity map supplies). Because a
// realistic groom is locally similar, Zinke evaluates every scattering event at the SAME θd, so the product in Eq 5 collapses
// to ā_f^n and the variance sum in Eq 8 to n·β̄f² — which is why this tier costs a pow and an exp, not a loop.
inline void dual_scatter_terms(KGraph& g, int af, int ab, int bf2, int db_shift, int sb2, int n_strands, int theta_d,
                               int theta_i, int theta_o, int dphi_di, int dphi_io, const DualScatterConfig& cfg, int& psi_g,
                               int& f_back)
{
    using namespace detail;
    const auto k     = [&](double c) { return kf(g, theta_d, c); };
    const int  af_s  = g.binary(KOp::Min, g.binary(KOp::Max, af, k(1.0e-5)), k(0.999)); // keep 1−ā_f² away from 0
    const int  cos_d = g.binary(KOp::Max, g.unary(KOp::Abs, g.unary(KOp::Cos, theta_d)), k(1.0e-4));

    // ── GLOBAL: T_f = d_f·∏ā_f  →  d_f·ā_f^n (locally-similar cluster); spread variance σ̄f² = Σβ̄f² → n·β̄f² ──
    const int t_f  = mul(g, k(cfg.d_f), g.unary(KOp::Exp, mul(g, n_strands, g.unary(KOp::Log, af_s))));
    const int sf2  = mul(g, n_strands, bf2);
    const int s_f  = mul(g, dv(g, azim_sel(g, dphi_di, true), cos_d), gauss_var(g, add(g, theta_d, theta_i), sf2));
    psi_g          = mul(g, t_f, s_f);

    // ── LOCAL: the backscattering series. Ā_1 = ā_b·ā_f²/(1−ā_f²), Ā_3 = ā_b³·ā_f²/(1−ā_f²)³ (Eq 11/13) — closed forms of
    //    the infinite path sums with one and three backward events; higher orders are negligible for hair (Eq 14). ──
    const int om    = g.binary(KOp::Max, sub(g, k(1.0), sq(g, af_s)), k(1.0e-5));
    const int a1    = dv(g, mul(g, ab, sq(g, af_s)), om);
    const int a3    = dv(g, mul(g, mul(g, ab, sq(g, ab)), sq(g, af_s)), mul(g, om, sq(g, om)));
    const int a_b   = add(g, a1, a3);
    // θ = (θo − θi)/2 is the difference angle Eq 10's 2/cosθ and Eq 15's spread are written against.
    const int th    = mul(g, k(0.5), sub(g, theta_o, theta_i));
    const int cos_t = g.binary(KOp::Max, g.unary(KOp::Abs, g.unary(KOp::Cos, th)), k(1.0e-4));
    const int s_b   = mul(g, dv(g, azim_sel(g, dphi_io, false), cos_t),
                        gauss_var(g, sub(g, add(g, theta_o, theta_i), db_shift), sb2));
    f_back          = mul(g, dv(g, k(2.0), cos_t), mul(g, a_b, s_b));
}

// Test/eval kernel for the dual-scattering tier. Buffers:
//   b0 lut (F32, bins·5, from build_hair_scatter_lut_kernel) · b1 in (F32, 6/lane: θd, θi, θo, Δφ_di, Δφ_io, n)
//   b2 out (F32, 2/lane: Ψ^G, f_back)
[[nodiscard]] inline KEntry build_dual_scatter_kernel(KGraph& g, const DualScatterConfig& cfg, int lut_bins)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int lut_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int in_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int base = g.binary(KOp::Mul, tid, cu(6));
    const auto ld  = [&](int off) {
        const int v = g.buffer_load(in_b, g.binary(KOp::Add, base, cu(static_cast<crd::u32>(off))));
        g.stmt_materialize(v);
        return v;
    };
    const int th_d = ld(0), th_i = ld(1), th_o = ld(2), dp_di = ld(3), dp_io = ld(4), nst = ld(5);

    // nearest-bin LUT fetch at θd (bins span [−π/2, π/2])
    const int fb   = mul(g, dv(g, add(g, th_d, ks(0.5 * kPi)), ks(kPi)), ks(static_cast<double>(lut_bins)));
    const int bin  = g.cast(g.binary(KOp::Min, g.binary(KOp::Max, fb, ks(0.0)), ks(static_cast<double>(lut_bins - 1))), DType::U32);
    const int lbase = g.binary(KOp::Mul, bin, cu(static_cast<crd::u32>(kLutStride)));
    const auto lut  = [&](int k2) {
        const int v = g.buffer_load(lut_b, g.binary(KOp::Add, lbase, cu(static_cast<crd::u32>(k2))));
        g.stmt_materialize(v);
        return v;
    };

    int psi_g = -1;
    int fback = -1;
    dual_scatter_terms(g, lut(0), lut(1), lut(2), lut(3), lut(4), nst, th_d, th_i, th_o, dp_di, dp_io, cfg, psi_g, fback);

    const int ob = g.binary(KOp::Mul, tid, cu(2));
    g.stmt_buffer_store(out_b, ob, psi_g);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(1)), fback);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ══════════ SELF-SHADOW — DEEP OPACITY MAPS (Yuksel & Keyser, EG 2008) ══════════
// The transmittance both scattering tiers consume (and the source of dual scattering's strand count n).
//
// The idea that makes it beat opacity shadow maps: instead of slicing the hair with planes at FIXED distances from the light,
// first render a depth map to get z0 — the depth at which hair BEGINS for that pixel — and place the layer boundaries at
// z0 + d_k. The separators then CONFORM to the shape of the groom, so (a) unshadowed direct illumination is captured exactly,
// (b) opacity interpolation happens INSIDE the hair volume, hiding layering artifacts, and (c) ~3 layers suffice where opacity
// shadow maps need 16-128 and still stripe. That conformance is the property the tests below pin down.
//
// Storage note (paper §3): depth + 3 opacity layers fit ONE RGBA target; n draw buffers give 4n−1 layers; an 8-bit depth map
// is visually indistinguishable from 16-bit float here, because the depth is only used to POSITION layers, never as a binary
// in/out shadow test.
struct DomConfig
{
    int    layers      = 4;    // K. The paper finds 3 usually sufficient.
    double span        = 4.0;  // total depth extent the layers cover, measured from z0
    int    frags_per_px = 16;  // fragments per pixel in the build buffer
};

// PASS 1+2 (build): one thread per pixel. First reduce the fragment depths to z0 (the depth map), then accumulate each
// fragment's opacity into the layer it falls in AND every layer behind it — so slot k holds the CUMULATIVE opacity through
// layer k, which is exactly what the lookup interpolates.
// Buffers: b0 frags (F32, pixels·frags·2 = [depth, alpha]) · b1 out DOM (F32, pixels·(1+K) = [z0, O_0..O_{K−1}]).
[[nodiscard]] inline KEntry build_dom_build_kernel(KGraph& g, const DomConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const int   K   = cfg.layers;
    const int   nf  = cfg.frags_per_px;
    const double dz = cfg.span / static_cast<double>(K);

    const int frag_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 1, true);
    const int tid    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int  mark = g.kernel_stmt_mark();
    const int  obase = g.binary(KOp::Mul, tid, cu(static_cast<crd::u32>(1 + K)));
    const auto oidx  = [&](int k) { return g.binary(KOp::Add, obase, cu(static_cast<crd::u32>(k))); };
    const int  fbase = g.binary(KOp::Mul, tid, cu(static_cast<crd::u32>(nf * 2)));
    // ⛔ BLOCK SCOPE: an arithmetic node is emitted as a temp at its FIRST USE. `fbase` is first used inside the depth-reduction
    // loop, so its temp would be declared in THAT loop's body — and the opacity loop below, a sibling scope, would then
    // reference an out-of-scope identifier ("undeclared identifier t<n>"). Pin both index bases at top level.
    g.stmt_materialize(obase);
    g.stmt_materialize(fbase);

    // ── depth map: z0 = min over this pixel's fragment depths ──
    g.stmt_buffer_store(out_b, oidx(0), ks(1.0e30));
    const int l1 = g.stmt_for_begin(cu(static_cast<crd::u32>(nf)));
    const int f1 = g.kernel_loop_var(l1);
    const int d1 = g.buffer_load(frag_b, g.binary(KOp::Add, fbase, g.binary(KOp::Mul, f1, cu(2))));
    g.stmt_buffer_store(out_b, oidx(0), g.binary(KOp::Min, g.buffer_load(out_b, oidx(0)), d1));
    g.stmt_for_end(l1);

    // ⛔ freeze z0 BEFORE the accumulation loop — buffer loads are inline and would otherwise re-read a slot we go on to touch
    const int z0 = g.buffer_load(out_b, oidx(0));
    g.stmt_materialize(z0);
    for (int k = 0; k < K; ++k) { g.stmt_buffer_store(out_b, oidx(1 + k), ks(0.0)); }

    // ── opacity: each fragment adds to its own layer and all layers BEHIND it ⇒ slot k is cumulative through layer k ──
    const int l2 = g.stmt_for_begin(cu(static_cast<crd::u32>(nf)));
    const int f2 = g.kernel_loop_var(l2);
    const int fo = g.binary(KOp::Add, fbase, g.binary(KOp::Mul, f2, cu(2)));
    const int df = g.buffer_load(frag_b, fo);
    const int af = g.buffer_load(frag_b, g.binary(KOp::Add, fo, cu(1)));
    g.stmt_materialize(df);
    g.stmt_materialize(af);
    // ⚠ PRECISION: t = depth − z0 is a cancelling subtraction of two potentially large depths, so its absolute error grows with
    // the scene's depth range. A fragment sitting EXACTLY on a layer boundary can therefore land either side of floor() — a
    // measure-zero tie, but it means layer assignment should never be relied on to be stable for boundary-coincident geometry
    // (and it is why the paper positions layers from a per-pixel z0 rather than from absolute depth).
    const int tf  = sub(g, df, z0);                                    // distance INTO the hair from the front surface
    const int lay = g.unary(KOp::Floor, dv(g, tf, ks(dz)));            // which layer this fragment lands in
    g.stmt_materialize(lay);
    for (int k = 0; k < K; ++k) // K is a host constant ⇒ unrolled; no nested runtime loop
    {
        const int contrib = g.select(g.binary(KOp::CmpLe, lay, ks(static_cast<double>(k))), af, ks(0.0));
        g.stmt_buffer_store(out_b, oidx(1 + k), add(g, g.buffer_load(out_b, oidx(1 + k)), contrib));
    }
    g.stmt_for_end(l2);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// PASS 3 (lookup): transmittance at an arbitrary depth. The accumulated opacity is piecewise-linearly interpolated through
// (0,0), (d_1,O_0), … (d_K,O_{K−1}) — interpolating INSIDE the hair volume is what hides the layer boundaries. Points past the
// last layer map onto it (the paper's recommended option: they then shadow themselves rather than vanishing).
// Buffers: b0 dom (F32, pixels·(1+K)) · b1 query (F32, 2/lane = [pixelIndex, depth]) · b2 out (F32, 2/lane = [T, opacity]).
[[nodiscard]] inline KEntry build_dom_lookup_kernel(KGraph& g, const DomConfig& cfg)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };
    const int   K   = cfg.layers;
    const double dz = cfg.span / static_cast<double>(K);

    const int dom_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int qry_b = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 2, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int qb   = g.binary(KOp::Mul, tid, cu(2));
    const int px   = g.cast(g.buffer_load(qry_b, qb), DType::U32);
    const int zq   = g.buffer_load(qry_b, g.binary(KOp::Add, qb, cu(1)));
    g.stmt_materialize(zq);
    const int  dbase = g.binary(KOp::Mul, px, cu(static_cast<crd::u32>(1 + K)));
    const auto dom   = [&](int k) {
        const int v = g.buffer_load(dom_b, g.binary(KOp::Add, dbase, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int z0 = dom(0);
    const int t  = sub(g, zq, z0);
    g.stmt_materialize(t);

    // piecewise-linear walk through the cumulative layers
    int op        = ks(0.0);
    int prev_o    = ks(0.0);
    double prev_b = 0.0;
    for (int k = 0; k < K; ++k)
    {
        const int    ok  = dom(1 + k);
        const double bnd = static_cast<double>(k + 1) * dz;
        const int    seg = g.binary(KOp::BitAnd, g.binary(KOp::CmpGe, t, ks(prev_b)), g.binary(KOp::CmpLt, t, ks(bnd)));
        const int    fr  = dv(g, sub(g, t, ks(prev_b)), ks(dz));
        op               = g.select(seg, add(g, prev_o, mul(g, sub(g, ok, prev_o), fr)), op);
        if (k == K - 1) { op = g.select(g.binary(KOp::CmpGe, t, ks(bnd)), ok, op); } // past the last layer ⇒ clamp onto it
        prev_o = ok;
        prev_b = bnd;
    }
    op = g.select(g.binary(KOp::CmpLe, t, ks(0.0)), ks(0.0), op); // in FRONT of the hair ⇒ unshadowed, exactly
    g.stmt_materialize(op);

    const int ob = g.binary(KOp::Mul, tid, cu(2));
    g.stmt_buffer_store(out_b, ob, g.unary(KOp::Exp, g.unary(KOp::Neg, op))); // Beer-Lambert transmittance
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(1)), op);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ══════════ TIER 2 (GOLD) — VOLUMETRIC MULTIPLE SCATTERING (Hu, Zhu, Lin, Zhang, Wang, Yan — TOG 45(4), July 2026) ══════════
// Matches path-traced ground truth at ~8.5x less cost on hair (3.8x vs Extended Dual Scattering on fur) and — critically —
// FIXES the case dual scattering gets visibly wrong: curly/coiled grooms, where rapidly changing fibre orientation violates
// dual scattering's local-similarity assumption and it over-darkens.
//
// The idea: at the scale where multiple scattering dominates, a dense groom behaves as an anisotropic PARTICIPATING MEDIUM.
// Keep explicit strands for the FIRST hit (high-frequency highlights), then hand all further transport to the medium
// (low-frequency glow). What makes it physically ours rather than a generic microflake volume:
//    · extinction is DIRECTIONAL: σ_t(ω) = σ_t^⊥·sinθ  with σ_t^⊥ = 2rN/(πd²)   (a ray along the fibres sees nothing)
//    · albedo comes from OUR BCSDF: Albedo(ωi) = ∫S dωo = ā_f + ā_b  ← exactly the LUT above
//    · the PHASE FUNCTION IS OUR BCSDF, normalised: P = S / ∫S. The paper is explicit that standard microflake/SGGX models
//      cannot be used, because their particles are opaque double-sided mirrors and hair TRANSMITS.
struct VolumeMsConfig
{
    int    octaves = 3;   // N — number of scattering octaves in the single-to-multiple series
    double a       = 0.5; // attenuation control: octave i is attenuated by exp(−aⁱ·τ) ⇒ higher orders penetrate DEEPER
    double c       = 0.5; // isotropy rate: P'_i = lerp(P, P_iso, 1−cⁱ) ⇒ higher orders progressively decorrelate in angle
    double gamma   = 1.0; // ∈[0,2], moderates colour saturation from repeated albedo application
    double fiber_r = 0.05; // fibre radius r, for σ_t^⊥ = 2rN/(πd²)
    double voxel_d = 1.0;  // voxelisation radius d
};

// The multiple-scattering estimator (Hu Eq 1/2/3). One thread per shading sample.
//   L = Σ_{i=0}^{N−1} σ_s·(γ·Albedo)ⁱ·L_light·P'_i·exp(−aⁱ·τ),   P'_i = lerp(P, P_iso, 1−cⁱ)
// The i=0 term is EXACTLY single scattering ((γA)⁰=1, a⁰=1, c⁰=1 ⇒ P'_0 = P) — the tests pin that down, because it is what
// makes the series an extension of the physically-sampled estimate rather than an unrelated fudge.
// ⚠ Albedo is applied as an RGB/spectral quantity in production: a SCALAR contribution factor causes severe colour loss and
// merely yields a brighter copy of single scattering (paper §3.3). This kernel is the scalar/per-channel core — run it per
// channel with that channel's albedo to preserve the characteristic multiple-scattering colour shift.
// Buffers: b0 in (F32, 5/lane = [σ_t^⊥, cos(ray,fibre), albedo, phase P, distance]) · b1 out (F32, 2/lane = [L, σ_t(ω)]).
[[nodiscard]] inline KEntry build_volume_ms_kernel(KGraph& g, const VolumeMsConfig& cfg)
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
    const int  ib   = g.binary(KOp::Mul, tid, cu(5));
    const auto ld   = [&](int k) {
        const int v = g.buffer_load(in_b, g.binary(KOp::Add, ib, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int stp = ld(0), cosrf = ld(1), alb = ld(2), phase = ld(3), dist = ld(4);

    // DIRECTIONAL extinction: σ_t(ω) = σ_t^⊥·sinθ, θ = angle(ray, local fibre direction). A ray travelling ALONG the fibres
    // sees essentially no extinction; one crossing them perpendicularly sees the maximum. This is the anisotropy that makes a
    // hair volume behave like hair rather than like fog.
    const int sin_t = safe_sqrt(g, g.binary(KOp::Max, sub(g, ks(1.0), sq(g, cosrf)), ks(0.0)));
    const int sig_t = mul(g, stp, sin_t);
    const int sig_s = mul(g, alb, sig_t);   // σ_s = Albedo·σ_t  (σ_a = σ_t − σ_s)
    const int tau   = mul(g, sig_t, dist);
    g.stmt_materialize(sig_t);
    g.stmt_materialize(sig_s);
    g.stmt_materialize(tau);

    const double kIso = 1.0 / (4.0 * kPi); // isotropic phase
    int          acc  = ks(0.0);
    for (int i = 0; i < cfg.octaves; ++i) // N is a host constant ⇒ unrolled; powers fold to literals
    {
        const double gai = crd::math::pow(cfg.gamma, static_cast<double>(i)); // γⁱ
        const double ai  = crd::math::pow(cfg.a, static_cast<double>(i));     // aⁱ
        const double ci  = crd::math::pow(cfg.c, static_cast<double>(i));     // cⁱ
        // (γ·Albedo)ⁱ — the albedo part must stay a runtime power since Albedo varies per sample
        const int albi = (i == 0) ? ks(1.0) : mul(g, ks(gai), g.unary(KOp::Exp, mul(g, ks(static_cast<double>(i)),
                                                                                   g.unary(KOp::Log, g.binary(KOp::Max, alb, ks(1.0e-6))))));
        // P'_i = lerp(P, P_iso, 1−cⁱ): octave 0 is exactly P; higher octaves decorrelate toward isotropic
        const int pi   = add(g, mul(g, ks(ci), phase), mul(g, ks(1.0 - ci), ks(kIso)));
        const int atten = g.unary(KOp::Exp, g.unary(KOp::Neg, mul(g, ks(ai), tau)));
        acc = add(g, acc, mul(g, mul(g, sig_s, albi), mul(g, pi, atten)));
    }

    const int ob = g.binary(KOp::Mul, tid, cu(2));
    g.stmt_buffer_store(out_b, ob, acc);
    g.stmt_buffer_store(out_b, g.binary(KOp::Add, ob, cu(1)), sig_t);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Voxelise strand segments into the equivalent medium (Hu §3.2 precomputation): per voxel, the average fibre direction ω̄ and
// the fibre count N that sets σ_t^⊥ = 2rN/(πd²).
// ⚠ Fibre direction is a LINE, not a vector: summing d and −d cancels. We flip each segment into a canonical hemisphere before
// averaging, which is exact under the paper's own stated assumption that fibres are LOCALLY COHERENT within a voxel. (A groom
// with genuinely bidirectional fibres in one voxel would need a structure-tensor/principal-axis average instead.)
// ⚠ One thread per VOXEL looping over segments — deterministic and oracle-checkable. Production would scatter segments with
// atomics; that trades reproducibility for speed and is a separate optimisation, not a different result.
// Buffers: b0 segs (F32, nseg·6 = [p0.xyz, p1.xyz]) · b1 out (F32, voxels·4 = [ω̄.xyz, count]).
[[nodiscard]] inline KEntry build_hair_voxelize_kernel(KGraph& g, int nseg, int gx, int gy, int gz, double cell)
{
    using namespace detail;
    const Shape shu = make_shape({1});
    const auto  cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto  ks  = [&](double v) { return g.constant(v, shu, DType::F32); };

    const int seg_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int tid   = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    // this thread's voxel coordinate
    const int vz = g.binary(KOp::Div, tid, cu(static_cast<crd::u32>(gx * gy)));
    const int r0 = g.binary(KOp::Mod, tid, cu(static_cast<crd::u32>(gx * gy)));
    const int vy = g.binary(KOp::Div, r0, cu(static_cast<crd::u32>(gx)));
    const int vx = g.binary(KOp::Mod, r0, cu(static_cast<crd::u32>(gx)));
    const int fx = g.cast(vx, DType::F32), fy = g.cast(vy, DType::F32), fz = g.cast(vz, DType::F32);
    g.stmt_materialize(fx); g.stmt_materialize(fy); g.stmt_materialize(fz);

    const int  ob   = g.binary(KOp::Mul, tid, cu(4));
    const auto oidx = [&](int k) { return g.binary(KOp::Add, ob, cu(static_cast<crd::u32>(k))); };
    for (int k = 0; k < 4; ++k) { g.stmt_buffer_store(out_b, oidx(k), ks(0.0)); }

    const int l  = g.stmt_for_begin(cu(static_cast<crd::u32>(nseg)));
    const int sv = g.kernel_loop_var(l);
    const int sb = g.binary(KOp::Mul, sv, cu(6));
    const auto sl = [&](int k) {
        const int v = g.buffer_load(seg_b, g.binary(KOp::Add, sb, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int ax = sl(0), ay = sl(1), az = sl(2), bx = sl(3), by = sl(4), bz = sl(5);
    // segment midpoint → voxel coordinate
    const int mx = mul(g, ks(0.5), add(g, ax, bx));
    const int my = mul(g, ks(0.5), add(g, ay, by));
    const int mz = mul(g, ks(0.5), add(g, az, bz));
    const int cx = g.unary(KOp::Floor, dv(g, mx, ks(cell)));
    const int cy = g.unary(KOp::Floor, dv(g, my, ks(cell)));
    const int cz = g.unary(KOp::Floor, dv(g, mz, ks(cell)));
    // in-grid mask: the dispatch rounds up to a multiple of the workgroup size, so lanes past gx·gy·gz are PADDING and must
    // contribute nothing (they would otherwise decompose to bogus voxel coordinates and accumulate garbage).
    const int in_grid = g.binary(KOp::CmpLt, tid, cu(static_cast<crd::u32>(gx * gy * gz)));
    const int hit = g.binary(KOp::BitAnd, in_grid,
                             g.binary(KOp::BitAnd, g.binary(KOp::BitAnd, g.binary(KOp::CmpEq, cx, fx), g.binary(KOp::CmpEq, cy, fy)),
                                      g.binary(KOp::CmpEq, cz, fz)));
    // direction, flipped into a canonical hemisphere (dx >= 0) so opposite parameterisations do not cancel
    const int dx0 = sub(g, bx, ax), dy0 = sub(g, by, ay), dz0 = sub(g, bz, az);
    const int len = g.binary(KOp::Max, safe_sqrt(g, add(g, add(g, sq(g, dx0), sq(g, dy0)), sq(g, dz0))), ks(1.0e-9));
    const int flip = g.select(g.binary(KOp::CmpGe, dx0, ks(0.0)), ks(1.0), ks(-1.0));
    const int w    = g.select(hit, ks(1.0), ks(0.0));
    const int sc   = mul(g, w, dv(g, flip, len));
    g.stmt_buffer_store(out_b, oidx(0), add(g, g.buffer_load(out_b, oidx(0)), mul(g, dx0, sc)));
    g.stmt_buffer_store(out_b, oidx(1), add(g, g.buffer_load(out_b, oidx(1)), mul(g, dy0, sc)));
    g.stmt_buffer_store(out_b, oidx(2), add(g, g.buffer_load(out_b, oidx(2)), mul(g, dz0, sc)));
    g.stmt_buffer_store(out_b, oidx(3), add(g, g.buffer_load(out_b, oidx(3)), w));
    g.stmt_for_end(l);

    // normalise the accumulated direction (⛔ freeze the raw sums first — inline loads would re-read after the first store)
    const int rx = g.buffer_load(out_b, oidx(0));
    const int ry = g.buffer_load(out_b, oidx(1));
    const int rz = g.buffer_load(out_b, oidx(2));
    g.stmt_materialize(rx); g.stmt_materialize(ry); g.stmt_materialize(rz);
    const int rl = g.binary(KOp::Max, safe_sqrt(g, add(g, add(g, sq(g, rx), sq(g, ry)), sq(g, rz))), ks(1.0e-9));
    g.stmt_buffer_store(out_b, oidx(0), dv(g, rx, rl));
    g.stmt_buffer_store(out_b, oidx(1), dv(g, ry, rl));
    g.stmt_buffer_store(out_b, oidx(2), dv(g, rz, rl));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::hairms
