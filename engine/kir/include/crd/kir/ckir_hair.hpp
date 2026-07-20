#pragma once

// ckir_hair.hpp — D-007 B18: the CKIR HAIR / FUR BCSDF (bidirectional CURVE scattering distribution function).
//
// A faithful transcription of the Chiang et al. 2016 production hair model ("A Practical and Controllable Hair and Fur Model
// for Production Path Tracing", the pbrt-v3 `hair.cpp` implementation), which is the energy-conserving near-field refinement
// of Marschner 2003 (d'Eon 2011). Unlike every surface BSDF in ckir_lighting.hpp, a hair fiber is a translucent dielectric
// CYLINDER — light takes paths through it (R = reflect off the cuticle; TT = transmit through; TRT = internal back-reflection;
// TRRT+ = higher internal bounces, the residual lobe that CONSERVES energy). Each path is separable into a LONGITUDINAL term
// Mp(θ) (a Bessel-I0 Gaussian along the fiber, tilted by the cuticle-scale angle α) and an AZIMUTHAL term Np(φ) (a trimmed
// logistic around the fiber), weighted by an attenuation Ap (Fresnel + Beer-Lambert absorption). Colour is ABSORPTION inside
// the fiber (σₐ from eumelanin/pheomelanin concentration, or inverted from an artist RGB).
//
// FRAME: `wo`/`wi` are unit vec3 in the fiber-local frame with x = FIBER TANGENT; sinθ = w.x, φ = atan2(w.z, w.y). The math is
// TRANSCENDENTAL-heavy (exp/log/asin/sinh/logistic) ⇒ this is a TO-ULP tier (like MBOIT/atmosphere): GPU matches the CPU
// oracle to ULP, and PHYSICAL correctness is certified independently by the white-furnace energy test (∫ f·|cosθᵢ| ≤ 1).
// All builders are pure CKIR value-graph builders on the raster (vector) path — no new device features.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_nodes.hpp>

