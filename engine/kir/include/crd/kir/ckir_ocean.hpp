#pragma once

// ckir_ocean.hpp — D-007 B16-a: FFT OCEAN (Tessendorf-family cascaded spectral ocean), authored in CKIR. The signature
// real-time ocean: a wave SPECTRUM in the frequency domain, evolved in time and inverse-FFT'd (our own `build_fft2d_c2c`,
// bit-exact all backends) into a height/displacement field. Statement-tier compute; bit-exact CPU oracle + backends.
//
// [B16-a-1] THE SPECTRUM (this file, first kernel) — the INITIAL frequency-domain state h0(k). The gold-standard bar is the
// **Horvath 2015 "Empirical Directional Wave Spectra" (DigiPro)** framework: a physical NON-directional energy spectrum
// (**JONSWAP**, fetch/wind-parameterised, peak-enhanced) times a frequency-dependent **directional spreading** (Mitsuyasu/
// Hasselmann cos-2s, with a **swell** boost at long wavelengths) — NOT the old flat Phillips spectrum. The gravity-capillary
// **dispersion** ω(k) includes finite DEPTH (`tanh(kh)`) and the capillary (surface-tension) term. h0(k) is that spectrum
// modulated by a per-texel complex Gaussian (Box-Muller from the B6 hash — deterministic, portable, no upload):
//     h0(k) = (ξr + i·ξi)·Δk·√Ψ(k)·amplitude,   Ψ(k) = S(ω)·D(ω,θ)·(dω/dk)/k,   Δk = 2π/L
// We precompute BOTH h0(k) and conj(h0(−k)) so the a-2 time-evolution is a cheap per-texel combine (no mirror gather).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_noise.hpp>
#include <crd/math/deterministic.hpp> // crd::math::pow (Math Mandate — no std:: transcendentals in engine code)
#include <crd/units/units.hpp>        // Length/Velocity/Acceleration/Angle for the physical spectrum inputs (ADR-0078)

#include <cmath>

namespace crd::kir::ocean
{

struct OceanConfig
{
    int                     n            = 256;                             // FFT resolution (n×n); MUST be a power of two
    crd::units::Length64     patch_length{250.0};                            // L — the tile size in metres (the spatial period)
    crd::units::Velocity64   wind_speed{10.0};                               // U10 — wind speed at 10 m (m/s); drives the JONSWAP peak + energy
    crd::units::Angle64      wind_dir{0.0};                                  // θ0 — wind/wave heading (radians)
    crd::units::Length64     fetch{100000.0};                                // F — fetch in metres (how far the wind has blown); sets peak + α
    crd::units::Length64     depth{200.0};                                   // water depth (m) — the dispersion tanh(kh) shallowing term
    crd::units::Acceleration64 gravity{9.81};                                // g (m/s²)
    double   swell           = 0.25;     // 0..1 — swell fraction: boosts long-wave directionality (narrow swell) — dimensionless
    double   peak_enhance    = 3.3;      // JONSWAP γ (peak sharpening); 1 = Pierson-Moskowitz — dimensionless
    double   amplitude       = 1.0;      // overall height scale (tunable; folds the spectrum normalisation) — dimensionless
    crd::units::Length64     small_wave{0.3};                                // capillary CUTOFF length (m) — suppress ripples below this (exp(−k²l²))
    double   choppiness      = 1.0;      // λ — horizontal displacement scale (sharpens crests, Gerstner-style); 0 = pure height — dimensionless
    double   foam_bias       = 0.0;      // foam appears where the displacement Jacobian J < foam_bias (crests pinch/overlap) — dimensionless
    double   foam_scale      = 1.0;      // foam ramp steepness: coverage = saturate((foam_bias − J)·foam_scale) — dimensionless
    double   foam_decay      = 0.92;     // per-frame foam persistence (temporal accumulation): foam = max(foam·decay, inject) — dimensionless
    crd::u32 seed            = 1234U;    // random seed for the phase field

    [[nodiscard]] bool valid() const noexcept
    {
        return n >= 8 && (n & (n - 1)) == 0 && patch_length.value > 0.0 && wind_speed.value > 0.0 && fetch.value > 0.0
               && depth.value > 0.0;
    }
};

// [B16-a-3] A CASCADE STACK — the multi-scale FFT ocean. Real oceans span wavelengths from long swells to fine capillary
// ripples, more than one N×N patch can resolve without aliasing. The gold-standard fix (Tessendorf / production) is C
// independent patches of DIFFERENT spatial period L_c (large → small); each is a full a-2 pipeline (spectrum → evolve → IFFT
// → assemble), and the water shader (a-4) samples + sums all C at world uv_c = worldpos / L_c. Sharing one grid resolution N,
// the C cascades' 4 packed spectra (4·C total) ride ONE batched inverse 2-D FFT — the DRAM-bound regime where the batched +
// fused crush finally pays off. `base` carries the shared wind/fetch/depth/foam params; only L and choppiness vary per cascade.
struct OceanCascadeConfig
{
    static constexpr int kMaxCascades                = 4;
    int                  count                       = 3;
    crd::units::Length64 patch_length[kMaxCascades]  = {crd::units::Length64{1000.0}, crd::units::Length64{250.0},
                                                        crd::units::Length64{60.0}, crd::units::Length64{15.0}}; // L per cascade (m), largest → smallest
    double               choppiness[kMaxCascades]    = {1.0, 1.0, 0.8, 0.6};        // λ per cascade (long swells less choppy) — dimensionless
    OceanConfig          base;                                                       // shared params (n/wind/fetch/depth/foam/…)

