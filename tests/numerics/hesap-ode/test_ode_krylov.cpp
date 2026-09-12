// v9-j (Krylov follow-on) gates: matrix-free Newton-Krylov (CVODE SPGMR) — BDF with KrylovOdeLinearSolver,
// the inner solve FGMRES over jacobian_vector, NO dense/sparse Jacobian ever assembled. Gates: a 1D heat
// MOL system vs its EXACT discrete eigenmode decay (the spatial mode is an exact eigenvector of the periodic
// Laplacian ⇒ no discretization error), matrix-free == the proven dense BDF trajectory, the preconditioner
// seam (a Jacobi PrecSolve cuts the GMRES iteration count), run-twice bit identity (the determinism moat —
// FGMRES is serial, jac-vec is the only parallel step and is bit-identical).

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/ode_krylov_solver.hpp>
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

constexpr f64 kPi = 3.141592653589793;

// 1D periodic heat equation, method-of-lines: u_t = ν·u_xx, central second difference. The dense Jacobian
// AND the matrix-free jacobian_vector are both provided (so the same problem drives the dense reference and
// the matrix-free Krylov run). Constant J ⇒ the analytic jac-vec is exact.
class Heat1D final : public ode::OdeFunction<f64>
{
public:
    Heat1D(usize nx, f64 nu) : ode::OdeFunction<f64>(/*jac*/ true, /*jacvec*/ true), m_n(nx), m_nu(nu)
    {
        m_dx = (2.0 * kPi) / static_cast<f64>(nx);
    }
    void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
    {
        for (usize i = 0; i < m_n; ++i)
        {
            d[i] = lap(y, i);
        }
    }
    [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64>, containers::Span<f64> j) const override
    {
        for (usize i = 0; i < m_n * m_n; ++i)
        {
            j[i] = 0.0;
        }
        const f64 coef = m_nu / (m_dx * m_dx);
        for (usize i = 0; i < m_n; ++i)
        {
            j[i * m_n + i] = -2.0 * coef;
            j[i * m_n + ((i + 1) % m_n)] += coef;
            j[i * m_n + ((i + m_n - 1) % m_n)] += coef;
        }
        return true;
    }
    [[nodiscard]] bool jacobian_vector(f64, containers::ConstSpan<f64>, containers::ConstSpan<f64> v,
                                       containers::Span<f64> jv) const override
    {
        const f64 coef = m_nu / (m_dx * m_dx);
        for (usize i = 0; i < m_n; ++i)
        {
            jv[i] = coef * (v[(i + 1) % m_n] - 2.0 * v[i] + v[(i + m_n - 1) % m_n]);
        }
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return m_n; }

    [[nodiscard]] f64 coef() const noexcept { return m_nu / (m_dx * m_dx); }
    [[nodiscard]] f64 dx() const noexcept { return m_dx; }

private:
    [[nodiscard]] f64 lap(containers::ConstSpan<f64> y, usize i) const
    {
        return coef() * (y[(i + 1) % m_n] - 2.0 * y[i] + y[(i + m_n - 1) % m_n]);
    }
    usize m_n;
    f64 m_nu;
    f64 m_dx;
};

