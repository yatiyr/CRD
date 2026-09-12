// v9-f gates: RODAS4 Rosenbrock + TR-BDF2 ESDIRK — order slopes (the tableau certificates), L-stability,
// Robertson conservation + curve, the bounded-cost property (Rosenbrock: nlu == attempts exactly, no
// Newton), the stiff crush, determinism. odeint same-language comparison = the WSL difftest (session log).

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/rosenbrock.hpp>
#include <crd/hesap/ode/sdirk.hpp>
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

// Two exact-solution probes, both ending at y(1) = 1/2:
//   AUTONOMOUS:     y' = -y^2,    y(0) = 1  (the dfdt path contributes exactly zero — pure stage algebra)
//   NON-AUTONOMOUS: y' = -2ty^2,  y(0) = 1  (∂f/∂t = -2y^2 ≠ 0 — exercises the d_i·h·dfdt terms; THE
//                   probe that exposed Boost.odeint's d4 sign bug: odeint measures asymptotic ORDER 1
//                   here, our rodas.f-corrected d4 restores order 4)
class Probe final : public ode::OdeFunction<f64>
{
public:
    explicit Probe(bool autonomous) : ode::OdeFunction<f64>(true), m_auto(autonomous) {}
    void rhs(f64 t, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = m_auto ? -y[0] * y[0] : -2.0 * t * y[0] * y[0];
    }
    [[nodiscard]] bool jacobian(f64 t, containers::ConstSpan<f64> y, containers::Span<f64> j) const override
    {
        j[0] = m_auto ? -2.0 * y[0] : -4.0 * t * y[0];
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 1; }

private:
    bool m_auto;
};

f64 fixed_h_error_ros(bool trbdf2, bool autonomous, f64 h, crd::memory::IAllocator* alloc)
{
    Probe f(autonomous);
    containers::Array<f64> y(alloc);
    y.resize(1);
    y[0] = 1.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e10; // never reject
    opts.atol = 1e10;
    opts.h0 = h;
    opts.hmax = h;
    const ode::OdeResult<f64> r =
        trbdf2 ? ode::integrate_trbdf2<f64>(f, 0.0, 1.0, containers::Span<f64>(y.data(), 1), opts, alloc)
               : ode::integrate_rosenbrock<f64>(f, 0.0, 1.0, containers::Span<f64>(y.data(), 1), opts, alloc);
    REQUIRE(r.success);
    return std::abs(y[0] - 0.5);
}

} // namespace

TEST_CASE("rosenbrock/trbdf2: empirical order slopes (4 and 2)", "[ode][rosenbrock][sdirk]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // Autonomous: pure stage algebra (matches odeint digit-for-digit pre-fix and post-fix).
    const f64 e1a = fixed_h_error_ros(false, true, 1.0 / 16.0, &alloc);
    const f64 e2a = fixed_h_error_ros(false, true, 1.0 / 32.0, &alloc);
    const f64 p_a = std::log2(e1a / e2a);
    INFO("rosenbrock AUTONOMOUS p_hat=" << p_a);
    CHECK(p_a > 3.6);
    CHECK(p_a < 4.6);

    // NON-autonomous: order 4 holds ONLY with rodas.f's d4 sign (odeint's +d4 degrades this to 1).
    const f64 e1n = fixed_h_error_ros(false, false, 1.0 / 16.0, &alloc);
    const f64 e2n = fixed_h_error_ros(false, false, 1.0 / 32.0, &alloc);
    const f64 p_n = std::log2(e1n / e2n);
    INFO("rosenbrock NON-AUTONOMOUS p_hat=" << p_n << " (e1=" << e1n << ")");
    CHECK(p_n > 3.5);
    CHECK(p_n < 4.7);

    const f64 e1t = fixed_h_error_ros(true, false, 1.0 / 64.0, &alloc);
    const f64 e2t = fixed_h_error_ros(true, false, 1.0 / 128.0, &alloc);
    const f64 p_tr = std::log2(e1t / e2t);
    INFO("trbdf2 p_hat=" << p_tr << " (e1=" << e1t << ", e2=" << e2t << ")");
    CHECK(p_tr > 1.7);
    CHECK(p_tr < 2.4);
}