namespace crd::kir::hair
{

inline constexpr double kPi  = 3.14159265358979323846;
inline constexpr int    kPMax = 3; // lobes R(0), TT(1), TRT(2), and the TRRT+ residual at index pMax(3)

namespace detail
{
using nodes::detail::bin; // broadcasting binary (vec/scalar mixed)

[[nodiscard]] inline int kf(KGraph& g, int like, double v) { return g.constant(v, g.node(like).shape, g.node(like).dtype()); }
[[nodiscard]] inline int mul(KGraph& g, int a, int b) { return g.binary(KOp::Mul, a, b); }
[[nodiscard]] inline int add(KGraph& g, int a, int b) { return g.binary(KOp::Add, a, b); }
[[nodiscard]] inline int sub(KGraph& g, int a, int b) { return g.binary(KOp::Sub, a, b); }
[[nodiscard]] inline int dv(KGraph& g, int a, int b) { return g.binary(KOp::Div, a, b); }
[[nodiscard]] inline int sq(KGraph& g, int a) { return g.binary(KOp::Mul, a, a); }

// SafeSqrt(x) = sqrt(max(x, 0)); SafeAsin(x) = asin(clamp(x, -1, 1)) — guard the domain (grazing angles push just past ±1).
[[nodiscard]] inline int safe_sqrt(KGraph& g, int x) { return g.unary(KOp::Sqrt, g.binary(KOp::Max, x, kf(g, x, 0.0))); }
[[nodiscard]] inline int safe_asin(KGraph& g, int x)
{
    return g.unary(KOp::Asin, g.binary(KOp::Min, g.binary(KOp::Max, x, kf(g, x, -1.0)), kf(g, x, 1.0)));
}

// Integer power by repeated multiplication (EXACT — no `pow` transcendental) for the βₘ^20 / βₙ^22 roughness mappings.
[[nodiscard]] inline int ipow(KGraph& g, int x, int n)
{
    int result = kf(g, x, 1.0);
    int base   = x;
    while (n > 0)
    {
        if ((n & 1) != 0) { result = mul(g, result, base); }
        n >>= 1;
        if (n > 0) { base = mul(g, base, base); }
    }
    return result;
}

// I0(x): modified Bessel of the first kind, order 0 — I0 = Σ x^(2i) / (4^i (i!)²), 10 terms (pbrt), as a Horner poly in x².
[[nodiscard]] inline int bessel_i0(KGraph& g, int x)
{
    static const double c[10] = {1.0,
                                 1.0 / 4.0,
                                 1.0 / 64.0,
                                 1.0 / 2304.0,
                                 1.0 / 147456.0,
                                 1.0 / 14745600.0,
                                 1.0 / 2123366400.0,
                                 1.0 / 416179814400.0,
                                 1.0 / 106542032486400.0,
                                 1.0 / 34519618525593600.0};
    const int x2  = sq(g, x);
    int       acc = kf(g, x, c[9]);
    for (int i = 8; i >= 0; --i) { acc = add(g, mul(g, acc, x2), kf(g, x, c[i])); } // Horner in x²
    return acc;
}
// LogI0(x): log of I0 — a large-x asymptotic branch (x>12) avoids overflow; else log(I0). Branchless select.
[[nodiscard]] inline int log_bessel_i0(KGraph& g, int x)
{
    const auto k    = [&](double v) { return kf(g, x, v); };
    const int  big  = add(g, x, mul(g, k(0.5), add(g, add(g, k(-crd::math::log(2.0 * kPi)), g.unary(KOp::Log, dv(g, k(1.0), x))), dv(g, k(1.0), mul(g, k(8.0), x)))));
    const int  smol = g.unary(KOp::Log, bessel_i0(g, x));
    return g.select(g.binary(KOp::CmpGt, x, k(12.0)), big, smol);
}

// Mp: the LONGITUDINAL scattering — a Bessel-I0-normalised Gaussian in θ with variance v. Small-v uses a log-domain form
// (I0 overflows), large-v the direct exp(-b)·I0(a)/(2v·sinh(1/v)). Branchless select on v ≤ 0.1.
[[nodiscard]] inline int hair_mp(KGraph& g, int cos_ti, int cos_to, int sin_ti, int sin_to, int v)
{
    const auto k    = [&](double val) { return kf(g, v, val); };
    const int  a    = dv(g, mul(g, cos_ti, cos_to), v);
    const int  b    = dv(g, mul(g, sin_ti, sin_to), v);
    const int  invv = dv(g, k(1.0), v);
    // small v: exp( LogI0(a) − b − 1/v + ln2 + log(1/(2v)) )
    const int  smol = g.unary(KOp::Exp, add(g, sub(g, sub(g, log_bessel_i0(g, a), b), invv),
                                            add(g, k(0.6931471805599453), g.unary(KOp::Log, dv(g, k(1.0), mul(g, k(2.0), v))))));
    // large v: exp(−b)·I0(a) / (sinh(1/v)·2v)
    const int  big  = dv(g, mul(g, g.unary(KOp::Exp, g.unary(KOp::Neg, b)), bessel_i0(g, a)),
                         mul(g, g.unary(KOp::Sinh, invv), mul(g, k(2.0), v)));
    return g.select(g.binary(KOp::CmpLe, v, k(0.1)), smol, big);
}

// FrDielectric(cosθᵢ, η): unpolarised dielectric Fresnel. Hair keeps cosθᵢ ≥ 0 and η > 1 (no back-face flip); TIR guarded.
[[nodiscard]] inline int fr_dielectric(KGraph& g, int cos_ti, int eta)
{
    const auto k       = [&](double v) { return kf(g, cos_ti, v); };
    const int  ci      = g.binary(KOp::Min, g.binary(KOp::Max, cos_ti, k(0.0)), k(1.0));
    const int  sin2i   = sub(g, k(1.0), sq(g, ci));
    const int  sin2t   = dv(g, sin2i, sq(g, eta));
    const int  ct      = safe_sqrt(g, sub(g, k(1.0), sin2t));
    const int  eci     = mul(g, eta, ci);
    const int  ect     = mul(g, eta, ct);
    const int  rpar    = dv(g, sub(g, eci, ct), add(g, eci, ct));   // (η·cosθᵢ − cosθₜ)/(η·cosθᵢ + cosθₜ)
    const int  rper    = dv(g, sub(g, ci, ect), add(g, ci, ect));   // (cosθᵢ − η·cosθₜ)/(cosθᵢ + η·cosθₜ)
    const int  res     = mul(g, k(0.5), add(g, sq(g, rpar), sq(g, rper)));
    return g.select(g.binary(KOp::CmpGe, sin2t, k(1.0)), k(1.0), res); // total internal reflection ⇒ 1
}

// TrimmedLogistic(x, s) on [−π, π] — the azimuthal roughness distribution (d'Eon's logistic detector, normalised over ±π).
[[nodiscard]] inline int trimmed_logistic(KGraph& g, int x, int s)
{
    const auto k   = [&](double v) { return kf(g, x, v); };
    const auto cdf = [&](int xx) { return dv(g, k(1.0), add(g, k(1.0), g.unary(KOp::Exp, g.unary(KOp::Neg, dv(g, xx, s))))); };
    const int  ax  = g.unary(KOp::Abs, x);
    const int  e   = g.unary(KOp::Exp, g.unary(KOp::Neg, dv(g, ax, s)));
    const int  lg  = dv(g, e, mul(g, s, sq(g, add(g, k(1.0), e))));      // Logistic(x, s) = e^(−|x|/s)/(s(1+e^(−|x|/s))²)
    const int  nrm = sub(g, cdf(k(kPi)), cdf(k(-kPi)));                  // CDF(π) − CDF(−π)
    return dv(g, lg, nrm);
}

// Np: the AZIMUTHAL scattering for lobe p — a trimmed logistic of the offset Δφ = φ − Φ(p) wrapped into [−π, π].
// Φ(p, γo, γt) = 2p·γt − 2γo + p·π. `p` is a compile-time lobe index.
[[nodiscard]] inline int hair_np(KGraph& g, int phi, int p, int s, int gamma_o, int gamma_t)
{
    const auto k     = [&](double v) { return kf(g, phi, v); };
    const int  bigPhi = add(g, sub(g, mul(g, k(2.0 * static_cast<double>(p)), gamma_t), mul(g, k(2.0), gamma_o)),
                            k(static_cast<double>(p) * kPi));
    int        dphi  = sub(g, phi, bigPhi);
    // wrap Δφ into [−π, π]: Δφ − 2π·round(Δφ / 2π)
    dphi = sub(g, dphi, mul(g, k(2.0 * kPi), g.unary(KOp::Round, dv(g, dphi, k(2.0 * kPi)))));
    return trimmed_logistic(g, dphi, s);
}

// B18-b medulla azimuthal phase: a WRAPPED-CAUCHY distribution — the exactly-normalizable circular analog of Henyey-Greenstein.
// p(φ) = (1/2π)·(1−ρ²)/(1+ρ²−2ρ·cosφ), which integrates to EXACTLY 1 over φ∈[−π,π] (no elliptic integrals, unlike a wrapped HG)
// ⇒ the medulla scattered lobe is energy-normalized in closed form. ρ ∈ [0,1) is the forward-scattering anisotropy (0 = isotropic).
[[nodiscard]] inline int medulla_azimuthal(KGraph& g, int phi, int rho)
{
    const auto k   = [&](double v) { return kf(g, phi, v); };
    const int  r2  = sq(g, rho);
    const int  num = sub(g, k(1.0), r2);                                                       // 1 − ρ²
    const int  den = sub(g, add(g, k(1.0), r2), mul(g, mul(g, k(2.0), rho), g.unary(KOp::Cos, phi))); // 1 + ρ² − 2ρ·cosφ
    return dv(g, mul(g, k(1.0 / (2.0 * kPi)), num), den);
}

} // namespace detail

// ── σₐ from pigment concentration (eumelanin `ce`, pheomelanin `cp`) — the PHYSICAL colour control (Chiang §4). vec3. ──────
[[nodiscard]] inline int sigma_a_from_melanin(KGraph& g, int ce, int cp)
{
    using namespace detail;
    const int eu = g.vec3(kf(g, ce, 0.419), kf(g, ce, 0.697), kf(g, ce, 1.37)); // eumelanin σₐ
    const int ph = g.vec3(kf(g, cp, 0.187), kf(g, cp, 0.4), kf(g, cp, 1.05));   // pheomelanin σₐ
    return bin(g, KOp::Add, bin(g, KOp::Mul, eu, ce), bin(g, KOp::Mul, ph, cp));
}
// ── σₐ inverted from an artist RGB reflectance `color` at azimuthal roughness βₙ (Chiang §4.2). vec3. ────────────────────
[[nodiscard]] inline int sigma_a_from_color(KGraph& g, int color, int beta_n)
{
    using namespace detail;
    const auto k = [&](double v) { return kf(g, beta_n, v); };
    // denom = 5.969 − 0.215βₙ + 2.532βₙ² − 10.73βₙ³ + 5.574βₙ⁴ + 0.245βₙ⁵
    int d = k(5.969);
    d     = add(g, d, mul(g, k(-0.215), beta_n));
    d     = add(g, d, mul(g, k(2.532), ipow(g, beta_n, 2)));
    d     = add(g, d, mul(g, k(-10.73), ipow(g, beta_n, 3)));
    d     = add(g, d, mul(g, k(5.574), ipow(g, beta_n, 4)));
    d     = add(g, d, mul(g, k(0.245), ipow(g, beta_n, 5)));
    const int t = bin(g, KOp::Div, g.unary(KOp::Log, color), d); // log(color)/denom, componentwise
    return bin(g, KOp::Mul, t, t);                               // squared
}

// ── The full hair BCSDF f(wo, wi). Returns vec3 radiance-scale. `sigma_a` vec3; the rest scalar. ────────────────────────
// βₘ = longitudinal roughness, βₙ = azimuthal roughness, alpha_deg = cuticle-scale tilt (degrees), h ∈ [−1,1] = fibre offset.
// The BCSDF core in SCALAR ANGLE coordinates (sinθ, cosθ, φ for out/in). No vec3/swizzle nodes ⇒ when σₐ is scalar the whole
// cone is fusable, so this lowers through the SCALAR elementwise emitter (the both-backend GPU==oracle gate). vec3 σₐ still
// yields a vec3 result (the coloured fibre) via `bin` broadcast — for the raster fragment path.
// The trailing `fur_*` are HOST-level (compile-time) fur/medulla controls (Yan 2017 double-cylinder, analytic form): fur_kappa =
// medullary index (relative medulla radius; 0 ⇒ pure hair, no medulla nodes emitted, graph byte-identical to B18-a), fur_sigma =
// medulla extinction σ_ms+σ_ma, fur_albedo = medulla single-scatter albedo α_m, fur_g = scattering anisotropy (wrapped-Cauchy ρ),
// fur_beta_s = scattered-lobe longitudinal variance (broad). The medulla splits the through-medulla TT/TRT energy into
// (1−S) unscattered · S·α_m scattered · S·(1−α_m) absorbed — energy-conserving by construction.
[[nodiscard]] inline int hair_bcsdf_eval_angles(KGraph& g, int sin_to, int cos_to, int phi_o, int sin_ti, int cos_ti,
                                                int phi_i, int h, int eta, int sigma_a, int beta_m, int beta_n, int alpha_deg,
                                                double fur_kappa = 0.0, double fur_sigma = 0.0, double fur_albedo = 0.0,
                                                double fur_g = 0.0, double fur_beta_s = 0.8)
{
    using namespace detail;
    const auto ks = [&](double v) { return kf(g, h, v); };  // scalar constant (matches h's shape)

    // --- refraction: transmission angle θt and the internal offset γt ---
    const int sin_tt = dv(g, sin_to, eta);
    const int cos_tt = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_tt)));
    const int etap   = dv(g, safe_sqrt(g, sub(g, sq(g, eta), sq(g, sin_to))), cos_to); // √(η²−sin²θo)/cosθo
    const int sin_gt = dv(g, h, etap);
    const int cos_gt = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_gt)));
    const int gamma_t = safe_asin(g, sin_gt);
    const int gamma_o = safe_asin(g, h);

    // --- Beer-Lambert transmittance along the internal path (vec3) ---
    const int dist = dv(g, mul(g, ks(2.0), cos_gt), cos_tt); // 2·cosγt / cosθt
    const int T    = g.unary(KOp::Exp, g.unary(KOp::Neg, bin(g, KOp::Mul, sigma_a, dist)));

    // --- attenuations Ap[0..pMax] (vec3): R, TT, TRT, TRRT-residual (Fresnel + transmittance) ---
    const int cos_go = safe_sqrt(g, sub(g, ks(1.0), sq(g, h)));
    const int f      = fr_dielectric(g, mul(g, cos_to, cos_go), eta); // Fresnel at the first interface (scalar)
    const int omf    = sub(g, ks(1.0), f);
    const int omf2   = sq(g, omf);
    const int fT     = bin(g, KOp::Mul, T, f);            // f·T (vec3)
    // SHAPE-POLYMORPHIC: no splats — the output follows σₐ's shape. Scalar σₐ ⇒ scalar f (dispatchable through the scalar
    // elementwise emitter for the both-backend GPU gate); vec3 σₐ ⇒ vec3 f (the coloured fibre). `bin` broadcasts the scalar
    // Fresnel `f`/`ks(1)` against the (scalar-or-vec3) transmittance `T` either way.
    int       ap[kPMax + 1];
    ap[0] = f;                                                             // R (scalar Fresnel)
    ap[1] = bin(g, KOp::Mul, T, omf2);                                     // TT = (1−f)²·T
    ap[2] = bin(g, KOp::Mul, bin(g, KOp::Mul, T, T), mul(g, omf2, f));     // TRT = (1−f)²·f·T²
    ap[3] = bin(g, KOp::Div, bin(g, KOp::Mul, ap[2], fT),                  // residual = Ap₂·f·T/(1 − f·T)
                 bin(g, KOp::Sub, ks(1.0), fT));

    // ── B18-b FUR MEDULLA energy split (Yan 2017 double-cylinder, analytic). HOST-guarded: kappa==0 emits NOTHING. ────────────
    int a_scatter = -1; // the scattered-lobe attenuation node (−1 = no medulla this build)
    if (fur_kappa > 0.0)
    {
        const int kap    = ks(fur_kappa);
        const int hk     = dv(g, h, kap);                                            // ray offset inside the medulla (norm to κ)
        const int cos_gm = safe_sqrt(g, sub(g, ks(1.0), sq(g, hk)));                 // medulla internal cosine
        const int d_med  = dv(g, mul(g, mul(g, ks(2.0), kap), cos_gm), cos_tt);      // medulla chord along the transmission ray
        const int inter  = sub(g, ks(1.0), g.unary(KOp::Exp, g.unary(KOp::Neg, mul(g, ks(fur_sigma), d_med)))); // S=1−exp(−σ_m·d)
        const int hit    = g.select(g.binary(KOp::CmpLt, g.unary(KOp::Abs, h), kap), inter, ks(0.0)); // 0 when the ray misses medulla
        const int s_sca  = mul(g, hit, ks(fur_albedo));                              // S·α_m — the scattered fraction
        const int keep   = sub(g, ks(1.0), hit);                                     // (1−S) — the unscattered survivors
        a_scatter        = bin(g, KOp::Mul, bin(g, KOp::Add, ap[1], ap[2]), s_sca);  // scattered energy drawn from TT+TRT
        ap[1]            = bin(g, KOp::Mul, ap[1], keep);                            // modulate unscattered TT
        ap[2]            = bin(g, KOp::Mul, ap[2], keep);                            // modulate unscattered TRT
    }

    // --- cuticle-scale tilt: sin/cos of α, 2α, 4α via the double-angle recurrence ---
    int s2a[3];
    int c2a[3];
    s2a[0] = g.unary(KOp::Sin, g.unary(KOp::Radians, alpha_deg));
    c2a[0] = safe_sqrt(g, sub(g, ks(1.0), sq(g, s2a[0])));
    for (int i = 1; i < 3; ++i)
    {
        s2a[i] = mul(g, mul(g, ks(2.0), c2a[i - 1]), s2a[i - 1]);
        c2a[i] = sub(g, sq(g, c2a[i - 1]), sq(g, s2a[i - 1]));
    }

    // --- longitudinal variances v[p] from βₘ, and the azimuthal logistic scale s from βₙ ---
    const int v0 = sq(g, add(g, add(g, ks(0.726), mul(g, ks(0.812), beta_m)), mul(g, ks(3.7), ipow(g, beta_m, 20))));
    int       vp[kPMax + 1];
    vp[0] = v0;
    vp[1] = mul(g, ks(0.25), v0);
    vp[2] = mul(g, ks(4.0), v0);
    vp[3] = vp[2];
    const int s_scale = mul(g, ks(0.626657069), add(g, add(g, mul(g, ks(0.265), beta_n), mul(g, ks(1.194), ipow(g, beta_n, 2))),
                                                    mul(g, ks(5.372), ipow(g, beta_n, 22))));

    // --- sum the p = 0..pMax−1 lobes (tilt-adjusted longitudinal angle per lobe) + the residual lobe ---
    const int phi = sub(g, phi_i, phi_o);
    int       fsum = ks(0.0); // scalar 0 — promotes to vec3 at the first coloured (vec3-T) lobe via `bin` broadcast
    for (int p = 0; p < kPMax; ++p)
    {
        int sin_top;
        int cos_top;
        if (p == 0) { sin_top = sub(g, mul(g, sin_to, c2a[1]), mul(g, cos_to, s2a[1])); cos_top = add(g, mul(g, cos_to, c2a[1]), mul(g, sin_to, s2a[1])); }
        else if (p == 1) { sin_top = add(g, mul(g, sin_to, c2a[0]), mul(g, cos_to, s2a[0])); cos_top = sub(g, mul(g, cos_to, c2a[0]), mul(g, sin_to, s2a[0])); }
        else { sin_top = add(g, mul(g, sin_to, c2a[2]), mul(g, cos_to, s2a[2])); cos_top = sub(g, mul(g, cos_to, c2a[2]), mul(g, sin_to, s2a[2])); }
        cos_top       = g.unary(KOp::Abs, cos_top);
        const int mp  = hair_mp(g, cos_ti, cos_top, sin_ti, sin_top, vp[p]);
        const int np  = hair_np(g, phi, p, s_scale, gamma_o, gamma_t);
        fsum          = bin(g, KOp::Add, fsum, bin(g, KOp::Mul, ap[p], mul(g, mp, np)));
    }
    const int mp_res = hair_mp(g, cos_ti, cos_to, sin_ti, sin_to, vp[kPMax]);
    fsum             = bin(g, KOp::Add, fsum, bin(g, KOp::Mul, ap[kPMax], mul(g, mp_res, ks(1.0 / (2.0 * kPi)))));

    // B18-b: the MEDULLA SCATTERED lobe — the S·α_m energy drawn from TT/TRT, redistributed by a broad normalized longitudinal
    // Gaussian (scattered variance v_s) × the wrapped-Cauchy azimuthal phase. Both integrate to 1, so a_scatter carries the energy
    // exactly (the milky, translucent medullary look of animal fur). Only present when a medulla was built (kappa > 0).
    if (a_scatter >= 0)
    {
        const int m_s = hair_mp(g, cos_ti, cos_to, sin_ti, sin_to, ks(fur_beta_s));
        const int d_s = medulla_azimuthal(g, phi, ks(fur_g));
        fsum          = bin(g, KOp::Add, fsum, bin(g, KOp::Mul, a_scatter, mul(g, m_s, d_s)));
    }

    // divide by |cosθᵢ| (pbrt's measure) — guarded so a grazing wi never divides by ~0.
    const int abscos = g.unary(KOp::Abs, cos_ti);
    return g.select(g.binary(KOp::CmpGt, abscos, ks(1.0e-5)), bin(g, KOp::Div, fsum, abscos), fsum);
}

