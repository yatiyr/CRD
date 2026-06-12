// v9-a gates: the fixed-step DRIVER — wiring through the OdeFunction contract, EXACT deterministic work
// counters (the work-precision currency), run-twice bit-identity, status paths, and edge dimensions.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/integrate.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>
#include <limits>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

namespace
{

// y' = -y, y(0) = 1: exact e^{-t}.
class DecayFunction final : public ode::OdeFunction<f64>
{
public:
    void rhs(f64 /*t*/, containers::ConstSpan<f64> y, containers::Span<f64> dydt) const override
    {
        dydt[0] = -y[0];
    }
    [[nodiscard]] usize dim() const noexcept override { return 1; }
};

} // namespace

TEST_CASE("integrate_fixed: accuracy + EXACT work counters per method", "[ode][integrate]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    DecayFunction f;
    const f64 exact = std::exp(-1.0);

    struct Row
    {
        ode::FixedMethod method;
        crd::u64 fev_per_step;
        f64 tol;
    };
    const Row rows[3] = {
        {ode::FixedMethod::Euler, 1, 5e-3},
        {ode::FixedMethod::Midpoint, 2, 1e-5},
        {ode::FixedMethod::Rk4, 4, 1e-10},
    };

    for (const Row& row : rows)
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        const usize nsteps = 100;
        const ode::OdeResult<f64> r =
            ode::integrate_fixed<f64>(f, 0.0, 1.0, nsteps, containers::Span<f64>(y.data(), 1), &alloc, row.method);

        CHECK(r.success);
        CHECK(r.status == ode::OdeStatus::Success);
        CHECK(r.t == 1.0);
        // Deterministic counters, exactly: nsteps attempts, all accepted, fev = per-step cost * steps.
        CHECK(r.work.nsteps == nsteps);
        CHECK(r.work.naccept == nsteps);
        CHECK(r.work.nreject == 0);
        CHECK(r.work.nfev == row.fev_per_step * nsteps);
        CHECK(r.work.njev == 0);
        CHECK(std::abs(y[0] - exact) < row.tol);
    }
}

TEST_CASE("integrate_fixed: run-twice bit identity (state AND counters)", "[ode][integrate][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    // A 3-state nonlinear system (Lorenz, the classic chaotic stress: bit-divergence amplifies fast).
    auto lorenz = [](f64 /*t*/, containers::ConstSpan<f64> y, containers::Span<f64> dydt)
    {
        const f64 sigma = 10.0;
        const f64 rho = 28.0;
        const f64 beta = 8.0 / 3.0;
        dydt[0] = sigma * (y[1] - y[0]);
        dydt[1] = y[0] * (rho - y[2]) - y[1];
        dydt[2] = y[0] * y[1] - beta * y[2];
    };
    const auto f = ode::make_ode_function<f64>(3, lorenz);

    auto run = [&](containers::Array<f64>& y) -> ode::OdeResult<f64>
    {
        y.resize(3);
        y[0] = 1.0;
        y[1] = 1.0;
        y[2] = 1.0;
        return ode::integrate_fixed<f64>(f, 0.0, 5.0, 5000, containers::Span<f64>(y.data(), 3), &alloc,
                                         ode::FixedMethod::Rk4);
    };

    containers::Array<f64> y1(&alloc);
    containers::Array<f64> y2(&alloc);
    const ode::OdeResult<f64> r1 = run(y1);
    const ode::OdeResult<f64> r2 = run(y2);

    REQUIRE(r1.success);
    REQUIRE(r2.success);
    CHECK(std::memcmp(y1.data(), y2.data(), 3 * sizeof(f64)) == 0); // bit-identical trajectory endpoint
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.nsteps == r2.work.nsteps);
}

TEST_CASE("integrate_fixed: status paths and edges", "[ode][integrate]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    DecayFunction f;

    SECTION("t1 == t0 is an immediate success with zero work")
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 3.5;
        const ode::OdeResult<f64> r =
            ode::integrate_fixed<f64>(f, 2.0, 2.0, 0, containers::Span<f64>(y.data(), 1), &alloc);
        CHECK(r.success);
        CHECK(r.work.nfev == 0);
        CHECK(y[0] == 3.5);
    }

    SECTION("n == 0 (empty state) is a defined success")
    {
        const auto empty = ode::make_ode_function<f64>(
            0, [](f64, containers::ConstSpan<f64>, containers::Span<f64>) {});
        const ode::OdeResult<f64> r = ode::integrate_fixed<f64>(empty, 0.0, 1.0, 10, {}, &alloc);
        CHECK(r.success);
        CHECK(r.work.nfev == 0);
    }

    SECTION("zero steps over a nonzero span is InvalidInput")
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        const ode::OdeResult<f64> r =
            ode::integrate_fixed<f64>(f, 0.0, 1.0, 0, containers::Span<f64>(y.data(), 1), &alloc);
        CHECK_FALSE(r.success);
        CHECK(r.status == ode::OdeStatus::InvalidInput);
    }

    SECTION("non-finite time bounds are InvalidInput")
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        const f64 inf = std::numeric_limits<f64>::infinity();
        const ode::OdeResult<f64> r =
            ode::integrate_fixed<f64>(f, 0.0, inf, 10, containers::Span<f64>(y.data(), 1), &alloc);
        CHECK(r.status == ode::OdeStatus::InvalidInput);
    }

    SECTION("a blowing-up state reports NotFinite, not garbage")
    {
        // y' = y^2 from y(0) = 1 blows up at t = 1; stepping past the singularity roughly squares the
        // magnitude per step (doubling the exponent), so ten steps overflow f64 with room to spare.
        const auto blowup = ode::make_ode_function<f64>(
            1, [](f64, containers::ConstSpan<f64> y, containers::Span<f64> dydt) { dydt[0] = y[0] * y[0]; });
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        const ode::OdeResult<f64> r =
            ode::integrate_fixed<f64>(blowup, 0.0, 2.0, 10, containers::Span<f64>(y.data(), 1), &alloc,
                                      ode::FixedMethod::Rk4);
        CHECK_FALSE(r.success);
        CHECK(r.status == ode::OdeStatus::NotFinite);
    }

    SECTION("backward integration (t1 < t0) works: decay reversed recovers growth")
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        const ode::OdeResult<f64> r =
            ode::integrate_fixed<f64>(f, 1.0, 0.0, 100, containers::Span<f64>(y.data(), 1), &alloc);
        CHECK(r.success);
        CHECK(y[0] == Catch::Approx(std::exp(1.0)).epsilon(1e-9));
    }
}

TEST_CASE("OdeFunction: capability defaults and the functor adapter", "[ode][contract]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    DecayFunction f;
    CHECK_FALSE(f.has_jacobian());
    CHECK_FALSE(f.has_jacobian_vector());

    // Unprovided capabilities return false ("not filled") per the contract.
    containers::Array<f64> jac(&alloc);
    jac.resize(1);
    CHECK_FALSE(f.jacobian(0.0, {}, containers::Span<f64>(jac.data(), 1)));

    const auto g = ode::make_ode_function<f64>(
        2, [](f64 t, containers::ConstSpan<f64> y, containers::Span<f64> dydt)
        {
            dydt[0] = y[1];
            dydt[1] = t;
        });
    CHECK(g.dim() == 2);
    f64 yv[2] = {0.0, 7.0};
    f64 dv[2] = {0.0, 0.0};
    g.rhs(3.0, containers::ConstSpan<f64>(yv, 2), containers::Span<f64>(dv, 2));
    CHECK(dv[0] == 7.0);
    CHECK(dv[1] == 3.0);
}