TEST_CASE("rosenbrock/trbdf2: L-stable fast-mode decay", "[ode][rosenbrock][sdirk]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    class F final : public ode::OdeFunction<f64>
    {
    public:
        F() : ode::OdeFunction<f64>(true) {}
        void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
        {
            d[0] = -y[0];
            d[1] = -1e4 * y[1];
        }
        [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
        {
            j[0] = -1.0;
            j[1] = 0.0;
            j[2] = 0.0;
            j[3] = -1e4;
            return true;
        }
        [[nodiscard]] usize dim() const noexcept override { return 2; }
    } f;

    for (int method = 0; method < 2; ++method)
    {
        containers::Array<f64> y(&alloc);
        y.resize(2);
        y[0] = 1.0;
        y[1] = 1.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-8;
        opts.atol = 1e-11;
        const ode::OdeResult<f64> r =
            (method == 0)
                ? ode::integrate_rosenbrock<f64>(f, 0.0, 2.0, containers::Span<f64>(y.data(), 2), opts, &alloc)
                : ode::integrate_trbdf2<f64>(f, 0.0, 2.0, containers::Span<f64>(y.data(), 2), opts, &alloc);
        REQUIRE(r.success);
        INFO("method " << method);
        CHECK(std::abs(y[0] - std::exp(-2.0)) < 1e-6);
        CHECK(std::abs(y[1]) < 1e-10); // the stiff mode dies cleanly (L-stability)
    }
}

TEST_CASE("rosenbrock: Robertson + the bounded-cost property (nlu == attempts, NO Newton)", "[ode][rosenbrock]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    Robertson f;
    containers::Array<f64> y(&alloc);
    y.resize(3);
    y[0] = 1.0;
    y[1] = 0.0;
    y[2] = 0.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-7;
    opts.atol = 1e-11;
    const ode::OdeResult<f64> r =
        ode::integrate_rosenbrock<f64>(f, 0.0, 1e4, containers::Span<f64>(y.data(), 3), opts, &alloc);
    REQUIRE(r.success);
    CHECK(std::abs(y[0] + y[1] + y[2] - 1.0) < 1e-6);
    // The Rosenbrock contract: NO iteration — exactly one LU and 6 evals + 1 dfdt eval per attempt.
    CHECK(r.work.nlu == r.work.nsteps);
    CHECK(r.work.nsol == 6 * r.work.nsteps);
    INFO("naccept=" << r.work.naccept << " nlu=" << r.work.nlu << " nfev=" << r.work.nfev);
}

TEST_CASE("trbdf2: Robertson conservation + the shared-matrix economy", "[ode][sdirk]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    Robertson f;
    containers::Array<f64> y(&alloc);
    y.resize(3);
    y[0] = 1.0;
    y[1] = 0.0;
    y[2] = 0.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-7;
    opts.atol = 1e-11;
    const ode::OdeResult<f64> r =
        ode::integrate_trbdf2<f64>(f, 0.0, 1e4, containers::Span<f64>(y.data(), 3), opts, &alloc);
    REQUIRE(r.success);
    CHECK(std::abs(y[0] + y[1] + y[2] - 1.0) < 1e-6);
    // Both implicit stages share ONE iteration matrix: far fewer factorizations than Newton solves.
    CHECK(r.work.nlu < r.work.nsol / 2);
    INFO("naccept=" << r.work.naccept << " nlu=" << r.work.nlu << " nsol=" << r.work.nsol);
}

TEST_CASE("rosenbrock: the stiff crush vs RK45 on VdP mu=1000", "[ode][rosenbrock][crush]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    VdpStiff f;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-6;
    opts.atol = 1e-8;

    containers::Array<f64> yr(&alloc);
    yr.resize(2);
    yr[0] = 2.0;
    yr[1] = 0.0;
    const ode::OdeResult<f64> rr =
        ode::integrate_rosenbrock<f64>(f, 0.0, 300.0, containers::Span<f64>(yr.data(), 2), opts, &alloc);
    REQUIRE(rr.success);

    containers::Array<f64> ye(&alloc);
    ye.resize(2);
    ye[0] = 2.0;
    ye[1] = 0.0;
    const ode::OdeResult<f64> re =
        ode::integrate_erk<f64>(f, 0.0, 300.0, containers::Span<f64>(ye.data(), 2), opts, &alloc, ode::ErkMethod::Rk45);
    REQUIRE(re.success);

    CHECK(std::abs(yr[0] - ye[0]) < 1e-3);
    INFO("RODAS4 nfev=" << rr.work.nfev << "  RK45 nfev=" << re.work.nfev);
    CHECK(re.work.nfev > 100 * rr.work.nfev);
}

TEST_CASE("rosenbrock/trbdf2: run-twice bit identity + FD-vs-analytic", "[ode][rosenbrock][sdirk][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    VdpStiff an;

    for (int method = 0; method < 2; ++method)
    {
        auto run = [&](const ode::OdeFunction<f64>& f, containers::Array<f64>& y) -> ode::OdeResult<f64>
        {
            y.resize(2);
            y[0] = 2.0;
            y[1] = 0.0;
            ode::OdeOptions<f64> opts;
            opts.rtol = 1e-7;
            opts.atol = 1e-9;
            return (method == 0)
                       ? ode::integrate_rosenbrock<f64>(f, 0.0, 50.0, containers::Span<f64>(y.data(), 2), opts, &alloc)
                       : ode::integrate_trbdf2<f64>(f, 0.0, 50.0, containers::Span<f64>(y.data(), 2), opts, &alloc);
        };
        containers::Array<f64> a(&alloc);
        containers::Array<f64> b(&alloc);
        const ode::OdeResult<f64> r1 = run(an, a);
        const ode::OdeResult<f64> r2 = run(an, b);
        REQUIRE(r1.success);
        INFO("method " << method);
        CHECK(std::memcmp(a.data(), b.data(), 2 * sizeof(f64)) == 0);
        CHECK(r1.work.nfev == r2.work.nfev);
        CHECK(r1.work.nlu == r2.work.nlu);

        // FD fallback agrees with analytic to tolerance-level accuracy.
        const auto fd = ode::make_ode_function<f64>(2,
                                                    [](f64, containers::ConstSpan<f64> y, containers::Span<f64> d)
                                                    {
                                                        d[0] = y[1];
                                                        d[1] = 1000.0 * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
                                                    });
        containers::Array<f64> c(&alloc);
        const ode::OdeResult<f64> r3 = run(fd, c);
        REQUIRE(r3.success);
        CHECK(std::abs(a[0] - c[0]) < 1e-4);
    }
}
