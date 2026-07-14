#pragma once

// ckir_atmosphere.hpp — D-007 B15-a: physically-based sky + atmosphere (Hillaire 2020, "A Scalable and Production Ready Sky
// and Atmosphere Rendering Technique" — the UE5 Sky-Atmosphere method), authored in CKIR. A planetary atmosphere is modelled
// as three participating media — Rayleigh (air molecules, ∝exp(−h/8km), wavelength-dependent, no absorption), Mie (aerosols,
// ∝exp(−h/1.2km), grey, forward-scattering), and ozone (a tent absorber centred at 25km) — and the sky is rendered from a
// small set of PRECOMPUTED LUTs: [1] TRANSMITTANCE T(r,μ) = the fraction of light surviving the path from altitude r in
// direction μ out to the top of the atmosphere; [2] MULTIPLE-SCATTERING Ψ (Hillaire's cheap isotropic 2nd-order-onward term);
// [3] SKY-VIEW (the final per-direction sky radiance); [4] AERIAL-PERSPECTIVE froxels (in-scatter + transmittance along the
// camera frustum, composited into B12-d fog). Every LUT is a statement-tier CKIR COMPUTE kernel — pure analytic scattering
// integrals, so the WHOLE thing is buildable + bit-exact-verifiable now (no scene ray-tracing leaf, unlike ReSTIR/DDGI).
//
// FP32 `precise` ⇒ bit-matches the CPU oracle (the only ULP sources are exp/sqrt/pow, all IEEE-faithful). Units: kilometres.
// Constants are the Hillaire/Bruneton Earth reference (RGB extinction in 1/km). This file is SCALARIZED (the compute emitters
// are scalar; per-channel R/G/B carried as separate F32 nodes), the same discipline as ckir_ddgi.hpp / ckir_restir.hpp.

#include <crd/kir/ckir.hpp>

namespace crd::kir::atmos
{

// The Earth-reference medium (Hillaire 2020 sample values; RGB scattering/absorption coefficients in 1/km, heights in km).
struct AtmosphereConfig
{
    double ground_radius = 6360.0; // Rg — planet surface radius (km)
    double top_radius    = 6460.0; // Rt — top of the atmosphere (km); 100 km shell

    double rayleigh_scatter_r = 0.005802; // β_R (1/km) — Rayleigh scattering == extinction (no absorption), per channel
    double rayleigh_scatter_g = 0.013558;
    double rayleigh_scatter_b = 0.033100;
    double rayleigh_height     = 8.0; // H_R — Rayleigh density scale height (km)

    double mie_scatter    = 0.003996; // β_M scattering (1/km, grey)
    double mie_extinction = 0.004440; // β_M extinction (scattering + absorption); absorption = ext − scatter
    double mie_height      = 1.2;     // H_M — Mie density scale height (km)
    double mie_g           = 0.8;     // Mie asymmetry (Cornette-Shanks)

    double ozone_absorb_r = 0.000650; // β_O ozone absorption (1/km, no scattering), per channel
    double ozone_absorb_g = 0.001881;
    double ozone_absorb_b = 0.000085;
    double ozone_center     = 25.0; // ozone tent centre (km)
    double ozone_half_width = 15.0; // ozone tent half-width (km); density = max(0, 1 − |h − centre|/half_width)

    int tlut_w        = 256; // transmittance LUT width  (μ axis)
    int tlut_h        = 64;  // transmittance LUT height (altitude axis)
    int transmittance_steps = 40; // ray-march samples for the transmittance integral (Hillaire uses 40)

    int    mslut_res    = 32;  // multiple-scattering LUT resolution (mslut_res × mslut_res, μ_sun × altitude — smooth ⇒ small)
    int    ms_dirs      = 8;   // sphere directions for the isotropic multi-scatter integral (Fibonacci-distributed)
    int    ms_steps     = 8;   // ray-march samples per direction
    double sun_illuminance = 1.0; // the LUT stores transfer per unit sun irradiance

    int    skyview_w    = 192; // sky-view LUT width  (azimuth relative to the sun)
    int    skyview_h    = 108; // sky-view LUT height (view zenith)
    int    sky_steps    = 30;  // ray-march samples along the view ray
    double camera_height = 0.5;   // camera altitude above the ground (km) baked into the sky-view + aerial LUTs
    double sky_sun_cos   = 0.35;  // cos of the sun zenith angle baked into the sky-view LUT (a time-of-day snapshot)