// ══════════ B18-b: the HUANG 2022 MICROFACET HAIR BCSDF (EGSR 2022, CGF 41(4), DOI 10.1111/cgf.14588) ══════════
// Research note: docs/research/2026-07-19-huang-microfacet-hair.md. Unlike Marschner/Chiang (separable Mp(θ)·Np(φ)), Huang treats
// the fibre as a ROUGH DIELECTRIC CYLINDER whose surface is a Smith microfacet surface, and applies Cook-Torrance ON THE CURVED
// SURFACE. The model is NON-SEPARABLE: it forms the half-vector ωh between ωi/ωo and integrates ITS distribution along the azimuth.
// For the R lobe that azimuthal integral has a CLOSED FORM under GGX (Appendix A) — the paper's headline, and the piece that stays
// bit-exact + portable in CKIR (the TT/TRT lobes need a Simpson quadrature; that is a separate builder).
//
// FRAME (Huang §3, Fig. 2): y = FIBRE TANGENT. θ = angle between ω and the x–z plane (⇒ sinθ = ω.y); φ = angle from the z axis to
// the projection of ω onto x–z. Hence ω = [sinφ·cosθ, sinθ, cosφ·cosθ]. Our Chiang-core (sinθ, cosθ, φ) inputs feed straight in.
// Because an untilted fibre (α=0) is rotationally symmetric about its axis, we ROTATE so φo = 0 and use Δφ = wrap(φi − φo); this
// removes every angle-wrapping hazard from the integration bounds.
namespace detail
{
// GGX NDF (Huang Eq 41): D(ωh, ωmα) = β² / ( π·(1 + (β²−1)(ωh·ωmα)²)² ).
[[nodiscard]] inline int huang_ggx_d(KGraph& g, int cos_hm, double beta)
{
    const auto k   = [&](double v) { return kf(g, cos_hm, v); };
    const int  den = add(g, k(1.0), mul(g, k(beta * beta - 1.0), sq(g, cos_hm)));
    return dv(g, k(beta * beta), mul(g, k(kPi), sq(g, den)));
}

// The ANALYTIC azimuthal antiderivative of the GGX NDF for the R lobe at α = 0 (Huang Eq 44), as a function of u = φh − φm:
//   T(u) = (A²−2)/(1−A²)^{3/2} · atan( tan u / √(1−A²) )  +  A²·sin 2u / ( (1−A²)·(A²·cos 2u + A² − 2) ),   A = cosθh·√(1−β²).
// ⚠ atan(tan u / s) is emitted as atan2(sin u, s·cos u): identical on u ∈ (−π/2, π/2) (where cos u > 0 — and the visible arc keeps
// u strictly inside that range) but numerically safe, since tan u OVERFLOWS f32 near ±π/2 while atan2 stays finite.
// The second denominator is bounded away from zero: (1−A²)(A²(1+cos2u) − 2) ≤ −2(1−A²)² < 0 for A² < 1.
[[nodiscard]] inline int huang_r_antideriv(KGraph& g, int u, int a2, int one_m_a2, int s)
{
    const auto k    = [&](double v) { return kf(g, u, v); };
    const int  su   = g.unary(KOp::Sin, u);
    const int  cu   = g.unary(KOp::Cos, u);
    const int  am2  = sub(g, a2, k(2.0));                                              // A² − 2
    const int  t1   = mul(g, dv(g, am2, mul(g, one_m_a2, s)),                          // (A²−2)/(1−A²)^{3/2}
                          g.binary(KOp::Atan2, su, mul(g, s, cu)));                    // atan(tan u/s), stable form
    const int  s2u  = mul(g, k(2.0), mul(g, su, cu));                                  // sin 2u
    const int  c2u  = sub(g, k(1.0), mul(g, k(2.0), sq(g, su)));                       // cos 2u
    const int  den2 = mul(g, one_m_a2, add(g, mul(g, a2, c2u), am2));                  // (1−A²)(A²cos2u + A²−2)
    return add(g, t1, dv(g, mul(g, a2, s2u), den2));
}
// ── scalar vec3 helpers (the compute emitter is SCALAR-only, so vectors are carried as three node ids) ────────────────────────
struct V3
{
    int x = -1, y = -1, z = -1;
};
[[nodiscard]] inline int v3dot(KGraph& g, const V3& a, const V3& b)
{
    return add(g, add(g, mul(g, a.x, b.x), mul(g, a.y, b.y)), mul(g, a.z, b.z));
}
[[nodiscard]] inline V3 v3cross(KGraph& g, const V3& a, const V3& b)
{
    return {sub(g, mul(g, a.y, b.z), mul(g, a.z, b.y)), sub(g, mul(g, a.z, b.x), mul(g, a.x, b.z)),
            sub(g, mul(g, a.x, b.y), mul(g, a.y, b.x))};
}
[[nodiscard]] inline V3 v3scale(KGraph& g, const V3& a, int s) { return {mul(g, a.x, s), mul(g, a.y, s), mul(g, a.z, s)}; }
[[nodiscard]] inline V3 v3norm(KGraph& g, const V3& a)
{
    const int l = g.binary(KOp::Max, safe_sqrt(g, v3dot(g, a, a)), kf(g, a.x, 1.0e-9));
    return {dv(g, a.x, l), dv(g, a.y, l), dv(g, a.z, l)};
}

// Duff et al. 2017 BRANCHLESS orthonormal basis around a unit normal n (the same construction the RT sampler uses).
inline void huang_basis(KGraph& g, const V3& n, V3& t, V3& b)
{
    const auto k   = [&](double v) { return kf(g, n.z, v); };
    const int  sgn = g.select(g.binary(KOp::CmpGe, n.z, k(0.0)), k(1.0), k(-1.0));
    const int  a   = dv(g, k(-1.0), add(g, sgn, n.z)); // |sgn + n.z| >= 1 ⇒ never singular
    const int  bb  = mul(g, mul(g, n.x, n.y), a);
    t              = {add(g, k(1.0), mul(g, sgn, mul(g, sq(g, n.x), a))), mul(g, sgn, bb), mul(g, g.unary(KOp::Neg, sgn), n.x)};
    b              = {bb, add(g, sgn, mul(g, sq(g, n.y), a)), g.unary(KOp::Neg, n.y)};
}

// Smith one-sided masking-shadowing for GGX: G1(ω) = 2|ω·n| / ( |ω·n| + √(β² + (1−β²)(ω·n)²) ).
[[nodiscard]] inline int huang_smith_g1(KGraph& g, int cos_wn, double beta)
{
    const auto k = [&](double v) { return kf(g, cos_wn, v); };
    const int  c = g.unary(KOp::Abs, cos_wn);
    const int  r = safe_sqrt(g, add(g, k(beta * beta), mul(g, k(1.0 - beta * beta), sq(g, c))));
    return dv(g, mul(g, k(2.0), c), g.binary(KOp::Max, add(g, c, r), k(1.0e-9)));
}

// Sample a GGX micronormal around mesonormal `n_in` from the VISIBLE-normal distribution (Heitz 2018), for view direction `v`.
// The normal is first FLIPPED to face `v`, so the caller can pass either orientation and rely on |dot| downstream (which is
// exactly how Huang Eq 31/32 are written). Returns the micronormal in world (fibre-frame) coordinates.
[[nodiscard]] inline V3 huang_vndf_sample(KGraph& g, const V3& n_in, const V3& v, int u1, int u2, double beta)
{
    const auto k   = [&](double val) { return kf(g, u1, val); };
    const int  vn  = v3dot(g, v, n_in);
    const int  flip = g.select(g.binary(KOp::CmpGe, vn, k(0.0)), k(1.0), k(-1.0));
    const V3   n   = v3scale(g, n_in, flip);
    V3         t, b;
    huang_basis(g, n, t, b);
    const int vx = v3dot(g, v, t), vy = v3dot(g, v, b), vz = v3dot(g, v, n);
    // stretch to the hemisphere configuration
    const V3  vh    = v3norm(g, {mul(g, k(beta), vx), mul(g, k(beta), vy), vz});
    const int lensq = add(g, sq(g, vh.x), sq(g, vh.y));
    const int big   = g.binary(KOp::CmpGt, lensq, k(1.0e-12));
    const int ilen  = dv(g, k(1.0), g.binary(KOp::Max, safe_sqrt(g, lensq), k(1.0e-9)));
    const V3  t1v   = {g.select(big, mul(g, g.unary(KOp::Neg, vh.y), ilen), k(1.0)),
                       g.select(big, mul(g, vh.x, ilen), k(0.0)), k(0.0)};
    const V3  t2v   = v3cross(g, vh, t1v);
    // uniform point on the projected (hemi)disk
    const int r   = safe_sqrt(g, u1);
    const int ph  = mul(g, k(2.0 * kPi), u2);
    const int p1  = mul(g, r, g.unary(KOp::Cos, ph));
    int       p2  = mul(g, r, g.unary(KOp::Sin, ph));
    const int sm  = mul(g, k(0.5), add(g, k(1.0), vh.z));
    p2            = add(g, mul(g, sub(g, k(1.0), sm), safe_sqrt(g, sub(g, k(1.0), sq(g, p1)))), mul(g, sm, p2));
    const int pz  = safe_sqrt(g, sub(g, sub(g, k(1.0), sq(g, p1)), sq(g, p2)));
    const V3  nh  = {add(g, add(g, mul(g, p1, t1v.x), mul(g, p2, t2v.x)), mul(g, pz, vh.x)),
                     add(g, add(g, mul(g, p1, t1v.y), mul(g, p2, t2v.y)), mul(g, pz, vh.y)),
                     add(g, add(g, mul(g, p1, t1v.z), mul(g, p2, t2v.z)), mul(g, pz, vh.z))};
    const V3  ne  = v3norm(g, {mul(g, k(beta), nh.x), mul(g, k(beta), nh.y), g.binary(KOp::Max, nh.z, k(0.0))});
    return {add(g, add(g, mul(g, ne.x, t.x), mul(g, ne.y, b.x)), mul(g, ne.z, n.x)),
            add(g, add(g, mul(g, ne.x, t.y), mul(g, ne.y, b.y)), mul(g, ne.z, n.y)),
            add(g, add(g, mul(g, ne.x, t.z), mul(g, ne.y, b.z)), mul(g, ne.z, n.z))};
}

// triple32 (Wellons) integer hash → uniform f32 in [0,1). Deterministic ⇒ the stochastic internal path is bit-identical on the
// GPU and in the oracle (u32 ops wrap mod 2^32 on both). Same construction the RT estimators use.
[[nodiscard]] inline int huang_hash01(KGraph& g, int seed)
{
    const Shape sh = make_shape({1});
    const auto  cu = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, DType::U32); };
    const auto  xr = [&](int a, int b) { return g.binary(KOp::BitXor, a, b); };
    const auto  mu = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto  sr = [&](int a, crd::u32 s) { return g.binary(KOp::Shr, a, cu(s)); };
    int         h  = seed;
    h = xr(h, sr(h, 17U)); h = mu(h, cu(0xED5AD4BBU));
    h = xr(h, sr(h, 11U)); h = mu(h, cu(0xAC4C1B51U));
    h = xr(h, sr(h, 15U)); h = mu(h, cu(0x31848BABU));
    h = xr(h, sr(h, 14U));
    return g.binary(KOp::Mul, g.cast(sr(h, 8U), DType::F32), g.constant(1.0 / 16777216.0, sh, DType::F32));
}

