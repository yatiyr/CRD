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
[[nodiscard]] inline int hair_bcsdf_eval_angles(KGraph& g, int sin_to, int cos_to, int phi_o, int sin_ti, int cos_ti,
                                                int phi_i, int h, int eta, int sigma_a, int beta_m, int beta_n, int alpha_deg)
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

    // divide by |cosθᵢ| (pbrt's measure) — guarded so a grazing wi never divides by ~0.
    const int abscos = g.unary(KOp::Abs, cos_ti);
    return g.select(g.binary(KOp::CmpGt, abscos, ks(1.0e-5)), bin(g, KOp::Div, fsum, abscos), fsum);
}

// The full hair BCSDF f(wo, wi) — the fibre-frame (x = tangent) wrapper: extract the scalar angles from `wo`/`wi` and call the
// scalar-angle core. `sigma_a` vec3 (coloured fibre) or scalar (monochrome); the rest scalar. For the RASTER fragment path.
[[nodiscard]] inline int hair_bcsdf_eval(KGraph& g, int wo, int wi, int h, int eta, int sigma_a, int beta_m, int beta_n,
                                         int alpha_deg)
{
    using namespace detail;
    const auto ks     = [&](double v) { return kf(g, h, v); };
    const int  sin_to = g.swizzle(wo, 0);
    const int  cos_to = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_to)));
    const int  phi_o  = g.binary(KOp::Atan2, g.swizzle(wo, 2), g.swizzle(wo, 1));
    const int  sin_ti = g.swizzle(wi, 0);
    const int  cos_ti = safe_sqrt(g, sub(g, ks(1.0), sq(g, sin_ti)));
    const int  phi_i  = g.binary(KOp::Atan2, g.swizzle(wi, 2), g.swizzle(wi, 1));
    return hair_bcsdf_eval_angles(g, sin_to, cos_to, phi_o, sin_ti, cos_ti, phi_i, h, eta, sigma_a, beta_m, beta_n, alpha_deg);
}

} // namespace crd::kir::hair
