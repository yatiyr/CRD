// test_implicit_diff.cpp — Phase 3.1.6 v16-g: the implicit-differentiation suite. The IFT VJPs (differentiate the
// SOLUTION, never the solver iterations) must match central FD of the re-solved problem, and be deterministic:
//   • root_vjp        — F(x*,θ)=0 : dL/dθ ≡ FD (re-solve the root via Newton, perturb θ).
//   • fixed_point_vjp — x*=g(x*,θ): dL/dθ ≡ FD.
//   • qp_eq_vjp       — argmin ½xᵀqmatx+qᵀx s.t. Ax=b : dL/d{qmat,q,A,b} ≡ FD (re-solve the KKT).

#include <crd/hesap/autodiff/dual.hpp>          // fwd::Dual for the Newton Jacobian
#include <crd/hesap/autodiff/implicit_diff.hpp> // the IFT VJPs under test (+ tape + dense LU)

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
namespace sp  = crd::hesap::autodiff::reverse::sparse;
namespace fwd = crd::hesap::autodiff::forward;
using crd::f64;
using Catch::Matchers::WithinAbs;
using D = fwd::Dual<f64>;

namespace
{
// coupled nonlinear root (diagonally dominant ⇒ Newton converges from 0): F(x,θ) =
//   [x0 + 0.3·sin(x0) + 0.2·x1 − θ0 ; 0.2·x0 + x1 + 0.3·sin(x1) − θ1]
struct RootF
{
    template <class T>
    void operator()(const T* x, const T* th, T* out, int, int) const
    {
        using crd::math::sin;
        out[0] = x[0] + 0.3 * sin(x[0]) + 0.2 * x[1] - th[0];
        out[1] = 0.2 * x[0] + x[1] + 0.3 * sin(x[1]) - th[1];
    }
};
// contraction fixed point: g(x,θ) = [0.4·sin(x0)+0.2·x1+0.1·θ0 ; 0.3·x0+0.35·cos(x1)+0.1·θ1]
struct FixG
{
    template <class T>
    void operator()(const T* x, const T* th, T* out, int, int) const
    {
        using crd::math::cos;
        using crd::math::sin;
        out[0] = 0.4 * sin(x[0]) + 0.2 * x[1] + 0.1 * th[0];
        out[1] = 0.3 * x[0] + 0.35 * cos(x[1]) + 0.1 * th[1];
    }
};

// Newton solve F(x,θ)=0 via a forward-Dual Jacobian (self-contained, reuses the dense LU).
template <class F>
void newton_solve(const F& Ffn, const f64* theta, f64* x, int n, int np)
{
    for (int it = 0; it < 40; ++it)
    {
        f64 fx[8];
        f64 jac[64];
        for (int col = 0; col < n; ++col)
        {
            D xd[8];
            D thd[8];
            D out[8];
            for (int i = 0; i < n; ++i) { xd[i] = D{x[i], col == i ? 1.0 : 0.0}; }
            for (int j = 0; j < np; ++j) { thd[j] = D{theta[j], 0.0}; }
            Ffn(xd, thd, out, n, np);
            for (int i = 0; i < n; ++i) { if (col == 0) { fx[i] = out[i].v; } jac[i * n + col] = out[i].d; }
        }
        f64 rhs[8];
        f64 dx[8];
        for (int i = 0; i < n; ++i) { rhs[i] = -fx[i]; }
        int piv[8];
        sp::dense_lu_factor(jac, piv, n);
        sp::dense_lu_solve(jac, piv, rhs, dx, n);
        f64 nrm = 0.0;
        for (int i = 0; i < n; ++i) { x[i] += dx[i]; nrm += dx[i] * dx[i]; }
        if (nrm < 1e-28) { break; }
    }
}
} // namespace