// A direction from (θ, φ) in the Huang frame: ω = [sinφ·cosθ, sinθ, cosφ·cosθ]  (y = fibre tangent).
[[nodiscard]] inline V3 huang_dir(KGraph& g, int sin_t, int cos_t, int phi)
{
    return {mul(g, g.unary(KOp::Sin, phi), cos_t), sin_t, mul(g, g.unary(KOp::Cos, phi), cos_t)};
}
// The mesonormal at azimuth φm with cuticle tilt α (Huang §3.2): ωmα = [sinφm·cosα, sinα, cosφm·cosα].
[[nodiscard]] inline V3 huang_mesonormal(KGraph& g, int phi_m, double alpha_rad)
{
    const auto k = [&](double v) { return kf(g, phi_m, v); };
    return {mul(g, g.unary(KOp::Sin, phi_m), k(crd::math::cos(alpha_rad))), k(crd::math::sin(alpha_rad)),
            mul(g, g.unary(KOp::Cos, phi_m), k(crd::math::cos(alpha_rad)))};
}
} // namespace detail

// The Huang R lobe (Eq 12 with the Smith G ≈ 1 — the paper's OWN stated assumption for low-roughness hair, Appendix A):
//   S_R(ωi,ωo) = R(ωh,ωo) / (8·cosθo·cosθi) · ∫ D(ωh, ωm) dφm
// integrated over the VISIBLE ARC: the φm with ωm·ωi > 0 AND ωm·ωo > 0. With α = 0, ωm = [sinφm, 0, cosφm], so
// ωm·ωi = cosθi·cos(φm − φi) ⇒ the arc is φm ∈ [max(Δφ,0) − π/2, min(Δφ,0) + π/2] once rotated to φo = 0. `beta` is the GGX
// roughness (host constant — it folds √(1−β²) and β²/2π into constants). Returns a SCALAR (monochrome R lobe; the R lobe carries
// no absorption, so it is achromatic by construction — colour enters via TT/TRT).
[[nodiscard]] inline int huang_r_lobe_angles(KGraph& g, int sin_to, int cos_to, int phi_o, int sin_ti, int cos_ti, int phi_i,
                                             int eta, double beta)
{
    using namespace detail;
    const auto ks = [&](double v) { return kf(g, sin_ti, v); };

    // rotate so φo = 0: Δφ = wrap(φi − φo) into (−π, π]
    int       dphi = sub(g, phi_i, phi_o);
    dphi           = sub(g, dphi, mul(g, ks(2.0 * kPi), g.unary(KOp::Round, dv(g, dphi, ks(2.0 * kPi)))));

    // ωo = [0, sinθo, cosθo];  ωi = [sinΔφ·cosθi, sinθi, cosΔφ·cosθi]   (Huang frame, y = tangent)
    const int ox = ks(0.0), oy = sin_to, oz = cos_to;
    const int ix = mul(g, g.unary(KOp::Sin, dphi), cos_ti);
    const int iy = sin_ti;
    const int iz = mul(g, g.unary(KOp::Cos, dphi), cos_ti);

    // half vector ωh = normalize(ωi + ωo)
    const int hx = add(g, ix, ox), hy = add(g, iy, oy), hz = add(g, iz, oz);
    const int hlen = g.binary(KOp::Max, safe_sqrt(g, add(g, add(g, sq(g, hx), sq(g, hy)), sq(g, hz))), ks(1.0e-6));
    const int cos_th = dv(g, safe_sqrt(g, add(g, sq(g, hx), sq(g, hz))), hlen); // cosθh = |xz-projection| / |ωh|
    const int phi_h  = g.binary(KOp::Atan2, hx, hz);                            // azimuth of ωh (from z), already in (−π, π]
    const int cos_ho = dv(g, add(g, add(g, mul(g, hx, ox), mul(g, hy, oy)), mul(g, hz, oz)), hlen); // ωh·ωo (Fresnel arg)

    // A = cosθh·√(1−β²);  s = √(1−A²)  (β is a host constant ⇒ √(1−β²) folds away)
    const int a      = mul(g, cos_th, ks(crd::math::sqrt(1.0 - beta * beta)));
    const int a2     = sq(g, a);
    const int one_m  = g.binary(KOp::Max, sub(g, ks(1.0), a2), ks(1.0e-7)); // guard A → 1 (β → 0 at normal incidence)
    const int s      = safe_sqrt(g, one_m);

    // visible arc, rotated frame (φo = 0):  φ_lo = max(Δφ,0) − π/2,  φ_hi = min(Δφ,0) + π/2
    const int zero   = ks(0.0);
    const int phi_lo = sub(g, g.binary(KOp::Max, dphi, zero), ks(0.5 * kPi));
    const int phi_hi = add(g, g.binary(KOp::Min, dphi, zero), ks(0.5 * kPi));
    // ∫ D dφm = (β²/2π)·[ T(φh − φ_hi) − T(φh − φ_lo) ]   (u stays inside (−π/2, π/2) over the arc — see the note above)
    const int ndf_int = mul(g, ks(beta * beta / (2.0 * kPi)),
                            sub(g, huang_r_antideriv(g, sub(g, phi_h, phi_hi), a2, one_m, s),
                                huang_r_antideriv(g, sub(g, phi_h, phi_lo), a2, one_m, s)));

    // S_R = R(ωh,ωo)·∫D / (8 cosθo cosθi); guard the grazing denominators, and clamp the NDF integral non-negative.
    const int fres = fr_dielectric(g, g.unary(KOp::Abs, cos_ho), eta);
    const int den  = mul(g, ks(8.0), mul(g, g.binary(KOp::Max, g.unary(KOp::Abs, cos_to), ks(1.0e-5)),
                                         g.binary(KOp::Max, g.unary(KOp::Abs, cos_ti), ks(1.0e-5))));
    return dv(g, mul(g, fres, g.binary(KOp::Max, ndf_int, zero)), den);
}

