// v9-d gates: BDF/NDF — exact-solution stiff accuracy, the Robertson problem (the classic stiff battery),
// THE CRUSH GATE (BDF beats the explicit family by orders of magnitude in evals on stiff problems —
// that's what a stiff solver is FOR), analytic-vs-FD Jacobian agreement, determinism. scipy
// trajectory-exactness with analytic Jacobians = the WSL difftest (session log).

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

namespace
{

// Stiff linear 2-state with EXACT solution: y' = diag(-1, -1000)·y; y_i(t) = y_i(0)·e^{lambda_i t}.
class StiffLinear final : public ode::OdeFunction<f64>
{
public:
    StiffLinear() : ode::OdeFunction<f64>(/*has_jacobian*/ true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -y[0];
        d[1] = -1000.0 * y[1];
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> jac) const override
    {
        jac[0] = -1.0;
        jac[1] = 0.0;
        jac[2] = 0.0;
        jac[3] = -1000.0;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
};

// Robertson (ROBER) — the classic stiff chemistry battery; conservation: y1 + y2 + y3 = 1.
class Robertson final : public ode::OdeFunction<f64>
{
public:
    Robertson() : ode::OdeFunction<f64>(true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -0.04 * y[0] + 1e4 * y[1] * y[2];
        d[1] = 0.04 * y[0] - 1e4 * y[1] * y[2] - 3e7 * y[1] * y[1];
        d[2] = 3e7 * y[1] * y[1];
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64> y, containers::Span<f64> j) const override
    {
        j[0] = -0.04;
        j[1] = 1e4 * y[2];
        j[2] = 1e4 * y[1];
        j[3] = 0.04;
        j[4] = -1e4 * y[2] - 6e7 * y[1];
        j[5] = -1e4 * y[1];
        j[6] = 0.0;
        j[7] = 6e7 * y[1];
        j[8] = 0.0;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 3; }
};

// Van der Pol mu = 1000 (the stiff showcase).
class VdpStiff final : public ode::OdeFunction<f64>
{
public:
    VdpStiff() : ode::OdeFunction<f64>(true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = y[1];
        d[1] = 1000.0 * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64> y, containers::Span<f64> j) const override
    {
        j[0] = 0.0;
        j[1] = 1.0;
        j[2] = 1000.0 * (-2.0 * y[0] * y[1]) - 1.0;
        j[3] = 1000.0 * (1.0 - y[0] * y[0]);
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
};

} // namespace

TEST_CASE("bdf: stiff-linear exact solution at tolerance", "[ode][bdf]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    StiffLinear f;

    containers::Array<f64> y(&alloc);
    y.resize(2);
    y[0] = 1.0;
    y[1] = 1.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-8;
    opts.atol = 1e-10;
    const ode::OdeResult<f64> r =
        ode::integrate_bdf<f64>(f, 0.0, 2.0, containers::Span<f64>(y.data(), 2), opts, &alloc);
    REQUIRE(r.success);
    CHECK(std::abs(y[0] - std::exp(-2.0)) < 1e-6);
    CHECK(std::abs(y[1] - std::exp(-2000.0)) < 1e-10); // ~0 — the fast mode must have died cleanly
    CHECK(r.work.njev >= 1);
    CHECK(r.work.nlu >= 1);
    INFO("naccept=" << r.work.naccept << " nfev=" << r.work.nfev << " nlu=" << r.work.nlu);
}

TEST_CASE("bdf: Robertson - accuracy + conservation + 1e5 horizon", "[ode][bdf]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    Robertson f;

    containers::Array<f64> y(&alloc);
    y.resize(3);
    y[0] = 1.0;
    y[1] = 0.0;
    y[2] = 0.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-8;
    opts.atol = 1e-12; // ROBER needs tight atol on the small y2 component
    const ode::OdeResult<f64> r =
        ode::integrate_bdf<f64>(f, 0.0, 1e5, containers::Span<f64>(y.data(), 3), opts, &alloc);
    REQUIRE(r.success);
    // Conservation (exact in the ODE): y1 + y2 + y3 == 1.
    CHECK(std::abs(y[0] + y[1] + y[2] - 1.0) < 1e-7);
    // The SUNDIALS-published ROBER decay curve: y1 = 0.03898 at t = 4e4 and 0.004940 at t = 4e5 ⇒ at
    // t = 1e5 y1 sits in (0.005, 0.04); quantitative pinning vs scipy BDF lives in the difftest.
    CHECK(y[0] > 0.005);
    CHECK(y[0] < 0.04);
    CHECK(y[1] > 0.0);
    CHECK(y[1] < 1e-5);
    INFO("y = " << y[0] << ", " << y[1] << ", " << y[2] << "; naccept=" << r.work.naccept << " nfev=" << r.work.nfev
                << " njev=" << r.work.njev << " nlu=" << r.work.nlu);
}

TEST_CASE("bdf: THE STIFF CRUSH - BDF beats RK45 by >100x evals on VdP mu=1000", "[ode][bdf][crush]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    VdpStiff f;

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-6;
    opts.atol = 1e-8;

    containers::Array<f64> yb(&alloc);
    yb.resize(2);
    yb[0] = 2.0;
    yb[1] = 0.0;
    const ode::OdeResult<f64> rb =
        ode::integrate_bdf<f64>(f, 0.0, 300.0, containers::Span<f64>(yb.data(), 2), opts, &alloc);
    REQUIRE(rb.success);

    containers::Array<f64> yr(&alloc);
    yr.resize(2);
    yr[0] = 2.0;
    yr[1] = 0.0;
    const ode::OdeResult<f64> rr =
        ode::integrate_erk<f64>(f, 0.0, 300.0, containers::Span<f64>(yr.data(), 2), opts, &alloc, ode::ErkMethod::Rk45);
    REQUIRE(rr.success);

    // Both arrive at the same answer...
    CHECK(std::abs(yb[0] - yr[0]) < 1e-3);
    // ...but the explicit method pays the stability-bound price (h ~ 1/1000 forever) while BDF cruises.
    INFO("BDF nfev=" << rb.work.nfev << " (naccept=" << rb.work.naccept << ", nlu=" << rb.work.nlu
                     << ")  RK45 nfev=" << rr.work.nfev << " (naccept=" << rr.work.naccept << ")");
    CHECK(rr.work.nfev > 100 * rb.work.nfev);
}

TEST_CASE("bdf: FD-Jacobian fallback agrees with analytic", "[ode][bdf]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // The same VdP-1000 RHS WITHOUT has_jacobian: the FD path must converge to the same answer.
    const auto fd = ode::make_ode_function<f64>(2,
                                                [](f64, containers::ConstSpan<f64> y, containers::Span<f64> d)
                                                {
                                                    d[0] = y[1];
                                                    d[1] = 1000.0 * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
                                                });
    VdpStiff an;

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-7;
    opts.atol = 1e-9;

    containers::Array<f64> y1(&alloc);
    y1.resize(2);
    y1[0] = 2.0;
    y1[1] = 0.0;
    REQUIRE(ode::integrate_bdf<f64>(fd, 0.0, 100.0, containers::Span<f64>(y1.data(), 2), opts, &alloc).success);

    containers::Array<f64> y2(&alloc);
    y2.resize(2);
    y2[0] = 2.0;
    y2[1] = 0.0;
    REQUIRE(ode::integrate_bdf<f64>(an, 0.0, 100.0, containers::Span<f64>(y2.data(), 2), opts, &alloc).success);

    CHECK(std::abs(y1[0] - y2[0]) < 1e-5);
    CHECK(std::abs(y1[1] - y2[1]) < 1e-5);
}

TEST_CASE("bdf: run-twice bit identity (state and every counter)", "[ode][bdf][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    Robertson f;

    auto run = [&](containers::Array<f64>& y) -> ode::OdeResult<f64>
    {
        y.resize(3);
        y[0] = 1.0;
        y[1] = 0.0;
        y[2] = 0.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-7;
        opts.atol = 1e-10;
        return ode::integrate_bdf<f64>(f, 0.0, 1000.0, containers::Span<f64>(y.data(), 3), opts, &alloc);
    };
    containers::Array<f64> a(&alloc);
    containers::Array<f64> b(&alloc);
    const ode::OdeResult<f64> r1 = run(a);
    const ode::OdeResult<f64> r2 = run(b);
    REQUIRE(r1.success);
    CHECK(std::memcmp(a.data(), b.data(), 3 * sizeof(f64)) == 0);
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.njev == r2.work.njev);
    CHECK(r1.work.nlu == r2.work.nlu);
    CHECK(r1.work.nsol == r2.work.nsol);
    CHECK(r1.work.naccept == r2.work.naccept);
}

TEST_CASE("bdf: backward integration + trivial edges", "[ode][bdf]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    StiffLinear f;

    SECTION("backward")
    {
        containers::Array<f64> y(&alloc);
        y.resize(2);
        y[0] = std::exp(-1.0);
        y[1] = 0.0; // the fast mode stays 0 (its backward growth is not exercised)
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-9;
        opts.atol = 1e-12;
        const ode::OdeResult<f64> r =
            ode::integrate_bdf<f64>(f, 1.0, 0.0, containers::Span<f64>(y.data(), 2), opts, &alloc);
        REQUIRE(r.success);
        CHECK(y[0] == Catch::Approx(1.0).epsilon(1e-7));
    }

    SECTION("t1 == t0")
    {
        containers::Array<f64> y(&alloc);
        y.resize(2);
        y[0] = 3.0;
        y[1] = 4.0;
        const ode::OdeResult<f64> r =
            ode::integrate_bdf<f64>(f, 5.0, 5.0, containers::Span<f64>(y.data(), 2), ode::OdeOptions<f64>{}, &alloc);
        CHECK(r.success);
        CHECK(y[0] == 3.0);
        CHECK(r.work.nfev == 0);
    }
}