    [[nodiscard]] OceanConfig cascade(int c) const noexcept
    {
        OceanConfig cfg  = base;
        cfg.patch_length = patch_length[c];
        cfg.choppiness   = choppiness[c];
        return cfg;
    }
    [[nodiscard]] bool valid() const noexcept { return count >= 1 && count <= kMaxCascades && base.valid(); }
};

namespace detail
{
namespace nz = crd::kir::nodes::noise;

inline constexpr double kPi        = 3.14159265358979323846;
inline constexpr double kCapillary = 7.4e-5; // σ/ρ (surface tension / water density), m³/s²

[[nodiscard]] inline int log2i(int n) noexcept
{
    int l = 0;
    while ((1 << l) < n) { ++l; }
    return l;
}

// Gravity-capillary finite-depth dispersion ω(k) = √((g·|k| + (σ/ρ)|k|³)·tanh(|k|·h)). Shared by the standalone and FUSED
// evolve kernels so both compute IDENTICAL phases (the fused row-IFFT is then bit-exact vs the un-fused pipeline). Returns
// |k| (floored at 1e-6, so cx=kx/|k| never divides by 0) and ω.
inline void dispersion(KGraph& g, int kx, int kz, double grav, double depth, int& km_out, int& w_out)
{
    const auto kf  = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };

    const int k2 = add(mul(kx, kx), mul(kz, kz));
    const int km = mx(g.unary(KOp::Sqrt, k2), kf(1e-6));
    const int th = g.unary(KOp::Tanh, mul(km, kf(depth)));
    const int k3 = mul(mul(km, km), km);
    const int gk = add(mul(km, kf(grav)), mul(k3, kf(kCapillary)));
    km_out       = km;
    w_out        = g.unary(KOp::Sqrt, mx(mul(gk, th), kf(1e-12)));
}

// Pack the 8 real ocean fields (height, displacement Dx/Dz, slope ∂h/∂x·∂h/∂z, foam-Jacobian gradients) into the 4 EXACTLY-
// Hermitian complex spectra C0..C3 (see build_ocean_evolve's header table) from h0=(ar,ai), conj(h0(−k))=(br,bi), the wave-
// vector (kx,kz,km=|k|), and e^{iωt}=(c,s). Shared by the standalone and fused evolve ⇒ identical bits.
inline void evolve_pack(KGraph& g, int kx, int kz, int km, int ar, int ai, int br, int bi, int c, int s, int out_re[4],
                        int out_im[4])
{
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto dv  = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto neg = [&](int a) { return g.unary(KOp::Neg, a); };

    const int hr  = add(sub(mul(ar, c), mul(ai, s)), add(mul(br, c), mul(bi, s)));
    const int hi  = add(add(mul(ar, s), mul(ai, c)), sub(mul(bi, c), mul(br, s)));
    const int cx  = dv(kx, km);
    const int cz  = dv(kz, km);
    const int dxx = mul(kx, cx);
    const int dzz = mul(kz, cz);
    const int dxz = mul(kx, cz);
    out_re[0] = add(hr, mul(cx, hr));
    out_im[0] = add(hi, mul(cx, hi));
    out_re[1] = sub(mul(cz, hi), mul(kx, hr));
    out_im[1] = sub(neg(mul(cz, hr)), mul(kx, hi));
    out_re[2] = sub(neg(mul(kz, hi)), mul(dxx, hi));
    out_im[2] = add(mul(kz, hr), mul(dxx, hr));
    out_re[3] = sub(mul(dzz, hr), mul(dxz, hi));
    out_im[3] = add(mul(dzz, hi), mul(dxz, hr));
}
} // namespace detail