// The full hair BCSDF f(wo, wi) — the fibre-frame (x = tangent) wrapper: extract the scalar angles from `wo`/`wi` and call the
// scalar-angle core. `sigma_a` vec3 (coloured fibre) or scalar (monochrome); the rest scalar. For the RASTER fragment path.
[[nodiscard]] inline int hair_bcsdf_eval(KGraph& g, int wo, int wi, int h, int eta, int sigma_a, int beta_m, int beta_n,
                                         int alpha_deg, double fur_kappa = 0.0, double fur_sigma = 0.0, double fur_albedo = 0.0,
                                         double fur_g = 0.0, double fur_beta_s = 0.8)
{
    using namespace detail;
    const auto ks     = [&](double v) { return kf(g, h, v); };
    const int  sin_to = g.swizzle(wo, 0);
    const int  cos_to = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_to)));
    const int  phi_o  = g.binary(KOp::Atan2, g.swizzle(wo, 2), g.swizzle(wo, 1));
    const int  sin_ti = g.swizzle(wi, 0);
    const int  cos_ti = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_ti)));
    const int  phi_i  = g.binary(KOp::Atan2, g.swizzle(wi, 2), g.swizzle(wi, 1));
    return hair_bcsdf_eval_angles(g, sin_to, cos_to, phi_o, sin_ti, cos_ti, phi_i, h, eta, sigma_a, beta_m, beta_n, alpha_deg,
                                  fur_kappa, fur_sigma, fur_albedo, fur_g, fur_beta_s);
}

