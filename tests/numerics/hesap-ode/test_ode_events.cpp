// v9-c gates: OdeSolution dense output (Hermite-exact sampling, segment lookup both directions) and
// event detection with scipy semantics (terminal stop at an ANALYTICALLY KNOWN root, direction filtering,
// non-terminal hit recording).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

TEST_CASE("solution: dense output is interpolation-exact where Hermite is exact", "[ode][solution]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // y' = cos(t), y(0) = 0 => y = sin(t). The Hermite fallback's interpolation error is O(h^4):
    // h^4/384 · max|y''''| = 0.1^4/384 ≈ 2.6e-7 at hmax = 0.1 — the gate encodes THE HERMITE BOUND,
    // not the integrator tolerance (native per-method interpolants are the named follow-up that
    // tightens this).
    const auto f = ode::make_ode_function<f64>(
        1, [](f64 t, containers::ConstSpan<f64>, containers::Span<f64> dydt) { dydt[0] = std::cos(t); });

    containers::Array<f64> y(&alloc);
    y.resize(1);
    y[0] = 0.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-10;
    opts.atol = 1e-12;
    opts.hmax = 0.1;
    ode::OdeSolution<f64> sol(&alloc);
    const ode::OdeResult<f64> r = ode::integrate_erk<f64>(f, 0.0, 6.0, containers::Span<f64>(y.data(), 1), opts,
                                                          &alloc, ode::ErkMethod::Rk45, &sol);
    REQUIRE(r.success);
    REQUIRE(sol.num_nodes() >= 3);

    containers::Array<f64> out(&alloc);
    out.resize(1);
    for (int k = 0; k <= 60; ++k)
    {
        const f64 t = 0.1 * static_cast<f64>(k);
        sol.eval(t, containers::Span<f64>(out.data(), 1));
        CHECK(std::abs(out[0] - std::sin(t)) < 3e-7); // the h^4/384 bound at hmax = 0.1
    }

    // Node endpoints reproduce the recorded states exactly (theta = 0/1 paths).
    sol.eval(sol.t_node(0), containers::Span<f64>(out.data(), 1));
    CHECK(out[0] == sol.y_node(0)[0]);
}

TEST_CASE("solution: backward trajectories locate segments correctly", "[ode][solution]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    const auto f = ode::make_ode_function<f64>(
        1, [](f64, containers::ConstSpan<f64> y, containers::Span<f64> dydt) { dydt[0] = -y[0]; });

    containers::Array<f64> y(&alloc);
    y.resize(1);
    y[0] = 1.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-10;
    opts.atol = 1e-12;
    opts.hmax = 0.1; // bound the Hermite O(h^4) interpolation error (max|y''''| = e^2 here)
    ode::OdeSolution<f64> sol(&alloc);
    const ode::OdeResult<f64> r = ode::integrate_erk<f64>(f, 2.0, 0.0, containers::Span<f64>(y.data(), 1), opts,
                                                          &alloc, ode::ErkMethod::Rk45, &sol);
    REQUIRE(r.success);

    containers::Array<f64> out(&alloc);
    out.resize(1);
    // Initial y(2) = 1 => y(t) = e^{2-t}.
    for (int k = 0; k <= 20; ++k)
    {
        const f64 t = 0.1 * static_cast<f64>(k);
        sol.eval(t, containers::Span<f64>(out.data(), 1));
        CHECK(std::abs(out[0] - std::exp(2.0 - t)) < 2e-6); // h^4/384 · e^2 ≈ 1.9e-6 at hmax = 0.1
    }
}

TEST_CASE("events: terminal event stops at the analytic root", "[ode][events]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // Projectile: y'' = -g  =>  state (y, v); y(t) = y0 - g t^2 / 2 hits 0 at t* = sqrt(2 y0 / g).
    const f64 grav = 9.81;
    const f64 y0 = 10.0;
    const auto f = ode::make_ode_function<f64>(
        2,
        [grav](f64, containers::ConstSpan<f64> s, containers::Span<f64> d)
        {
            d[0] = s[1];
            d[1] = -grav;
        });

    using G = f64 (*)(f64, containers::ConstSpan<f64>);
    ode::FunctorOdeEvent<f64, G> ground([](f64, containers::ConstSpan<f64> s) -> f64 { return s[0]; },
                                        /*direction*/ -1.0, /*terminal*/ true);

    containers::Array<f64> y(&alloc);
    y.resize(2);
    y[0] = y0;
    y[1] = 0.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-10;
    opts.atol = 1e-12;
    ode::OdeEvent<f64>* events[1] = {&ground};
    const ode::OdeResult<f64> r =
        ode::integrate_erk<f64>(f, 0.0, 10.0, containers::Span<f64>(y.data(), 2), opts, &alloc,
                                ode::ErkMethod::Rk45, nullptr, containers::ConstSpan<ode::OdeEvent<f64>*>(events, 1));

    const f64 t_star = std::sqrt(2.0 * y0 / grav);
    CHECK(r.status == ode::OdeStatus::EventTerminal);
    CHECK(r.event_index == 0);
    CHECK(r.t == Catch::Approx(t_star).epsilon(1e-9));
    CHECK(std::abs(y[0]) < 1e-8); // the state was truncated to the event point
    CHECK(y[1] == Catch::Approx(-grav * t_star).epsilon(1e-8));
}

TEST_CASE("events: direction filtering + non-terminal hit recording", "[ode][events]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // y = sin(t): y' = cos(t). Start at t0 = 0.5 (NOT on a zero — starting exactly at g = 0 records a
    // legitimate hit at t0 under scipy semantics). Crossings in (0.5, 7]: pi (down) and 2*pi (up).
    const auto f = ode::make_ode_function<f64>(
        1, [](f64 t, containers::ConstSpan<f64>, containers::Span<f64> dydt) { dydt[0] = std::cos(t); });

    containers::Array<f64> up_hits(&alloc);
    containers::Array<f64> down_hits(&alloc);
    using G = f64 (*)(f64, containers::ConstSpan<f64>);
    const G g = [](f64, containers::ConstSpan<f64> s) -> f64 { return s[0]; };
    ode::FunctorOdeEvent<f64, G> up_event(g, +1.0, false, &up_hits);
    ode::FunctorOdeEvent<f64, G> down_event(g, -1.0, false, &down_hits);

    containers::Array<f64> y(&alloc);
    y.resize(1);
    y[0] = std::sin(0.5);
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-10;
    opts.atol = 1e-12;
    ode::OdeEvent<f64>* events[2] = {&up_event, &down_event};
    const ode::OdeResult<f64> r =
        ode::integrate_erk<f64>(f, 0.5, 7.0, containers::Span<f64>(y.data(), 1), opts, &alloc,
                                ode::ErkMethod::Rk45, nullptr, containers::ConstSpan<ode::OdeEvent<f64>*>(events, 2));
    REQUIRE(r.success); // non-terminal events never stop the run

    const f64 pi = 3.14159265358979323846;
    REQUIRE(down_hits.size() == 1);
    CHECK(down_hits[0] == Catch::Approx(pi).epsilon(1e-8));
    REQUIRE(up_hits.size() == 1);
    CHECK(up_hits[0] == Catch::Approx(2.0 * pi).epsilon(1e-8));
}
