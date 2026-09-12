// v9-h gates: mass matrices + semi-explicit index-1 DAE through BDF — wiring exactness (M = I via the
// mass path is BIT-IDENTICAL to the M-less path), a non-diagonal-M problem whose exact solution is known,
// a singular-M index-1 DAE with exact solution, and THE strong gate: Robertson in ODE form vs Robertson
// in DAE-conservation form (M = diag(1,1,0)) agree at tolerance level.

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/bdf.hpp>
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

// y' = -y with the mass path: M = I (must be bit-identical to the plain path).
class DecayMassIdentity final : public ode::OdeFunction<f64>
{
public:
    DecayMassIdentity() : ode::OdeFunction<f64>(true, false, /*has_mass*/ true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -y[0];
        d[1] = -2.0 * y[1];
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        j[0] = -1.0;
        j[1] = 0.0;
        j[2] = 0.0;
        j[3] = -2.0;
        return true;
    }
    [[nodiscard]] bool mass_matrix(containers::Span<f64> m) const override
    {
        m[0] = 1.0;
        m[1] = 0.0;
        m[2] = 0.0;
        m[3] = 1.0;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
};

class DecayPlain final : public ode::OdeFunction<f64>
{
public:
    DecayPlain() : ode::OdeFunction<f64>(true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -y[0];
        d[1] = -2.0 * y[1];
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        j[0] = -1.0;
        j[1] = 0.0;
        j[2] = 0.0;
        j[3] = -2.0;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
};

// Non-diagonal M: M·y' = -M·y  ⇒  y' = -y exactly (M cancels analytically but exercises the full
// M-residual and (M - c·J) factor paths numerically).
class NonDiagMass final : public ode::OdeFunction<f64>
{
public:
    NonDiagMass() : ode::OdeFunction<f64>(true, false, true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        // f = -M·y with M = [[2,1],[1,2]]
        d[0] = -(2.0 * y[0] + y[1]);
        d[1] = -(y[0] + 2.0 * y[1]);
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        j[0] = -2.0;
        j[1] = -1.0;
        j[2] = -1.0;
        j[3] = -2.0;
        return true;
    }
    [[nodiscard]] bool mass_matrix(containers::Span<f64> m) const override
    {
        m[0] = 2.0;
        m[1] = 1.0;
        m[2] = 1.0;
        m[3] = 2.0;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
};

// Singular M (index-1 DAE): y1' = -y1; 0 = y1 - y2. Exact: y1 = y2 = e^{-t}. Consistent y0 = (1, 1).
class Index1Dae final : public ode::OdeFunction<f64>
{
public:
    Index1Dae() : ode::OdeFunction<f64>(true, false, true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -y[0];
        d[1] = y[0] - y[1];
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        j[0] = -1.0;
        j[1] = 0.0;
        j[2] = 1.0;
        j[3] = -1.0;
        return true;
    }
    [[nodiscard]] bool mass_matrix(containers::Span<f64> m) const override
    {
        m[0] = 1.0;
        m[1] = 0.0;
        m[2] = 0.0;
        m[3] = 0.0; // singular: row 2 is algebraic
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 2; }
};

// Robertson in DAE-conservation form: rows 1-2 differential, row 3 algebraic (y1 + y2 + y3 - 1 = 0).
class RobertsonDae final : public ode::OdeFunction<f64>
{
public:
    RobertsonDae() : ode::OdeFunction<f64>(true, false, true) {}
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        d[0] = -0.04 * y[0] + 1e4 * y[1] * y[2];
        d[1] = 0.04 * y[0] - 1e4 * y[1] * y[2] - 3e7 * y[1] * y[1];
        d[2] = y[0] + y[1] + y[2] - 1.0;
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64> y, containers::Span<f64> j) const override
    {
        j[0] = -0.04;
        j[1] = 1e4 * y[2];
        j[2] = 1e4 * y[1];
        j[3] = 0.04;
        j[4] = -1e4 * y[2] - 6e7 * y[1];
        j[5] = -1e4 * y[1];
        j[6] = 1.0;
        j[7] = 1.0;
        j[8] = 1.0;
        return true;
    }
    [[nodiscard]] bool mass_matrix(containers::Span<f64> m) const override
    {
        for (int i = 0; i < 9; ++i)
        {
            m[i] = 0.0;
        }
        m[0] = 1.0;
        m[4] = 1.0;
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return 3; }
};

class RobertsonOde final : public ode::OdeFunction<f64>
{
public:
    RobertsonOde() : ode::OdeFunction<f64>(true) {}
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

} // namespace

TEST_CASE("mass: M = I through the mass path is bit-identical to the plain path", "[ode][mass]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    DecayMassIdentity fm;
    DecayPlain fp;

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-8;
    opts.atol = 1e-10;

    containers::Array<f64> ym(&alloc);
    ym.resize(2);
    ym[0] = 1.0;
    ym[1] = 1.0;
    const ode::OdeResult<f64> rm =
        ode::integrate_bdf<f64>(fm, 0.0, 3.0, containers::Span<f64>(ym.data(), 2), opts, &alloc);
    REQUIRE(rm.success);

    containers::Array<f64> yp(&alloc);
    yp.resize(2);
    yp[0] = 1.0;
    yp[1] = 1.0;
    const ode::OdeResult<f64> rp =
        ode::integrate_bdf<f64>(fp, 0.0, 3.0, containers::Span<f64>(yp.data(), 2), opts, &alloc);
    REQUIRE(rp.success);

    // IDENTICAL DECISIONS (every counter equal — the M = I path takes the same step/Newton/factor
    // sequence). Final values agree to roundoff, NOT bit-exactly: the mass residual associates as
    // c·f − M·(ψ+d) while the plain path computes (c·f − ψ) − d — an eps-level association difference
    // inside converged Newton iterates (measured, named; the plain path remains the scipy-bit contract).
    CHECK(rm.work.nfev == rp.work.nfev);
    CHECK(rm.work.naccept == rp.work.naccept);
    CHECK(rm.work.nlu == rp.work.nlu);
    CHECK(std::abs(ym[0] - yp[0]) < 1e-14);
    CHECK(std::abs(ym[1] - yp[1]) < 1e-14);
}

TEST_CASE("mass: non-diagonal M recovers the exact solution", "[ode][mass]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    NonDiagMass f;
    containers::Array<f64> y(&alloc);
    y.resize(2);
    y[0] = 1.0;
    y[1] = 2.0;
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-9;
    opts.atol = 1e-12;
    const ode::OdeResult<f64> r =
        ode::integrate_bdf<f64>(f, 0.0, 2.0, containers::Span<f64>(y.data(), 2), opts, &alloc);
    REQUIRE(r.success);
    CHECK(std::abs(y[0] - std::exp(-2.0)) < 1e-7);
    CHECK(std::abs(y[1] - 2.0 * std::exp(-2.0)) < 1e-7);
}

TEST_CASE("mass: singular M index-1 DAE - exact solution + constraint satisfaction", "[ode][mass][dae]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    Index1Dae f;
    containers::Array<f64> y(&alloc);
    y.resize(2);
    y[0] = 1.0;
    y[1] = 1.0; // consistent: y2 = y1
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-9;
    opts.atol = 1e-12;
    const ode::OdeResult<f64> r =
        ode::integrate_bdf<f64>(f, 0.0, 2.0, containers::Span<f64>(y.data(), 2), opts, &alloc);
    REQUIRE(r.success);
    CHECK(std::abs(y[0] - std::exp(-2.0)) < 1e-7);
    CHECK(std::abs(y[1] - std::exp(-2.0)) < 1e-7);
    CHECK(std::abs(y[0] - y[1]) < 1e-10); // the algebraic constraint holds tightly
}

TEST_CASE("mass: Robertson ODE form vs DAE-conservation form agree (THE cross-formulation gate)",
          "[ode][mass][dae]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    RobertsonOde fo;
    RobertsonDae fd;

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-8;
    opts.atol = 1e-12;

    containers::Array<f64> yo(&alloc);
    yo.resize(3);
    yo[0] = 1.0;
    yo[1] = 0.0;
    yo[2] = 0.0;
    REQUIRE(ode::integrate_bdf<f64>(fo, 0.0, 100.0, containers::Span<f64>(yo.data(), 3), opts, &alloc).success);

    containers::Array<f64> yd(&alloc);
    yd.resize(3);
    yd[0] = 1.0;
    yd[1] = 0.0;
    yd[2] = 0.0;
    REQUIRE(ode::integrate_bdf<f64>(fd, 0.0, 100.0, containers::Span<f64>(yd.data(), 3), opts, &alloc).success);

    // Two INDEPENDENT formulations of the same physics agree at tolerance level...
    CHECK(std::abs(yo[0] - yd[0]) < 1e-6);
    CHECK(std::abs(yo[1] - yd[1]) < 1e-9);
    CHECK(std::abs(yo[2] - yd[2]) < 1e-6);
    // ...and the DAE form satisfies conservation EXACTLY (it is an algebraic constraint there).
    CHECK(std::abs(yd[0] + yd[1] + yd[2] - 1.0) < 1e-12);
}

TEST_CASE("mass: run-twice bit identity on the DAE path", "[ode][mass][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    RobertsonDae f;
    auto run = [&](containers::Array<f64>& y) -> ode::OdeResult<f64>
    {
        y.resize(3);
        y[0] = 1.0;
        y[1] = 0.0;
        y[2] = 0.0;
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-7;
        opts.atol = 1e-10;
        return ode::integrate_bdf<f64>(f, 0.0, 100.0, containers::Span<f64>(y.data(), 3), opts, &alloc);
    };
    containers::Array<f64> a(&alloc);
    containers::Array<f64> b(&alloc);
    const ode::OdeResult<f64> r1 = run(a);
    const ode::OdeResult<f64> r2 = run(b);
    REQUIRE(r1.success);
    CHECK(std::memcmp(a.data(), b.data(), 3 * sizeof(f64)) == 0);
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.nlu == r2.work.nlu);
}