// ── B18-a GPU gate: the scalar (monochrome-σₐ) BCSDF packaged as a COMPUTE KERNEL. ───────────────────────────────────────────
// The white-furnace test (CPU) certifies PHYSICAL correctness; this certifies PORTABILITY — the device output must match the CPU
// oracle to ULP on every backend (the CKIR contract, exactly like the atmosphere/cloud kernels). Because the whole cone is scalar
// (monochrome σₐ) it rides the scalar compute emitter. Input buffer 0 packs 6 F32/lane [sinθo, φo, sinθi, φi, h, σₐ]; output
// buffer 1 is 1 F32/lane = f(ωo,ωi). The material constants (η, βₘ, βₙ, cuticle-tilt α) are baked from the config.
// Which fibre model the kernel evaluates. Both live in this header and share the frame + Fresnel; pick per material.
//  Chiang — the separable, energy-conserving R/TT/TRT/TRRT near-field model (+ the analytic medulla). Cheap, artist-parameterized.
//  HuangR — the NON-separable microfacet R lobe with the analytic GGX azimuthal integral (physically-based highlight + glint).
//  HuangFull — R (analytic) + TT + TRT (combined MC-Simpson, Eq 31/32): the complete Huang microfacet fibre BCSDF.
enum class HairModel : crd::u32
{
    Chiang    = 0,
    HuangR    = 1,
    HuangFull = 2
};

struct HairKernelConfig
{
    HairModel model      = HairModel::Chiang;
    double    huang_beta = 0.3;  // GGX surface roughness for the Huang model
    // Huang TT/TRT: PINNED composite-Simpson sub-interval count over φm1 (forced even). The paper suggests a step of ~0.7β
    // (≈28 sub-intervals at β=0.08); pinning it — never adapting — is what keeps the estimator oracle-reproducible.
    int    simpson_n       = 32;
    double huang_alpha_rad = 0.0;  // cuticle scale tilt α (radians; Huang §3.2 shifts the mesonormal only)
    bool   include_r       = true; // add the analytic R lobe to the TT/TRT total
    bool   include_tt      = true; // per-lobe toggles (authoring control + the energy-attribution diagnostic)
    bool   include_trt     = true;
    double eta       = 1.55; // fibre IOR (keratin)
    double beta_m    = 0.3;  // longitudinal roughness
    double beta_n    = 0.3;  // azimuthal roughness
    double alpha_deg = 2.0;  // cuticle-scale tilt (degrees)
    // B18-b fur/medulla (Yan 2017 double-cylinder, analytic): kappa = medullary index (0 ⇒ pure hair), sigma_m = medulla
    // extinction, albedo_m = medulla single-scatter albedo, aniso_g = wrapped-Cauchy anisotropy, beta_s = scattered variance.
    double fur_kappa   = 0.0;
    double fur_sigma   = 2.0;
    double fur_albedo  = 0.9;
    double fur_g       = 0.0;
    double fur_beta_s  = 0.8;
};