// Tridiagonal (Thomas-solve) preconditioner for (I − c·J): solves the tridiagonal part of the heat iteration
// matrix (diagonal 1+2c·coef, off-diagonals −c·coef) exactly via Thomas, ignoring the two periodic corner
// entries — the classic strong preconditioner for 1D MOL diffusion. (A Jacobi preconditioner is useless
// here: the heat operator's diagonal is CONSTANT, so M⁻¹ = scalar·I and GMRES is invariant under scalar
// scaling — it would not change the iteration count at all.) The CVODE PrecSetup/PrecSolve pattern: setup()
// factors the constant tridiagonal once (per c), apply() runs the forward/back sweeps.
class HeatTridiagPrec final : public ode::OdeKrylovPreconditioner<f64>
{
public:
    HeatTridiagPrec(crd::memory::IAllocator* alloc, usize n, f64 coef)
        : m_n(n), m_coef(coef), m_cprime(alloc), m_denom(alloc), m_dprime(alloc)
    {
        m_cprime.resize(n);
        m_denom.resize(n);
        m_dprime.resize(n);
    }
    [[nodiscard]] bool setup(f64 c, f64, containers::ConstSpan<f64>) override
    {
        m_a = -c * m_coef;             // sub/super diagonal
        m_b = 1.0 + 2.0 * c * m_coef;  // diagonal
        m_denom[0] = m_b;
        m_cprime[0] = m_a / m_b;
        for (usize i = 1; i < m_n; ++i)
        {
            m_denom[i] = m_b - m_a * m_cprime[i - 1];
            m_cprime[i] = m_a / m_denom[i];
        }
        return true;
    }
    void apply(containers::ConstSpan<f64> r, containers::Span<f64> z) const override
    {
        m_dprime[0] = r[0] / m_b;
        for (usize i = 1; i < m_n; ++i)
        {
            m_dprime[i] = (r[i] - m_a * m_dprime[i - 1]) / m_denom[i];
        }
        z[m_n - 1] = m_dprime[m_n - 1];
        for (usize ii = m_n - 1; ii-- > 0;)
        {
            z[ii] = m_dprime[ii] - m_cprime[ii] * z[ii + 1];
        }
    }

private:
    usize m_n;
    f64 m_coef;
    f64 m_a = 0.0;
    f64 m_b = 1.0;
    containers::Array<f64> m_cprime;
    containers::Array<f64> m_denom;
    mutable containers::Array<f64> m_dprime;
};

void fill_mode(containers::Array<f64>& u, usize nx, f64 dx, int k)
{
    u.resize(nx);
    for (usize i = 0; i < nx; ++i)
    {
        u[i] = std::sin(static_cast<f64>(k) * static_cast<f64>(i) * dx);
    }
}

} // namespace

TEST_CASE("krylov: matrix-free BDF matches the exact discrete heat eigenmode", "[ode][krylov]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize nx = 64;
    Heat1D f(nx, 1.0);
    const f64 dx = f.dx();
    const int k = 1;
    const f64 lambda_h = -4.0 * f.coef() * std::sin(static_cast<f64>(k) * dx / 2.0) * std::sin(static_cast<f64>(k) * dx / 2.0);
    const f64 t_end = 0.5;

    containers::Array<f64> u(&alloc);
    fill_mode(u, nx, dx, k);

    ode::KrylovOdeLinearSolver<f64> krylov(&alloc, /*restart*/ nx);
    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-9;
    opts.atol = 1e-11;
    const ode::OdeResult<f64> r =
        ode::integrate_bdf<f64>(f, 0.0, t_end, containers::Span<f64>(u.data(), nx), opts, &alloc, &krylov);
    REQUIRE(r.success);

    const f64 decay = std::exp(lambda_h * t_end);
    f64 maxerr = 0.0;
    for (usize i = 0; i < nx; ++i)
    {
        const f64 exact = decay * std::sin(static_cast<f64>(k) * static_cast<f64>(i) * dx);
        maxerr = std::max(maxerr, std::abs(u[i] - exact));
    }
    INFO("maxerr=" << maxerr << "  GMRES iters=" << krylov.total_gmres_iterations() << "  nlu=" << r.work.nlu);
    CHECK(maxerr < 1e-6);
    CHECK(krylov.total_gmres_iterations() > 0);
}

TEST_CASE("krylov: matrix-free == the proven dense BDF trajectory", "[ode][krylov]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize nx = 48;
    Heat1D f(nx, 0.7);
    const f64 dx = f.dx();

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-8;
    opts.atol = 1e-10;

    containers::Array<f64> ud(&alloc);
    fill_mode(ud, nx, dx, 2);
    const ode::OdeResult<f64> rd =
        ode::integrate_bdf<f64>(f, 0.0, 0.4, containers::Span<f64>(ud.data(), nx), opts, &alloc); // internal dense LU
    REQUIRE(rd.success);

    containers::Array<f64> uk(&alloc);
    fill_mode(uk, nx, dx, 2);
    // Tight forcing here: this test's POINT is bit-for-bit reproduction of the direct-solve trajectory
    // (the default 0.05 forcing is inexact-Newton — correct but not direct-solve-identical).
    ode::KrylovOdeLinearSolver<f64> krylov(&alloc, /*restart*/ nx, 1e-9);
    const ode::OdeResult<f64> rk =
        ode::integrate_bdf<f64>(f, 0.0, 0.4, containers::Span<f64>(uk.data(), nx), opts, &alloc, &krylov);
    REQUIRE(rk.success);

    f64 maxdiff = 0.0;
    for (usize i = 0; i < nx; ++i)
    {
        maxdiff = std::max(maxdiff, std::abs(ud[i] - uk[i]));
    }
    INFO("dense-vs-matfree maxdiff=" << maxdiff);
    CHECK(maxdiff < 1e-6);
}

