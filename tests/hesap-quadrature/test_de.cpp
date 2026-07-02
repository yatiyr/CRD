// crd-hesap-quadrature v13-i — double-exponential (tanh-sinh / exp-sinh / sinh-sinh) quadrature, gated vs analytic
// integrals (endpoint singularities + semi/doubly-infinite ranges) + determinism.

#include <crd/hesap/quadrature/quadrature.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace q = crd::hesap::quadrature;
using crd::f64;
using crd::containers::ConstSpan;

namespace
{
bool close(f64 g, f64 r, f64 rtol, f64 atol) noexcept
{
    return std::abs(g - r) <= atol + rtol * std::abs(r);
}
} // namespace

TEST_CASE("v13-i: tanh-sinh DE -- finite intervals + endpoint singularities", "[v13-i][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto rule = q::build_tanh_sinh_rule<f64>(&alloc);
    // left-endpoint singularities — full precision
    const auto r1 = q::integrate_tanh_sinh<f64>(rule, [](f64 x) { return 1.0 / std::sqrt(x); }, 0.0, 1.0, 1e-12, 1e-12);
    CHECK(r1.ok());
    CHECK(close(r1.value, 2.0, 1e-10, 1e-11)); // ∫_0^1 x^-1/2 = 2
    CHECK(close(q::integrate_tanh_sinh<f64>(
                    rule, [](f64 x) { return std::log(x); }, 0.0, 1.0, 1e-12, 1e-12)
                    .value,
                -1.0, 1e-10, 1e-11)); // ∫_0^1 ln x = -1
    // smooth — matches scipy.quad's value
    CHECK(close(q::integrate_tanh_sinh<f64>(
                    rule, [](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0, 1e-12, 1e-12)
                    .value,
                0.562449792050565, 1e-10, 1e-11));
    // right-endpoint singularity — resolved to ~1e-8 (the single-argument f(x) interface limit)
    CHECK(close(q::integrate_tanh_sinh<f64>(
                    rule, [](f64 x) { return 1.0 / std::sqrt(1.0 - x); }, 0.0, 1.0, 1e-7, 1e-7)
                    .value,
                2.0, 1e-6, 1e-7));
}

TEST_CASE("v13-i: exp-sinh DE -- semi-infinite a..inf", "[v13-i][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto rule = q::build_exp_sinh_rule<f64>(&alloc);
    CHECK(close(q::integrate_exp_sinh<f64>(
                    rule, [](f64 x) { return std::exp(-x * x); }, 0.0, 1e-12, 1e-12)
                    .value,
                0.8862269254527580, 1e-10, 1e-11)); // ∫_0^∞ e^-x² = √π/2
    CHECK(close(q::integrate_exp_sinh<f64>(
                    rule, [](f64 x) { return std::exp(-x); }, 0.0, 1e-12, 1e-12)
                    .value,
                1.0, 1e-10, 1e-11)); // ∫_0^∞ e^-x = 1
    CHECK(close(q::integrate_exp_sinh<f64>(
                    rule, [](f64 x) { return 1.0 / (1.0 + x * x); }, 0.0, 1e-12, 1e-12)
                    .value,
                1.5707963267948966, 1e-10, 1e-11)); // ∫_0^∞ 1/(1+x²) = π/2
    CHECK(close(q::integrate_exp_sinh<f64>(
                    rule, [](f64 x) { return 1.0 / (x * x); }, 1.0, 1e-12, 1e-12)
                    .value,
                1.0, 1e-9, 1e-10)); // ∫_1^∞ x^-2 = 1 (a-shifted)
}

TEST_CASE("v13-i: sinh-sinh DE -- doubly-infinite (-inf,inf) + determinism", "[v13-i][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto rule = q::build_sinh_sinh_rule<f64>(&alloc);
    const auto r1 = q::integrate_sinh_sinh<f64>(rule, [](f64 x) { return std::exp(-x * x); }, 1e-12, 1e-12);
    CHECK(r1.ok());
    CHECK(close(r1.value, 1.7724538509055159, 1e-10, 1e-11)); // ∫_-∞^∞ e^-x² = √π
    CHECK(close(q::integrate_sinh_sinh<f64>(
                    rule, [](f64 x) { return 1.0 / (1.0 + x * x); }, 1e-12, 1e-12)
                    .value,
                3.141592653589793, 1e-10, 1e-11)); // ∫_-∞^∞ 1/(1+x²) = π
    const auto r2 = q::integrate_sinh_sinh<f64>(rule, [](f64 x) { return std::exp(-x * x); }, 1e-12, 1e-12);
    CHECK(r1.value == r2.value); // deterministic
}

TEST_CASE("v13-i: Clenshaw-Curtis -- fixed rule (degree-n exactness) + nested adaptive", "[v13-i][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto cc = q::build_clenshaw_curtis_rule<f64>(&alloc, 16);
    for (int k = 0; k <= 16; ++k) // exact for degree ≤ n
    {
        const auto r = q::integrate_chebyshev<f64>(cc, [k](f64 x) { return std::pow(x, k); }, -1.0, 1.0);
        const f64 ex = (k % 2 == 1) ? 0.0 : 2.0 / (k + 1);
        INFO("cc k=" << k);
        CHECK(close(r.value, ex, 1e-12, 1e-13));
    }
    CHECK(close(q::integrate_chebyshev<f64>(
                    cc, [](f64 x) { return std::exp(x); }, 0.0, 1.0)
                    .value,
                1.7182818284590452, 1e-12, 1e-13));
    // nested adaptive (the GSL cquad peer)
    const auto ra = q::integrate_clenshaw_curtis<f64>(
        &alloc, [](f64 x) { return std::exp(x) * std::cos(2 * x); }, 0.0, 1.0, 1e-12, 1e-12);
    CHECK(ra.ok());
    CHECK(close(ra.value, 0.562449792050565, 1e-10, 1e-11));
}

TEST_CASE("v13-i: Fejer rule -- degree-(n-1) exactness", "[v13-i][quadrature]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto fr = q::build_fejer_rule<f64>(&alloc, 16);
    for (int k = 0; k <= 15; ++k)
    {
        const auto r = q::integrate_chebyshev<f64>(fr, [k](f64 x) { return std::pow(x, k); }, -1.0, 1.0);
        const f64 ex = (k % 2 == 1) ? 0.0 : 2.0 / (k + 1);
        INFO("fejer k=" << k);
        CHECK(close(r.value, ex, 1e-12, 1e-13));
    }
    CHECK(close(q::integrate_chebyshev<f64>(
                    fr, [](f64 x) { return std::exp(x); }, 0.0, 1.0)
                    .value,
                1.7182818284590452, 1e-10, 1e-11));
}

TEST_CASE("v13-i: Romberg -- function (vs analytic) + samples (vs scipy.romb)", "[v13-i][quadrature]")
{
    CHECK(close(q::integrate_romberg<f64>([](f64 x) { return std::exp(x); }, 0.0, 1.0, 1e-12, 1e-12).value,
                1.7182818284590452, 1e-11, 1e-12)); // ∫_0^1 e^x = e-1
    CHECK(close(
        q::integrate_romberg<f64>([](f64 x) { return std::sin(x); }, 0.0, 3.14159265358979323846, 1e-12, 1e-12).value,
        2.0, 1e-11, 1e-12)); // ∫_0^π sin = 2
    CHECK(q::integrate_romberg<f64>([](f64 x) { return x; }, 0.0, 1.0, 0.0, 0.0).status == q::QuadStatus::BadInput);
    // romberg_samples (2^5+1=33 pts) bit-close to scipy.integrate.romb
    f64 y[33];
    for (int i = 0; i < 33; ++i)
    {
        const f64 x = i / 32.0;
        y[i] = std::exp(x) * std::cos(2 * x);
    }
    CHECK(close(q::romberg_samples<f64>(ConstSpan<f64>{y, 33}, 1.0 / 32.0), 0.56244979205056256, 1e-12, 1e-13));
    // determinism
    auto rb = []
    {
        return q::integrate_romberg<f64>([](f64 x) { return std::exp(x); }, 0.0, 1.0, 1e-12, 1e-12).value;
    };
    CHECK(rb() == rb());
}