    [[nodiscard]] bool valid() const noexcept { return top_radius > ground_radius && transmittance_steps >= 1; }
};

// Build the TRANSMITTANCE LUT kernel — one thread per texel (μ, altitude). The Bruneton parameterisation maps the unit square
// (u = μ axis, v = altitude axis) to a physical (r, μ, distance-to-top d), then ray-marches the extinction integral
// τ = ∫₀ᵈ σ_e(h(t)) dt and stores T = exp(−τ) per channel. σ_e = β_R·exp(−h/H_R) + β_M_ext·exp(−h/H_M) + β_O·tent(h).
// Buffer: 0 = out (F32, W·H·3 = RGB transmittance). Dispatch W·H/64 workgroups.
[[nodiscard]] inline KEntry build_atmos_transmittance(KGraph& g, const AtmosphereConfig& cfg)
{
    const auto kf   = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto fmin = [&](int a, int b) { return g.binary(KOp::Min, a, b); };

    const double rg = cfg.ground_radius;
    const double rt = cfg.top_radius;
    const double h2 = rt * rt - rg * rg; // H² (compile-time, no sqrt); H itself is a single graph Sqrt node below
    const int    n  = cfg.transmittance_steps;
    const double w  = static_cast<double>(cfg.tlut_w);
    const double ht = static_cast<double>(cfg.tlut_h);

    const int out_b = g.buffer_decl(DType::F32, 0, 0, true);
    const int p     = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark   = g.kernel_stmt_mark();
    const int h_node = g.unary(KOp::Sqrt, kf(h2)); // H = sqrt(Rt²−Rg²), the Bruneton mapping's length scale
    // texel (ix, iy) from the linear id via FLOAT floor (exact for ids < 2²⁴): iy = floor(p/W), ix = p − iy·W.
    const int fp   = g.cast(p, DType::F32);
    const int iy_f = g.unary(KOp::Floor, divv(fp, kf(w)));
    const int ix_f = sub(fp, mul(iy_f, kf(w)));
    const int uu   = divv(add(ix_f, kf(0.5)), kf(w));  // x_mu ∈ (0,1)
    const int vv   = divv(add(iy_f, kf(0.5)), kf(ht)); // x_r  ∈ (0,1)

    // Bruneton inverse map, reformulated CANCELLATION-FREE (km-scale radii square to ~4e7 where f32 ulp is ~4, so the naive
    // μ = (H²−ρ²−d²)/(2rd) loses precision and the long horizon march amplifies it). Key identity: ρ = H·v ⇒ ρ² = H²·v² and
    // H²−ρ² = H²(1−v²) — computed from the baked exact H² and the O(1) factor (1−v²), never as a difference of large squares.
    // Likewise d_max = ρ+H = H(v+1). Only d² remains a large square, and it cancels against H²(1−v²) only where μ≈0 (genuine).
    const int v2    = mul(vv, vv);
    const int rho2  = mul(kf(h2), v2);                                  // ρ² = H²·v²
    const int r     = g.unary(KOp::Sqrt, add(rho2, kf(rg * rg)));       // r = sqrt(ρ²+Rg²)
    const int d_min = sub(kf(rt), r);
    const int d_max = mul(h_node, add(kf(1.0), vv));                    // ρ+H = H(v+1)
    const int d     = fmax(add(d_min, mul(uu, sub(d_max, d_min))), kf(1e-4)); // total path to the top of the atmosphere
    const int h2m   = mul(kf(h2), sub(kf(1.0), v2));                    // H²−ρ² = H²(1−v²), cancellation-free
    const int mu    = fmax(fmin(divv(sub(h2m, mul(d, d)), mul(mul(kf(2.0), r), d)), kf(1.0)), kf(-1.0));

    const int dt      = divv(d, kf(static_cast<double>(n)));
    const int two_r_mu = mul(mul(kf(2.0), r), mu);
    int       tau_r   = kf(0.0);
    int       tau_g   = kf(0.0);
    int       tau_b   = kf(0.0);
    for (int i = 0; i < n; ++i)
    {
        const int t     = mul(kf(static_cast<double>(i) + 0.5), dt);                      // sample at segment centre
        const int r_t   = g.unary(KOp::Sqrt, add(add(mul(r, r), mul(two_r_mu, t)), mul(t, t))); // radius at the sample
        const int alt   = fmax(sub(r_t, kf(rg)), kf(0.0));                                // altitude above ground
        const int rho_r = g.unary(KOp::Exp, g.unary(KOp::Neg, divv(alt, kf(cfg.rayleigh_height)))); // Rayleigh density
        const int rho_m = g.unary(KOp::Exp, g.unary(KOp::Neg, divv(alt, kf(cfg.mie_height))));       // Mie density
        const int rho_o = fmax(sub(kf(1.0), divv(g.unary(KOp::Abs, sub(alt, kf(cfg.ozone_center))), kf(cfg.ozone_half_width))), kf(0.0));
        const int se_r  = add(add(mul(kf(cfg.rayleigh_scatter_r), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_r), rho_o));
        const int se_g  = add(add(mul(kf(cfg.rayleigh_scatter_g), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_g), rho_o));
        const int se_b  = add(add(mul(kf(cfg.rayleigh_scatter_b), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_b), rho_o));
        tau_r           = add(tau_r, mul(se_r, dt));
        tau_g           = add(tau_g, mul(se_g, dt));
        tau_b           = add(tau_b, mul(se_b, dt));
    }
    const int op3 = mul(p, ku(3));
    g.stmt_buffer_store(out_b, op3, g.unary(KOp::Exp, g.unary(KOp::Neg, tau_r)));
    g.stmt_buffer_store(out_b, add(op3, ku(1)), g.unary(KOp::Exp, g.unary(KOp::Neg, tau_g)));
    g.stmt_buffer_store(out_b, add(op3, ku(2)), g.unary(KOp::Exp, g.unary(KOp::Neg, tau_b)));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the MULTIPLE-SCATTERING LUT kernel (Hillaire 2020 §5.3) — the cheap isotropic 2nd-order-and-beyond scattering that fills
// the sky's shadowed regions (without it a physically-based sky is far too dark). One thread per texel (μ_sun, altitude). At the
// texel's point we integrate SINGLE scattering over a sphere of `ms_dirs` directions (each a short ray-march that samples the
// TRANSMITTANCE LUT for the sun-transmittance at every step), forming two sphere-averaged quantities: L₂ (the 2nd-order radiance)
// and f_ms (the fraction a uniform unit radiance gets re-scattered). The full isotropic series then sums in closed form:
// Ψ = L₂·(1 + f_ms + f_ms² + …) = L₂/(1 − f_ms). Buffers: 0 = transmittance LUT (F32 tlut_w·tlut_h·3, IN), 1 = out Ψ (F32
// res·res·3). Dispatch res·res/64 workgroups (res·res must be a multiple of 64). ms_dirs directions are Fibonacci-distributed.
[[nodiscard]] inline KEntry build_atmos_multiscatter(KGraph& g, const AtmosphereConfig& cfg)
{
    const auto kf   = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto fmin = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto neg  = [&](int a) { return g.unary(KOp::Neg, a); };
    const auto sqrt = [&](int a) { return g.unary(KOp::Sqrt, a); };
    const auto expn = [&](int a) { return g.unary(KOp::Exp, neg(a)); };

    const double rg   = cfg.ground_radius;
    const double rt   = cfg.top_radius;
    const double h2   = rt * rt - rg * rg;
    const int    res  = cfg.mslut_res;
    const int    nd   = cfg.ms_dirs;
    const int    nst  = cfg.ms_steps;
    const int    tw   = cfg.tlut_w;
    const int    th   = cfg.tlut_h;
    const double inv4pi = 0.25 * 0.3183098861837907; // 1/(4π)

    const int tlut_b = g.buffer_decl(DType::F32, 0, 0, false); // transmittance LUT (input)
    const int out_b  = g.buffer_decl(DType::F32, 0, 1, true);
    const int p      = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark   = g.kernel_stmt_mark();
    const int h_node = sqrt(kf(h2));

    // bilinear sample of the transmittance LUT via the FORWARD Bruneton mapping (r,μ)→(u,v). Returns RGB into tr/tg/tb.
    const auto sample_tlut = [&](int rs, int mus, int& tr, int& tg, int& tb) {
        const int rho   = sqrt(fmax(sub(mul(rs, rs), kf(rg * rg)), kf(0.0)));      // ρ = sqrt(r²−Rg²)
        const int x_r   = fmin(fmax(divv(rho, h_node), kf(0.0)), kf(1.0));
        const int disc  = fmax(add(mul(mul(rs, rs), sub(mul(mus, mus), kf(1.0))), kf(rt * rt)), kf(0.0)); // r²(μ²−1)+Rt²
        const int dd    = fmax(add(neg(mul(rs, mus)), sqrt(disc)), kf(0.0));       // distance to the top boundary
        const int d_min = sub(kf(rt), rs);
        const int d_max = add(rho, h_node);
        const int x_mu  = fmin(fmax(divv(sub(dd, d_min), fmax(sub(d_max, d_min), kf(1e-4))), kf(0.0)), kf(1.0));
        const int fx    = mul(x_mu, kf(static_cast<double>(tw - 1)));
        const int fy    = mul(x_r, kf(static_cast<double>(th - 1)));
        const int x0f   = g.unary(KOp::Floor, fx);
        const int y0f   = g.unary(KOp::Floor, fy);
        const int txf   = sub(fx, x0f);
        const int tyf   = sub(fy, y0f);
        const int x0    = g.cast(x0f, DType::U32);
        const int y0    = g.cast(y0f, DType::U32);
        const int x1    = g.cast(fmin(add(x0f, kf(1.0)), kf(static_cast<double>(tw - 1))), DType::U32);
        const int y1    = g.cast(fmin(add(y0f, kf(1.0)), kf(static_cast<double>(th - 1))), DType::U32);
        g.stmt_materialize(x0); // freeze the shared bilinear coords (the 4 taps × 3 channels reference them 12× otherwise)
        g.stmt_materialize(y0);
        g.stmt_materialize(x1);
        g.stmt_materialize(y1);
        g.stmt_materialize(txf);
        g.stmt_materialize(tyf);
        const auto idx  = [&](int xu, int yu, crd::u32 c) { return add(mul(add(mul(yu, ku(static_cast<crd::u32>(tw))), xu), ku(3)), ku(c)); };
        const auto bl   = [&](crd::u32 c) {
            const int a00 = g.buffer_load(tlut_b, idx(x0, y0, c));
            const int a10 = g.buffer_load(tlut_b, idx(x1, y0, c));
            const int a01 = g.buffer_load(tlut_b, idx(x0, y1, c));
            const int a11 = g.buffer_load(tlut_b, idx(x1, y1, c));
            const int a0  = add(a00, mul(txf, sub(a10, a00)));
            const int a1  = add(a01, mul(txf, sub(a11, a01)));
            return add(a0, mul(tyf, sub(a1, a0)));
        };
        tr = bl(0);
        tg = bl(1);
        tb = bl(2);
    };

    // texel (ix,iy) → (μ_sun ∈ [−1,1], r_view ∈ [Rg,Rt]) via a simple linear map (the multiscatter LUT is low-res + smooth).
    const int fp    = g.cast(p, DType::F32);
    const int iy_f  = g.unary(KOp::Floor, divv(fp, kf(static_cast<double>(res))));
    const int ix_f  = sub(fp, mul(iy_f, kf(static_cast<double>(res))));
    const int mus   = sub(mul(divv(add(ix_f, kf(0.5)), kf(static_cast<double>(res))), kf(2.0)), kf(1.0)); // μ_sun
    const int rv    = add(kf(rg), mul(divv(add(iy_f, kf(0.5)), kf(static_cast<double>(res))), kf(rt - rg))); // r_view
    const int sun_x = sqrt(fmax(sub(kf(1.0), mul(mus, mus)), kf(0.0))); // sun dir = (sun_x, 0, μ_sun)

    const double golden = 2.399963229728653; // π(3−√5)
    int          lsum_r = kf(0.0);
    int          lsum_g = kf(0.0);
    int          lsum_b = kf(0.0);
    int          fsum_r = kf(0.0);
    int          fsum_g = kf(0.0);
    int          fsum_b = kf(0.0);
    for (int i = 0; i < nd; ++i)
    {
        const double yv = 1.0 - (2.0 * static_cast<double>(i) + 1.0) / static_cast<double>(nd); // Fibonacci sphere: z
        const int    rr = sqrt(kf(1.0 - yv * yv));
        const int    dx = mul(rr, g.unary(KOp::Cos, kf(golden * static_cast<double>(i))));
        const int    dz = mul(rr, g.unary(KOp::Sin, kf(golden * static_cast<double>(i))));
        // (the ω y-component folds away: r(t)² uses dx²+dy²=1−dz², and the sun lies in the x–z plane so x·S is dy-independent.)

        // distance to the boundary along ω from P=(0,0,r_view): r(t)² = r_view² + 2·r_view·dz·t + t².
        const int rdz     = mul(rv, dz);
        const int disc_t  = fmax(add(mul(mul(rv, rv), sub(mul(dz, dz), kf(1.0))), kf(rt * rt)), kf(0.0)); // to top
        const int t_top   = add(neg(rdz), sqrt(disc_t));
        const int disc_g  = add(mul(mul(rv, rv), sub(mul(dz, dz), kf(1.0))), kf(rg * rg));                // to ground
        const int t_gnear = sub(neg(rdz), sqrt(fmax(disc_g, kf(0.0))));
        const int hit_gnd = g.binary(KOp::CmpGt, mul(g.select(g.binary(KOp::CmpGt, disc_g, kf(0.0)), kf(1.0), kf(-1.0)), t_gnear), kf(0.0));
        const int t_max   = fmax(g.select(hit_gnd, t_gnear, t_top), kf(1e-3));
        const int dt      = divv(t_max, kf(static_cast<double>(nst)));

        int l_r = kf(0.0);
        int l_g = kf(0.0);
        int l_b = kf(0.0);
        int f_r = kf(0.0);
        int f_g = kf(0.0);
        int f_b = kf(0.0);
        int tau_r = kf(0.0);
        int tau_g = kf(0.0);
        int tau_b = kf(0.0);
        for (int j = 0; j < nst; ++j)
        {
            const int t     = mul(kf(static_cast<double>(j) + 0.5), dt);
            const int r_x   = sqrt(add(add(mul(rv, rv), mul(mul(kf(2.0), rdz), t)), mul(t, t))); // radius at the sample
            g.stmt_materialize(r_x); // freeze into a register: r_x feeds the bilinear LUT sample repeatedly (breaks the re-walk)
            const int xz    = add(rv, mul(t, dz));                                               // z-coordinate of the sample
            const int alt   = fmax(sub(r_x, kf(rg)), kf(0.0));
            const int mu_s  = divv(add(mul(mul(t, dx), sun_x), mul(xz, mus)), r_x);              // sun-cos at the sample
            g.stmt_materialize(mu_s);
            int       ts_r = 0;
            int       ts_g = 0;
            int       ts_b = 0;
            sample_tlut(r_x, mu_s, ts_r, ts_g, ts_b);                                            // sun transmittance
            const int rho_r = expn(divv(alt, kf(cfg.rayleigh_height)));
            const int rho_m = expn(divv(alt, kf(cfg.mie_height)));
            const int rho_o = fmax(sub(kf(1.0), divv(g.unary(KOp::Abs, sub(alt, kf(cfg.ozone_center))), kf(cfg.ozone_half_width))), kf(0.0));
            // per-channel scattering σ_s (Rayleigh + Mie, no ozone) and extinction σ_e (+ Mie absorption + ozone).
            const int ss_r = add(mul(kf(cfg.rayleigh_scatter_r), rho_r), mul(kf(cfg.mie_scatter), rho_m));
            const int ss_g = add(mul(kf(cfg.rayleigh_scatter_g), rho_r), mul(kf(cfg.mie_scatter), rho_m));
            const int ss_b = add(mul(kf(cfg.rayleigh_scatter_b), rho_r), mul(kf(cfg.mie_scatter), rho_m));
            const int se_r = add(add(mul(kf(cfg.rayleigh_scatter_r), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_r), rho_o));
            const int se_g = add(add(mul(kf(cfg.rayleigh_scatter_g), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_g), rho_o));
            const int se_b = add(add(mul(kf(cfg.rayleigh_scatter_b), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_b), rho_o));
            const int tp_r = expn(tau_r); // transmittance from P to the sample
            const int tp_g = expn(tau_g);
            const int tp_b = expn(tau_b);
            // single-scatter from the sun (isotropic phase 1/4π · sun illuminance), transported back to P.
            const int k_ill = kf(inv4pi * cfg.sun_illuminance);
            l_r = add(l_r, mul(mul(tp_r, mul(mul(ss_r, ts_r), k_ill)), dt));
            l_g = add(l_g, mul(mul(tp_g, mul(mul(ss_g, ts_g), k_ill)), dt));
            l_b = add(l_b, mul(mul(tp_b, mul(mul(ss_b, ts_b), k_ill)), dt));
            f_r = add(f_r, mul(mul(tp_r, ss_r), dt)); // MultiScatAs1: transfer for a uniform unit radiance
            f_g = add(f_g, mul(mul(tp_g, ss_g), dt));
            f_b = add(f_b, mul(mul(tp_b, ss_b), dt));
            tau_r = add(tau_r, mul(se_r, dt));
            tau_g = add(tau_g, mul(se_g, dt));
            tau_b = add(tau_b, mul(se_b, dt));
        }
        lsum_r = add(lsum_r, l_r);
        lsum_g = add(lsum_g, l_g);
        lsum_b = add(lsum_b, l_b);
        fsum_r = add(fsum_r, f_r);
        fsum_g = add(fsum_g, f_g);
        fsum_b = add(fsum_b, f_b);
    }
    // sphere average (uniform sampling: solid-angle 4π/N × isotropic phase 1/4π = 1/N), then the closed-form isotropic series.
    const int invn = kf(1.0 / static_cast<double>(nd));
    const auto psi = [&](int lsum, int fsum) {
        const int l2  = mul(lsum, invn);
        const int fms = fmin(mul(fsum, invn), kf(0.999)); // clamp f_ms < 1 for the series
        return divv(l2, sub(kf(1.0), fms));
    };
    const int op3 = mul(p, ku(3));
    g.stmt_buffer_store(out_b, op3, psi(lsum_r, fsum_r));
    g.stmt_buffer_store(out_b, add(op3, ku(1)), psi(lsum_g, fsum_g));
    g.stmt_buffer_store(out_b, add(op3, ku(2)), psi(lsum_b, fsum_b));

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// Build the SKY-VIEW LUT kernel (Hillaire 2020 §5.4) — the actual per-direction sky radiance seen from the camera, the LUT the
// sky pass reads. One thread per texel (azimuth-relative-to-sun, view zenith). It ray-marches the view ray and at each step adds
// SINGLE scattering (Rayleigh + Cornette-Shanks Mie phase, weighted by the sun transmittance from the TRANSMITTANCE LUT) plus
// MULTIPLE scattering (isotropic, from the MULTISCATTER LUT), attenuated by the transmittance back to the camera. Camera height
// + sun direction are baked (a time-of-day snapshot). Buffers: 0 = transmittance LUT (IN), 1 = multiscatter LUT (IN), 2 = out
// sky radiance (F32 skyview_w·skyview_h·3). Dispatch skyview_w·skyview_h/64 workgroups.
[[nodiscard]] inline KEntry build_atmos_skyview(KGraph& g, const AtmosphereConfig& cfg)
{
    const auto kf   = [&](double v) { return g.constant(v, make_shape({1}), DType::F32); };
    const auto ku   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), make_shape({1}), DType::U32); };
    const auto add  = [&](int a, int b) { return g.binary(KOp::Add, a, b); };
    const auto sub  = [&](int a, int b) { return g.binary(KOp::Sub, a, b); };
    const auto mul  = [&](int a, int b) { return g.binary(KOp::Mul, a, b); };
    const auto divv = [&](int a, int b) { return g.binary(KOp::Div, a, b); };
    const auto fmax = [&](int a, int b) { return g.binary(KOp::Max, a, b); };
    const auto fmin = [&](int a, int b) { return g.binary(KOp::Min, a, b); };
    const auto neg  = [&](int a) { return g.unary(KOp::Neg, a); };
    const auto sqrt = [&](int a) { return g.unary(KOp::Sqrt, a); };
    const auto expn = [&](int a) { return g.unary(KOp::Exp, neg(a)); };

