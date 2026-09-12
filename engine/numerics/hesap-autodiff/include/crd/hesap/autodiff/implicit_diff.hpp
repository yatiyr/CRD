#pragma once

// implicit_diff.hpp — Phase 3.1.6 v16-g: the IMPLICIT-DIFFERENTIATION suite — differentiate the SOLUTION of an
// equation (a root, a fixed point, an argmin) via the implicit function theorem, NEVER by unrolling the solver. So
// the backward is O(1) linear solves independent of the number of solver iterations (vs unrolled AD's O(iterations)),
// and reuses the solver's own factor. jaxopt is UNMAINTAINED, cvxpylayers/Theseus are Python/GPU — the deterministic
// C++ implicit-diff lane has NO living peer; Cerid owns it. Self-contained (reuses tape.hpp for the Jacobians +
// sparse_reverse's dense LU for the implicit solves). ADR-0097.
//
//   • ROOT / Newton:  F(x*,θ)=0  ⇒  ∂x*/∂θ = −(∂F/∂x)⁻¹ ∂F/∂θ.  VJP: z = (∂F/∂x)⁻ᵀ x̄ ; θ̄ = −(∂F/∂θ)ᵀ z.
//   • FIXED POINT:    x* = g(x*,θ)  ⇒  F = g − x (the same rule with ∂F/∂x = ∂g/∂x − I).
//   • EQUALITY QP:    min ½xᵀQx+qᵀx s.t. Ax=b  ⇒  KKT [Q Aᵀ; A 0][x*;ν*] = [−q; b] (OptNet). VJP via the KKT solve.

#include <crd/hesap/autodiff/sparse_reverse.hpp> // dense_lu_factor / dense_lu_solve / dense_lu_solve_t
#include <crd/hesap/autodiff/tape.hpp>           // reverse Var (∂F/∂x, ∂F/∂θ via the tape)

#include <crd/core/types.hpp>

namespace crd::hesap::autodiff::reverse
{
namespace sp = crd::hesap::autodiff::reverse::sparse;

// ---- ROOT / nonlinear-solve VJP (IFT) ------------------------------------------------------------------------
// F: `void F(const Var* x, const Var* θ, Var* out, int n, int np)` — out = F(x,θ) (n eqns). Given the root x* (found
// by any solver) + the loss cotangent x̄ (n), fills theta_bar (np) = dL/dθ. NEVER touches the solver iterations.
// Scratch: vscr Var[2n+np], jac[n*n], piv[n], z[n], tmp[n].
template <class F>
inline void root_vjp(const F& Ffn, const crd::f64* x_star, const crd::f64* theta, const crd::f64* xbar,
                     crd::f64* theta_bar, int n, int np, Tape& tape, Var* vscr, crd::f64* jac, int* piv, crd::f64* z,
                     crd::f64* tmp) noexcept
{
    tape.reset();
    Var* xv  = vscr;
    Var* thv = xv + n;
    Var* out = thv + np;
    for (int i = 0; i < n; ++i) { xv[i] = make_leaf(tape, x_star[i]); }
    for (int j = 0; j < np; ++j) { thv[j] = make_leaf(tape, theta[j]); }
    Ffn(xv, thv, out, n, np);
    // ∂F/∂x (n×n): one backward per equation, seeded with e_i
    for (int i = 0; i < n; ++i)
    {
        tape.zero_adjoints();
        tape.seed(out[i].node, 1.0);
        tape.backward();
        for (int k = 0; k < n; ++k) { jac[i * n + k] = tape.grad(xv[k].node); }
    }
    // z = (∂F/∂x)⁻ᵀ x̄
    sp::dense_lu_factor(jac, piv, n);
    sp::dense_lu_solve_t(jac, piv, xbar, z, n, tmp);
    // θ̄ = −(∂F/∂θ)ᵀ z : seed the F output with z, ONE backward, read the θ-grads
    tape.zero_adjoints();
    for (int i = 0; i < n; ++i) { tape.seed(out[i].node, z[i]); }
    tape.backward();
    for (int j = 0; j < np; ++j) { theta_bar[j] = -tape.grad(thv[j].node); }
}

// ---- FIXED-POINT VJP: x* = g(x*,θ) — reuse the root rule with F = g − x ---------------------------------------
// g: `void g(const Var* x, const Var* θ, Var* out, int n, int np)` — out = g(x,θ).
template <class G>
inline void fixed_point_vjp(const G& g, const crd::f64* x_star, const crd::f64* theta, const crd::f64* xbar,
                            crd::f64* theta_bar, int n, int np, Tape& tape, Var* vscr, crd::f64* jac, int* piv,
                            crd::f64* z, crd::f64* tmp) noexcept
{
    const auto fwrap = [&g](const Var* x, const Var* th, Var* out, int nn, int npp) noexcept
    {
        g(x, th, out, nn, npp);
        for (int i = 0; i < nn; ++i) { out[i] = out[i] - x[i]; } // F = g − x
    };
    root_vjp(fwrap, x_star, theta, xbar, theta_bar, n, np, tape, vscr, jac, piv, z, tmp);
}

// ---- EQUALITY-QP VJP (OptNet): min ½xᵀQx+qᵀx s.t. Ax=b -------------------------------------------------------
// KKT M=[Q Aᵀ; A 0] (size N=n+m), M[x*;ν*]=[−q;b]. Given x̄ (n), fills gQ(n*n), gq(n), gA(m*n), gb(m).
// Solve M d = [−x̄; 0] (M symmetric) ⇒ gq=d_x, gb=−d_ν, gQ=½(d_x x*ᵀ + x* d_xᵀ), gA=d_ν x*ᵀ + ν* d_xᵀ.
// Scratch: kkt[(n+m)²], piv[n+m], rhs[n+m], dvec[n+m].
inline void qp_eq_vjp(const crd::f64* Q, const crd::f64* A, const crd::f64* x_star, const crd::f64* nu_star,
                      const crd::f64* xbar, int n, int m, crd::f64* gQ, crd::f64* gq, crd::f64* gA, crd::f64* gb,
                      crd::f64* kkt, int* piv, crd::f64* rhs, crd::f64* dvec) noexcept
{
    const int ntot = n + m;
    for (int i = 0; i < ntot * ntot; ++i) { kkt[i] = 0.0; }
    for (int i = 0; i < n; ++i) // top-left Q
    {
        for (int j = 0; j < n; ++j) { kkt[i * ntot + j] = Q[i * n + j]; }
    }
    for (int i = 0; i < m; ++i) // A (bottom-left) and Aᵀ (top-right)
    {
        for (int j = 0; j < n; ++j)
        {
            kkt[(n + i) * ntot + j] = A[i * n + j];
            kkt[j * ntot + (n + i)] = A[i * n + j];
        }
    }
    for (int i = 0; i < n; ++i) { rhs[i] = -xbar[i]; }
    for (int i = 0; i < m; ++i) { rhs[n + i] = 0.0; }
    sp::dense_lu_factor(kkt, piv, ntot);
    sp::dense_lu_solve(kkt, piv, rhs, dvec, ntot);
    const crd::f64* dx = dvec;     // n
    const crd::f64* dn = dvec + n; // m
    for (int j = 0; j < n; ++j) { gq[j] = dx[j]; }
    for (int i = 0; i < m; ++i) { gb[i] = -dn[i]; }
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { gQ[i * n + j] = 0.5 * (dx[i] * x_star[j] + x_star[i] * dx[j]); }
    }
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j) { gA[i * n + j] = dn[i] * x_star[j] + nu_star[i] * dx[j]; }
    }
}

} // namespace crd::hesap::autodiff::reverse