// ── B18-b2: the Huang TT and TRT lobes (Eq 23/24, evaluated per Eq 31/32) ───────────────────────────────────────────────────
// These lobes are non-separable AND 3-D/5-D in closed form, so Huang evaluates them with a COMBINED MC-Simpson estimator: a 1-D
// composite Simpson quadrature over the mesonormal azimuth φm1, and for each node ONE stochastic internal light path — sample the
// micronormal ωh1 from the VISIBLE-normal distribution and refract to ωt (for TRT additionally sample ωh2 and reflect to ωtr).
// The FINAL micronormal is not sampled: it is DETERMINED by the required exit direction (ωh2 = normalize(−ωt + ωo/η) for TT,
// ωh3 = normalize(ωtr + ωo/η) for TRT) — a connect-to-the-outgoing-direction step, structurally like NEE.
// ⚠ DETERMINISM: the sub-interval count is pinned (never adaptive) and every uniform comes from triple32 keyed on (lane, node),
//   so this stochastic estimator is bit-reproducible ⇒ GPU == CPU oracle exactly (the ReSTIR / path-tracer discipline).
// ⚠ ABSORPTION uses Huang's CORRECTED internal path length 2cosγt — Marschner's 2+2cos(2γt) drops a square root (Huang §3.1.2);
//   the chord is additionally clamped ≥ 0 since a negative path length is unphysical.
// Emits statements accumulating into out_b[tid]; `cfg.include_r` folds in the analytic R lobe.
inline void huang_emit_tt_trt(KGraph& g, int out_b, int tid, int sin_to, int cos_to, int sin_ti, int cos_ti, int dphi,
                              int sigma_a, int eta, const HairKernelConfig& cfg)
{
    using namespace detail;
    const auto   ks   = [&](double v) { return kf(g, sin_ti, v); };
    const Shape  shu  = make_shape({1});
    const auto   cu   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, DType::U32); };
    const auto   absn = [&](int a) { return g.unary(KOp::Abs, a); };
    const double beta = cfg.huang_beta;
    const int    nseg = (cfg.simpson_n < 2 ? 2 : (cfg.simpson_n & ~1)); // Simpson needs an EVEN sub-interval count
    const double hstep = kPi / static_cast<double>(nseg);               // the arc is exactly π wide

    const V3 wo = huang_dir(g, sin_to, cos_to, ks(0.0)); // rotated frame: ωo azimuth 0
    const V3 wi = huang_dir(g, sin_ti, cos_ti, dphi);

    g.stmt_buffer_store(out_b, tid, ks(0.0)); // Simpson accumulator ← 0

    // integrate φm1 over the arc where light can ENTER the fibre (ωm1·ωi > 0): φm1 ∈ (Δφ − π/2, Δφ + π/2)
    const int lo    = sub(g, dphi, ks(0.5 * kPi));
    const int floop = g.stmt_for_begin(cu(static_cast<crd::u32>(nseg + 1)));
    const int iv    = g.kernel_loop_var(floop); // 0 .. nseg
    const int phim1 = add(g, lo, mul(g, g.cast(iv, DType::F32), ks(hstep)));

    // composite-Simpson weights: 1 at both ends, 4 on odd nodes, 2 on even interior nodes
    const int is_end = g.binary(KOp::BitOr, g.binary(KOp::CmpEq, iv, cu(0U)),
                                g.binary(KOp::CmpEq, iv, cu(static_cast<crd::u32>(nseg))));
    const int is_odd = g.binary(KOp::CmpEq, g.binary(KOp::Mod, iv, cu(2U)), cu(1U));
    const int wgt    = g.select(is_end, ks(1.0), g.select(is_odd, ks(4.0), ks(2.0)));

    // deterministic uniforms for this (lane, node)
    const int sd = g.binary(KOp::Add, g.binary(KOp::Mul, iv, cu(0x9E3779B9U)), cu(1U));
    const int u1 = huang_hash01(g, g.binary(KOp::BitXor, g.binary(KOp::Mul, tid, cu(0x632BE5ABU)), sd));
    const int u2 = huang_hash01(g, g.binary(KOp::BitXor, g.binary(KOp::Mul, tid, cu(0x85157AF5U)), g.binary(KOp::Mul, sd, cu(0xC2B2AE35U))));
    const int u3 = huang_hash01(g, g.binary(KOp::BitXor, g.binary(KOp::Mul, tid, cu(0x27220A95U)), g.binary(KOp::Mul, sd, cu(0x165667B1U))));
    const int u4 = huang_hash01(g, g.binary(KOp::BitXor, g.binary(KOp::Mul, tid, cu(0x9E3779B1U)), g.binary(KOp::Mul, sd, cu(0x85EBCA6BU))));

    // ── interface ①: mesonormal, sampled micronormal, Fresnel, refraction inward ──
    const V3  m1   = huang_mesonormal(g, phim1, cfg.huang_alpha_rad);
    const V3  h1   = huang_vndf_sample(g, m1, wi, u1, u2, beta);
    // ⛔ EMIT EXPLOSION: Select/Cmp are INLINE ops in the compute emitter, so every downstream use of a select-heavy value
    // RE-EXPANDS its whole subtree textually. The VNDF sampler is select-heavy (normal flip + the degenerate-basis branch) and
    // its result feeds ~a dozen dots ⇒ the GLSL blows up exponentially and the shader compiler never finishes. Materializing
    // the sampler/refraction outputs pins them to temps, so each use is a single identifier. (Same class as the unrolled-sort
    // select explosion scar.)
    g.stmt_materialize(h1.x); g.stmt_materialize(h1.y); g.stmt_materialize(h1.z);
    const int acih = absn(v3dot(g, wi, h1));
    const int t1f  = sub(g, ks(1.0), fr_dielectric(g, acih, eta));
    const int disc = safe_sqrt(g, sub(g, add(g, sq(g, eta), sq(g, acih)), ks(1.0)));
    const int cf1  = sub(g, acih, disc); // ωt = (1/η)·( (|ωi·ωh1| − √(η²+|ωi·ωh1|²−1))·ωh1 − ωi )
    const V3  wt   = v3norm(g, {dv(g, sub(g, mul(g, cf1, h1.x), wi.x), eta), dv(g, sub(g, mul(g, cf1, h1.y), wi.y), eta),
                                dv(g, sub(g, mul(g, cf1, h1.z), wi.z), eta)});
    g.stmt_materialize(wt.x); g.stmt_materialize(wt.y); g.stmt_materialize(wt.z);
    const int phit  = g.binary(KOp::Atan2, wt.x, wt.z);
    const int costt = g.binary(KOp::Max, safe_sqrt(g, add(g, sq(g, wt.x), sq(g, wt.z))), ks(1.0e-5));
    const int chdt  = g.binary(KOp::Max, mul(g, ks(2.0), g.unary(KOp::Cos, add(g, sub(g, phit, phim1), ks(kPi)))), ks(0.0));
    const int a_t   = g.unary(KOp::Exp, g.unary(KOp::Neg, mul(g, sigma_a, dv(g, chdt, costt)))); // Eq 17 (corrected chord)
    const int cwim1 = v3dot(g, wi, m1);
    const int g1t   = huang_smith_g1(g, v3dot(g, wt, m1), beta);
    const int dwtm1 = g.binary(KOp::Max, absn(v3dot(g, wt, m1)), ks(1.0e-6));

    // ── TT (Eq 31): ωh2 is DETERMINED by the exit direction — ω̄h2 = −ωt + ωo/η (its norm enters the estimator) ──
    const int phim2 = sub(g, mul(g, ks(2.0), phit), phim1);
    const V3  m2    = huang_mesonormal(g, phim2, -cfg.huang_alpha_rad);
    const V3  bh2   = {sub(g, dv(g, wo.x, eta), wt.x), sub(g, dv(g, wo.y, eta), wt.y), sub(g, dv(g, wo.z, eta), wt.z)};
    const int bh2n  = g.binary(KOp::Max, safe_sqrt(g, v3dot(g, bh2, bh2)), ks(1.0e-6));
    const V3  h2    = {dv(g, bh2.x, bh2n), dv(g, bh2.y, bh2n), dv(g, bh2.z, bh2n)};
    const int inv_e = dv(g, ks(1.0), eta); // EXITING the fibre ⇒ the relative IOR inverts (TIR ⇒ Fresnel 1 ⇒ T = 0)
    const int t2f   = sub(g, ks(1.0), fr_dielectric(g, absn(v3dot(g, wt, h2)), inv_e));
    const int num_tt = mul(g, mul(g, mul(g, t1f, t2f), mul(g, g1t, mul(g, huang_ggx_d(g, v3dot(g, h2, m2), beta),
                                                                       huang_smith_g1(g, v3dot(g, wo, m2), beta)))), a_t);
    const int geo_tt = dv(g, mul(g, mul(g, absn(cwim1), absn(v3dot(g, wo, h2))), absn(v3dot(g, wt, h2))), dwtm1);
    int       f_tt   = dv(g, mul(g, num_tt, geo_tt), mul(g, sq(g, eta), sq(g, bh2n)));
    f_tt = g.select(g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, cwim1, ks(0.0)),
                             g.binary(KOp::CmpGt, v3dot(g, wo, m2), ks(0.0))), f_tt, ks(0.0));

    // ── TRT (Eq 32): sample ωh2 about m2 (view = −ωt), reflect internally, then ωh3 determined by ωo ──
    const V3  nwt  = {g.unary(KOp::Neg, wt.x), g.unary(KOp::Neg, wt.y), g.unary(KOp::Neg, wt.z)};
    const V3  h2r  = huang_vndf_sample(g, m2, nwt, u3, u4, beta); // visible-normal semantics ⇒ sampled about the −ωt-facing side
    // ⛔ ORIENTATION: the sampler returns a micronormal facing the VIEW (−ωt), but Huang's ωtr = 2|ωt·ωh2|·ωh2 − ωt assumes ωh2 is
    // oriented WITH ωt (ωt·ωh2 > 0). Using the view-facing orientation reflects to the wrong hemisphere and inflates TRT energy.
    const int or2  = g.select(g.binary(KOp::CmpGe, v3dot(g, wt, h2r), ks(0.0)), ks(1.0), ks(-1.0));
    const V3  h2s  = v3scale(g, h2r, or2);
    g.stmt_materialize(h2s.x); g.stmt_materialize(h2s.y); g.stmt_materialize(h2s.z); // second VNDF — same explosion hazard
    const int cth2 = v3dot(g, wt, h2s);
    const int r2   = fr_dielectric(g, absn(cth2), inv_e); // internal reflectance at ②
    const int k2   = mul(g, ks(2.0), absn(cth2));         // ωtr = 2|ωt·ωh2|·ωh2 − ωt
    const V3  wtr  = v3norm(g, {sub(g, mul(g, k2, h2s.x), wt.x), sub(g, mul(g, k2, h2s.y), wt.y), sub(g, mul(g, k2, h2s.z), wt.z)});
    g.stmt_materialize(wtr.x); g.stmt_materialize(wtr.y); g.stmt_materialize(wtr.z);
    const int phitr = g.binary(KOp::Atan2, wtr.x, wtr.z);
    const int costr = g.binary(KOp::Max, safe_sqrt(g, add(g, sq(g, wtr.x), sq(g, wtr.z))), ks(1.0e-5));
    const int chdr  = g.binary(KOp::Max, mul(g, ks(2.0), g.unary(KOp::Cos, add(g, sub(g, phitr, phim2), ks(kPi)))), ks(0.0));
    const int a_tr  = g.unary(KOp::Exp, g.unary(KOp::Neg, mul(g, sigma_a, dv(g, chdr, costr)))); // Eq 25
    const V3  m3    = huang_mesonormal(g, add(g, sub(g, phim1, mul(g, ks(2.0), sub(g, phit, phitr))), ks(kPi)), -cfg.huang_alpha_rad);
    const V3  bh3   = {add(g, dv(g, wo.x, eta), wtr.x), add(g, dv(g, wo.y, eta), wtr.y), add(g, dv(g, wo.z, eta), wtr.z)};
    const int bh3n  = g.binary(KOp::Max, safe_sqrt(g, v3dot(g, bh3, bh3)), ks(1.0e-6));
    const V3  h3    = {dv(g, bh3.x, bh3n), dv(g, bh3.y, bh3n), dv(g, bh3.z, bh3n)};
    const int t3f   = sub(g, ks(1.0), fr_dielectric(g, absn(v3dot(g, wtr, h3)), inv_e));
    const int num_trt = mul(g, mul(g, mul(g, t1f, mul(g, r2, t3f)),
                                   mul(g, mul(g, g1t, huang_smith_g1(g, v3dot(g, wtr, m2), beta)),
                                       mul(g, huang_ggx_d(g, v3dot(g, h3, m3), beta), huang_smith_g1(g, v3dot(g, wo, m3), beta)))),
                               mul(g, a_t, a_tr));
    const int geo_trt = dv(g, mul(g, mul(g, absn(v3dot(g, wtr, h3)), absn(v3dot(g, wo, h3))),
                                  mul(g, absn(cwim1), absn(v3dot(g, wt, m2)))),
                           mul(g, dwtm1, g.binary(KOp::Max, absn(v3dot(g, wtr, m2)), ks(1.0e-6))));
    int f_trt = dv(g, mul(g, num_trt, geo_trt), mul(g, sq(g, eta), sq(g, bh3n)));
    f_trt = g.select(g.binary(KOp::BitAnd, g.binary(KOp::CmpGt, cwim1, ks(0.0)),
                              g.binary(KOp::CmpGt, v3dot(g, wo, m3), ks(0.0))), f_trt, ks(0.0));

    if (!cfg.include_tt) { f_tt = ks(0.0); }
    if (!cfg.include_trt) { f_trt = ks(0.0); }
    g.stmt_buffer_store(out_b, tid, add(g, g.buffer_load(out_b, tid), mul(g, wgt, add(g, f_tt, f_trt))));
    g.stmt_for_end(floop);

    // Simpson scale (h/3) folded with the 1/(2 cosθo cosθi) prefactor; optionally + the analytic R lobe.
    const int pref = dv(g, ks(hstep / 3.0), mul(g, ks(2.0), mul(g, g.binary(KOp::Max, absn(cos_to), ks(1.0e-5)),
                                                               g.binary(KOp::Max, absn(cos_ti), ks(1.0e-5)))));
    int total = mul(g, g.buffer_load(out_b, tid), pref);
    if (cfg.include_r)
    {
        total = add(g, total, huang_r_lobe_angles(g, sin_to, cos_to, ks(0.0), sin_ti, cos_ti, dphi, eta, beta));
    }
    g.stmt_buffer_store(out_b, tid, g.binary(KOp::Max, total, ks(0.0)));
}