    const double rg   = cfg.ground_radius;
    const double rt   = cfg.top_radius;
    const double h2   = rt * rt - rg * rg;
    const int    w    = cfg.skyview_w;
    const int    hh   = cfg.skyview_h;
    const int    nst  = cfg.sky_steps;
    const int    tw   = cfg.tlut_w;
    const int    thh  = cfg.tlut_h;
    const int    res  = cfg.mslut_res;
    const double musn = cfg.sky_sun_cos;
    const double pi   = 3.14159265358979323846;
    const double kray = 3.0 / (16.0 * pi);
    const double kmie = 3.0 / (8.0 * pi);
    const double gm   = cfg.mie_g;

    const int tlut_b = g.buffer_decl(DType::F32, 0, 0, false);
    const int msl_b  = g.buffer_decl(DType::F32, 0, 1, false);
    const int out_b  = g.buffer_decl(DType::F32, 0, 2, true);
    const int p      = add(mul(g.builtin(KBuiltin::WorkgroupIndex), ku(64)), g.builtin(KBuiltin::LocalInvocationIndex));

    const int mark   = g.kernel_stmt_mark();
    const int h_node = sqrt(kf(h2));

    // generic bilinear tap of an (W×H×3) buffer at continuous texel coords (fx∈[0,W−1], fy∈[0,H−1]).
    const auto bilinear3 = [&](int buf, int wi, int hi, int fx, int fy, int& or_, int& og, int& ob) {
        const int x0f = g.unary(KOp::Floor, fx);
        const int y0f = g.unary(KOp::Floor, fy);
        const int txf = sub(fx, x0f);
        const int tyf = sub(fy, y0f);
        const int x0  = g.cast(x0f, DType::U32);
        const int y0  = g.cast(y0f, DType::U32);
        const int x1  = g.cast(fmin(add(x0f, kf(1.0)), kf(static_cast<double>(wi - 1))), DType::U32);
        const int y1  = g.cast(fmin(add(y0f, kf(1.0)), kf(static_cast<double>(hi - 1))), DType::U32);
        // freeze the shared bilinear coords into registers — the 4 taps × 3 channels reference them 12× (no per-channel recompute).
        g.stmt_materialize(x0);
        g.stmt_materialize(y0);
        g.stmt_materialize(x1);
        g.stmt_materialize(y1);
        g.stmt_materialize(txf);
        g.stmt_materialize(tyf);
        const auto idx = [&](int xu, int yu, crd::u32 c) { return add(mul(add(mul(yu, ku(static_cast<crd::u32>(wi))), xu), ku(3)), ku(c)); };
        const auto bl  = [&](crd::u32 c) {
            const int a00 = g.buffer_load(buf, idx(x0, y0, c));
            const int a10 = g.buffer_load(buf, idx(x1, y0, c));
            const int a01 = g.buffer_load(buf, idx(x0, y1, c));
            const int a11 = g.buffer_load(buf, idx(x1, y1, c));
            const int a0  = add(a00, mul(txf, sub(a10, a00)));
            const int a1  = add(a01, mul(txf, sub(a11, a01)));
            return add(a0, mul(tyf, sub(a1, a0)));
        };
        or_ = bl(0);
        og  = bl(1);
        ob  = bl(2);
    };
    // transmittance LUT sample via the Bruneton forward map (r,μ)→(u,v).
    const auto sample_tlut = [&](int rs, int mus, int& tr, int& tg, int& tb) {
        const int rho   = sqrt(fmax(sub(mul(rs, rs), kf(rg * rg)), kf(0.0)));
        const int x_r   = fmin(fmax(divv(rho, h_node), kf(0.0)), kf(1.0));
        const int disc  = fmax(add(mul(mul(rs, rs), sub(mul(mus, mus), kf(1.0))), kf(rt * rt)), kf(0.0));
        const int dd    = fmax(add(neg(mul(rs, mus)), sqrt(disc)), kf(0.0));
        const int d_min = sub(kf(rt), rs);
        const int d_max = add(rho, h_node);
        const int x_mu  = fmin(fmax(divv(sub(dd, d_min), fmax(sub(d_max, d_min), kf(1e-4))), kf(0.0)), kf(1.0));
        bilinear3(tlut_b, tw, thh, mul(x_mu, kf(static_cast<double>(tw - 1))), mul(x_r, kf(static_cast<double>(thh - 1))), tr, tg, tb);
    };
    // multiscatter LUT sample via its linear (μ_sun,altitude)→(u,v) map.
    const auto sample_msl = [&](int rs, int mus, int& mr, int& mg, int& mb) {
        const int x_mu = fmin(fmax(mul(add(mus, kf(1.0)), kf(0.5)), kf(0.0)), kf(1.0));
        const int x_r  = fmin(fmax(divv(sub(rs, kf(rg)), kf(rt - rg)), kf(0.0)), kf(1.0));
        bilinear3(msl_b, res, res, mul(x_mu, kf(static_cast<double>(res - 1))), mul(x_r, kf(static_cast<double>(res - 1))), mr, mg, mb);
    };

