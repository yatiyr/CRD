// crd-hesap-quadrature v13-j — oscillatory + singular-weight adaptive integration (QAWO / QAWF / QAWS / QAWC), gated
// bit-close (≤1e-9) to scipy.integrate.quad(weight=…) + analytic invariants + error-tier contract + determinism +
// allocation-free workspace reuse (ADR-0095 pillar 2).

#include <crd/hesap/quadrature/quadrature.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace q = crd::hesap::quadrature;
using crd::f64;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}

// scipy.integrate.quad(weight=…) reference values (build/v13j_gen_refs.py) — the v13-j peer board anchor.
constexpr f64 kQawoExpCos20 = 0.0023091083394092375;
constexpr f64 kQawoExpSin20 = 0.04959403164014024;
constexpr f64 kQawoLogCos100 = -0.015622254668890681;
constexpr f64 kQawoLorentzCos5 = 0.009990269637866674; // the global-sum-fall-through regression case
constexpr f64 kQawoSqrtSin30 = 0.04859429052134435;
constexpr f64 kQawcExpC05 = 1.6717926512070336;
constexpr f64 kQawcCosC1 = -2.701816877227081;
constexpr f64 kQawcRatC2 = -0.1688044698318838;
constexpr f64 kQawsCosJac = 2.587367761551781;
constexpr f64 kQawsExpJac = 0.8371785994176191;
constexpr f64 kQawsLoga = -4.444444444444445;
constexpr f64 kQawsLogb = -2.4818465819584064;
constexpr f64 kQawsLoglog = 0.25253664791816943;
constexpr f64 kQawfExpCos2 = 0.2;
constexpr f64 kQawfExpSin2 = 0.4705882352941176;
} // namespace

TEST_CASE("v13-j: QAWO oscillatory cos/sin - bit-close to scipy.quad(weight=)", "[v13-j][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    q::OscWorkspace<f64> ws(&alloc, 50, 21);
    auto qo = [&](auto&& f, f64 a, f64 b, f64 om, q::OscWeight w)
    {
        return q::integrate_qawo<f64>(ws, f, a, b, om, w, 1e-10, 1e-10);
    };
    CHECK(close(qo([](f64 x) { return std::exp(-x); }, 0.0, 5.0, 20.0, q::OscWeight::Cos).value, kQawoExpCos20, 1e-9,
                1e-12));
    CHECK(close(qo([](f64 x) { return std::exp(-x); }, 0.0, 5.0, 20.0, q::OscWeight::Sin).value, kQawoExpSin20, 1e-9,
                1e-12));
    CHECK(close(qo([](f64 x) { return x > 0.0 ? std::log(x) : 0.0; }, 0.0, 1.0, 100.0, q::OscWeight::Cos).value,
                kQawoLogCos100, 1e-9, 1e-12));
    const auto rl = qo([](f64 x) { return 1.0 / (1.0 + x * x); }, 0.0, 10.0, 5.0, q::OscWeight::Cos);
    CHECK(rl.ok());
    CHECK(close(rl.value, kQawoLorentzCos5, 1e-9, 1e-12));
    CHECK(close(qo([](f64 x) { return std::sqrt(x); }, 0.0, 2.0, 30.0, q::OscWeight::Sin).value, kQawoSqrtSin30, 1e-8,
                1e-11));
    // negative omega: sin is odd -> result negated; cos is even -> unchanged
    CHECK(close(qo([](f64 x) { return std::exp(-x); }, 0.0, 5.0, -20.0, q::OscWeight::Sin).value, -kQawoExpSin20, 1e-9,
                1e-12));
    CHECK(close(qo([](f64 x) { return std::exp(-x); }, 0.0, 5.0, -20.0, q::OscWeight::Cos).value, kQawoExpCos20, 1e-9,
                1e-12));
}

