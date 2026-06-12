// v9-e gates: Radau IIA(5) — stiff accuracy on exact solutions, Robertson, L-stability behavior, the
// stiff crush vs explicit, determinism. scipy trajectory-exactness = the WSL difftest (session log).

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/radau.hpp>
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

} // namespace

TEST_CASE("radau: stiff-linear exact solution + L-stable fast-mode decay", "[ode][radau]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // y' = diag(-1, -10000)*y — even stiffer than the BDF gate; Radau is L-stable.
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

    containers::Array<f64> y(&alloc);
    y.resize(2);
    y[0] = 1.0;
    y[1] = 1.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-9;
    opts.atol = 1e-12;
    const ode::OdeResult<f64> r =
        ode::integrate_radau<f64>(f, 0.0, 2.0, containers::Span<f64>(y.data(), 2), opts, &alloc);
    REQUIRE(r.success);
    CHECK(std::abs(y[0] - std::exp(-2.0)) < 1e-8);
    CHECK(std::abs(y[1]) < 1e-12); // e^{-20000} == 0; L-stability kills it without ringing
    CHECK(r.work.nlu >= 2);        // the real+complex factorization pair
}

TEST_CASE("radau: Robertson 1e5 horizon - conservation + the published decay curve", "[ode][radau]")
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
    opts.atol = 1e-12;
    const ode::OdeResult<f64> r =
        ode::integrate_radau<f64>(f, 0.0, 1e5, containers::Span<f64>(y.data(), 3), opts, &alloc);
    REQUIRE(r.success);
    CHECK(std::abs(y[0] + y[1] + y[2] - 1.0) < 1e-7);
    CHECK(y[0] > 0.005);
    CHECK(y[0] < 0.04);
    INFO("y1=" << y[0] << " naccept=" << r.work.naccept << " nfev=" << r.work.nfev << " nlu=" << r.work.nlu);
}

TEST_CASE("radau: agrees with BDF-free reference (RK45 on the smooth part) + stiff crush", "[ode][radau][crush]")
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
        ode::integrate_radau<f64>(f, 0.0, 300.0, containers::Span<f64>(yr.data(), 2), opts, &alloc);
    REQUIRE(rr.success);

    containers::Array<f64> ye(&alloc);
    ye.resize(2);
    ye[0] = 2.0;
    ye[1] = 0.0;
    const ode::OdeResult<f64> re =
        ode::integrate_erk<f64>(f, 0.0, 300.0, containers::Span<f64>(ye.data(), 2), opts, &alloc, ode::ErkMethod::Rk45);
    REQUIRE(re.success);

    CHECK(std::abs(yr[0] - ye[0]) < 1e-3);
    INFO("Radau nfev=" << rr.work.nfev << " (naccept=" << rr.work.naccept << ")  RK45 nfev=" << re.work.nfev);
    CHECK(re.work.nfev > 100 * rr.work.nfev); // the implicit-stiff crush, again ⇒ >100x
}

TEST_CASE("radau: run-twice bit identity", "[ode][radau][determinism]")
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
        return ode::integrate_radau<f64>(f, 0.0, 1000.0, containers::Span<f64>(y.data(), 3), opts, &alloc);
    };
    containers::Array<f64> a(&alloc);
    containers::Array<f64> b(&alloc);
    const ode::OdeResult<f64> r1 = run(a);
    const ode::OdeResult<f64> r2 = run(b);
    REQUIRE(r1.success);
    CHECK(std::memcmp(a.data(), b.data(), 3 * sizeof(f64)) == 0);
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.nlu == r2.work.nlu);
    CHECK(r1.work.naccept == r2.work.naccept);
}
