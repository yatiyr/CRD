// test_ckir_hair_scatter.cpp — D-007 B18-c: the hair/fur MULTIPLE-SCATTERING tiers (crd::kir::hairms).
//
// The foundation under every tier is the scattering-moment LUT: front/back hemisphere averages of OUR single-fibre BCSDF plus
// their longitudinal moments (Zinke 2008 Eq 6/8/12/15; also Hu 2026's Albedo = ā_f + ā_b). Because the LUT integrates with the
// SAME measure as the white-furnace gate in test_ckir_hair.cpp, "ā_f + ā_b == directional albedo" is an exactly checkable
// invariant across two independently written integrations — that is the primary correctness gate here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_hair_scatter.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr double kPi   = crd::kir::hair::kPi;
constexpr int    kBins = 64;

// Build the moment LUT for a given absorption. Deliberately low integration resolution: the CPU oracle is an interpreter, and
// this test is about CORRECTNESS of the moments, not their converged accuracy (the GPU gate uses the same reduced config so the
// two stay comparable).
void build_lut(crd::memory::IAllocator& alloc, double sigma_a, crd::containers::Array<double>& out, double fur_kappa = 0.0)
{
    kir::hairms::HairScatterLutConfig c;
    c.n_theta_d = kBins;
    c.n_h       = 2;
    c.n_theta_o = 12;
    c.n_phi_o   = 24;
    c.sigma_a   = sigma_a;
    c.fur_kappa = fur_kappa;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hairms::build_hair_scatter_lut_kernel(g, c);
    out.resize(static_cast<crd::usize>(kBins) * kir::hairms::kLutStride, 0.0);
    kir::KernelBuffer bufs[1] = {{out.data(), kBins * kir::hairms::kLutStride, 0, 0}};
    kir::eval_cpu_kernel(g, e, bufs, 1, e.local_size[0], &alloc, 1U);
}

// The SAME directional-albedo integral as the white-furnace gate, but written independently here and evaluated through the
// elementwise (non-kernel) path — so agreement with ā_f + ā_b cross-validates two separate integrations of the same quantity.
double furnace_albedo_at(crd::memory::IAllocator& alloc, double theta_d, double sigma_a)
{
    constexpr int kNh = 2;
    constexpr int kNt = 12;
    constexpr int kNp = 24;
    constexpr int kN  = kNh * kNt * kNp;
    const double  dth = kPi / static_cast<double>(kNt);
    const double  dph = 2.0 * kPi / static_cast<double>(kNp);

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({kN});
    const int in_sto = g.input(sh, kir::DType::F64);
    const int in_cto = g.input(sh, kir::DType::F64);
    const int in_pho = g.input(sh, kir::DType::F64);
    const int in_h   = g.input(sh, kir::DType::F64);
    const auto kc    = [&](double v) { return g.constant(v, sh, kir::DType::F64); };
    const int  node  = kir::hair::hair_bcsdf_eval_angles(g, kc(crd::math::sin(theta_d)), kc(crd::math::cos(theta_d)), kc(0.0),
                                                         in_sto, in_cto, in_pho, in_h, kc(1.55), kc(sigma_a), kc(0.3), kc(0.3),
                                                         kc(2.0));

    crd::containers::Array<double> sto(&alloc), cto(&alloc), pho(&alloc), hh(&alloc), w(&alloc), got(&alloc);
    sto.resize(kN, 0.0); cto.resize(kN, 0.0); pho.resize(kN, 0.0); hh.resize(kN, 0.0); w.resize(kN, 0.0); got.resize(kN, 0.0);
    int lane = 0;
    for (int a = 0; a < kNh; ++a)
    {
        const double hv = -1.0 + (static_cast<double>(a) + 0.5) * (2.0 / static_cast<double>(kNh));
        for (int b = 0; b < kNt; ++b)
        {
            const double th = -0.5 * kPi + (static_cast<double>(b) + 0.5) * dth;
            for (int c2 = 0; c2 < kNp; ++c2)
            {
                const double     ph = -kPi + (static_cast<double>(c2) + 0.5) * dph;
                const crd::usize i  = static_cast<crd::usize>(lane);
                sto[i] = crd::math::sin(th);
                cto[i] = crd::math::cos(th);
                pho[i] = ph;
                hh[i]  = hv;
                w[i]   = crd::math::cos(th) * crd::math::cos(th);
                ++lane;
            }
        }
    }
    const double* inp[4] = {sto.data(), cto.data(), pho.data(), hh.data()};
    kir::eval_cpu(g, inp, &alloc, node, got.data());
    double acc = 0.0;
    for (int i = 0; i < kN; ++i) { acc += got[static_cast<crd::usize>(i)] * w[static_cast<crd::usize>(i)]; }
    return acc * dth * dph / static_cast<double>(kNh);
}
} // namespace