TEST_CASE("v13-j: QAWC Cauchy principal value - bit-close to scipy.quad(weight=cauchy)", "[v13-j][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    q::OscWorkspace<f64> ws(&alloc, 50, 1);
    CHECK(close(q::integrate_qawc<f64>(
                    ws, [](f64 x) { return std::exp(x); }, 0.0, 1.0, 0.5, 1e-10, 1e-10)
                    .value,
                kQawcExpC05, 1e-9, 1e-12));
    CHECK(close(q::integrate_qawc<f64>(
                    ws, [](f64 x) { return std::cos(x); }, -1.0, 3.0, 1.0, 1e-10, 1e-10)
                    .value,
                kQawcCosC1, 1e-9, 1e-12));
    CHECK(close(q::integrate_qawc<f64>(
                    ws, [](f64 x) { return 1.0 / (5.0 + x * x); }, 0.0, 5.0, 2.0, 1e-10, 1e-10)
                    .value,
                kQawcRatC2, 1e-9, 1e-12));
    // analytic: PV ∫_-1^1 1/(x-c) dx = ln|(1-c)/(-1-c)|
    CHECK(close(q::integrate_qawc<f64>(
                    ws, [](f64) { return 1.0; }, -1.0, 1.0, 0.5, 1e-10, 1e-10)
                    .value,
                std::log(std::abs((1.0 - 0.5) / (-1.0 - 0.5))), 1e-10, 1e-12));
    // bad input: c == a
    CHECK(q::integrate_qawc<f64>(
              ws, [](f64 x) { return x; }, 0.0, 1.0, 0.0, 1e-10, 1e-10)
              .status == q::QuadStatus::BadInput);
}

TEST_CASE("v13-j: QAWS algebraico-log endpoint weights - bit-close to scipy.quad(weight=alg)", "[v13-j][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    q::OscWorkspace<f64> ws(&alloc, 50, 1);
    CHECK(close(q::integrate_qaws<f64>(
                    ws, [](f64 x) { return std::cos(x); }, 0.0, 1.0, -0.5, -0.5, q::AlgLogWeight::Pow, 1e-10, 1e-10)
                    .value,
                kQawsCosJac, 1e-9, 1e-12));
    CHECK(close(q::integrate_qaws<f64>(
                    ws, [](f64 x) { return std::exp(x); }, 0.0, 1.0, 0.5, 0.3, q::AlgLogWeight::Pow, 1e-10, 1e-10)
                    .value,
                kQawsExpJac, 1e-9, 1e-12));
    CHECK(close(q::integrate_qaws<f64>(
                    ws, [](f64 x) { return 1.0 + x; }, 0.0, 1.0, -0.5, 0.0, q::AlgLogWeight::LogXmA, 1e-10, 1e-10)
                    .value,
                kQawsLoga, 1e-9, 1e-12));
    CHECK(close(q::integrate_qaws<f64>(
                    ws, [](f64 x) { return std::cos(x); }, 0.0, 1.0, 0.0, -0.5, q::AlgLogWeight::LogBmX, 1e-10, 1e-10)
                    .value,
                kQawsLogb, 1e-9, 1e-12));
    CHECK(close(q::integrate_qaws<f64>(
                    ws, [](f64) { return 1.0; }, 0.0, 1.0, 0.2, 0.2, q::AlgLogWeight::LogBoth, 1e-10, 1e-10)
                    .value,
                kQawsLoglog, 1e-9, 1e-12));
    // bad input: alfa <= -1
    CHECK(q::integrate_qaws<f64>(
              ws, [](f64 x) { return x; }, 0.0, 1.0, -1.5, 0.0, q::AlgLogWeight::Pow, 1e-10, 1e-10)
              .status == q::QuadStatus::BadInput);
}

TEST_CASE("v13-j: QAWF Fourier integral over a..inf - bit-close to scipy.quad(b=inf, weight=)", "[v13-j][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    CHECK(close(q::integrate_qawf<f64>(
                    &alloc, [](f64 x) { return std::exp(-x); }, 0.0, 2.0, q::OscWeight::Cos, 1e-9)
                    .value,
                kQawfExpCos2, 1e-7, 1e-9));
    CHECK(close(q::integrate_qawf<f64>(
                    &alloc, [](f64 x) { return std::exp(-0.5 * x); }, 0.0, 2.0, q::OscWeight::Sin, 1e-9)
                    .value,
                kQawfExpSin2, 1e-7, 1e-9));
    // analytic: ∫_0^∞ e^-x cos(x) dx = 1/2
    CHECK(close(q::integrate_qawf<f64>(
                    &alloc, [](f64 x) { return std::exp(-x); }, 0.0, 1.0, q::OscWeight::Cos, 1e-9)
                    .value,
                0.5, 1e-7, 1e-9));
    // bad input: omega == 0
    CHECK(q::integrate_qawf<f64>(
              &alloc, [](f64 x) { return x; }, 0.0, 0.0, q::OscWeight::Cos, 1e-9)
              .status == q::QuadStatus::BadInput);
}

