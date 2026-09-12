// v9-b gates: the embedded adaptive ERK family — empirical order slopes per method (the oracle-free
// tableau certificate: a coefficient typo destroys the slope), tolerance-tracking accuracy, the Arenstorf
// orbit (the classic DOPRI showcase: a periodic three-body orbit that must CLOSE), exact deterministic
// counters, run-twice bit identity, and status paths. scipy step-sequence cross-verification is the
// WSL-side difftest (session log).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

namespace
{

// Fixed-h order probe: run the ERK with rtol huge (so the controller never rejects) and h0 = hmax = h
// pinning the step. Returns |y(1) - exact| for y' = -2*t*y^2, y(0) = 1 (exact 1/(1+t^2)).
f64 erk_fixed_h_error(ode::ErkMethod method, f64 h, crd::memory::IAllocator* alloc)
{
    const auto f = ode::make_ode_function<f64>(
        1, [](f64 t, containers::ConstSpan<f64> y, containers::Span<f64> dydt)
        { dydt[0] = -2.0 * t * y[0] * y[0]; });
    containers::Array<f64> y(alloc);
    y.resize(1);
    y[0] = 1.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e10; // never reject; never grow beyond hmax
    opts.atol = 1e10;
    opts.h0 = h;
    opts.hmax = h;
    const ode::OdeResult<f64> r =
        ode::integrate_erk<f64>(f, 0.0, 1.0, containers::Span<f64>(y.data(), 1), opts, alloc, method);
    REQUIRE(r.success);
    return std::abs(y[0] - 0.5);
}

} // namespace

TEST_CASE("erk: empirical order slopes certify every tableau", "[ode][erk]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    struct Row
    {
        ode::ErkMethod method;
        f64 expected_order;
        f64 h1; // coarse h (DOP853 needs larger h to stay above roundoff)
        f64 slack_lo;
        f64 slack_hi;
    };
    const Row rows[5] = {
        {ode::ErkMethod::Rk23, 3.0, 1.0 / 32.0, 0.4, 0.6},
        {ode::ErkMethod::Rk45, 5.0, 1.0 / 16.0, 0.4, 0.6},
        {ode::ErkMethod::CashKarp, 5.0, 1.0 / 16.0, 0.4, 0.6},
        {ode::ErkMethod::Tsit5, 5.0, 1.0 / 16.0, 0.4, 0.6},
        {ode::ErkMethod::Dop853, 8.0, 1.0 / 4.0, 0.8, 1.2},
    };
    for (const Row& row : rows)
    {
        const f64 e1 = erk_fixed_h_error(row.method, row.h1, &alloc);
        const f64 e2 = erk_fixed_h_error(row.method, row.h1 / 2.0, &alloc);
        const f64 p_hat = std::log2(e1 / e2);
        INFO("method " << static_cast<int>(row.method) << "  e1=" << e1 << "  e2=" << e2 << "  p_hat=" << p_hat);
        CHECK(p_hat > row.expected_order - row.slack_lo);
        CHECK(p_hat < row.expected_order + row.slack_hi);
    }
}

TEST_CASE("erk: adaptive accuracy tracks the tolerance", "[ode][erk]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // y' = -y over [0, 5]; exact e^{-5}. At rtol = atol = 1e-10 every method must land well inside 1e-8.
    const auto f = ode::make_ode_function<f64>(
        1, [](f64, containers::ConstSpan<f64> y, containers::Span<f64> dydt) { dydt[0] = -y[0]; });
    const f64 exact = std::exp(-5.0);

    const ode::ErkMethod methods[5] = {ode::ErkMethod::Rk23, ode::ErkMethod::Rk45, ode::ErkMethod::CashKarp,
                                       ode::ErkMethod::Tsit5, ode::ErkMethod::Dop853};
    for (const ode::ErkMethod m : methods)
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-10;
        opts.atol = 1e-10;
        const ode::OdeResult<f64> r =
            ode::integrate_erk<f64>(f, 0.0, 5.0, containers::Span<f64>(y.data(), 1), opts, &alloc, m);
        REQUIRE(r.success);
        INFO("method " << static_cast<int>(m) << "  err=" << std::abs(y[0] - exact) << "  naccept="
                       << r.work.naccept << "  nreject=" << r.work.nreject << "  nfev=" << r.work.nfev);
        CHECK(std::abs(y[0] - exact) < 1e-8);
        // Counter identities: every attempt costs (stages-1) + 1 evals; +2 init (f0 + select_initial_step).
        usize stages = 6;
        if (m == ode::ErkMethod::Rk23)
        {
            stages = 3;
        }
        else if (m == ode::ErkMethod::Dop853)
        {
            stages = 12;
        }
        CHECK(r.work.nfev == 2 + r.work.nsteps * stages);
        CHECK(r.work.nsteps == r.work.naccept + r.work.nreject);
    }
}