TEST_CASE("v16-g: root_vjp (IFT) == FD of the re-solved root; deterministic", "[autodiff][reverse][implicit]")
{
    constexpr int              n = 2;
    constexpr int              np = 2;
    crd::memory::TlsfAllocator alloc(8 << 20);
    rev::Tape                  tape(&alloc);
    rev::Var                   vscr[2 * n + np];
    f64                        theta[np] = {1.3, 0.7};
    f64                        xbar[n] = {1.0, 0.5};

    f64 x_star[n] = {0.0, 0.0};
    newton_solve(RootF{}, theta, x_star, n, np);

    f64 jac[n * n];
    f64 z[n];
    f64 tmp[n];
    f64 tbar[np];
    f64 tbar2[np];
    int piv[n];
    rev::root_vjp(RootF{}, x_star, theta, xbar, tbar, n, np, tape, vscr, jac, piv, z, tmp);
    rev::root_vjp(RootF{}, x_star, theta, xbar, tbar2, n, np, tape, vscr, jac, piv, z, tmp);
    for (int j = 0; j < np; ++j) { CHECK(tbar[j] == tbar2[j]); } // deterministic

    // FD: perturb θ, re-solve the root, L = xbar·x*
    auto loss = [&](const f64* th) -> f64
    {
        f64 xs[n] = {0.0, 0.0};
        newton_solve(RootF{}, th, xs, n, np);
        return xbar[0] * xs[0] + xbar[1] * xs[1];
    };
    const f64 hh = 1e-6;
    for (int j = 0; j < np; ++j)
    {
        f64 th[np] = {theta[0], theta[1]};
        th[j]      = theta[j] + hh;
        const f64 fp = loss(th);
        th[j]        = theta[j] - hh;
        CHECK_THAT(tbar[j], WithinAbs((fp - loss(th)) / (2.0 * hh), 1e-6));
    }
}

TEST_CASE("v16-g: fixed_point_vjp (IFT) == FD of the re-solved fixed point", "[autodiff][reverse][implicit]")
{
    constexpr int              n = 2;
    constexpr int              np = 2;
    crd::memory::TlsfAllocator alloc(8 << 20);
    rev::Tape                  tape(&alloc);
    rev::Var                   vscr[2 * n + np];
    f64                        theta[np] = {0.9, 1.1};
    f64                        xbar[n] = {0.7, 1.0};

    // solve the fixed point x = g(x) by iteration (contraction)
    auto solve_fp = [&](const f64* th, f64* xs)
    {
        xs[0] = 0.0; xs[1] = 0.0;
        for (int it = 0; it < 200; ++it) { f64 g[n]; FixG{}(xs, th, g, n, np); xs[0] = g[0]; xs[1] = g[1]; }
    };
    f64 x_star[n];
    solve_fp(theta, x_star);

    f64 jac[n * n];
    f64 z[n];
    f64 tmp[n];
    f64 tbar[np];
    int piv[n];
    rev::fixed_point_vjp(FixG{}, x_star, theta, xbar, tbar, n, np, tape, vscr, jac, piv, z, tmp);

    auto loss = [&](const f64* th) -> f64 { f64 xs[n]; solve_fp(th, xs); return xbar[0] * xs[0] + xbar[1] * xs[1]; };
    const f64 hh = 1e-6;
    for (int j = 0; j < np; ++j)
    {
        f64 th[np] = {theta[0], theta[1]};
        th[j]      = theta[j] + hh;
        const f64 fp = loss(th);
        th[j]        = theta[j] - hh;
        CHECK_THAT(tbar[j], WithinAbs((fp - loss(th)) / (2.0 * hh), 1e-6));
    }
}

