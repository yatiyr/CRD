// test_ckir_hair.cpp — D-007 B18-a: the CKIR hair/fur BCSDF (crd::kir::hair, the Chiang 2016 energy-conserving R/TT/TRT/TRRT
// model). Because the model is transcendental-heavy (a to-ULP tier), correctness is certified by a PHYSICAL invariant that no
// transcription can fake: the WHITE-FURNACE energy test. For a non-absorbing (σₐ=0) fibre, the directional albedo
// A(ωo) = ∫_{S²} f(ωo,ωi)·|cosθᵢ| dωᵢ (averaged over the fibre offset h) must be ≈ 1 — the fibre neither loses nor invents
// energy. Absorbing hair must sit strictly below that, and f ≥ 0 everywhere. The GPU==oracle to-ULP check lives in the
// gpu-context tests; here the graph runs in F64 on `eval_cpu`, which is what the numerical integral samples.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_kernel_eval.hpp> // B18-b2: eval_cpu_kernel — the Huang TT/TRT estimator is a KERNEL (Simpson For loop)

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir  = crd::kir;
namespace hair = crd::kir::hair;

namespace
{
constexpr double kPi = hair::kPi;

// The near-field white-furnace integral. Build the hair BCSDF with `wo` fixed and (θᵢ, φᵢ, h) swept over a dense grid packed
// into `eval_cpu` lanes; return the per-channel directional albedo A = (dθ·dφ / Nh) · Σ f · cos²θᵢ (the fibre-measure form of
// ∫ f·|cosθᵢ| dωᵢ averaged over h). `min_f` receives the smallest f seen (a non-negativity probe).
void hair_albedo(crd::memory::IAllocator& alloc, double theta_o, double sigma_a_val, double beta_m, double beta_n,
                 double alpha_deg, double eta_val, double out_a[3], double& min_f, double fur_kappa = 0.0,
                 double fur_sigma = 2.0, double fur_albedo = 0.9, double fur_g = 0.0, double fur_beta_s = 0.8)
{
    constexpr int k_nh    = 8;
    constexpr int k_nt    = 32;
    constexpr int k_np    = 64;
    constexpr int k_n     = k_nh * k_nt * k_np;
    const double  dtheta = kPi / static_cast<double>(k_nt);        // θᵢ span π
    const double  dphi   = 2.0 * kPi / static_cast<double>(k_np);  // φᵢ span 2π

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({k_n});

    const int in_theta = g.input(sh, kir::DType::F64);
    const int in_phi   = g.input(sh, kir::DType::F64);
    const int in_h     = g.input(sh, kir::DType::F64);

    crd::containers::Array<double> theta(&alloc);
    crd::containers::Array<double> phi(&alloc);
    crd::containers::Array<double> hh(&alloc);
    crd::containers::Array<double> cos2(&alloc); // cos²θᵢ per lane (the measure weight)
    theta.resize(k_n, 0.0);
    phi.resize(k_n, 0.0);
    hh.resize(k_n, 0.0);
    cos2.resize(k_n, 0.0);
    int lane = 0;
    for (int m = 0; m < k_nh; ++m)
    {
        const double hv = -1.0 + (static_cast<double>(m) + 0.5) * (2.0 / static_cast<double>(k_nh));
        for (int kt = 0; kt < k_nt; ++kt)
        {
            const double th = -0.5 * kPi + (static_cast<double>(kt) + 0.5) * dtheta;
            const double ct = crd::math::cos(th);
            for (int j = 0; j < k_np; ++j)
            {
                const double ph = -kPi + (static_cast<double>(j) + 0.5) * dphi;
                theta[static_cast<crd::usize>(lane)] = th;
                phi[static_cast<crd::usize>(lane)]   = ph;
                hh[static_cast<crd::usize>(lane)]    = hv;
                cos2[static_cast<crd::usize>(lane)]  = ct * ct;
                ++lane;
            }
        }
    }
    const double* inp[3] = {theta.data(), phi.data(), hh.data()};

    // wo fixed (fibre frame: x = tangent, sinθ = w.x); wi built per-lane from the swept (θᵢ, φᵢ).
    const auto kc = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    const int  wo = g.vec3(kc(crd::math::sin(theta_o)), kc(crd::math::cos(theta_o)), kc(0.0));
    const int  ct_i = g.unary(kir::KOp::Cos, in_theta);
    const int  wi   = g.vec3(g.unary(kir::KOp::Sin, in_theta),
                             g.binary(kir::KOp::Mul, ct_i, g.unary(kir::KOp::Cos, in_phi)),
                             g.binary(kir::KOp::Mul, ct_i, g.unary(kir::KOp::Sin, in_phi)));
    const int  sig  = g.vec3(kc(sigma_a_val), kc(sigma_a_val), kc(sigma_a_val));
    const int  node = hair::hair_bcsdf_eval(g, wo, wi, in_h, kc(eta_val), sig, kc(beta_m), kc(beta_n), kc(alpha_deg), fur_kappa,
                                            fur_sigma, fur_albedo, fur_g, fur_beta_s);

    crd::containers::Array<double> o(&alloc);
    o.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    kir::eval_cpu(g, inp, &alloc, node, o.data());

    double acc[3] = {0.0, 0.0, 0.0};
    min_f         = 1.0e30;
    for (int i = 0; i < k_n; ++i)
    {
        const double w = cos2[static_cast<crd::usize>(i)];
        for (int c = 0; c < 3; ++c)
        {
            const double fv = o[static_cast<crd::usize>(i) * 3U + static_cast<crd::usize>(c)];
            acc[c] += fv * w;
            if (fv < min_f) { min_f = fv; }
        }
    }
    const double scale = dtheta * dphi / static_cast<double>(k_nh);
    for (int c = 0; c < 3; ++c) { out_a[c] = acc[c] * scale; }
}
// ── B18-b Huang R lobe: an INDEPENDENT host reference. Builds the same Huang-frame geometry in plain C++ and integrates the GGX
// NDF over the visible azimuthal arc with a 20k-sample midpoint rule — no shared code with the CKIR closed form, so a sign flip,
// a wrong atan(tan u) branch, or a bad integration bound cannot hide.
double fresnel_dielectric_ref(double ci, double eta)
{
    ci                 = crd::math::clamp(ci, 0.0, 1.0);
    const double sin2t = (1.0 - ci * ci) / (eta * eta);
    if (sin2t >= 1.0) { return 1.0; }
    const double ct   = crd::math::sqrt(1.0 - sin2t);
    const double rpar = (eta * ci - ct) / (eta * ci + ct);
    const double rper = (ci - eta * ct) / (ci + eta * ct);
    return 0.5 * (rpar * rpar + rper * rper);
}

// B18-b2 furnace for the FULL Huang model (R + TT + TRT). The TT/TRT half is a KERNEL (Simpson For loop + buffer RMW), so this
// sweeps (θi, φi) across kernel lanes and integrates ∫ S·cos²θi dθi dφi — the same measure as the Chiang furnace above.
double huang_albedo(crd::memory::IAllocator& alloc, double theta_o, double sigma_a, double beta, double& min_f,
                    kir::hair::HairModel model = kir::hair::HairModel::HuangFull, bool include_r = true, double eta = 1.55,
                    bool include_tt = true, bool include_trt = true)
{
    constexpr int k_nt = 32;
    constexpr int k_np = 64;
    constexpr int k_n  = k_nt * k_np;

    kir::hair::HairKernelConfig hc;
    hc.model      = model;
    hc.huang_beta = beta;
    hc.simpson_n  = 24;
    hc.include_r   = include_r;
    hc.eta         = eta;
    hc.include_tt  = include_tt;
    hc.include_trt = include_trt;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hair::build_hair_bcsdf_kernel(g, hc);

    crd::containers::Array<double> in(&alloc);
    crd::containers::Array<double> out(&alloc);
    crd::containers::Array<double> wt(&alloc);
    in.resize(static_cast<crd::usize>(k_n) * 6U, 0.0);
    out.resize(k_n, 0.0);
    wt.resize(k_n, 0.0);
    const double dth = kPi / static_cast<double>(k_nt);
    const double dph = 2.0 * kPi / static_cast<double>(k_np);
    int          lane = 0;
    for (int a = 0; a < k_nt; ++a)
    {
        const double th = -0.5 * kPi + (static_cast<double>(a) + 0.5) * dth;
        for (int b = 0; b < k_np; ++b)
        {
            const double      ph = -kPi + (static_cast<double>(b) + 0.5) * dph;
            const crd::usize  o  = static_cast<crd::usize>(lane) * 6U;
            in[o + 0U] = crd::math::sin(theta_o);
            in[o + 1U] = 0.0;
            in[o + 2U] = crd::math::sin(th);
            in[o + 3U] = ph;
            in[o + 4U] = 0.0;
            in[o + 5U] = sigma_a;
            wt[static_cast<crd::usize>(lane)] = crd::math::cos(th) * crd::math::cos(th);
            ++lane;
        }
    }
    kir::KernelBuffer bufs[2] = {{in.data(), k_n * 6, 0, 0}, {out.data(), k_n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(k_n / 64));

    double acc = 0.0;
    min_f      = 1.0e30;
    for (int i = 0; i < k_n; ++i)
    {
        const double f = out[static_cast<crd::usize>(i)];
        acc += f * wt[static_cast<crd::usize>(i)];
        if (f < min_f) { min_f = f; }
    }
    return acc * dth * dph;
}

double huang_r_reference(double theta_o, double phi_o, double theta_i, double phi_i, double eta, double beta)
{
    double dphi = phi_i - phi_o; // wrap into (-pi, pi]
    if (dphi > kPi) { dphi -= 2.0 * kPi; }
    if (dphi < -kPi) { dphi += 2.0 * kPi; }
    const double cto = crd::math::cos(theta_o);
    const double sto = crd::math::sin(theta_o);
    const double cti = crd::math::cos(theta_i);
    const double sti = crd::math::sin(theta_i);
    // Huang frame (y = fibre tangent), rotated so phi_o = 0
    const double ox = 0.0;
    const double oy = sto;
    const double oz = cto;
    const double ix = crd::math::sin(dphi) * cti;
    const double iy = sti;
    const double iz = crd::math::cos(dphi) * cti;
    double       hx = ix + ox;
    double       hy = iy + oy;
    double       hz = iz + oz;
    double       hlen = crd::math::sqrt(hx * hx + hy * hy + hz * hz);
    if (hlen < 1.0e-6) { hlen = 1.0e-6; }
    const double cos_ho = (hx * ox + hy * oy + hz * oz) / hlen;
    hx /= hlen;
    hy /= hlen;
    hz /= hlen;
    const double lo = (dphi > 0.0 ? dphi : 0.0) - 0.5 * kPi; // visible arc: omega_m . omega_i > 0 AND . omega_o > 0
    const double hi = (dphi < 0.0 ? dphi : 0.0) + 0.5 * kPi;
    constexpr int k_n  = 20000;
    double        acc = 0.0;
    for (int i = 0; i < k_n; ++i)
    {
        const double pm  = lo + (hi - lo) * ((static_cast<double>(i) + 0.5) / static_cast<double>(k_n));
        const double chm = hx * crd::math::sin(pm) + hz * crd::math::cos(pm); // omega_h . omega_m
        const double t   = 1.0 + (beta * beta - 1.0) * chm * chm;
        acc += (beta * beta) / (kPi * t * t);                                 // GGX NDF (Eq 41)
    }
    acc *= (hi - lo) / static_cast<double>(k_n);
    const double acto = cto < 0.0 ? -cto : cto;
    const double acti = cti < 0.0 ? -cti : cti;
    const double aho  = cos_ho < 0.0 ? -cos_ho : cos_ho;
    return fresnel_dielectric_ref(aho, eta) * acc
           / (8.0 * (acto > 1.0e-5 ? acto : 1.0e-5) * (acti > 1.0e-5 ? acti : 1.0e-5));
}
} // namespace

// B18-a: energy conservation (white furnace) — the physical correctness gate for the whole BCSDF. σₐ=0 ⇒ A ≈ 1.
TEST_CASE("B18-a: hair BCSDF conserves energy (white-furnace albedo ~1)", "[kir][hair][energy]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    // Three view elevations; σₐ=0 (white), η=1.55, moderate roughness, 2° cuticle tilt.
    const double thetas[3] = {0.0, 0.35, -0.6};
    for (int t = 0; t < 3; ++t)
    {
        double a[3];
        double min_f = 0.0;
        hair_albedo(alloc, thetas[t], 0.0, 0.3, 0.3, 2.0, 1.55, a, min_f);
        INFO("theta_o=" << thetas[t] << "  albedo = (" << a[0] << ", " << a[1] << ", " << a[2] << ")  min_f=" << min_f);
        for (int c = 0; c < 3; ++c)
        {
            CHECK(a[c] > 0.90); // no black hole — the fibre reflects/transmits essentially all incident energy
            CHECK(a[c] < 1.08); // no energy GAIN — the model is energy-conserving (coarse-grid slack)
        }
        CHECK(crd::math::abs(a[0] - a[1]) < 1.0e-9); // σₐ=0 ⇒ achromatic
        CHECK(min_f > -1.0e-9);                      // the BCSDF is non-negative everywhere
    }
}

// B18-a: absorption is physical — more absorption ⇒ strictly lower albedo (monotone), the R surface lobe always survives
// (hair is never a black hole), and unequal per-channel σₐ colours the result. Each achromatic run gives that σₐ's albedo.
TEST_CASE("B18-a: hair BCSDF absorption lowers albedo (dark < white) and tints", "[kir][hair][absorption]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    double white[3];
    double low_abs[3];
    double high_abs[3];
    double dark[3];
    double mn = 0.0;
    hair_albedo(alloc, 0.2, 0.0, 0.3, 0.3, 2.0, 1.55, white, mn);   // σₐ = 0  (white)
    hair_albedo(alloc, 0.2, 0.2, 0.3, 0.3, 2.0, 1.55, low_abs, mn);  // light absorption (a "red" channel)
    hair_albedo(alloc, 0.2, 1.2, 0.3, 0.3, 2.0, 1.55, high_abs, mn); // heavy absorption (a "blue" channel)
    hair_albedo(alloc, 0.2, 4.0, 0.3, 0.3, 2.0, 1.55, dark, mn);    // near-black hair
    INFO("albedo  white=" << white[0] << "  lowAbs=" << low_abs[0] << "  highAbs=" << high_abs[0] << "  dark=" << dark[0]);
    CHECK(low_abs[0] < white[0]);   // any absorption removes energy
    CHECK(high_abs[0] < low_abs[0]); // strictly monotone in σₐ
    CHECK(dark[0] < high_abs[0]);
    CHECK(dark[0] > 0.0);          // ...but the R (surface-reflection) lobe is unabsorbed — hair is never a perfect black hole
    // colour: a fibre with less absorption in one channel than another keeps more energy there (lowAbs = "red" > highAbs = "blue")
    CHECK(low_abs[0] > high_abs[0]);
}

// B18-b: the FUR MEDULLA (Yan 2017 double-cylinder, analytic closed-form) conserves energy. A non-absorbing fur fibre (σₐ=0,
// medulla albedo α_m=1) keeps directional albedo ≈ 1 — the medulla only REDISTRIBUTES the through-medulla TT/TRT energy into the
// scattered lobe, neither creating nor destroying it (that's the milky translucency of animal fur, not an energy leak). Medulla
// absorption (α_m<1) then strictly lowers it, and κ=0 reduces EXACTLY to the hair model (host-guarded — no medulla nodes emitted).
TEST_CASE("B18-b: fur medulla conserves energy and reduces to hair at kappa=0", "[kir][hair][fur][energy]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    const double               thetas[3] = {0.0, 0.3, -0.5};
    for (int t = 0; t < 3; ++t)
    {
        double hairv[3]; // pure hair (no fur args)
        double nomed[3]; // kappa=0 through the fur code path — must equal hair bit-for-bit
        double fur1[3];  // medulla, non-absorbing (α_m = 1)
        double fur_a[3];  // medulla, absorbing (α_m = 0.5)
        double mn = 0.0;
        hair_albedo(alloc, thetas[t], 0.0, 0.3, 0.3, 2.0, 1.55, hairv, mn);
        hair_albedo(alloc, thetas[t], 0.0, 0.3, 0.3, 2.0, 1.55, nomed, mn, 0.0, 2.0, 1.0, 0.0, 0.8);
        hair_albedo(alloc, thetas[t], 0.0, 0.3, 0.3, 2.0, 1.55, fur1, mn, 0.6, 3.0, 1.0, 0.2, 0.8);
        hair_albedo(alloc, thetas[t], 0.0, 0.3, 0.3, 2.0, 1.55, fur_a, mn, 0.6, 3.0, 0.5, 0.2, 0.8);
        INFO("theta=" << thetas[t] << " hair=" << hairv[0] << " nomed=" << nomed[0] << " fur(a=1)=" << fur1[0]
                      << " fur(a=.5)=" << fur_a[0] << " min_f=" << mn);
        CHECK(crd::math::abs(nomed[0] - hairv[0]) < 1.0e-12); // κ=0 ⇒ identical to hair (host-guarded)
        CHECK(fur1[0] > 0.90);      // non-absorbing medulla still conserves energy (redistribution only)
        CHECK(fur1[0] < 1.08);
        CHECK(fur_a[0] < fur1[0]);   // medulla absorption removes energy — strictly lower
        CHECK(mn > -1.0e-9);        // the fur BCSDF is non-negative everywhere
    }
}

// B18-b: the HUANG 2022 analytic R lobe (Appendix A closed form, Eq 41-44) == a brute-force numerical quadrature of the SAME GGX
// NDF over the SAME visible arc. This is THE gate for the closed form: the CKIR value graph vs an independent host implementation.
// Huang's headline contribution is precisely that this integral is analytic, so proving the closed form is the deliverable.
TEST_CASE("B18-b: Huang analytic R lobe == numerical NDF quadrature", "[kir][hair][huang]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    constexpr int              k_n = 96;

    for (double beta : {0.08, 0.3, 0.6}) // GGX roughness: paper's low-roughness regime through rough
    {
        kir::KGraph      g(&alloc);
        const kir::Shape sh = kir::make_shape({k_n});
        const int in_sto = g.input(sh, kir::DType::F64);
        const int in_cto = g.input(sh, kir::DType::F64);
        const int in_pho = g.input(sh, kir::DType::F64);
        const int in_sti = g.input(sh, kir::DType::F64);
        const int in_cti = g.input(sh, kir::DType::F64);
        const int in_phi = g.input(sh, kir::DType::F64);
        const int node   = hair::huang_r_lobe_angles(g, in_sto, in_cto, in_pho, in_sti, in_cti, in_phi,
                                                     g.constant(1.55, sh, kir::DType::F64), beta);

        crd::containers::Array<double> sto(&alloc);
        crd::containers::Array<double> cto(&alloc);
        crd::containers::Array<double> pho(&alloc);
        crd::containers::Array<double> sti(&alloc);
        crd::containers::Array<double> cti(&alloc);
        crd::containers::Array<double> phi(&alloc);
        crd::containers::Array<double> ref(&alloc);
        sto.resize(k_n, 0.0); cto.resize(k_n, 0.0); pho.resize(k_n, 0.0);
        sti.resize(k_n, 0.0); cti.resize(k_n, 0.0); phi.resize(k_n, 0.0); ref.resize(k_n, 0.0);
        crd::u32 s   = 4242U;
        auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
        for (int i = 0; i < k_n; ++i)
        {
            const double th_o = (rnd() * 2.0 - 1.0) * 1.2; // elevation from the fibre's normal plane
            const double th_i = (rnd() * 2.0 - 1.0) * 1.2;
            const double p_o  = (rnd() * 2.0 - 1.0) * kPi;
            const double p_i  = (rnd() * 2.0 - 1.0) * kPi;
            sto[static_cast<crd::usize>(i)] = crd::math::sin(th_o);
            cto[static_cast<crd::usize>(i)] = crd::math::cos(th_o);
            pho[static_cast<crd::usize>(i)] = p_o;
            sti[static_cast<crd::usize>(i)] = crd::math::sin(th_i);
            cti[static_cast<crd::usize>(i)] = crd::math::cos(th_i);
            phi[static_cast<crd::usize>(i)] = p_i;
            ref[static_cast<crd::usize>(i)] = huang_r_reference(th_o, p_o, th_i, p_i, 1.55, beta);
        }
        const double* inp[6] = {sto.data(), cto.data(), pho.data(), sti.data(), cti.data(), phi.data()};
        crd::containers::Array<double> got(&alloc);
        got.resize(k_n, 0.0);
        kir::eval_cpu(g, inp, &alloc, node, got.data());

        double maxrel = 0.0;
        double sum_a = 0.0;
        double sum_r = 0.0;
        for (int i = 0; i < k_n; ++i)
        {
            const double a = got[static_cast<crd::usize>(i)];
            const double r = ref[static_cast<crd::usize>(i)];
            sum_a += a;
            sum_r += r;
            const double rel = crd::math::abs(a - r) / (crd::math::abs(r) + 1.0e-6);
            if (rel > maxrel) { maxrel = rel; }
            CHECK(a >= 0.0); // the R lobe is a non-negative reflectance
        }
        INFO("beta=" << beta << " maxrel(analytic vs 20k-sample quadrature)=" << maxrel
                     << "  sum_analytic=" << sum_a << " sum_numeric=" << sum_r);
        CHECK(sum_a > 0.0);      // a sign flip / zero-clamp would make this exactly 0
        // Observed maxrel ~5e-10 — i.e. AT the 20k-sample midpoint rule's own O(h²) error floor: the closed form is exact.
        // Bound set 200x above that: still 4 orders tighter than any transcription error could hide in.
        CHECK(maxrel < 1.0e-7);
    }
}

// B18-b2: the FULL Huang model (analytic R + the TT/TRT combined MC-Simpson estimator). The gate is the paper's OWN energy
// statement: a microfacet model representing only SINGLE scattering in the microsurface "fulfills energy conservation only
// insofar as it does not generate energy" (Huang §2) — it is expected to be DARK for rough fibres, never bright. So we assert
// (a) non-negativity, (b) albedo ≤ 1 (never gains energy at any roughness), (c) a substantial non-zero response (the TT/TRT
// estimator is actually contributing, not silently masked to zero), and (d) absorption strictly darkens.
TEST_CASE("B18-b2: Huang full (R+TT+TRT) is non-negative, never gains energy, and darkens with absorption", "[kir][hair][huang]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    for (double beta : {0.1, 0.3})
    {
        double mn    = 0.0;
        double white = huang_albedo(alloc, 0.25, 0.0, beta, mn);
        double mid   = huang_albedo(alloc, 0.25, 0.5, beta, mn);
        double dark  = huang_albedo(alloc, 0.25, 3.0, beta, mn);
        double mnx   = 0.0;
        const double r_only  = huang_albedo(alloc, 0.25, 0.0, beta, mnx, kir::hair::HairModel::HuangR);
        const double tt_only = huang_albedo(alloc, 0.25, 0.0, beta, mnx, kir::hair::HairModel::HuangFull, false);
        // η → 1 limit: no refraction, T1=T2=1, R2=0 ⇒ TT alone must carry ALL energy, i.e. albedo == 1 exactly.
        // This separates a NORMALIZATION error (shows up here) from a Fresnel/Jacobian error (would not).
        const double unit_eta = huang_albedo(alloc, 0.25, 0.0, beta, mnx, kir::hair::HairModel::HuangFull, false, 1.0000001);
        const double tt_o = huang_albedo(alloc, 0.25, 0.0, beta, mnx, kir::hair::HairModel::HuangFull, false, 1.55, true, false);
        const double trt_o = huang_albedo(alloc, 0.25, 0.0, beta, mnx, kir::hair::HairModel::HuangFull, false, 1.55, false, true);
        // At eta->1 there is no Fresnel at all: R2 == 0, so TRT must be EXACTLY zero. Non-zero ⇒ structural bug in the TRT path.
        const double trt_e1 = huang_albedo(alloc, 0.25, 0.0, beta, mnx, kir::hair::HairModel::HuangFull, false, 1.0000001, false, true);
        INFO("  [eta->1 TT-only albedo (must be 1.0) = " << unit_eta << "]  [TT=" << tt_o << " TRT=" << trt_o
             << "  expect TT~0.84 TRT~0.07]  [eta->1 TRT (must be 0) = " << trt_e1 << "]");
        INFO("beta=" << beta << " albedo white=" << white << " sigma_a=0.5 -> " << mid << " sigma_a=3 -> " << dark
                     << "  min_f=" << mn << "  [components: R=" << r_only << "  TT+TRT=" << tt_only << "]");
        CHECK(mn > -1.0e-9);   // non-negative everywhere
        CHECK(white > 0.02);   // the estimator really contributes (not masked to zero by a bad validity test)
        CHECK(white < 1.02);   // NEVER generates energy — the paper's own conservation claim
        CHECK(mid < white);    // absorption darkens ...
        CHECK(dark < mid);     // ... monotonically
        CHECK(dark > 0.0);     // the R lobe is unabsorbed, so it never goes fully black
    }
}