TEST_CASE("erk: the Arenstorf orbit closes (the classic DOPRI showcase)", "[ode][erk]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // Restricted three-body (Hairer I, p.130): mu = 0.012277471; the closed orbit with period
    // T = 17.0652165601579625588917206249. State (x, y, vx, vy).
    const f64 mu = 0.012277471;
    const f64 mu1 = 1.0 - mu;
    const auto f = ode::make_ode_function<f64>(
        4,
        [mu, mu1](f64, containers::ConstSpan<f64> s, containers::Span<f64> d)
        {
            const f64 x = s[0];
            const f64 y = s[1];
            const f64 d1 = std::pow((x + mu) * (x + mu) + y * y, 1.5);
            const f64 d2 = std::pow((x - mu1) * (x - mu1) + y * y, 1.5);
            d[0] = s[2];
            d[1] = s[3];
            d[2] = x + 2.0 * s[3] - mu1 * (x + mu) / d1 - mu * (x - mu1) / d2;
            d[3] = y - 2.0 * s[2] - mu1 * y / d1 - mu * y / d2;
        });

    const f64 period = 17.0652165601579625588917206249;
    containers::Array<f64> y(&alloc);
    y.resize(4);
    const f64 y0[4] = {0.994, 0.0, 0.0, -2.00158510637908252240537862224};
    for (int i = 0; i < 4; ++i)
    {
        y[i] = y0[i];
    }

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-11;
    opts.atol = 1e-11;
    const ode::OdeResult<f64> r =
        ode::integrate_erk<f64>(f, 0.0, period, containers::Span<f64>(y.data(), 4), opts, &alloc,
                                ode::ErkMethod::Dop853);
    REQUIRE(r.success);
    // One full period returns to the start (the orbit is unstable — tight tolerances are REQUIRED).
    INFO("returned (" << y[0] << ", " << y[1] << ")  nfev=" << r.work.nfev);
    CHECK(std::abs(y[0] - y0[0]) < 1e-5);
    CHECK(std::abs(y[1] - y0[1]) < 1e-5);
}

TEST_CASE("erk: run-twice bit identity (state and every counter)", "[ode][erk][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    const auto f = ode::make_ode_function<f64>(
        2,
        [](f64, containers::ConstSpan<f64> y, containers::Span<f64> d)
        {
            d[0] = y[1];
            d[1] = (1.0 - y[0] * y[0]) * y[1] - y[0]; // Van der Pol mu=1 (nonstiff regime)
        });

    auto run = [&](containers::Array<f64>& y) -> ode::OdeResult<f64>
    {
        y.resize(2);
        y[0] = 2.0;
        y[1] = 0.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-8;
        opts.atol = 1e-8;
        return ode::integrate_erk<f64>(f, 0.0, 10.0, containers::Span<f64>(y.data(), 2), opts, &alloc,
                                       ode::ErkMethod::Rk45);
    };
    containers::Array<f64> y1(&alloc);
    containers::Array<f64> y2(&alloc);
    const ode::OdeResult<f64> r1 = run(y1);
    const ode::OdeResult<f64> r2 = run(y2);
    REQUIRE(r1.success);
    CHECK(std::memcmp(y1.data(), y2.data(), 2 * sizeof(f64)) == 0);
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.naccept == r2.work.naccept);
    CHECK(r1.work.nreject == r2.work.nreject);
}

TEST_CASE("erk: status paths (backward, max_steps, t1 == t0)", "[ode][erk]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto f = ode::make_ode_function<f64>(
        1, [](f64, containers::ConstSpan<f64> y, containers::Span<f64> dydt) { dydt[0] = -y[0]; });

    SECTION("backward integration recovers growth")
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-10;
        opts.atol = 1e-12;
        const ode::OdeResult<f64> r =
            ode::integrate_erk<f64>(f, 1.0, 0.0, containers::Span<f64>(y.data(), 1), opts, &alloc);
        REQUIRE(r.success);
        CHECK(y[0] == Catch::Approx(std::exp(1.0)).epsilon(1e-8));
    }

    SECTION("max_steps caps the work and reports honestly")
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-12;
        opts.atol = 1e-14;
        opts.max_steps = 3;
        const ode::OdeResult<f64> r =
            ode::integrate_erk<f64>(f, 0.0, 100.0, containers::Span<f64>(y.data(), 1), opts, &alloc);
        CHECK_FALSE(r.success);
        CHECK(r.status == ode::OdeStatus::MaxSteps);
        CHECK(r.work.nsteps == 3);
        CHECK(r.t < 100.0);
    }

    SECTION("t1 == t0 trivial success")
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 7.0;
        const ode::OdeResult<f64> r =
            ode::integrate_erk<f64>(f, 2.0, 2.0, containers::Span<f64>(y.data(), 1), ode::OdeOptions<f64>{}, &alloc);
        CHECK(r.success);
        CHECK(y[0] == 7.0);
        CHECK(r.work.nfev == 0);
    }
}