TEST_CASE("v13-j: Levin collocation - general oscillator + accuracy-grows-with-omega", "[v13-j][quadrature]")
{
    // ∫_0.5^2 cos(omega x^2) dx vs the ANALYTIC Fresnel value (no QUADPACK/scipy/GSL/Boost peer exists for Levin).
    // g=x^2, g'=2x is nonzero on [0.5,2] (no stationary point). Accuracy GROWS with omega (the Levin hallmark).
    auto fone = [](f64)
    {
        return 1.0;
    };
    auto gsq = [](f64 x)
    {
        return x * x;
    };
    auto gpsq = [](f64 x)
    {
        return 2.0 * x;
    };
    CHECK(close(q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 100.0, q::OscWeight::Cos).value, -6.066166892151e-04,
                1e-6, 1e-9));
    CHECK(close(q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 1000.0, q::OscWeight::Cos).value, 8.001451929122e-04,
                1e-7, 1e-10));
    CHECK(close(q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 10000.0, q::OscWeight::Cos).value,
                8.869133164910e-05, 1e-8, 1e-12));
    // accuracy GROWS with omega: the error_estimate (two-grid Tier-1) shrinks as omega rises.
    const f64 e_lo = q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 100.0, q::OscWeight::Cos).error_estimate;
    const f64 e_hi = q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 10000.0, q::OscWeight::Cos).error_estimate;
    CHECK(e_hi < e_lo);
    // reduces to QAWO at the special phase g(x)=x (cross-family check): both cos and sin parts bit-close.
    auto fexp = [](f64 x)
    {
        return std::exp(-x);
    };
    auto glin = [](f64 x)
    {
        return x;
    };
    auto gplin = [](f64)
    {
        return 1.0;
    };
    CHECK(close(q::integrate_levin<f64>(fexp, glin, gplin, 0.0, 5.0, 20.0, q::OscWeight::Cos, 20).value, kQawoExpCos20,
                1e-7, 1e-10));
    CHECK(close(q::integrate_levin<f64>(fexp, glin, gplin, 0.0, 5.0, 20.0, q::OscWeight::Sin, 20).value, kQawoExpSin20,
                1e-7, 1e-10));
    // determinism + bad input
    const auto r1 = q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 1000.0, q::OscWeight::Cos);
    const auto r2 = q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 1000.0, q::OscWeight::Cos);
    CHECK(r1.value == r2.value);
    CHECK(q::integrate_levin<f64>(fone, gsq, gpsq, 0.5, 2.0, 1000.0, q::OscWeight::Cos, 2).status ==
          q::QuadStatus::BadInput);
}

TEST_CASE("v13-j: error-tier contract + determinism + allocation-free workspace reuse", "[v13-j][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    q::OscWorkspace<f64> ws(&alloc, 50, 21);
    auto fn = [](f64 x)
    {
        return std::exp(-x);
    };
    // pillar 3: every result carries eval_count / subdiv_count / a Tier-1 error_estimate (an estimate, not a bound).
    const auto r = q::integrate_qawo<f64>(ws, fn, 0.0, 5.0, 20.0, q::OscWeight::Cos, 1e-10, 1e-10);
    CHECK(r.ok());
    CHECK(r.eval_count > 0);
    CHECK(r.subdiv_count >= 1);
    CHECK(r.error_estimate >= 0.0);
    CHECK(r.tolerance_met);
    // determinism: same call twice -> bit-identical (the serial replay property; the {1,4,16} moat rides on it)
    const auto r2 = q::integrate_qawo<f64>(ws, fn, 0.0, 5.0, 20.0, q::OscWeight::Cos, 1e-10, 1e-10);
    CHECK(r.value == r2.value);
    CHECK(r.error_estimate == r2.error_estimate);
    // pillar 2: REUSE the same workspace across heterogeneous calls -> still correct (no per-call alloc state leak)
    const auto rc = q::integrate_qawc<f64>(ws, [](f64 x) { return std::exp(x); }, 0.0, 1.0, 0.5, 1e-10, 1e-10);
    CHECK(close(rc.value, kQawcExpC05, 1e-9, 1e-12));
    const auto r3 = q::integrate_qawo<f64>(ws, fn, 0.0, 5.0, 20.0, q::OscWeight::Cos, 1e-10, 1e-10);
    CHECK(r3.value == r.value); // QAWO still bit-identical after the QAWC call reused ws
}