TEST_CASE("v16-g: qp_eq_vjp (OptNet) == FD of the re-solved equality qmatP", "[autodiff][reverse][implicit][qp]")
{
    constexpr int nq = 3;
    constexpr int mq = 1;
    // qmat SPD, q, A (1×3), b
    f64 qmat[nq * nq] = {2.0, 0.3, 0.1, 0.3, 1.6, 0.2, 0.1, 0.2, 1.4};
    f64 q[nq]      = {-1.0, 0.5, 0.7};
    f64 amat[mq * nq] = {1.0, 1.0, 1.0};
    f64 b[mq]      = {1.0};
    f64 xbar[nq]   = {1.0, 0.4, 0.7};

    // forward: solve KKT [qmat Aᵀ; A 0][x;ν] = [−q; b]
    auto solve_qp = [&](const f64* qm, const f64* qq, const f64* am, const f64* bb, f64* xs, f64* nus)
    {
        constexpr int nkkt = nq + mq;
        f64 mkkt[nkkt * nkkt];
        f64 rhs[nkkt];
        f64 y[nkkt];
        f64 tmp[nkkt];
        int piv[nkkt];
        for (int i = 0; i < nkkt * nkkt; ++i) { mkkt[i] = 0.0; }
        for (int i = 0; i < nq; ++i) { for (int j = 0; j < nq; ++j) { mkkt[i * nkkt + j] = qm[i * nq + j]; } }
        for (int i = 0; i < mq; ++i) { for (int j = 0; j < nq; ++j) { mkkt[(nq + i) * nkkt + j] = am[i * nq + j]; mkkt[j * nkkt + (nq + i)] = am[i * nq + j]; } }
        for (int i = 0; i < nq; ++i) { rhs[i] = -qq[i]; }
        for (int i = 0; i < mq; ++i) { rhs[nq + i] = bb[i]; }
        sp::dense_lu_factor(mkkt, piv, nkkt);
        sp::dense_lu_solve(mkkt, piv, rhs, y, nkkt);
        for (int i = 0; i < nq; ++i) { xs[i] = y[i]; }
        for (int i = 0; i < mq; ++i) { nus[i] = y[nq + i]; }
        (void)tmp;
    };
    f64 x_star[nq];
    f64 nu_star[mq];
    solve_qp(qmat, q, amat, b, x_star, nu_star);

    constexpr int nkkt = nq + mq;
    f64 gqmat[nq * nq];
    f64 gq[nq];
    f64 gamat[mq * nq];
    f64 gb[mq];
    f64 kkt[nkkt * nkkt];
    f64 rhs[nkkt];
    f64 dvec[nkkt];
    int piv[nkkt];
    rev::qp_eq_vjp(qmat, amat, x_star, nu_star, xbar, nq, mq, gqmat, gq, gamat, gb, kkt, piv, rhs, dvec);

    auto loss_of = [&](const f64* qm, const f64* qq, const f64* am, const f64* bb) -> f64
    { f64 xs[nq];
    f64 nus[mq]; solve_qp(qm, qq, am, bb, xs, nus); f64 loss = 0.0; for (int i = 0; i < nq; ++i) { loss += xbar[i] * xs[i]; } return loss; };
    const f64 hh = 1e-6;
    // gq, gamat, gb (unconstrained params) — direct FD
    for (int j = 0; j < nq; ++j) { f64 qq[nq] = {q[0], q[1], q[2]}; qq[j] += hh; const f64 fp = loss_of(qmat, qq, amat, b); qq[j] -= 2 * hh; CHECK_THAT(gq[j], WithinAbs((fp - loss_of(qmat, qq, amat, b)) / (2 * hh), 1e-6)); }
    for (int i = 0; i < mq; ++i) { f64 bb[mq] = {b[0]}; bb[i] += hh; const f64 fp = loss_of(qmat, q, amat, bb); bb[i] -= 2 * hh; CHECK_THAT(gb[i], WithinAbs((fp - loss_of(qmat, q, amat, bb)) / (2 * hh), 1e-6)); }
    for (int i = 0; i < mq; ++i)
    {
        for (int j = 0; j < nq; ++j)
        {
            f64 am[mq * nq] = {amat[0], amat[1], amat[2]};
            am[i * nq + j] += hh;
            const f64 fp = loss_of(qmat, q, am, b);
            am[i * nq + j] -= 2 * hh;
            CHECK_THAT(gamat[i * nq + j], WithinAbs((fp - loss_of(qmat, q, am, b)) / (2 * hh), 1e-6));
        }
    }
    // gqmat (symmetric): perturb qmat[i][j]+qmat[j][i] together, compare to gqmat[i][j]+gqmat[j][i] (off-diag) or gqmat[i][i] (diag)
    for (int i = 0; i < nq; ++i)
    {
        for (int j = i; j < nq; ++j)
        {
            f64 qm[nq * nq];
            for (int k = 0; k < nq * nq; ++k) { qm[k] = qmat[k]; }
            qm[i * nq + j] += hh;
            if (i != j) { qm[j * nq + i] += hh; }
            const f64 fp = loss_of(qm, q, amat, b);
            qm[i * nq + j] -= 2 * hh;
            if (i != j) { qm[j * nq + i] -= 2 * hh; }
            const f64 fd  = (fp - loss_of(qm, q, amat, b)) / (2 * hh);
            const f64 ana = (i == j) ? gqmat[i * nq + i] : (gqmat[i * nq + j] + gqmat[j * nq + i]);
            CHECK_THAT(ana, WithinAbs(fd, 1e-6));
        }
    }
}