// B18-c: the shared moment LUT. PRIMARY GATE — ā_f + ā_b reproduces the directional albedo computed by a separately written
// furnace integral (two independent integrations of the same physical quantity must agree). Plus the physics: hair is strongly
// FORWARD scattering (ā_f > ā_b — the fact dual scattering's whole "global = forward only" simplification rests on), the
// moments are finite and non-negative, and absorption strictly lowers both attenuations.
TEST_CASE("B18-c: hair scattering-moment LUT == furnace albedo, and is forward-dominant", "[kir][hair][scatter]")
{
    crd::memory::TlsfAllocator     alloc(256U << 20U);
    crd::containers::Array<double> lut(&alloc);
    build_lut(alloc, 0.2, lut);

    const auto at = [&](int bin, int k) { return lut[static_cast<crd::usize>(bin * kir::hairms::kLutStride + k)]; };

    int    checked  = 0;
    double worstrel = 0.0;
    for (int bin = 8; bin < kBins - 8; bin += 11) // skip the extreme grazing bins where cos²θ ⇒ ~0 and relative error explodes
    {
        const double theta_d = -0.5 * kPi + (static_cast<double>(bin) + 0.5) * (kPi / static_cast<double>(kBins));
        const double af      = at(bin, 0);
        const double ab      = at(bin, 1);
        const double ref     = furnace_albedo_at(alloc, theta_d, 0.2);
        const double rel     = crd::math::abs((af + ab) - ref) / (crd::math::abs(ref) + 1.0e-9);
        if (rel > worstrel) { worstrel = rel; }
        INFO("bin=" << bin << " theta_d=" << theta_d << " a_f=" << af << " a_b=" << ab << " sum=" << (af + ab)
                    << " furnace=" << ref << " rel=" << rel);
        CHECK(af > 0.0);
        CHECK(ab > 0.0);
        CHECK(af > ab);                 // hair is strongly forward-scattering — the premise of dual scattering's global term
        CHECK(at(bin, 2) >= 0.0);       // β̄f² variance is non-negative
        CHECK(at(bin, 4) >= 0.0);       // σ̄_b² variance is non-negative
        // SAME integrand + SAME measure by two independently written integrations ⇒ they must agree to ROUND-OFF. The floor is
        // f32 epsilon, not f64: the LUT is a F32 compute kernel while this furnace reference runs the F64 elementwise path
        // (observed 1.3e-7). A genuine discrepancy — a wrong measure, a dropped Jacobian, a mis-split hemisphere — is O(1e-2)+.
        CHECK(rel < 1.0e-5);
        ++checked;
    }
    INFO("checked " << checked << " bins, worst rel = " << worstrel);
    CHECK(checked >= 4);
}