    // texel → (azimuth φ relative to sun ∈ [−π,π], view zenith θ ∈ [0,π]).
    const int fp   = g.cast(p, DType::F32);
    const int iy_f = g.unary(KOp::Floor, divv(fp, kf(static_cast<double>(w))));
    const int ix_f = sub(fp, mul(iy_f, kf(static_cast<double>(w))));
    const int uu   = divv(add(ix_f, kf(0.5)), kf(static_cast<double>(w)));
    const int vv   = divv(add(iy_f, kf(0.5)), kf(static_cast<double>(hh)));
    const int phi  = sub(mul(uu, kf(2.0 * pi)), kf(pi));
    const int thet = mul(vv, kf(pi));
    const int cosp = g.unary(KOp::Cos, phi); // azimuth enters only via cos φ (the sun is in the x–z plane ⇒ V_y drops out)
    const int sint = g.unary(KOp::Sin, thet);
    const int cost = g.unary(KOp::Cos, thet);
    const int vx   = mul(sint, cosp);
    const int vz   = cost;
    const int sunx = sqrt(kf(1.0 - musn * musn)); // sin(sun zenith) = sqrt(1−μ_sun²) (μ_sun∈[−1,1] ⇒ arg ≥ 0)

    const int rcam = kf(rg + cfg.camera_height);
    // cos angle between the view ray and the sun (for the phase functions): dot(V, S), S=(sunx,0,μ_sun).
    const int csv  = add(mul(vx, sunx), mul(vz, kf(musn)));
    const int csv2 = mul(csv, csv);
    const int p_r  = mul(kf(kray), add(kf(1.0), csv2));                                    // Rayleigh phase 3/(16π)(1+c²)
    const int cs_b = sub(kf(1.0 + gm * gm), mul(kf(2.0 * gm), csv));                       // 1+g²−2g·c
    const int cs_d = mul(kf(2.0 + gm * gm), g.binary(KOp::Pow, cs_b, kf(1.5)));            // (2+g²)(1+g²−2gc)^1.5
    const int p_m  = divv(mul(mul(kf(kmie), kf(1.0 - gm * gm)), add(kf(1.0), csv2)), cs_d); // Cornette-Shanks Mie phase