[[nodiscard]] inline KEntry build_hair_bcsdf_kernel(KGraph& g, const HairKernelConfig& cfg)
{
    using namespace detail;
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };

    const int in_b  = g.buffer_decl(DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(DType::F32, 0, 1, true);
    const int p     = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark   = g.kernel_stmt_mark();
    const int base   = g.binary(KOp::Mul, p, ku(6));
    const int sin_to = g.buffer_load(in_b, base);
    const int phi_o  = g.buffer_load(in_b, g.binary(KOp::Add, base, ku(1)));
    const int sin_ti = g.buffer_load(in_b, g.binary(KOp::Add, base, ku(2)));
    const int phi_i  = g.buffer_load(in_b, g.binary(KOp::Add, base, ku(3)));
    const int h      = g.buffer_load(in_b, g.binary(KOp::Add, base, ku(4)));
    const int sig    = g.buffer_load(in_b, g.binary(KOp::Add, base, ku(5)));
    // Freeze the per-lane loads at top-level (main) scope: the BCSDF's branchless selects can lower to if-blocks, and a shared temp
    // declared inside the first block would be out of scope in a sibling block (the ckir_if_block_shared_temp scar). No barriers
    // here ⇒ the inputs never change, so hoisting is safe.
    g.stmt_materialize(sin_to);
    g.stmt_materialize(phi_o);
    g.stmt_materialize(sin_ti);
    g.stmt_materialize(phi_i);
    g.stmt_materialize(h);
    g.stmt_materialize(sig);
    const auto ks     = [&](double v) { return kf(g, h, v); };
    const int  cos_to = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_to)));
    const int  cos_ti = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_ti)));
    g.stmt_materialize(cos_to);
    g.stmt_materialize(cos_ti);
    if (cfg.model == HairModel::HuangFull)
    {
        // R + TT + TRT: the TT/TRT half emits a Simpson loop, so it writes the accumulator itself. Rotate to φo = 0 first.
        int dphi = sub(g, phi_i, phi_o);
        dphi     = sub(g, dphi, mul(g, ks(2.0 * kPi), g.unary(KOp::Round, dv(g, dphi, ks(2.0 * kPi)))));
        g.stmt_materialize(dphi);
        huang_emit_tt_trt(g, out_b, p, sin_to, cos_to, sin_ti, cos_ti, dphi, sig, ks(cfg.eta), cfg);
    }
    else
    {
        const int f = (cfg.model == HairModel::HuangR)
                          ? huang_r_lobe_angles(g, sin_to, cos_to, phi_o, sin_ti, cos_ti, phi_i, ks(cfg.eta), cfg.huang_beta)
                          : hair_bcsdf_eval_angles(g, sin_to, cos_to, phi_o, sin_ti, cos_ti, phi_i, h, ks(cfg.eta), sig,
                                                   ks(cfg.beta_m), ks(cfg.beta_n), ks(cfg.alpha_deg), cfg.fur_kappa,
                                                   cfg.fur_sigma, cfg.fur_albedo, cfg.fur_g, cfg.fur_beta_s);
        g.stmt_buffer_store(out_b, p, f);
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::hair