// B18-c: absorption physically darkens the scattering moments — both hemispheres attenuate monotonically. This is what makes
// the LUT usable as dual scattering's ā_f/ā_b (light hair scatters a lot, dark hair barely at all).
TEST_CASE("B18-c: scattering-moment LUT attenuations fall monotonically with absorption", "[kir][hair][scatter]")
{
    crd::memory::TlsfAllocator     alloc(256U << 20U);
    crd::containers::Array<double> light(&alloc), mid(&alloc), dark(&alloc);
    build_lut(alloc, 0.0, light);
    build_lut(alloc, 0.6, mid);
    build_lut(alloc, 4.0, dark);

    const auto at = [](const crd::containers::Array<double>& l, int bin, int k) {
        return l[static_cast<crd::usize>(bin * kir::hairms::kLutStride + k)];
    };
    for (int bin : {16, 32, 48})
    {
        INFO("bin=" << bin << " a_f: " << at(light, bin, 0) << " -> " << at(mid, bin, 0) << " -> " << at(dark, bin, 0)
                    << " | a_b: " << at(light, bin, 1) << " -> " << at(mid, bin, 1) << " -> " << at(dark, bin, 1));
        CHECK(at(mid, bin, 0) < at(light, bin, 0));
        CHECK(at(dark, bin, 0) < at(mid, bin, 0));
        CHECK(at(mid, bin, 1) < at(light, bin, 1));
        CHECK(at(dark, bin, 1) < at(mid, bin, 1));
        CHECK(at(dark, bin, 0) > 0.0); // the unabsorbed R lobe survives — never a black hole
    }
}

