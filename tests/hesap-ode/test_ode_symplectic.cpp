// v9-g gates: the symplectic family — empirical order slopes (Verlet 2, Yoshida4 4, Yoshida6 6), the
// LONG-RUN ENERGY gate (bounded oscillation, NO secular drift, vs same-h RK4's monotone drift — the
// reason these integrators exist), and time-reversibility.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/steppers.hpp>
#include <crd/hesap/ode/symplectic.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

namespace
{

// Kepler 2D: a(x) = -x / |x|^3. Orbit with a = 1, e = 0.6: x0 = (1-e, 0), v0 = (0, sqrt((1+e)/(1-e))).
// Energy E = |v|^2/2 - 1/|x| = -1/2 exactly.
void kepler_acc(f64 /*t*/, containers::ConstSpan<f64> x, containers::Span<f64> a)
{
    const f64 r2 = x[0] * x[0] + x[1] * x[1];
    const f64 inv_r3 = 1.0 / (r2 * std::sqrt(r2));
    a[0] = -x[0] * inv_r3;
    a[1] = -x[1] * inv_r3;
}

f64 kepler_energy(containers::ConstSpan<f64> x, containers::ConstSpan<f64> v)
{
    const f64 r = std::sqrt(x[0] * x[0] + x[1] * x[1]);
    return 0.5 * (v[0] * v[0] + v[1] * v[1]) - 1.0 / r;
}

struct KeplerState
{
    f64 x[2];
    f64 v[2];
    f64 a[2];
};

void kepler_init(KeplerState& s, f64 e)
{
    s.x[0] = 1.0 - e;
    s.x[1] = 0.0;
    s.v[0] = 0.0;
    s.v[1] = std::sqrt((1.0 + e) / (1.0 - e));
    kepler_acc(0.0, containers::ConstSpan<f64>(s.x, 2), containers::Span<f64>(s.a, 2));
}

// method: 0 = symplectic Euler, 1 = velocity Verlet, 2 = Yoshida4, 3 = Yoshida6.
void kepler_step(KeplerState& s, int method, f64 t, f64 h, containers::Span<f64> scratch)
{
    const containers::Span<f64> x(s.x, 2);
    const containers::Span<f64> v(s.v, 2);
    const containers::Span<f64> a(s.a, 2);
    switch (method)
    {
        case 0:
            ode::step_symplectic_euler(kepler_acc, t, x, v, h, scratch);
            break;
        case 1:
            ode::step_velocity_verlet(kepler_acc, t, x, v, a, h, scratch);
            break;
        case 2:
            ode::step_composition(kepler_acc, t, x, v, a, h,
                                  containers::ConstSpan<f64>(ode::yoshida4_w, 3), scratch);
            break;
        default:
            ode::step_composition(kepler_acc, t, x, v, a, h,
                                  containers::ConstSpan<f64>(ode::yoshida6_w, 7), scratch);
            break;
    }
}

// Final position error after one full period (T = 2*pi for a = 1) at step h.
f64 kepler_period_error(int method, f64 h, containers::Span<f64> scratch)
{
    KeplerState s;
    kepler_init(s, 0.3);
    const f64 period = 2.0 * 3.14159265358979323846;
    const usize nsteps = static_cast<usize>(std::llround(period / h));
    const f64 h_exact = period / static_cast<f64>(nsteps);
    for (usize i = 0; i < nsteps; ++i)
    {
        kepler_step(s, method, static_cast<f64>(i) * h_exact, h_exact, scratch);
    }
    const f64 dx = s.x[0] - (1.0 - 0.3);
    const f64 dy = s.x[1] - 0.0;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

TEST_CASE("symplectic: empirical order slopes (Verlet 2, Yoshida4 4, Yoshida6 6)", "[ode][symplectic]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    containers::Array<f64> scratch(&alloc);
    scratch.resize(2);
    const containers::Span<f64> sc(scratch.data(), 2);

    struct Row
    {
        int method;
        f64 expected;
        f64 h1;
        f64 slack;
    };
    const Row rows[3] = {
        {1, 2.0, 1.0 / 256.0, 0.3},
        {2, 4.0, 1.0 / 64.0, 0.4},
        {3, 6.0, 1.0 / 16.0, 0.7},
    };
    for (const Row& row : rows)
    {
        const f64 e1 = kepler_period_error(row.method, row.h1, sc);
        const f64 e2 = kepler_period_error(row.method, row.h1 / 2.0, sc);
        const f64 p_hat = std::log2(e1 / e2);
        INFO("method " << row.method << "  e1=" << e1 << "  e2=" << e2 << "  p_hat=" << p_hat);
        CHECK(p_hat > row.expected - row.slack);
        CHECK(p_hat < row.expected + row.slack);
    }
}

TEST_CASE("symplectic: bounded energy over 1500+ orbits where RK4 drifts (THE symplectic property)",
          "[ode][symplectic][energy]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    containers::Array<f64> scratch(&alloc);
    scratch.resize(8); // rk4 on the 4-state first-order form needs 3*4 = 12... sized below
    scratch.resize(12);
    const containers::Span<f64> sc(scratch.data(), scratch.size());

    // h = 0.2 is the GAME-relevant regime (cheap steps): VV's energy error is bounded at the h^2 scale
    // forever, while RK4's secular drift accumulates ~h^4 per unit time and overtakes it. (At h = 0.05
    // RK4 is accurate enough that 1500 orbits show no advantage — measured; the symplectic win is a
    // large-h / long-horizon property, exactly the game/physics operating point.)
    const f64 h = 0.2;
    const usize nsteps = 50000; // t = 10000 ~ 1590 orbits

    // Velocity Verlet: track max |dE| over the run and over the LAST tenth — boundedness means the late
    // maximum is no worse than the global one (no secular growth).
    KeplerState s;
    kepler_init(s, 0.3);
    const f64 e0 = kepler_energy(containers::ConstSpan<f64>(s.x, 2), containers::ConstSpan<f64>(s.v, 2));
    f64 vv_max_drift = 0.0;
    f64 vv_first_tenth_max = 0.0;
    f64 vv_last_tenth_max = 0.0;
    for (usize i = 0; i < nsteps; ++i)
    {
        kepler_step(s, 1, static_cast<f64>(i) * h, h, sc);
        const f64 drift =
            std::abs(kepler_energy(containers::ConstSpan<f64>(s.x, 2), containers::ConstSpan<f64>(s.v, 2)) - e0);
        vv_max_drift = drift > vv_max_drift ? drift : vv_max_drift;
        if (i < nsteps / 10)
        {
            vv_first_tenth_max = drift > vv_first_tenth_max ? drift : vv_first_tenth_max;
        }
        if (i >= nsteps - nsteps / 10)
        {
            vv_last_tenth_max = drift > vv_last_tenth_max ? drift : vv_last_tenth_max;
        }
    }

    // RK4 (same h) on the first-order 4-state form: energy drifts SECULARLY.
    auto rhs = [](f64, containers::ConstSpan<f64> st, containers::Span<f64> d)
    {
        const f64 r2 = st[0] * st[0] + st[1] * st[1];
        const f64 inv_r3 = 1.0 / (r2 * std::sqrt(r2));
        d[0] = st[2];
        d[1] = st[3];
        d[2] = -st[0] * inv_r3;
        d[3] = -st[1] * inv_r3;
    };
    containers::Array<f64> yr(&alloc);
    yr.resize(4);
    yr[0] = 1.0 - 0.3;
    yr[1] = 0.0;
    yr[2] = 0.0;
    yr[3] = std::sqrt(1.3 / 0.7);
    const containers::ConstSpan<f64> yr_in(yr.data(), 4);
    const containers::Span<f64> yr_out(yr.data(), 4);
    for (usize i = 0; i < nsteps; ++i)
    {
        ode::step_rk4(rhs, static_cast<f64>(i) * h, yr_in, h, yr_out, sc);
    }
    const f64 rk4_drift = std::abs(
        kepler_energy(containers::ConstSpan<f64>(yr.data(), 2), containers::ConstSpan<f64>(yr.data() + 2, 2)) - e0);

    INFO("VV max |dE| = " << vv_max_drift << " (first tenth " << vv_first_tenth_max << ", last tenth "
                          << vv_last_tenth_max << "); RK4 final |dE| = " << rk4_drift);
    CHECK(vv_max_drift < 5e-2);                          // bounded at the h^2 scale
    CHECK(vv_last_tenth_max < 2.0 * vv_first_tenth_max); // NO secular growth
    CHECK(rk4_drift > 3.0 * vv_max_drift);               // same-h RK4 has visibly drifted
}

TEST_CASE("symplectic: time reversibility (forward N then backward N returns to start)", "[ode][symplectic]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    containers::Array<f64> scratch(&alloc);
    scratch.resize(2);
    const containers::Span<f64> sc(scratch.data(), 2);

    const f64 h = 0.01;
    const usize nsteps = 1000;

    KeplerState s;
    kepler_init(s, 0.3);
    const f64 x0 = s.x[0];
    const f64 v1_0 = s.v[1];

    for (usize i = 0; i < nsteps; ++i)
    {
        kepler_step(s, 1, static_cast<f64>(i) * h, h, sc);
    }
    for (usize i = 0; i < nsteps; ++i)
    {
        kepler_step(s, 1, 0.0, -h, sc); // autonomous force — t is immaterial
    }

    // Verlet is exactly time-reversible in exact arithmetic; FP leaves ~roundoff accumulation.
    CHECK(std::abs(s.x[0] - x0) < 1e-9);
    CHECK(std::abs(s.x[1]) < 1e-9);
    CHECK(std::abs(s.v[0]) < 1e-9);
    CHECK(std::abs(s.v[1] - v1_0) < 1e-9);
}

TEST_CASE("symplectic: run-twice bit identity", "[ode][symplectic][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    containers::Array<f64> scratch(&alloc);
    scratch.resize(2);
    const containers::Span<f64> sc(scratch.data(), 2);

    auto run = [&](KeplerState& s)
    {
        kepler_init(s, 0.6);
        for (usize i = 0; i < 5000; ++i)
        {
            kepler_step(s, 3, static_cast<f64>(i) * 0.01, 0.01, sc); // Yoshida6
        }
    };
    KeplerState s1;
    KeplerState s2;
    run(s1);
    run(s2);
    CHECK(s1.x[0] == s2.x[0]);
    CHECK(s1.x[1] == s2.x[1]);
    CHECK(s1.v[0] == s2.v[0]);
    CHECK(s1.v[1] == s2.v[1]);
}
