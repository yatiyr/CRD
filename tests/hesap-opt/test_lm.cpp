// crd-hesap-opt v7-e-1 — nonlinear least-squares (Levenberg-Marquardt + Gauss-Newton). Validates: (1) LM converges
// on an exponential curve fit (recovers the true parameters, cost→0); (2) LM solves the 2-residual Rosenbrock
// (minimizer (1,1), cost 0); (3) Gauss-Newton converges on a well-conditioned fit; (4) a robust loss (Cauchy)
// recovers the true parameters far better than plain least-squares when the data has an outlier; (5) run-twice
// bit-identity (determinism by construction — the dense LM is serial scalar; the cross-worker {1..16} moat over a
// PARALLEL Jacobian assembly is v7-e-2, the sparse slice).

#include <crd/hesap/opt/opt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;
using Catch::Matchers::WithinAbs;

namespace
{
// Exponential fit r_i = a·exp(b·t_i) − y_i, with x = (a, b). Jacobian: ∂r_i/∂a = exp(b·t_i),
// ∂r_i/∂b = a·t_i·exp(b·t_i). Data is generated from (a*, b*) and (optionally) one corrupted point.
class ExpFit final : public opt::ResidualFunction<crd::f64>
{
public:
    ExpFit(const crd::f64* t, const crd::f64* y, crd::usize m)
        : opt::ResidualFunction<crd::f64>(/*has_jacobian=*/true), m_t(t), m_y(y), m_m(m)
    {
    }
    void residuals(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> r) const override
    {
        for (crd::usize i = 0; i < m_m; ++i)
        {
            r[i] = x[0] * std::exp(x[1] * m_t[i]) - m_y[i];
        }
    }
    [[nodiscard]] crd::usize num_residuals() const noexcept override { return m_m; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    [[nodiscard]] bool jacobian(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> jac) const override
    {
        for (crd::usize i = 0; i < m_m; ++i)
        {
            const crd::f64 e = std::exp(x[1] * m_t[i]);
            jac[i * 2 + 0] = e;
            jac[i * 2 + 1] = x[0] * m_t[i] * e;
        }
        return true;
    }

private:
    const crd::f64* m_t;
    const crd::f64* m_y;
    crd::usize      m_m;
};

// Rosenbrock as 2-residual least-squares: r0 = 10(x1 − x0²), r1 = 1 − x0. ½‖r‖² is the Rosenbrock function;
// minimizer (1,1), cost 0.
class RosenbrockLS final : public opt::ResidualFunction<crd::f64>
{
public:
    RosenbrockLS() : opt::ResidualFunction<crd::f64>(/*has_jacobian=*/true) {}
    void residuals(crd::containers::ConstSpan<crd::f64> x, crd::containers::Span<crd::f64> r) const override
    {
        r[0] = 10.0 * (x[1] - x[0] * x[0]);
        r[1] = 1.0 - x[0];
    }
    [[nodiscard]] crd::usize num_residuals() const noexcept override { return 2; }
    [[nodiscard]] crd::usize n() const noexcept override { return 2; }
    [[nodiscard]] bool jacobian(crd::containers::ConstSpan<crd::f64> x,
                                crd::containers::Span<crd::f64> jac) const override
    {
        jac[0 * 2 + 0] = -20.0 * x[0];
        jac[0 * 2 + 1] = 10.0;
        jac[1 * 2 + 0] = -1.0;
        jac[1 * 2 + 1] = 0.0;
        return true;
    }
};
} // namespace

TEST_CASE("v7-e-1 LM converges on an exponential curve fit", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    constexpr crd::usize m = 10;
    crd::f64             t[m];
    crd::f64             y[m];
    const crd::f64       a_true = 2.0;
    const crd::f64       b_true = 0.5;
    for (crd::usize i = 0; i < m; ++i)
    {
        t[i] = 0.1 * static_cast<crd::f64>(i);
        y[i] = a_true * std::exp(b_true * t[i]); // clean data ⇒ cost → 0
    }
    ExpFit res(t, y, m);

    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = 1.0; // start away from (2, 0.5)
    x0[1] = 0.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 100;
    auto r = opt::minimize_levenberg_marquardt<crd::f64>(res, {x0.data(), 2}, opts, &alloc);

    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK_THAT(r.x[0], WithinAbs(a_true, 1e-5));
    CHECK_THAT(r.x[1], WithinAbs(b_true, 1e-5));
    CHECK(r.fx < 1e-12); // ½‖r‖² → 0 on clean data
}

TEST_CASE("v7-e-1 LM solves the 2-residual Rosenbrock", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    RosenbrockLS res;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = -1.2;
    x0[1] = 1.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 200;
    auto r = opt::minimize_levenberg_marquardt<crd::f64>(res, {x0.data(), 2}, opts, &alloc);

    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK_THAT(r.x[0], WithinAbs(1.0, 1e-6));
    CHECK_THAT(r.x[1], WithinAbs(1.0, 1e-6));
    CHECK(r.fx < 1e-12);
}

TEST_CASE("v7-e-1 Gauss-Newton converges on a well-conditioned fit", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    constexpr crd::usize m = 10;
    crd::f64             t[m];
    crd::f64             y[m];
    for (crd::usize i = 0; i < m; ++i)
    {
        t[i] = 0.1 * static_cast<crd::f64>(i);
        y[i] = 2.0 * std::exp(0.5 * t[i]);
    }
    ExpFit res(t, y, m);
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = 1.8; // close start ⇒ undamped GN is safe
    x0[1] = 0.4;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 100;
    auto r = opt::minimize_gauss_newton<crd::f64>(res, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r.status == opt::OptStatus::Success);
    CHECK_THAT(r.x[0], WithinAbs(2.0, 1e-5));
    CHECK_THAT(r.x[1], WithinAbs(0.5, 1e-5));
}

TEST_CASE("v7-e-1 robust Cauchy loss resists an outlier", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    constexpr crd::usize m = 12;
    crd::f64             t[m];
    crd::f64             y[m];
    const crd::f64       a_true = 2.0;
    const crd::f64       b_true = 0.5;
    for (crd::usize i = 0; i < m; ++i)
    {
        t[i] = 0.1 * static_cast<crd::f64>(i);
        y[i] = a_true * std::exp(b_true * t[i]);
    }
    y[5] += 5.0; // a gross outlier

    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 200;

    ExpFit res(t, y, m);
    x0[0] = 1.0;
    x0[1] = 0.0;
    auto r_plain = opt::minimize_levenberg_marquardt<crd::f64>(res, {x0.data(), 2}, opts, &alloc);
    x0[0] = 1.0;
    x0[1] = 0.0;
    auto r_robust = opt::minimize_levenberg_marquardt<crd::f64>(res, {x0.data(), 2}, opts, &alloc, 1e-3, false,
                                                                opt::RobustLoss::Cauchy, 0.5);

    const crd::f64 err_plain = std::fabs(r_plain.x[0] - a_true) + std::fabs(r_plain.x[1] - b_true);
    const crd::f64 err_robust = std::fabs(r_robust.x[0] - a_true) + std::fabs(r_robust.x[1] - b_true);
    CHECK(r_robust.status == opt::OptStatus::Success);
    CHECK(err_robust < err_plain); // the robust fit is closer to truth despite the outlier
    CHECK(err_robust < 0.1);
}

TEST_CASE("v7-e-1 LM run-twice bit-identity (determinism by construction)", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    RosenbrockLS res;
    crd::containers::Array<crd::f64> x0(&alloc);
    x0.resize(2);
    x0[0] = -1.2;
    x0[1] = 1.0;
    opt::OptOptions<crd::f64> opts;
    opts.grad_tol = 1e-10;
    opts.max_iters = 200;
    auto r1 = opt::minimize_levenberg_marquardt<crd::f64>(res, {x0.data(), 2}, opts, &alloc);
    auto r2 = opt::minimize_levenberg_marquardt<crd::f64>(res, {x0.data(), 2}, opts, &alloc);
    REQUIRE(r1.iterations == r2.iterations);
    CHECK(r1.x[0] == r2.x[0]);
    CHECK(r1.x[1] == r2.x[1]);
    CHECK(r1.fx == r2.fx);
}