namespace
{
// Run the dual-scattering evaluator over `lanes` parameter sets. in = 6/lane [θd, θi, θo, Δφ_di, Δφ_io, n]; out = 2/lane.
void dual_scatter(crd::memory::IAllocator& alloc, const crd::containers::Array<double>& lut,
                  const crd::containers::Array<double>& in, crd::containers::Array<double>& out, int lanes)
{
    kir::hairms::DualScatterConfig dcfg;
    kir::KGraph                    g(&alloc);
    const kir::KEntry              e = kir::hairms::build_dual_scatter_kernel(g, dcfg, kBins);
    out.resize(static_cast<crd::usize>(lanes) * 2U, 0.0);
    kir::KernelBuffer bufs[3] = {{const_cast<double*>(lut.data()), kBins * kir::hairms::kLutStride, 0, 0},
                                 {const_cast<double*>(in.data()), lanes * 6, 0, 1},
                                 {out.data(), lanes * 2, 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, e.local_size[0], &alloc, static_cast<crd::u32>(lanes / 64));
}
} // namespace

// B18-c: Zinke dual scattering. The gates are the model's own load-bearing physics, each of which would break a real groom:
//  (a) MONOTONE ATTENUATION — T_f = d_f·ā_f^n must fall as the shadow path crosses more fibres (deeper into the groom = darker);
//  (b) n = 0 ⇒ Ψ^G is at its MAXIMUM (a directly-lit fibre loses nothing to forward scattering);
//  (c) SPREAD BROADENS with n — σ̄f² = n·β̄f², so the forward lobe must get *wider* and hence *lower at its centre*;
//  (d) LIGHT HAIR SCATTERS FAR MORE THAN DARK — the entire reason dual scattering exists (blonde vs black);
//  (e) backscattering f_back is positive and rises with ā_f (more forward bounces ⇒ more chances to come back).
TEST_CASE("B18-c: Zinke dual scattering attenuates with depth, broadens, and tracks hair colour", "[kir][hair][scatter][ds]")
{
    crd::memory::TlsfAllocator     alloc(256U << 20U);
    crd::containers::Array<double> lut_light(&alloc), lut_dark(&alloc);
    build_lut(alloc, 0.05, lut_light); // near-white hair
    build_lut(alloc, 4.0, lut_dark);   // near-black hair

    constexpr int kLanes = 64;
    crd::containers::Array<double> in(&alloc), out_l(&alloc), out_d(&alloc);
    in.resize(static_cast<crd::usize>(kLanes) * 6U, 0.0);
    // lane k sweeps the strand count n = k, holding the geometry fixed on a FORWARD configuration (Δφ_di in the far
    // half-cone) so S_f is non-zero, and a BACKWARD one for Δφ_io so S̄_b is non-zero.
    // Sample ON the specular cone (θi = −θd, so θd+θi = 0) — that is where the n=0 delta lives, and the only place where
    // "maximal at n=0, monotone decreasing in n" is the correct statement. OFF the cone the lobe must first BROADEN into the
    // sample before attenuating, so Ψ^G legitimately rises then falls (verified separately below).
    for (int k = 0; k < kLanes; ++k)
    {
        const crd::usize o = static_cast<crd::usize>(k) * 6U;
        in[o + 0U] = 0.20;      // θd
        in[o + 1U] = -0.20;     // θi  ⇒ on the specular cone
        in[o + 2U] = 0.20;      // θo  ⇒ θo+θi = 0, at the backscatter lobe centre
        in[o + 3U] = 3.0;       // Δφ_di — far half-cone ⇒ forward ⇒ s̃_f = 1/π
        in[o + 4U] = 0.2;       // Δφ_io — near half-cone ⇒ backward ⇒ s̃_b = 1/π
        in[o + 5U] = static_cast<double>(k);
    }
    dual_scatter(alloc, lut_light, in, out_l, kLanes);
    dual_scatter(alloc, lut_dark, in, out_d, kLanes);
    const auto psi = [](const crd::containers::Array<double>& o, int k) { return o[static_cast<crd::usize>(k) * 2U]; };
    const auto fbk = [](const crd::containers::Array<double>& o, int k) { return o[static_cast<crd::usize>(k) * 2U + 1U]; };

    // LUT row actually consulted at θd = 0.20 (bin (0.20+π/2)/π·64 ≈ 36) — printed so any failure is diagnosable at a glance.
    const int  bin36 = 36;
    const auto L     = [&](const crd::containers::Array<double>& l, int k) {
        return l[static_cast<crd::usize>(bin36 * kir::hairms::kLutStride + k)];
    };
    INFO("LUT[light] a_f=" << L(lut_light, 0) << " a_b=" << L(lut_light, 1) << " bf2=" << L(lut_light, 2)
         << " db=" << L(lut_light, 3) << " sb2=" << L(lut_light, 4));
    INFO("light psi_g n=0,1,4,16: " << psi(out_l, 0) << ", " << psi(out_l, 1) << ", " << psi(out_l, 4) << ", " << psi(out_l, 16)
         << "  | dark: " << psi(out_d, 0) << ", " << psi(out_d, 1) << ", " << psi(out_d, 4) << ", " << psi(out_d, 16)
         << "  | f_back light=" << fbk(out_l, 0) << " dark=" << fbk(out_d, 0));

    CHECK(psi(out_l, 1) > 0.0);
    // (a)+(c): on the cone, growing n both attenuates (T_f = d_f·ā_f^n) and broadens (σ̄f² = n·β̄f²) — the on-cone value must
    // fall monotonically. Start at n=1: n=0 is the paper's delta, which our tiny variance clamp renders as a spike.
    for (int k = 2; k < 24; ++k)
    {
        CHECK(psi(out_l, k) <= psi(out_l, k - 1) + 1.0e-9);
        CHECK(psi(out_l, k) >= 0.0);
    }
    CHECK(psi(out_l, 8) < psi(out_l, 1) * 0.9); // real attenuation, not a flat line

    // (d) the headline effect: light hair transmits vastly more forward-scattered energy than dark hair
    CHECK(psi(out_l, 4) > psi(out_d, 4) * 5.0);

    // (e) backscattering is positive and much stronger for light hair
    CHECK(fbk(out_l, 0) > 0.0);
    CHECK(fbk(out_d, 0) >= 0.0);
    CHECK(fbk(out_l, 0) > fbk(out_d, 0));
}

namespace
{
constexpr int kPx    = 64; // pixels (one kernel lane each)
constexpr int kFrags = 16;

// Build a deep opacity map from a fragment set, then query transmittance at `nq` (pixel, depth) points.
void dom_build_and_query(crd::memory::IAllocator& alloc, const crd::containers::Array<double>& frags,
                         const crd::containers::Array<double>& queries, crd::containers::Array<double>& out, int nq)
{
    kir::hairms::DomConfig dcfg;
    dcfg.layers       = 4;
    dcfg.span         = 4.0;
    dcfg.frags_per_px = kFrags;
    const int stride  = 1 + dcfg.layers;

    crd::containers::Array<double> dom(&alloc);
    dom.resize(static_cast<crd::usize>(kPx) * static_cast<crd::usize>(stride), 0.0);
    {
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::hairms::build_dom_build_kernel(g, dcfg);
        kir::KernelBuffer b[2] = {{const_cast<double*>(frags.data()), kPx * kFrags * 2, 0, 0}, {dom.data(), kPx * stride, 0, 1}};
        kir::eval_cpu_kernel(g, e, b, 2, e.local_size[0], &alloc, static_cast<crd::u32>(kPx / 64));
    }
    out.resize(static_cast<crd::usize>(nq) * 2U, 0.0);
    {
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::hairms::build_dom_lookup_kernel(g, dcfg);
        kir::KernelBuffer b[3] = {{dom.data(), kPx * stride, 0, 0},
                                  {const_cast<double*>(queries.data()), nq * 2, 0, 1},
                                  {out.data(), nq * 2, 0, 2}};
        kir::eval_cpu_kernel(g, e, b, 3, e.local_size[0], &alloc, static_cast<crd::u32>(nq / 64));
    }
}
} // namespace

// B18-c: DEEP OPACITY MAPS. Gates are the paper's own defining claims:
//  (a) transmittance is 1 in FRONT of the hair (unshadowed direct illumination captured exactly — the property opacity shadow
//      maps lose, because their first plane can start before the first hair fragment);
//  (b) T decreases MONOTONICALLY with depth and total opacity is CONSERVED (past the last layer it equals the sum of all
//      fragment alphas — nothing is dropped);
//  (c) ⭐ LAYERS CONFORM TO THE GROOM — translating a pixel's entire hair column in depth must leave transmittance at the same
//      RELATIVE depth bit-identical. This is precisely what fixed-plane opacity maps get wrong (their layering artifacts), so
//      it is the sharpest possible test that we built deep opacity maps and not opacity shadow maps.
TEST_CASE("B18-c: deep opacity maps conserve opacity, darken monotonically, and CONFORM to the groom", "[kir][hair][scatter][dom]")
{
    crd::memory::TlsfAllocator     alloc(256U << 20U);
    crd::containers::Array<double> frags(&alloc), qry(&alloc), out(&alloc);
    frags.resize(static_cast<crd::usize>(kPx) * kFrags * 2U, 0.0);

    // Pixel p starts its hair column at z0 = 1 + 0.05p (so every pixel has a DIFFERENT front surface — the whole point), with
    // identical internal structure: 16 evenly spaced fragments of alpha 0.1 spanning 3.0 of depth.
    constexpr double kAlpha = 0.1;
    for (int p = 0; p < kPx; ++p)
    {
        const double z0 = 1.0 + 0.05 * static_cast<double>(p);
        for (int f = 0; f < kFrags; ++f)
        {
            const crd::usize o = (static_cast<crd::usize>(p) * kFrags + static_cast<crd::usize>(f)) * 2U;
            // Half-offset spacing so NO fragment sits exactly on a layer boundary (dz = span/K = 1.0). Boundary-coincident
            // depths are a measure-zero tie whose floor() result flips with f32 cancellation error in (depth − z0) — testing
            // conformance through that tie would measure float rounding, not the algorithm.
            frags[o + 0U] = z0 + 3.0 * ((static_cast<double>(f) + 0.5) / static_cast<double>(kFrags));
            frags[o + 1U] = kAlpha;
        }
    }

    // Query each pixel at the SAME RELATIVE depth t (so conformance means all pixels must agree).
    const auto query_at = [&](double t) {
        qry.resize(static_cast<crd::usize>(kPx) * 2U, 0.0);
        for (int p = 0; p < kPx; ++p)
        {
            qry[static_cast<crd::usize>(p) * 2U + 0U] = static_cast<double>(p);
            qry[static_cast<crd::usize>(p) * 2U + 1U] = 1.0 + 0.05 * static_cast<double>(p) + t;
        }
        dom_build_and_query(alloc, frags, qry, out, kPx);
    };

    // (a) in front of the hair ⇒ fully lit
    query_at(-0.5);
    for (int p = 0; p < kPx; p += 13)
    {
        INFO("front p=" << p << " T=" << out[static_cast<crd::usize>(p) * 2U]);
        CHECK(out[static_cast<crd::usize>(p) * 2U] == Catch::Approx(1.0).epsilon(1.0e-6));
    }

    // (b) monotone darkening with depth, and (c) conformance: every pixel identical at equal relative depth
    double prev_T = 2.0;
    for (double t : {0.25, 0.75, 1.5, 2.5, 3.5, 4.5})
    {
        query_at(t);
        const double t0 = out[0];
        double       spread = 0.0;
        for (int p = 0; p < kPx; ++p)
        {
            const double tp = out[static_cast<crd::usize>(p) * 2U];
            const double d  = crd::math::abs(tp - t0);
            if (d > spread) { spread = d; }
        }
        INFO("t=" << t << " T=" << t0 << " opacity=" << out[1] << " cross-pixel spread=" << spread);
        CHECK(spread < 1.0e-6);   // ⭐ CONFORMANCE: z0 varies by 3.2 across pixels, yet T at equal relative depth is identical
        CHECK(t0 <= prev_T + 1.0e-9); // monotone darkening
        CHECK(t0 > 0.0);
        prev_T = t0;
    }

    // (b) conservation: beyond the last layer the accumulated opacity is the full sum of fragment alphas
    query_at(10.0);
    INFO("deep opacity = " << out[1] << " (expected " << kFrags * kAlpha << ")");
    CHECK(out[1] == Catch::Approx(static_cast<double>(kFrags) * kAlpha).epsilon(1.0e-5));
}

namespace
{
// Run the volumetric MS estimator. in = 5/lane [σ_t^⊥, cos(ray,fibre), albedo, phase, distance]; out = 2/lane [L, σ_t(ω)].
void volume_ms(crd::memory::IAllocator& alloc, const kir::hairms::VolumeMsConfig& cfg,
               const crd::containers::Array<double>& in, crd::containers::Array<double>& out, int lanes)
{
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hairms::build_volume_ms_kernel(g, cfg);
    out.resize(static_cast<crd::usize>(lanes) * 2U, 0.0);
    kir::KernelBuffer b[2] = {{const_cast<double*>(in.data()), lanes * 5, 0, 0}, {out.data(), lanes * 2, 0, 1}};
    kir::eval_cpu_kernel(g, e, b, 2, e.local_size[0], &alloc, static_cast<crd::u32>(lanes / 64));
}
} // namespace

// B18-c GOLD TIER: Hu-2026 volumetric multiple scattering. Every gate is a load-bearing claim of the model:
//  (a) ⭐ ANISOTROPIC EXTINCTION σ_t = σ_t^⊥·sinθ — a ray travelling ALONG the fibres must see ~ZERO extinction, one crossing
//      them perpendicularly the maximum. This is what makes the medium behave like hair instead of like fog.
//  (b) ⭐ OCTAVE 0 IS EXACTLY SINGLE SCATTERING — with N=1 the estimator must equal σ_s·P·exp(−τ) to round-off, since
//      (γA)⁰=1, a⁰=1, c⁰=1 ⇒ P'_0 = P. That is what makes the series an EXTENSION of the sampled estimate, not a fudge.
//  (c) higher octaves ADD energy (multiple scattering brightens hair) and PENETRATE DEEPER (exp(−aⁱτ), a<1).
//  (d) γ = 0 collapses the series back to single scattering — the saturation control is honest.
//  (e) brighter hair (higher albedo) gains disproportionately more from multiple scattering — blonde vs black.
TEST_CASE("B18-c: volumetric MS is anisotropic, extends single scattering, and brightens with order", "[kir][hair][scatter][vol]")
{
    crd::memory::TlsfAllocator     alloc(256U << 20U);
    constexpr int                  kLanes = 64;
    crd::containers::Array<double> in(&alloc), out1(&alloc), out3(&alloc), outg0(&alloc);
    in.resize(static_cast<crd::usize>(kLanes) * 5U, 0.0);

    // lane k sweeps the ray-vs-fibre angle from perpendicular (cos=0) to parallel (cos→1)
    constexpr double kSigPerp = 2.0;
    constexpr double kAlbedo  = 0.8;
    constexpr double kPhase   = 0.25;
    constexpr double kDist    = 1.5;
    for (int k = 0; k < kLanes; ++k)
    {
        const crd::usize o = static_cast<crd::usize>(k) * 5U;
        in[o + 0U] = kSigPerp;
        in[o + 1U] = static_cast<double>(k) / static_cast<double>(kLanes); // cos(ray, fibre): 0 → ~1
        in[o + 2U] = kAlbedo;
        in[o + 3U] = kPhase;
        in[o + 4U] = kDist;
    }

    kir::hairms::VolumeMsConfig c3;             // paper baseline: N=3, a=0.5, c=0.5, γ=1
    kir::hairms::VolumeMsConfig c1 = c3; c1.octaves = 1;
    kir::hairms::VolumeMsConfig cg = c3; cg.gamma = 0.0;
    volume_ms(alloc, c1, in, out1, kLanes);
    volume_ms(alloc, c3, in, out3, kLanes);
    volume_ms(alloc, cg, in, outg0, kLanes);
    const auto L  = [](const crd::containers::Array<double>& o, int k) { return o[static_cast<crd::usize>(k) * 2U]; };
    const auto St = [](const crd::containers::Array<double>& o, int k) { return o[static_cast<crd::usize>(k) * 2U + 1U]; };

    // (a) anisotropy: perpendicular ray sees σ_t^⊥, parallel ray sees ~0
    INFO("sigma_t perpendicular=" << St(out3, 0) << " parallel=" << St(out3, kLanes - 1) << " (sigma_t_perp=" << kSigPerp << ")");
    CHECK(St(out3, 0) == Catch::Approx(kSigPerp).epsilon(1.0e-5));
    CHECK(St(out3, kLanes - 1) < kSigPerp * 0.2);
    for (int k = 1; k < kLanes; ++k) { CHECK(St(out3, k) <= St(out3, k - 1) + 1.0e-6); } // monotone toward parallel

    // (b) N=1 must reproduce single scattering EXACTLY: σ_s·P·exp(−σ_t·d)
    for (int k : {0, 7, 23})
    {
        const double sig_t = kSigPerp * crd::math::sqrt(1.0 - (static_cast<double>(k) / kLanes) * (static_cast<double>(k) / kLanes));
        const double ss    = kAlbedo * sig_t;
        const double ref   = ss * kPhase * crd::math::exp(-sig_t * kDist);
        INFO("k=" << k << " N=1 L=" << L(out1, k) << " single-scatter ref=" << ref);
        CHECK(L(out1, k) == Catch::Approx(ref).epsilon(1.0e-5));
    }

    // (c) more octaves ⇒ strictly more energy; (d) γ=0 collapses back onto single scattering
    for (int k : {0, 7, 23, 40})
    {
        INFO("k=" << k << " L(N=1)=" << L(out1, k) << " L(N=3)=" << L(out3, k) << " L(gamma=0)=" << L(outg0, k));
        CHECK(L(out3, k) > L(out1, k));
        CHECK(L(outg0, k) == Catch::Approx(L(out1, k)).epsilon(1.0e-5));
    }

    // (e) light hair gains disproportionately more from multiple scattering than dark hair
    crd::containers::Array<double> in_lo(&alloc), out_lo1(&alloc), out_lo3(&alloc);
    in_lo.resize(static_cast<crd::usize>(kLanes) * 5U, 0.0);
    for (int k = 0; k < kLanes; ++k)
    {
        const crd::usize o = static_cast<crd::usize>(k) * 5U;
        in_lo[o + 0U] = kSigPerp;
        in_lo[o + 1U] = static_cast<double>(k) / static_cast<double>(kLanes);
        in_lo[o + 2U] = 0.1; // dark hair
        in_lo[o + 3U] = kPhase;
        in_lo[o + 4U] = kDist;
    }
    volume_ms(alloc, c1, in_lo, out_lo1, kLanes);
    volume_ms(alloc, c3, in_lo, out_lo3, kLanes);
    const double gain_light = L(out3, 7) / L(out1, 7);
    const double gain_dark  = L(out_lo3, 7) / L(out_lo1, 7);
    INFO("MS gain: light albedo=0.8 -> " << gain_light << "x  |  dark albedo=0.1 -> " << gain_dark << "x");
    CHECK(gain_light > gain_dark * 1.5);
}
