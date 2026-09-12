// v9-a gates: the stepper KERNELS — single-step algebraic exactness vs hand-computed values, empirical
// h-refinement order slopes (the oracle-free order certificate), and the in-place aliasing contract.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/steppers.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstring>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

namespace
{

// y' = lambda*y, n = 1 — every explicit RK step has a closed-form one-step amplification polynomial.
struct LinearRhs
{
    f64 lambda;
    void operator()(f64 /*t*/, containers::ConstSpan<f64> y, containers::Span<f64> dydt) const
    {
        dydt[0] = lambda * y[0];
    }
};

} // namespace

TEST_CASE("kernels: single-step amplification matches the hand-computed RK polynomials", "[ode][steppers]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    // h*lambda = -0.5 exactly representable: euler 1+z, midpoint 1+z+z^2/2, rk4 sum z^k/k!, k=0..4.
    const f64 lambda = -2.0;
    const f64 h = 0.25;
    const f64 z = h * lambda; // -0.5

    containers::Array<f64> scratch(&alloc);
    scratch.resize(ode::rk4_scratch(1));
    containers::Array<f64> y(&alloc);
    y.resize(1);
    const containers::ConstSpan<f64> y_in(y.data(), 1);
    const containers::Span<f64> y_out(y.data(), 1);
    const containers::Span<f64> sc(scratch.data(), scratch.size());
    LinearRhs rhs{lambda};

    y[0] = 1.0;
    ode::step_euler(rhs, 0.0, y_in, h, y_out, sc);
    CHECK(y[0] == 1.0 + z); // 0.5 — exact in FP

    y[0] = 1.0;
    ode::step_midpoint(rhs, 0.0, y_in, h, y_out, sc);
    CHECK(y[0] == 1.0 + z + 0.5 * z * z); // 0.625 — exact in FP

    y[0] = 1.0;
    ode::step_rk4(rhs, 0.0, y_in, h, y_out, sc);
    const f64 expected = 1.0 + z + z * z / 2.0 + z * z * z / 6.0 + z * z * z * z / 24.0;
    CHECK(y[0] == Catch::Approx(expected).epsilon(1e-15));
}

TEST_CASE("kernels: empirical h-refinement slopes certify orders 1 / 2 / 4", "[ode][steppers]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    // Nonlinear scalar IVP with a known solution: y' = -2*t*y^2, y(0) = 1  =>  y(t) = 1/(1+t^2).
    auto rhs = [](f64 t, containers::ConstSpan<f64> y, containers::Span<f64> dydt)
    { dydt[0] = -2.0 * t * y[0] * y[0]; };
    const f64 y_exact = 0.5; // y(1)

    containers::Array<f64> scratch(&alloc);
    scratch.resize(ode::rk4_scratch(1));
    const containers::Span<f64> sc(scratch.data(), scratch.size());

    auto run = [&](int method, usize nsteps) -> f64
    {
        containers::Array<f64> y(&alloc);
        y.resize(1);
        y[0] = 1.0;
        const containers::ConstSpan<f64> y_in(y.data(), 1);
        const containers::Span<f64> y_out(y.data(), 1);
        const f64 h = 1.0 / static_cast<f64>(nsteps);
        for (usize i = 0; i < nsteps; ++i)
        {
            const f64 t = static_cast<f64>(i) * h;
            if (method == 0)
            {
                ode::step_euler(rhs, t, y_in, h, y_out, sc);
            }
            else if (method == 1)
            {
                ode::step_midpoint(rhs, t, y_in, h, y_out, sc);
            }
            else
            {
                ode::step_rk4(rhs, t, y_in, h, y_out, sc);
            }
        }
        return std::abs(y[0] - y_exact);
    };

    const f64 expected_order[3] = {1.0, 2.0, 4.0};
    for (int method = 0; method < 3; ++method)
    {
        const f64 e1 = run(method, 64);
        const f64 e2 = run(method, 128);
        const f64 p_hat = std::log2(e1 / e2);
        INFO("method " << method << "  e(h)=" << e1 << "  e(h/2)=" << e2 << "  p_hat=" << p_hat);
        CHECK(p_hat > expected_order[method] - 0.25);
        CHECK(p_hat < expected_order[method] + 0.35);
    }
}

TEST_CASE("kernels: vector system (harmonic oscillator) + in-place aliasing is bit-identical", "[ode][steppers]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    // x'' = -x as the 2-state system [x, v]' = [v, -x]; one RK4 step from (1, 0).
    auto rhs = [](f64 /*t*/, containers::ConstSpan<f64> y, containers::Span<f64> dydt)
    {
        dydt[0] = y[1];
        dydt[1] = -y[0];
    };

    containers::Array<f64> scratch(&alloc);
    scratch.resize(ode::rk4_scratch(2));
    const containers::Span<f64> sc(scratch.data(), scratch.size());
    const f64 h = 0.1;

    // Out-of-place reference.
    containers::Array<f64> y0(&alloc);
    y0.resize(2);
    y0[0] = 1.0;
    y0[1] = 0.0;
    containers::Array<f64> y_sep(&alloc);
    y_sep.resize(2);
    ode::step_rk4(rhs, 0.0, containers::ConstSpan<f64>(y0.data(), 2), h, containers::Span<f64>(y_sep.data(), 2), sc);

    // In-place (y_out aliases y).
    containers::Array<f64> y_ip(&alloc);
    y_ip.resize(2);
    y_ip[0] = 1.0;
    y_ip[1] = 0.0;
    ode::step_rk4(rhs, 0.0, containers::ConstSpan<f64>(y_ip.data(), 2), h, containers::Span<f64>(y_ip.data(), 2), sc);

    CHECK(std::memcmp(y_sep.data(), y_ip.data(), 2 * sizeof(f64)) == 0);

    // RK4 on the rotation system is the degree-4 Taylor rotation: M = c4*I + s4*A with A = [[0,1],[-1,0]],
    // so from (1, 0): x -> c4, v -> -s4.
    const f64 c4 = 1.0 - h * h / 2.0 + h * h * h * h / 24.0;
    const f64 s4 = h - h * h * h / 6.0;
    CHECK(y_ip[0] == Catch::Approx(c4).epsilon(1e-15));
    CHECK(y_ip[1] == Catch::Approx(-s4).epsilon(1e-15));
}