    const int rdz    = mul(rcam, vz);
    const int disc_t = fmax(add(mul(mul(rcam, rcam), sub(mul(vz, vz), kf(1.0))), kf(rt * rt)), kf(0.0));
    const int t_top  = add(neg(rdz), sqrt(disc_t));
    const int disc_g = add(mul(mul(rcam, rcam), sub(mul(vz, vz), kf(1.0))), kf(rg * rg));
    const int t_gn   = sub(neg(rdz), sqrt(fmax(disc_g, kf(0.0))));
    const int hitg   = g.binary(KOp::CmpGt, mul(g.select(g.binary(KOp::CmpGt, disc_g, kf(0.0)), kf(1.0), kf(-1.0)), t_gn), kf(0.0));
    const int t_max  = fmax(g.select(hitg, t_gn, t_top), kf(1e-3));
    const int dt     = divv(t_max, kf(static_cast<double>(nst)));

    int l_r = kf(0.0);
    int l_g = kf(0.0);
    int l_b = kf(0.0);
    int tau_r = kf(0.0);
    int tau_g = kf(0.0);
    int tau_b = kf(0.0);
    for (int j = 0; j < nst; ++j)
    {
        const int t    = mul(kf(static_cast<double>(j) + 0.5), dt);
        const int r_x  = sqrt(add(add(mul(rcam, rcam), mul(mul(kf(2.0), rdz), t)), mul(t, t)));
        g.stmt_materialize(r_x); // freeze into a register: r_x feeds two nested bilinear LUT samples (breaks the re-walk fan-out)
        const int xz   = add(rcam, mul(t, vz));
        const int alt  = fmax(sub(r_x, kf(rg)), kf(0.0));
        const int mu_s = divv(add(mul(mul(t, vx), sunx), mul(xz, kf(musn))), r_x);
        g.stmt_materialize(mu_s);
        int ts_r = 0;
        int ts_g = 0;
        int ts_b = 0;
        sample_tlut(r_x, mu_s, ts_r, ts_g, ts_b);
        int ms_r = 0;
        int ms_g = 0;
        int ms_b = 0;
        sample_msl(r_x, mu_s, ms_r, ms_g, ms_b);
        const int rho_r = expn(divv(alt, kf(cfg.rayleigh_height)));
        const int rho_m = expn(divv(alt, kf(cfg.mie_height)));
        const int rho_o = fmax(sub(kf(1.0), divv(g.unary(KOp::Abs, sub(alt, kf(cfg.ozone_center))), kf(cfg.ozone_half_width))), kf(0.0));
        const int rm_p  = mul(kf(cfg.mie_scatter), rho_m);      // Mie scatter density (grey)
        const int se_r  = add(add(mul(kf(cfg.rayleigh_scatter_r), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_r), rho_o));
        const int se_g  = add(add(mul(kf(cfg.rayleigh_scatter_g), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_g), rho_o));
        const int se_b  = add(add(mul(kf(cfg.rayleigh_scatter_b), rho_r), mul(kf(cfg.mie_extinction), rho_m)), mul(kf(cfg.ozone_absorb_b), rho_o));
        const int tv_r  = expn(tau_r);
        const int tv_g  = expn(tau_g);
        const int tv_b  = expn(tau_b);
        // in-scatter = single (phase-weighted, sun-lit) + multiple (isotropic). β_R per channel; Mie grey.
        const auto inscat = [&](double br, int ts, int ms, int rrho) {
            const int rr_p   = mul(kf(br), rho_r);                           // Rayleigh scatter density (this channel)
            const int single = mul(mul(add(mul(rr_p, p_r), mul(rm_p, p_m)), ts), kf(cfg.sun_illuminance));
            const int multi  = mul(add(rr_p, rm_p), ms);
            return mul(rrho, add(single, multi));
        };
        l_r = add(l_r, mul(inscat(cfg.rayleigh_scatter_r, ts_r, ms_r, tv_r), dt));
        l_g = add(l_g, mul(inscat(cfg.rayleigh_scatter_g, ts_g, ms_g, tv_g), dt));
        l_b = add(l_b, mul(inscat(cfg.rayleigh_scatter_b, ts_b, ms_b, tv_b), dt));
        tau_r = add(tau_r, mul(se_r, dt));
        tau_g = add(tau_g, mul(se_g, dt));
        tau_b = add(tau_b, mul(se_b, dt));
    }
    const int op3 = mul(p, ku(3));
    g.stmt_buffer_store(out_b, op3, l_r);
    g.stmt_buffer_store(out_b, add(op3, ku(1)), l_g);
    g.stmt_buffer_store(out_b, add(op3, ku(2)), l_b);

    KEntry e;
    e.stage             = KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir::atmos