// Build the ocean SPECTRUM kernel — one thread per (n,m) frequency texel of the n×n grid. Writes:
//   buffer 0 = h0 packed float4 per texel: [h0(k).re, h0(k).im, conj(h0(−k)).re, conj(h0(−k)).im]  (4·n² floats) — feeds a-2.
//   buffer 1 = the deterministic spectral magnitude Δk·√Ψ(k) per texel (n² floats) — the FIELD-INDEPENDENT spectrum for tests/viz.
// Dispatch n²/64 workgroups.
[[nodiscard]] inline KEntry build_ocean_spectrum(KGraph& g, const OceanConfig& cfg)
{
    namespace d  = detail;
    namespace hh = detail::nz::detail;

    const int    n     = cfg.n;
    const int    lgn   = d::log2i(n);
    const int    half  = n / 2;
    const double patch = cfg.patch_length.value;
    const double grav  = cfg.gravity.value;
    const double wind  = cfg.wind_speed.value;
    const double fetch = cfg.fetch.value;
    const double depth = cfg.depth.value;
    const double dk    = 2.0 * d::kPi / patch;

    // JONSWAP fetch/wind parameters (Hasselmann et al.): peak angular frequency + Phillips α + Mitsuyasu peak spread.
    const double wp    = 22.0 * crd::math::pow(grav * grav / (wind * fetch), 1.0 / 3.0);
    const double alpha = 0.076 * crd::math::pow(wind * wind / (fetch * grav), 0.22);
    const double sp    = 11.5 * crd::math::pow(grav / (wind * wp), 2.5);

    const auto kf  = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto dv  = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto mn  = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto un  = [&](KOp op, int a) { return g.unary(op, a); };

    const int h0b  = g.buffer_decl(DType::F32, 0, 0, true);
    const int ampb = g.buffer_decl(DType::F32, 0, 1, true);

    const int mark = g.kernel_stmt_mark();
    const int p    = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    // texel (ni, mi) ∈ [0,N)² and the CENTERED integer frequency (nc, mc) ∈ [−N/2, N/2).
    const int ni = g.binary(KOp::BitAnd, p, ku(static_cast<crd::u32>(n - 1)));
    const int mi = g.binary(KOp::BitAnd, g.binary(KOp::Shr, p, ku(static_cast<crd::u32>(lgn))), ku(static_cast<crd::u32>(n - 1)));
    const int nc = sub(g.cast(ni, DType::F32), kf(static_cast<double>(half)));
    const int mc = sub(g.cast(mi, DType::F32), kf(static_cast<double>(half)));
    const int kx = mul(nc, kf(dk));
    const int kz = mul(mc, kf(dk));

    // Ψ(k)·Δk² magnitude and the complex Gaussian h0 for a wavevector (wx,wz) whose phase random comes from texel (ri,rj).
    // Returns the spectral magnitude `amp = Δk·√Ψ` and writes the complex h0 = amp·(ξr + iξi) into (ore, oim).
    const auto h0comp = [&](int wx, int wz, int ri, int rj, int& oamp, int& ore, int& oim) {
        const int k2    = add(mul(wx, wx), mul(wz, wz));
        const int k     = un(KOp::Sqrt, k2);
        const int ksafe = mx(k, kf(1e-6));
        const int tiny  = g.binary(KOp::CmpLt, k, kf(1e-5)); // the k≈0 DC cell carries no wave

        // gravity-capillary dispersion with finite depth: ω = √((g·k + (σ/ρ)·k³)·tanh(k·h))
        const int kh    = mul(ksafe, kf(depth));
        const int th    = un(KOp::Tanh, kh);
        const int k3    = mul(mul(ksafe, ksafe), ksafe);
        const int gk    = add(mul(ksafe, kf(grav)), mul(k3, kf(d::kCapillary)));
        const int w     = un(KOp::Sqrt, mx(mul(gk, th), kf(1e-12)));
        // dω/dk = f'/(2ω), f = (g·k + (σ/ρ)k³)·tanh(kh); f' = (g+3(σ/ρ)k²)tanh + (g·k+(σ/ρ)k³)·h·sech²
        const int sech2 = sub(kf(1.0), mul(th, th));
        const int fp    = add(mul(add(kf(grav), mul(kf(3.0 * d::kCapillary), mul(ksafe, ksafe))), th), mul(mul(gk, kf(depth)), sech2));
        const int dwk   = dv(fp, mul(kf(2.0), w));

        // JONSWAP S(ω) = α·g²/ω⁵ · exp(−5/4·(ωp/ω)⁴) · γ^r,  r = exp(−(ω−ωp)²/(2σ²ωp²)), σ = 0.07 (ω≤ωp) else 0.09
        const int wr    = dv(w, kf(wp));
        const int w2    = mul(w, w);
        const int w5    = mx(mul(mul(w2, w2), w), kf(1e-20));
        const int wpw   = dv(kf(wp), w);
        const int wpw2  = mul(wpw, wpw);
        const int pm    = un(KOp::Exp, un(KOp::Neg, mul(kf(1.25), mul(wpw2, wpw2)))); // exp(−5/4·(ωp/ω)⁴)
        const int sig   = g.select(g.binary(KOp::CmpLe, w, kf(wp)), kf(0.07), kf(0.09));
        const int dws   = sub(w, kf(wp));
        const int rexp  = un(KOp::Exp, un(KOp::Neg, dv(mul(dws, dws), mul(kf(2.0 * wp * wp), mul(sig, sig)))));
        const int gpow  = g.binary(KOp::Pow, kf(cfg.peak_enhance), rexp);              // γ^r
        const int jsw0  = mul(mul(dv(mul(kf(alpha), kf(grav * grav)), w5), pm), gpow);
        // TMA (Horvath 2015): Kitaigorodskii finite-depth attenuation Φ(ω,h). ωh = ω√(h/g); Φ = ½ωh² (ωh≤1),
        // 1−½(2−ωh)² (1<ωh<2), else 1 (ωh≥2, deep water ⇒ NO attenuation, TMA = JONSWAP). CLAMP ωh≤2 in the mid
        // branch so ωh≥2 gives exactly 1 — an unclamped (2−ωh)² goes large and would drive Φ negative (⇒ zero spectrum).
        const int wh    = mul(w, kf(std::sqrt(depth / grav)));
        const int t2h   = sub(kf(2.0), mn(wh, kf(2.0)));
        const int phi   = mn(mx(g.select(g.binary(KOp::CmpLe, wh, kf(1.0)), mul(kf(0.5), mul(wh, wh)),
                                         sub(kf(1.0), mul(kf(0.5), mul(t2h, t2h)))), kf(0.0)), kf(1.0));
        const int jsw   = mul(jsw0, phi);

        // directional spread D(ω,θ) = ½√(s/π)·cos^{2s}((θ−θ0)/2). Mitsuyasu s = sp·(ω/ωp)^μ, μ=5 (ω≤ωp) else −2.5;
        // swell boosts s for the long waves (ω<ωp) ⇒ a narrow, strongly-directional swell.
        const int theta = g.binary(KOp::Atan2, wz, wx);
        const int dth   = sub(theta, kf(cfg.wind_dir.value));
        const int wrapd = sub(dth, mul(kf(2.0 * d::kPi), un(KOp::Floor, add(dv(dth, kf(2.0 * d::kPi)), kf(0.5))))); // → (−π,π]
        const int mu    = g.select(g.binary(KOp::CmpLe, w, kf(wp)), kf(5.0), kf(-2.5));
        const int s0    = mul(kf(sp), g.binary(KOp::Pow, wr, mu));
        const int relu  = mx(sub(wpw, kf(1.0)), kf(0.0));                              // >0 only for ω<ωp
        const int s     = mn(mul(s0, add(kf(1.0), mul(kf(cfg.swell * 10.0), relu))), kf(200.0));
        const int cosh  = mx(un(KOp::Cos, mul(wrapd, kf(0.5))), kf(0.0));
        const int dnrm  = mul(kf(0.5), un(KOp::Sqrt, dv(s, kf(d::kPi))));
        const int dsp   = mul(dnrm, g.binary(KOp::Pow, cosh, mul(kf(2.0), s)));

        // Ψ(k) = S·D·(dω/dk)/k, with the small-wave (capillary) suppression exp(−k²l²).
        const int supp  = un(KOp::Exp, un(KOp::Neg, mul(k2, kf(cfg.small_wave.value * cfg.small_wave.value))));
        const int psi   = mx(mul(dv(mul(mul(jsw, dsp), dwk), ksafe), supp), kf(0.0));
        const int amp0  = mul(kf(cfg.amplitude * dk), un(KOp::Sqrt, psi));
        oamp            = g.select(tiny, kf(0.0), amp0);

        // complex Gaussian ξ = Box-Muller(u0,u1) from the B6 hash of the texel (deterministic + portable).
        int cn[3];
        hh::cell_vec3_from3(g, g.cast(ri, DType::I32), g.cast(rj, DType::I32), g.constant(static_cast<double>(cfg.seed), make_shape({1}), DType::I32), ksafe, cn);
        const int u0 = mx(mn(cn[0], kf(1.0)), kf(1e-6));
        const int rr = un(KOp::Sqrt, mul(kf(-2.0), un(KOp::Log, u0)));
        const int ph = mul(kf(2.0 * d::kPi), cn[1]);
        const int xr = mul(rr, un(KOp::Cos, ph));
        const int xi = mul(rr, un(KOp::Sin, ph));
        ore          = mul(oamp, xr);
        oim          = mul(oamp, xi);
    };

    // h0(k) at this texel, and conj(h0(−k)) from the MIRROR texel (N−ni, N−mi) with the negated wavevector.
    int amp0 = 0;
    int hkr  = 0;
    int hki  = 0;
    h0comp(kx, kz, ni, mi, amp0, hkr, hki);
    const int rev_col = g.binary(KOp::BitAnd, sub(ku(static_cast<crd::u32>(n)), ni), ku(static_cast<crd::u32>(n - 1)));
    const int rev_row = g.binary(KOp::BitAnd, sub(ku(static_cast<crd::u32>(n)), mi), ku(static_cast<crd::u32>(n - 1)));
    int ampm = 0;
    int hmr  = 0;
    int hmi  = 0;
    h0comp(un(KOp::Neg, kx), un(KOp::Neg, kz), rev_col, rev_row, ampm, hmr, hmi);

    const int base = mul(p, ku(4));
    g.stmt_buffer_store(h0b, base, hkr);
    g.stmt_buffer_store(h0b, add(base, ku(1)), hki);
    g.stmt_buffer_store(h0b, add(base, ku(2)), hmr);              // conj(h0(−k)).re = h0(−k).re
    g.stmt_buffer_store(h0b, add(base, ku(3)), un(KOp::Neg, hmi)); // conj(h0(−k)).im = −h0(−k).im
    g.stmt_buffer_store(ampb, p, amp0);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// [B16-a-2] TIME EVOLUTION + companion spectra — advance the frozen initial spectrum h0(k) to time t and derive every
// frequency-domain field a choppy, foaming ocean needs, PACKED for ONE batched inverse 2-D FFT. The evolved height spectrum
//   h~(k,t) = h0(k)·e^{iω(k)t} + conj(h0(−k))·e^{−iω(k)t}
// is exactly Hermitian (a-1's mirror-pair storage makes h~(−k) = conj(h~(k)) bit-for-bit), so its inverse FFT is REAL — and
// so is every companion field's. That lets us use Tessendorf's packing: two real Hermitian spectra X,Y ride ONE complex FFT
// as Z = X + i·Y (after the IFFT, real part = X's field, imag part = Y's field). FOUR packed complex spectra carry all EIGHT
// real fields the ocean needs (height, horizontal displacement Dx/Dz, slope ∂h/∂x·∂h/∂z, displacement gradients for the
// foam Jacobian):
//   C0 = h~              + i·(−i kx/|k|)·h~   → (height,          displacement Dx)
//   C1 = (−i kz/|k|)·h~  + i·(i kx)·h~        → (displacement Dz, slope ∂h/∂x)
//   C2 = (i kz)·h~       + i·(kx²/|k|)·h~     → (slope ∂h/∂z,     ∂Dx/∂x  [Jacobian xx])
//   C3 = (kz²/|k|)·h~    + i·(kx kz/|k|)·h~   → (∂Dz/∂z [Jac zz], ∂Dx/∂z  [Jac xz])
// h0 is read in the a-1 CENTERED texel layout (DC at N/2); the packed spectra are written in STANDARD FFT order (DC at 0) so
// build_ocean_ifft consumes them directly. NO 1/N² — the ocean IFFT is the UNNORMALISED inverse transform h(x)=Σ_k h~(k)
// e^{i k·x} (Tessendorf's convention; the physical scale lives in a-1's √Ψ·Δk·amplitude). One thread per texel; grid N²/64.
// Buffers: h0=(0,0) ro packed float4/texel · params=(0,1) ro {t} · spec_re=(0,2) rw · spec_im=(0,3) rw (each 4·N², field f
// occupying [f·N², (f+1)·N²) in FFT-order row-major).
[[nodiscard]] inline KEntry build_ocean_evolve(KGraph& g, const OceanConfig& cfg)
{
    namespace d = detail;

    const int    n     = cfg.n;
    const int    lgn   = d::log2i(n);
    const int    half  = n / 2;
    const int    rc    = n * n;
    const double patch = cfg.patch_length.value;
    const double grav  = cfg.gravity.value;
    const double depth = cfg.depth.value;
    const double dk    = 2.0 * d::kPi / patch;

    const auto kf  = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto un  = [&](KOp op, int a) { return g.unary(op, a); };

    const int h0b  = g.buffer_decl(DType::F32, 0, 0, false);
    const int parb = g.buffer_decl(DType::F32, 0, 1, false);
    const int srb  = g.buffer_decl(DType::F32, 0, 2, true);
    const int sib  = g.buffer_decl(DType::F32, 0, 3, true);

    const int mark = g.kernel_stmt_mark();
    const int p    = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int ni   = g.binary(KOp::BitAnd, p, ku(static_cast<crd::u32>(n - 1)));
    const int mi   = g.binary(KOp::BitAnd, g.binary(KOp::Shr, p, ku(static_cast<crd::u32>(lgn))), ku(static_cast<crd::u32>(n - 1)));
    const int nc   = sub(g.cast(ni, DType::F32), kf(static_cast<double>(half)));
    const int mc   = sub(g.cast(mi, DType::F32), kf(static_cast<double>(half)));
    const int kx   = mul(nc, kf(dk));
    const int kz   = mul(mc, kf(dk));

    // dispersion ω(k) + e^{iωt}, then pack the 8 real fields into 4 Hermitian complex spectra (shared detail:: helpers ⇒ the
    // fused row-IFFT computes BIT-IDENTICAL spectra).
    int km = 0;
    int w  = 0;
    detail::dispersion(g, kx, kz, grav, depth, km, w);
    const int t     = g.buffer_load(parb, ku(0));
    const int wt    = mul(w, t);
    const int c     = un(KOp::Cos, wt);
    const int s     = un(KOp::Sin, wt);
    const int base4 = mul(p, ku(4));
    const int ar    = g.buffer_load(h0b, base4);
    const int ai    = g.buffer_load(h0b, add(base4, ku(1)));
    const int br    = g.buffer_load(h0b, add(base4, ku(2)));
    const int bi    = g.buffer_load(h0b, add(base4, ku(3)));
    int       cr[4];
    int       ci[4];
    detail::evolve_pack(g, kx, kz, km, ar, ai, br, bi, c, s, cr, ci);

    // write to STANDARD FFT order (fftshift the centered texel): idx = ((mi+N/2)&(N−1))·N + ((ni+N/2)&(N−1)).
    const int oc  = g.binary(KOp::BitAnd, add(ni, ku(static_cast<crd::u32>(half))), ku(static_cast<crd::u32>(n - 1)));
    const int orw = g.binary(KOp::BitAnd, add(mi, ku(static_cast<crd::u32>(half))), ku(static_cast<crd::u32>(n - 1)));
    const int op  = add(mul(orw, ku(static_cast<crd::u32>(n))), oc);

    const auto wr = [&](int field, int vr, int vi) {
        const int fo = add(op, ku(static_cast<crd::u32>(field * rc)));
        g.stmt_buffer_store(srb, fo, vr);
        g.stmt_buffer_store(sib, fo, vi);
    };
    wr(0, cr[0], ci[0]);
    wr(1, cr[1], ci[1]);
    wr(2, cr[2], ci[2]);
    wr(3, cr[3], ci[3]);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// [B16-a-2 FUSION — THE CRUSH] Fold build_ocean_evolve INTO the row-IFFT's first global read. A standalone evolve pass would
// write 8·N² floats of packed spectrum that the row IFFT immediately reads back; this ONE kernel instead computes h̃(k,t) and
// the field-f packed value INLINE as it loads each element into shared, then runs the radix-4 row IFFT — eliminating that
// whole spectrum global round-trip AND one dispatch. The batched strided column IFFT + assemble follow (assemble is an
// inherent cross-field gather, so it cannot fuse). This is the fewer-global-round-trips win a cuFFT-based ocean cannot match
// (cuFFT cannot fuse the wave physics into its transform) — the honest crush lever the bare IFFT lacks. Radix-4 ⇒ cols a
// power of FOUR. WorkgroupIndex = field·N + row (field ∈ {0..3} = the 4 packed spectra, row ∈ [0,N)); output is batch-
// contiguous x (field f's row at (f·N+row)·N). Bit-exact vs the un-fused evolve→row-IFFT (shared detail:: helpers + the exact
// radix-4 stages of build_fft1d_radix4). Buffers: h0=(0,0) ro packed float4/texel · params=(0,1) ro {t} · tw_re=(0,2) ·
// tw_im=(0,3) (cols-point twiddles W_N) · out_re=(0,4) rw · out_im=(0,5) rw.
[[nodiscard]] inline KEntry build_ocean_evolve_rowfft(KGraph& g, const OceanConfig& cfg)
{
    namespace d = detail;

    const int    n       = cfg.n; // FFT size = cols (square ocean)
    const int    half    = n / 2;
    const int    p4      = d::log2i(n) / 2; // log4(n); n MUST be a power of 4
    const int    quarter = n / 4;
    const double grav    = cfg.gravity.value;
    const double depth   = cfg.depth.value;
    const double dk      = 2.0 * d::kPi / cfg.patch_length.value;

    const auto kf  = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto uw  = [&](crd::u32 v) { return static_cast<crd::u32>(v); };

    const int h0b   = g.buffer_decl(DType::F32, 0, 0, false);
    const int parb  = g.buffer_decl(DType::F32, 0, 1, false);
    const int tw_re = g.buffer_decl(DType::F32, 0, 2, false);
    const int tw_im = g.buffer_decl(DType::F32, 0, 3, false);
    const int outre = g.buffer_decl(DType::F32, 0, 4, true);
    const int outim = g.buffer_decl(DType::F32, 0, 5, true);
    const int a_re  = g.shared_decl(DType::F32, n);
    const int a_im  = g.shared_decl(DType::F32, n);
    const int b_re  = g.shared_decl(DType::F32, n);
    const int b_im  = g.shared_decl(DType::F32, n);

    const int tid   = g.builtin(KBuiltin::LocalInvocationIndex);          // butterfly 0..N/4-1
    const int wg    = g.builtin(KBuiltin::WorkgroupIndex);                // field·N + row
    const int field = g.binary(KOp::Div, wg, ku(uw(static_cast<crd::u32>(n))));
    const int row   = g.binary(KOp::Mod, wg, ku(uw(static_cast<crd::u32>(n))));
    const int base  = mul(wg, ku(uw(static_cast<crd::u32>(n))));          // batch-contiguous output base
    const auto boff = [&](int idx) { return add(base, idx); };

    // workgroup-uniform: t, the FFT-order row's centered texel-row mi + kz.
    const int t  = g.buffer_load(parb, ku(0));
    const int mi = g.binary(KOp::BitAnd, add(row, ku(uw(static_cast<crd::u32>(half)))), ku(uw(static_cast<crd::u32>(n - 1))));
    const int mc = sub(g.cast(mi, DType::F32), kf(static_cast<double>(half)));
    const int kz = mul(mc, kf(dk));

    const int mark = g.kernel_stmt_mark();
    // STAGE-0 LOAD with EVOLVE FUSED: shared[idx] = the evolved packed field-`field` value for FFT-order (row, col=idx).
    for (int m = 0; m < 4; ++m)
    {
        const int idx  = add(tid, ku(uw(static_cast<crd::u32>(m * quarter))));
        const int ni   = g.binary(KOp::BitAnd, add(idx, ku(uw(static_cast<crd::u32>(half)))), ku(uw(static_cast<crd::u32>(n - 1))));
        const int nc   = sub(g.cast(ni, DType::F32), kf(static_cast<double>(half)));
        const int kx   = mul(nc, kf(dk));
        int       km   = 0;
        int       w    = 0;
        d::dispersion(g, kx, kz, grav, depth, km, w);
        const int cosv = g.unary(KOp::Cos, mul(w, t));
        const int sinv = g.unary(KOp::Sin, mul(w, t));
        const int pc   = add(mul(mi, ku(uw(static_cast<crd::u32>(n)))), ni);
        const int b4   = mul(pc, ku(4));
        const int ar   = g.buffer_load(h0b, b4);
        const int ai   = g.buffer_load(h0b, add(b4, ku(1)));
        const int br   = g.buffer_load(h0b, add(b4, ku(2)));
        const int bi   = g.buffer_load(h0b, add(b4, ku(3)));
        int       cr[4];
        int       ci[4];
        d::evolve_pack(g, kx, kz, km, ar, ai, br, bi, cosv, sinv, cr, ci);
        // pick this workgroup's field (uniform): field<1?C0 : field<2?C1 : field<3?C2 : C3.
        const int re = g.select(g.binary(KOp::CmpLt, field, ku(1)), cr[0],
                                g.select(g.binary(KOp::CmpLt, field, ku(2)), cr[1],
                                         g.select(g.binary(KOp::CmpLt, field, ku(3)), cr[2], cr[3])));
        const int im = g.select(g.binary(KOp::CmpLt, field, ku(1)), ci[0],
                                g.select(g.binary(KOp::CmpLt, field, ku(2)), ci[1],
                                         g.select(g.binary(KOp::CmpLt, field, ku(3)), ci[2], ci[3])));
        g.stmt_shared_store(a_re, idx, re);
        g.stmt_shared_store(a_im, idx, im);
    }
    g.stmt_barrier();

    // radix-4 Stockham INVERSE stages — identical to build_fft1d_radix4 (inverse), writing the row IFFT to out.
    for (int s = 0; s < p4; ++s)
    {
        const bool even = (s % 2) == 0;
        const int  sre  = even ? a_re : b_re;
        const int  sim  = even ? a_im : b_im;
        const int  dre  = even ? b_re : a_re;
        const int  dim  = even ? b_im : a_im;
        const int  rs   = 1 << (2 * s);
        const int  ll   = 1 << (2 * s + 2);
        const int  nl   = n / ll;
        const int  gidx = g.binary(KOp::Div, tid, ku(uw(static_cast<crd::u32>(rs))));
        const int  jidx = g.binary(KOp::Mod, tid, ku(uw(static_cast<crd::u32>(rs))));
        int        ar[4];
        int        ai[4];
        for (int m = 0; m < 4; ++m)
        {
            const int inm = add(add(mul(gidx, ku(uw(static_cast<crd::u32>(rs)))), jidx), ku(uw(static_cast<crd::u32>(m * quarter))));
            const int sr  = g.shared_load(sre, inm);
            const int sii = g.shared_load(sim, inm);
            if (m == 0) { ar[0] = sr; ai[0] = sii; continue; }
            const int twidx = mul(jidx, ku(uw(static_cast<crd::u32>(m * nl))));
            const int wr    = g.buffer_load(tw_re, twidx);
            const int wi    = g.unary(KOp::Neg, g.buffer_load(tw_im, twidx)); // inverse conjugates
            ar[m] = sub(mul(sr, wr), mul(sii, wi));
            ai[m] = add(mul(sr, wi), mul(sii, wr));
        }
        const int t0r = add(ar[0], ar[2]); const int t0i = add(ai[0], ai[2]);
        const int t1r = sub(ar[0], ar[2]); const int t1i = sub(ai[0], ai[2]);
        const int t2r = add(ar[1], ar[3]); const int t2i = add(ai[1], ai[3]);
        const int t3r = sub(ar[1], ar[3]); const int t3i = sub(ai[1], ai[3]);
        int       xr[4];
        int       xi[4];
        xr[0] = add(t0r, t2r); xi[0] = add(t0i, t2i);
        xr[2] = sub(t0r, t2r); xi[2] = sub(t0i, t2i);
        xr[1] = sub(t1r, t3i); xi[1] = add(t1i, t3r); // inverse rotation
        xr[3] = add(t1r, t3i); xi[3] = sub(t1i, t3r);
        for (int k = 0; k < 4; ++k)
        {
            const int outk = add(add(mul(gidx, ku(uw(static_cast<crd::u32>(ll)))), jidx), ku(uw(static_cast<crd::u32>(k * rs))));
            g.stmt_shared_store(dre, outk, xr[k]);
            g.stmt_shared_store(dim, outk, xi[k]);
        }
        g.stmt_barrier();
    }

    const bool odd = (p4 % 2) == 1;
    const int  fre = odd ? b_re : a_re;
    const int  fim = odd ? b_im : a_im;
    for (int m = 0; m < 4; ++m)
    {
        const int idx = add(tid, ku(uw(static_cast<crd::u32>(m * quarter))));
        g.stmt_buffer_store(outre, boff(idx), g.shared_load(fre, idx));
        g.stmt_buffer_store(outim, boff(idx), g.shared_load(fim, idx));
    }

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(quarter);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// [B16-a-2] ASSEMBLE — turn the four inverse-FFT'd complex spatial fields into the renderer-ready ocean surface. After
// build_ocean_ifft the four packed fields decode (real, imag) as:
//   f0 → (height h,        horizontal displacement Dx)
//   f1 → (displacement Dz, slope ∂h/∂x)
//   f2 → (slope ∂h/∂z,     ∂Dx/∂x = Jacobian xx)
//   f3 → (∂Dz/∂z = Jac zz, ∂Dx/∂z = Jac xz)
// Outputs per pixel: a DISPLACEMENT map [λ·Dx, h, λ·Dz, foam] (world offset of the surface point + foam coverage) and a
// NORMAL map [nx, ny, nz, J] (the shading normal + the raw folding Jacobian). Foam forms where the choppy horizontal
// displacement makes the surface fold over — the Jacobian J = (1+λ·Jxx)(1+λ·Jzz) − (λ·Jxz)² goes small/negative at pinched
// crests: coverage = saturate((foam_bias − J)·foam_scale). Buffers: fr=(0,0) ro (4·N² field reals) · fi=(0,1) ro (imags) ·
// disp=(0,2) rw (float4/pixel) · norm=(0,3) rw (float4/pixel). One thread per pixel; grid N²/64.
[[nodiscard]] inline KEntry build_ocean_assemble(KGraph& g, const OceanConfig& cfg)
{
    const int    rc  = cfg.n * cfg.n;
    const double lam = cfg.choppiness;

    const auto kf  = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto dv  = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto mx  = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto mn  = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto neg = [&](int a) { return g.unary(KOp::Neg, a); };
    const auto sat = [&](int a) { return mx(mn(a, kf(1.0)), kf(0.0)); };

    const int frb = g.buffer_decl(DType::F32, 0, 0, false);
    const int fib = g.buffer_decl(DType::F32, 0, 1, false);
    const int dsb = g.buffer_decl(DType::F32, 0, 2, true);
    const int nmb = g.buffer_decl(DType::F32, 0, 3, true);

    const int mark = g.kernel_stmt_mark();
    const int q    = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const auto fld = [&](int buf, int field) { return g.buffer_load(buf, add(q, ku(static_cast<crd::u32>(field * rc)))); };
    const int h   = fld(frb, 0); // f0.re — height
    const int dx  = fld(fib, 0); // f0.im — displacement x
    const int dz  = fld(frb, 1); // f1.re — displacement z
    const int sx  = fld(fib, 1); // f1.im — ∂h/∂x
    const int sz  = fld(frb, 2); // f2.re — ∂h/∂z
    const int jxx = fld(fib, 2); // f2.im — ∂Dx/∂x
    const int jzz = fld(frb, 3); // f3.re — ∂Dz/∂z
    const int jxz = fld(fib, 3); // f3.im — ∂Dx/∂z

    // folding Jacobian → foam coverage
    const int gxx  = add(kf(1.0), mul(kf(lam), jxx));
    const int gzz  = add(kf(1.0), mul(kf(lam), jzz));
    const int gxz  = mul(kf(lam), jxz);
    const int jac  = sub(mul(gxx, gzz), mul(gxz, gxz));
    const int foam = sat(mul(sub(kf(cfg.foam_bias), jac), kf(cfg.foam_scale)));

    // height-field normal n = normalize(−∂h/∂x, 1, −∂h/∂z)
    const int nlen = g.unary(KOp::Sqrt, add(add(mul(sx, sx), mul(sz, sz)), kf(1.0)));
    const int inv  = dv(kf(1.0), nlen);
    const int nx   = mul(neg(sx), inv);
    const int ny   = inv;
    const int nz   = mul(neg(sz), inv);

    const int q4 = mul(q, ku(4));
    g.stmt_buffer_store(dsb, q4, mul(kf(lam), dx));
    g.stmt_buffer_store(dsb, add(q4, ku(1)), h);
    g.stmt_buffer_store(dsb, add(q4, ku(2)), mul(kf(lam), dz));
    g.stmt_buffer_store(dsb, add(q4, ku(3)), foam);
    g.stmt_buffer_store(nmb, q4, nx);
    g.stmt_buffer_store(nmb, add(q4, ku(1)), ny);
    g.stmt_buffer_store(nmb, add(q4, ku(2)), nz);
    g.stmt_buffer_store(nmb, add(q4, ku(3)), jac);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// [B16-a-3] TEMPORAL FOAM ACCUMULATION — foam is not instantaneous: whitecaps linger after a wave crest pinches, then fade.
// Each frame foam PERSISTS (× foam_decay) and is re-INJECTED where the current Jacobian coverage is higher:
//   foam(t) = max(foam(t−1)·decay, inject(t)),   inject(t) = the assemble kernel's Jacobian coverage (the disp.w channel).
// One thread per texel; grid N²/64. Buffers: prev=(0,0) ro (last frame's foam) · inject=(0,1) ro (this frame's coverage) ·
// out=(0,2) rw (next frame's foam). Ping-pong prev/out across frames.
[[nodiscard]] inline KEntry build_ocean_foam_accumulate(KGraph& g, const OceanConfig& cfg)
{
    const auto kf = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };

    const int prevb = g.buffer_decl(DType::F32, 0, 0, false);
    const int injb  = g.buffer_decl(DType::F32, 0, 1, false);
    const int outb  = g.buffer_decl(DType::F32, 0, 2, true);

    const int mark = g.kernel_stmt_mark();
    const int q    = g.binary(KOp::Add, g.binary(KOp::Mul, g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));
    const int decayed = g.binary(KOp::Mul, g.buffer_load(prevb, q), kf(cfg.foam_decay));
    g.stmt_buffer_store(outb, q, g.binary(KOp::Max, decayed, g.buffer_load(injb, q)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::ocean