TEST_CASE("krylov: the preconditioner seam cuts GMRES iterations (CVODE PrecSolve)", "[ode][krylov]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize nx = 64;
    Heat1D f(nx, 1.0);
    const f64 dx = f.dx();

    ode::OdeOptions<f64> opts;
    opts.rtol = 1e-8;
    opts.atol = 1e-10;

    // Both solvers pinned to the SAME tight forcing (1e-7) so the GMRES count is large enough to show the
    // preconditioner's reduction cleanly — apples-to-apples (the production default forcing is 0.05).
    containers::Array<f64> u_un(&alloc);
    fill_mode(u_un, nx, dx, 1);
    ode::KrylovOdeLinearSolver<f64> plain(&alloc, /*restart*/ nx, 1e-7);
    const ode::OdeResult<f64> r_un =
        ode::integrate_bdf<f64>(f, 0.0, 0.5, containers::Span<f64>(u_un.data(), nx), opts, &alloc, &plain);
    REQUIRE(r_un.success);

    containers::Array<f64> u_pc(&alloc);
    fill_mode(u_pc, nx, dx, 1);
    HeatTridiagPrec prec(&alloc, nx, f.coef());
    ode::KrylovOdeLinearSolver<f64> precond(&alloc, /*restart*/ nx, 1e-7, 1000, &prec);
    const ode::OdeResult<f64> r_pc =
        ode::integrate_bdf<f64>(f, 0.0, 0.5, containers::Span<f64>(u_pc.data(), nx), opts, &alloc, &precond);
    REQUIRE(r_pc.success);

    f64 maxdiff = 0.0;
    for (usize i = 0; i < nx; ++i)
    {
        maxdiff = std::max(maxdiff, std::abs(u_un[i] - u_pc[i]));
    }
    INFO("plain iters=" << plain.total_gmres_iterations() << "  precond iters=" << precond.total_gmres_iterations()
                        << "  maxdiff=" << maxdiff);
    CHECK(maxdiff < 1e-6); // same answer
    CHECK(precond.total_gmres_iterations() < plain.total_gmres_iterations()); // the seam works
}

TEST_CASE("krylov: run-twice bit identity (determinism moat)", "[ode][krylov][determinism]")
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize nx = 48;

    auto run = [&](containers::Array<f64>& out) -> ode::OdeResult<f64>
    {
        Heat1D f(nx, 1.0);
        fill_mode(out, nx, f.dx(), 1);
        ode::KrylovOdeLinearSolver<f64> krylov(&alloc, nx);
        ode::OdeOptions<f64> opts;
        opts.rtol = 1e-8;
        opts.atol = 1e-10;
        return ode::integrate_bdf<f64>(f, 0.0, 0.5, containers::Span<f64>(out.data(), nx), opts, &alloc, &krylov);
    };

    containers::Array<f64> a(&alloc);
    containers::Array<f64> b(&alloc);
    const ode::OdeResult<f64> r1 = run(a);
    const ode::OdeResult<f64> r2 = run(b);
    REQUIRE(r1.success);
    CHECK(std::memcmp(a.data(), b.data(), nx * sizeof(f64)) == 0);
    CHECK(r1.work.nfev == r2.work.nfev);
    CHECK(r1.work.nlu == r2.work.nlu);
    CHECK(r1.work.nsol == r2.work.nsol);
}
