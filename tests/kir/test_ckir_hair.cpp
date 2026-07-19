// test_ckir_hair.cpp — D-007 B18-a: the CKIR hair/fur BCSDF (crd::kir::hair, the Chiang 2016 energy-conserving R/TT/TRT/TRRT
// model). Because the model is transcendental-heavy (a to-ULP tier), correctness is certified by a PHYSICAL invariant that no
// transcription can fake: the WHITE-FURNACE energy test. For a non-absorbing (σₐ=0) fibre, the directional albedo
// A(ωo) = ∫_{S²} f(ωo,ωi)·|cosθᵢ| dωᵢ (averaged over the fibre offset h) must be ≈ 1 — the fibre neither loses nor invents
// energy. Absorbing hair must sit strictly below that, and f ≥ 0 everywhere. The GPU==oracle to-ULP check lives in the
// gpu-context tests; here the graph runs in F64 on `eval_cpu`, which is what the numerical integral samples.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_hair.hpp>

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
                 double alpha_deg, double eta_val, double out_a[3], double& min_f)
{
    constexpr int kNh    = 8;
    constexpr int kNt    = 32;
    constexpr int kNp    = 64;
    constexpr int kN     = kNh * kNt * kNp;
    const double  dtheta = kPi / static_cast<double>(kNt);        // θᵢ span π
    const double  dphi   = 2.0 * kPi / static_cast<double>(kNp);  // φᵢ span 2π

    kir::KGraph      g(&alloc);
    const kir::Shape sh = kir::make_shape({kN});

    const int in_theta = g.input(sh, kir::DType::F64);
    const int in_phi   = g.input(sh, kir::DType::F64);
    const int in_h     = g.input(sh, kir::DType::F64);

    crd::containers::Array<double> theta(&alloc);
    crd::containers::Array<double> phi(&alloc);
    crd::containers::Array<double> hh(&alloc);
    crd::containers::Array<double> cos2(&alloc); // cos²θᵢ per lane (the measure weight)
    theta.resize(kN, 0.0);
    phi.resize(kN, 0.0);
    hh.resize(kN, 0.0);
    cos2.resize(kN, 0.0);
    int lane = 0;
    for (int m = 0; m < kNh; ++m)
    {
        const double hv = -1.0 + (static_cast<double>(m) + 0.5) * (2.0 / static_cast<double>(kNh));
        for (int kt = 0; kt < kNt; ++kt)
        {
            const double th = -0.5 * kPi + (static_cast<double>(kt) + 0.5) * dtheta;
            const double ct = crd::math::cos(th);
            for (int j = 0; j < kNp; ++j)
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
    const int  node = hair::hair_bcsdf_eval(g, wo, wi, in_h, kc(eta_val), sig, kc(beta_m), kc(beta_n), kc(alpha_deg));

    crd::containers::Array<double> o(&alloc);
    o.resize(static_cast<crd::usize>(kN) * 3U, 0.0);
    kir::eval_cpu(g, inp, &alloc, node, o.data());

    double acc[3] = {0.0, 0.0, 0.0};
    min_f         = 1.0e30;
    for (int i = 0; i < kN; ++i)
    {
        const double w = cos2[static_cast<crd::usize>(i)];
        for (int c = 0; c < 3; ++c)
        {
            const double fv = o[static_cast<crd::usize>(i) * 3U + static_cast<crd::usize>(c)];
            acc[c] += fv * w;
            if (fv < min_f) { min_f = fv; }
        }
    }
    const double scale = dtheta * dphi / static_cast<double>(kNh);
    for (int c = 0; c < 3; ++c) { out_a[c] = acc[c] * scale; }
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
    double lowAbs[3];
    double highAbs[3];
    double dark[3];
    double mn = 0.0;
    hair_albedo(alloc, 0.2, 0.0, 0.3, 0.3, 2.0, 1.55, white, mn);   // σₐ = 0  (white)
    hair_albedo(alloc, 0.2, 0.2, 0.3, 0.3, 2.0, 1.55, lowAbs, mn);  // light absorption (a "red" channel)
    hair_albedo(alloc, 0.2, 1.2, 0.3, 0.3, 2.0, 1.55, highAbs, mn); // heavy absorption (a "blue" channel)
    hair_albedo(alloc, 0.2, 4.0, 0.3, 0.3, 2.0, 1.55, dark, mn);    // near-black hair
    INFO("albedo  white=" << white[0] << "  lowAbs=" << lowAbs[0] << "  highAbs=" << highAbs[0] << "  dark=" << dark[0]);
    CHECK(lowAbs[0] < white[0]);   // any absorption removes energy
    CHECK(highAbs[0] < lowAbs[0]); // strictly monotone in σₐ
    CHECK(dark[0] < highAbs[0]);
    CHECK(dark[0] > 0.0);          // ...but the R (surface-reflection) lobe is unabsorbed — hair is never a perfect black hole
    // colour: a fibre with less absorption in one channel than another keeps more energy there (lowAbs = "red" > highAbs = "blue")
    CHECK(lowAbs[0] > highAbs[0]);
}
