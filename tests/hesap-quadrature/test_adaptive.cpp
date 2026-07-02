// crd-hesap-quadrature v13-h — the Gauss-Kronrod rule + the adaptive QAG integrator, gated vs scipy.integrate.quad +
// degree-(3n+1) exactness + the bounded-workspace/no-recursion (WCET) structural guard + the Lyness-Kaganove
// honesty test (the error ESTIMATE is not a bound) + determinism.

#include <crd/hesap/quadrature/quadrature.hpp>

#include "adaptive_refs.inc"

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace q = crd::hesap::quadrature;
using crd::containers::ConstSpan;
using crd::f64;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}
} // namespace

TEST_CASE("v13-h: GK21 rule -- degree-31 exactness + single-panel value vs scipy", "[v13-h][quadrature]")
{
    for (int k = 0; k <= 31; ++k) // (2n+1)=21-point Kronrod is exact to degree 3n+1 = 31
    {
        const auto r     = q::gauss_kronrod_21<f64>([k](f64 x) { return std::pow(x, k); }, -1.0, 1.0);
        const f64  exact = (k % 2 == 1) ? 0.0 : 2.0 / (k + 1);
        INFO("k=" << k);
        CHECK(close(r.value, exact, 1e-12, 1e-13));
    }
    const auto g = q::gauss_kronrod_21<f64>([](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0);
    CHECK(close(g.value, ref_gk21_val, 1e-13, 1e-14)); // bit-matches scipy.integrate.quad single panel
    CHECK(g.abserr > 0.0);
    CHECK(g.abserr < 1e-13); // smooth ⇒ tiny error estimate
}

TEST_CASE("v13-h: adaptive QAG converges vs scipy.integrate.quad + result contract + bad input", "[v13-h][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    struct
    {
        f64 (*f)(f64);
        f64 a, b;
    } cases[] = {
        {[](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0}, {[](f64 x) { return std::sin(x); }, 0.0, 3.14159265358979323846},
        {[](f64 x) { return std::exp(-x * x); }, 0.0, 3.0},             {[](f64 x) { return 1.0 / (1.0 + x * x); }, 0.0, 5.0},
        {[](f64 x) { return x * std::exp(-x); }, 0.0, 10.0}};
    for (int i = 0; i < 5; ++i)
    {
        const auto r = q::integrate_adaptive<f64>(&alloc, cases[i].f, cases[i].a, cases[i].b, 1e-12, 1e-12, 50);
        INFO("case " << i);
        CHECK(r.ok());
        CHECK(r.tolerance_met);
        CHECK(close(r.value, ref_quad[i], 1e-10, 1e-11)); // vs scipy.quad
        CHECK(std::abs(r.value - ref_quad[i]) <= r.error_estimate + 1e-13); // the estimate covers the actual error here
        CHECK(r.eval_count >= 21);
    }
    // bad input
    CHECK(q::integrate_adaptive<f64>(&alloc, [](f64 x) { return x; }, 0.0, 1.0, 0.0, 0.0, 50).status
          == q::QuadStatus::BadInput);
    CHECK(q::integrate_adaptive<f64>(&alloc, [](f64 x) { return x; }, 0.0, 1.0, 1e-9, 1e-9, 0).status
          == q::QuadStatus::BadInput);
}

TEST_CASE("v13-h: adaptive respects the WCET subdivision bound (no unbounded recursion)", "[v13-h][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 18);
    // A demanding integrand at a tight tolerance with a TINY budget: the driver must stop at the bound and report
    // MaxSubdivisions — never spin, never overrun the fixed work-stack (the MISRA/WCET property).
    auto       f = [](f64 x) { return std::exp(-100.0 * (x - 0.5) * (x - 0.5)); };
    const auto r = q::integrate_adaptive<f64>(&alloc, f, 0.0, 1.0, 1e-14, 0.0, 3);
    CHECK(r.status == q::QuadStatus::MaxSubdivisions);
    CHECK_FALSE(r.tolerance_met);
    CHECK(r.subdiv_count <= 3); // never exceeded the budget
}

TEST_CASE("v13-h: Lyness-Kaganove honesty -- the error estimate is NOT a bound", "[v13-h][quadrature]")
{
    // A Gaussian peak at x=0.3 narrower than the GK21 node spacing on [0,1]: every node sees f≈0, so the single
    // panel returns ~0 with a TINY error estimate — yet the true integral is √π/1000 ≈ 1.77e-3. The estimate is
    // confidently wrong. This is the Tier-1 caveat the contract documents (error_estimate is never an enclosure).
    const auto g    = q::gauss_kronrod_21<f64>([](f64 x) { return std::exp(-1.0e6 * (x - 0.3) * (x - 0.3)); }, 0.0, 1.0);
    const f64  true_val = 1.7724538509055159 / 1000.0; // √π / 1000
    CHECK(std::abs(g.value) < 1e-8);   // the panel saw ~nothing
    CHECK(g.abserr < 1e-6);            // ...and "confidently" reports near-zero error
    CHECK(std::abs(g.value - true_val) > 1e-4); // ...while being far from the truth: the estimate was fooled
}

TEST_CASE("v13-h: QAGS handles endpoint singularities via Wynn-eps extrapolation", "[v13-h][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    // Endpoint-singular integrands where plain QAG converges only slowly — QAGS extrapolates to full precision.
    const auto r1 = q::integrate_qags<f64>(&alloc, [](f64 x) { return 1.0 / std::sqrt(x); }, 0.0, 1.0, 1e-10, 1e-10, 50);
    CHECK(r1.ok());
    CHECK(close(r1.value, 2.0, 1e-8, 1e-9)); // ∫_0^1 x^-1/2 = 2 (singular at 0)
    const auto r2 = q::integrate_qags<f64>(&alloc, [](f64 x) { return std::log(x); }, 0.0, 1.0, 1e-10, 1e-10, 50);
    CHECK(r2.ok());
    CHECK(close(r2.value, -1.0, 1e-8, 1e-9)); // ∫_0^1 ln x = -1
    const auto r3 = q::integrate_qags<f64>(&alloc, [](f64 x) { return 1.0 / std::sqrt(1.0 - x); }, 0.0, 1.0, 1e-10, 1e-10, 50);
    CHECK(r3.ok());
    CHECK(close(r3.value, 2.0, 1e-8, 1e-9)); // ∫_0^1 (1-x)^-1/2 = 2 (singular at 1)
    const auto r4 = q::integrate_qags<f64>(&alloc, [](f64 x) { return std::pow(x, -0.8); }, 0.0, 1.0, 1e-10, 1e-10, 50);
    CHECK(r4.ok());
    CHECK(close(r4.value, 5.0, 1e-7, 1e-8)); // ∫_0^1 x^-0.8 = 1/0.2 = 5 (strong singularity)

    // On smooth integrands QAGS agrees with scipy.integrate.quad (its default) and with QAG.
    const auto s = q::integrate_qags<f64>(&alloc, [](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0, 1e-12,
                                          1e-12, 50);
    CHECK(close(s.value, ref_quad[0], 1e-10, 1e-11));

    // determinism
    const auto d1 = q::integrate_qags<f64>(&alloc, [](f64 x) { return 1.0 / std::sqrt(x); }, 0.0, 1.0, 1e-10, 1e-10, 50);
    CHECK(d1.value == r1.value);
}

TEST_CASE("v13-h: QAGI integrates semi/doubly-infinite ranges (transform + QAGS) vs analytic", "[v13-h][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const auto a1 = q::integrate_qagi<f64>(&alloc, [](f64 x) { return std::exp(-x); }, 0.0, 1, 1e-10, 1e-10, 50);
    CHECK(a1.ok());
    CHECK(close(a1.value, 1.0, 1e-8, 1e-9)); // ∫_0^∞ e^-x = 1
    const auto a2 = q::integrate_qagi<f64>(&alloc, [](f64 x) { return std::exp(-x * x); }, 0.0, 1, 1e-10, 1e-10, 50);
    CHECK(a2.ok());
    CHECK(close(a2.value, 0.8862269254527580, 1e-8, 1e-9)); // ∫_0^∞ e^-x² = √π/2
    const auto a3 = q::integrate_qagi<f64>(&alloc, [](f64 x) { return std::exp(-x * x); }, 0.0, 2, 1e-10, 1e-10, 50);
    CHECK(a3.ok());
    CHECK(close(a3.value, 1.7724538509055159, 1e-8, 1e-9)); // ∫_-∞^∞ e^-x² = √π (inf=2)
    const auto a4 = q::integrate_qagi<f64>(&alloc, [](f64 x) { return std::exp(x); }, 0.0, -1, 1e-10, 1e-10, 50);
    CHECK(a4.ok());
    CHECK(close(a4.value, 1.0, 1e-8, 1e-9)); // ∫_-∞^0 e^x = 1 (inf=-1)
    const auto a5 = q::integrate_qagi<f64>(&alloc, [](f64 x) { return 1.0 / (x * x); }, 1.0, 1, 1e-10, 1e-10, 50);
    CHECK(a5.ok());
    CHECK(close(a5.value, 1.0, 1e-8, 1e-9)); // ∫_1^∞ x^-2 = 1
    const auto a6 = q::integrate_qagi<f64>(&alloc, [](f64 x) { return 1.0 / (1.0 + x * x); }, 0.0, 2, 1e-10, 1e-10, 50);
    CHECK(a6.ok());
    CHECK(close(a6.value, 3.141592653589793, 1e-8, 1e-9)); // ∫_-∞^∞ 1/(1+x²) = π (inf=2)

    CHECK(q::integrate_qagi<f64>(&alloc, [](f64 x) { return x; }, 0.0, 3, 1e-9, 1e-9, 50).status
          == q::QuadStatus::BadInput); // inf ∉ {-1,1,2}
    const auto d = q::integrate_qagi<f64>(&alloc, [](f64 x) { return std::exp(-x); }, 0.0, 1, 1e-10, 1e-10, 50);
    CHECK(d.value == a1.value); // deterministic
}

TEST_CASE("v13-h: QNG non-adaptive integrator (Patterson 10/21/43/87) vs scipy/analytic + exactness", "[v13-h][quadrature]")
{
    const auto r1 = q::integrate_qng<f64>([](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0, 1e-10, 1e-10);
    CHECK(r1.ok());
    CHECK(close(r1.value, ref_quad[0], 1e-10, 1e-11)); // vs scipy.quad
    CHECK(r1.eval_count <= 87);
    const auto r2 = q::integrate_qng<f64>([](f64 x) { return std::sin(x); }, 0.0, 3.14159265358979323846, 1e-10, 1e-10);
    CHECK(r2.ok());
    CHECK(close(r2.value, 2.0, 1e-10, 1e-11));
    // exactness: a low-degree polynomial is integrated exactly at the 21-point level (no escalation)
    const auto rp = q::integrate_qng<f64>([](f64 x) { return x * x * x * x; }, -1.0, 1.0, 1e-12, 1e-12);
    CHECK(close(rp.value, 0.4, 1e-13, 1e-14)); // ∫_-1^1 x^4 = 2/5
    CHECK(rp.eval_count == 21);
    // bad input + determinism
    CHECK(q::integrate_qng<f64>([](f64 x) { return x; }, 0.0, 1.0, 0.0, 0.0).status == q::QuadStatus::BadInput);
    CHECK(q::integrate_qng<f64>([](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0, 1e-10, 1e-10).value
          == r1.value);
}

TEST_CASE("v13-h: QAGP integrates with known break-points (singularities + discontinuities)", "[v13-h][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    f64                        bp1[] = {1.0};
    // interior singularity at x=1: ∫_0^3 |x-1|^-1/2 = 2 + 2√2
    const auto r1 = q::integrate_qagp<f64>(
        &alloc, [](f64 x) { return 1.0 / std::sqrt(std::abs(x - 1.0)); }, 0.0, 3.0, ConstSpan<f64>{bp1, 1}, 1e-9, 1e-9, 50);
    CHECK(r1.ok());
    CHECK(close(r1.value, 2.0 + 2.0 * std::sqrt(2.0), 1e-8, 1e-9));
    // jump discontinuity at x=1: f=1 (x<1), f=2 (x≥1) ⇒ ∫_0^2 = 3 (each piece constant ⇒ exact)
    const auto r2 = q::integrate_qagp<f64>(
        &alloc, [](f64 x) { return x < 1.0 ? 1.0 : 2.0; }, 0.0, 2.0, ConstSpan<f64>{bp1, 1}, 1e-10, 1e-10, 50);
    CHECK(r2.ok());
    CHECK(close(r2.value, 3.0, 1e-10, 1e-11));
    // no break-points ⇒ equals plain QAGS on the whole interval
    const auto r3 = q::integrate_qagp<f64>(
        &alloc, [](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0, ConstSpan<f64>{bp1, 0}, 1e-12, 1e-12, 50);
    CHECK(close(r3.value, ref_quad[0], 1e-10, 1e-11));
    // determinism
    const auto d = q::integrate_qagp<f64>(
        &alloc, [](f64 x) { return 1.0 / std::sqrt(std::abs(x - 1.0)); }, 0.0, 3.0, ConstSpan<f64>{bp1, 1}, 1e-9, 1e-9, 50);
    CHECK(d.value == r1.value);
}

TEST_CASE("v13-h: adaptive integration is deterministic", "[v13-h][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    auto       f  = [](f64 x) { return std::exp(x) * std::cos(2 * x); };
    const auto r1 = q::integrate_adaptive<f64>(&alloc, f, 0.0, 1.0, 1e-12, 1e-12, 50);
    const auto r2 = q::integrate_adaptive<f64>(&alloc, f, 0.0, 1.0, 1e-12, 1e-12, 50);
    CHECK(r1.value == r2.value); // bit-identical
    CHECK(r1.error_estimate == r2.error_estimate);
    CHECK(r1.subdiv_count == r2.subdiv_count);
}
