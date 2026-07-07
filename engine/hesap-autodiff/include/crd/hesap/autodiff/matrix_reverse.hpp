#pragma once

// matrix_reverse.hpp — Phase 3.1.6 v16-d: MATRIX-CALCULUS reverse differentials (VJPs) = the exact TRANSPOSE of the
// v15-f JVPs (Giles NA-08/01, Seeger/Murray arXiv:1602.07527, JAX `linalg`). The v16-d principle mirrors v15-f:
// differentiate the SOLUTION reusing the STORED FACTOR — NEVER AD-through the O(n³) factorization loop. VALUE-ONLY
// drivers (logdet / eigvals / svdvals) never divide by (λ_i−λ_j) or σ, so they stay FINITE at repeated/zero spectra
// where JAX/PyTorch (eigenvector F-matrix) return NaN. Self-contained (reuses matrix_jvp's gemm/trisolve/cholesky +
// sparse_reverse's dense LU), allocation-free (caller scratch), deterministic. ADR-0097.
//
// Every rule is verified by the ADJOINT IDENTITY  ⟨ȳ, JVP(v)⟩ == ⟨VJP(ȳ), v⟩  against the FD-gated v15-f JVP
// (test_matrix_reverse.cpp), plus direct central-FD and JAX value+grad parity.
//
// The rules:
//   gemm  C=A·B      : Ā = C̄·Bᵀ, B̄ = Aᵀ·C̄.
//   solve X=A⁻¹B     : B̄ = A⁻ᵀ·X̄ (one back-solve on the stored factor), Ā = −B̄·Xᵀ  (SPD: symmetrised).
//   chol  A=L·Lᵀ     : Ā = sym(L⁻ᵀ·Φ(Lᵀ·L̄)·L⁻¹),  Φ = tril, halved diagonal.
//   logdet(A)        : Ā = ḡ·A⁻ᵀ  (SPD: ḡ·A⁻¹) — value-only, degeneracy-free.
//   eigvals(A=QΛQᵀ)  : Ā = Q·diag(λ̄)·Qᵀ — value-only, finite at repeated λ.
//   svdvals(A=UΣVᵀ)  : Ā = U·diag(σ̄)·Vᵀ — value-only, finite at repeated/zero σ.

#include <crd/hesap/autodiff/matrix_jvp.hpp>     // gemm / gemm_tn / trisolve_lower / trisolve_lower_t / cholesky
#include <crd/hesap/autodiff/sparse_reverse.hpp> // dense_lu_factor / dense_lu_solve / dense_lu_solve_t (general solve)

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::reverse::matrix
{
namespace mj = crd::hesap::autodiff::forward::matrix;
namespace sp = crd::hesap::autodiff::reverse::sparse;

// C[m×n] = A[m×k]·Bᵀ  (B stored n×k) — the transposed-right gemm the VJPs need.
inline void gemm_nt(const crd::f64* a, const crd::f64* b, crd::f64* c, int m, int k, int n) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            crd::f64 s = 0.0;
            for (int t = 0; t < k; ++t) { s += a[i * k + t] * b[j * k + t]; }
            c[i * n + j] = s;
        }
    }
}

// ---- gemm  C = A·B  (A[m×k], B[k×p]) : Ā = C̄·Bᵀ (m×k) ; B̄ = Aᵀ·C̄ (k×p) --------------------------------------
inline void gemm_vjp(const crd::f64* a, const crd::f64* b, const crd::f64* gc, crd::f64* ga, crd::f64* gb, int m,
                     int k, int p) noexcept
{
    gemm_nt(gc, b, ga, m, p, k);     // Ā[m×k] = C̄[m×p]·Bᵀ[p×k]
    mj::gemm_tn(a, gc, gb, k, m, p); // B̄[k×p] = Aᵀ[k×m]·C̄[m×p]
}

// ---- GENERAL solve  X = A⁻¹B  (A n×n LU-factored a_lu/piv, X/B n×p) --------------------------------------------
// Given X̄: B̄ = A⁻ᵀ·X̄ (per-column back-solve on the STORED factor) ; Ā = −B̄·Xᵀ (n×n). scratch rhs/sol/tmp (n each).
inline void solve_lu_vjp(const crd::f64* a_lu, const int* piv, const crd::f64* x, const crd::f64* xbar, crd::f64* ga,
                         crd::f64* gb, int n, int p, crd::f64* rhs, crd::f64* sol, crd::f64* tmp) noexcept
{
    for (int j = 0; j < p; ++j) // B̄[:,j] = A⁻ᵀ·X̄[:,j]
    {
        for (int i = 0; i < n; ++i) { rhs[i] = xbar[i * p + j]; }
        sp::dense_lu_solve_t(a_lu, piv, rhs, sol, n, tmp);
        for (int i = 0; i < n; ++i) { gb[i * p + j] = sol[i]; }
    }
    gemm_nt(gb, x, ga, n, p, n);                       // ga = B̄·Xᵀ (n×n)
    for (int i = 0; i < n * n; ++i) { ga[i] = -ga[i]; } // Ā = −B̄·Xᵀ
}

// ---- SPD solve  X = A⁻¹B  (A = L·Lᵀ, X/B n×p) -----------------------------------------------------------------
// Given X̄: B̄ = A⁻¹·X̄ (chol back-solve reusing L) ; Ā = −sym(B̄·Xᵀ) (A symmetric). scratch t1(n*p), m1(n*n).
inline void solve_spd_vjp(const crd::f64* l, const crd::f64* x, const crd::f64* xbar, crd::f64* ga, crd::f64* gb,
                          int n, int p, crd::f64* t1, crd::f64* m1) noexcept
{
    mj::trisolve_lower(l, xbar, t1, n, p);   // L·y = X̄
    mj::trisolve_lower_t(l, t1, gb, n, p);   // Lᵀ·B̄ = y  ⇒ B̄ = A⁻¹X̄
    gemm_nt(gb, x, m1, n, p, n);             // m1 = B̄·Xᵀ
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { ga[i * n + j] = -0.5 * (m1[i * n + j] + m1[j * n + i]); } // −sym
    }
}

// ---- Cholesky  A = L·Lᵀ  (given L̄ lower-tri) : Ā = sym(L⁻ᵀ·Φ(Lᵀ·L̄)·L⁻¹) --------------------------------------
// Φ(X) = tril(X) with HALVED diagonal (Murray). scratch s,y,w each n×n.
inline void cholesky_vjp(const crd::f64* l, const crd::f64* lbar, crd::f64* ga, int n, crd::f64* s, crd::f64* y,
                         crd::f64* w) noexcept
{
    mj::gemm_tn(l, lbar, s, n, n, n); // s = Lᵀ·L̄
    for (int i = 0; i < n; ++i)       // Φ(s): lower-tri, halved diagonal, zero above
    {
        for (int j = 0; j < n; ++j)
        {
            if (j > i) { s[i * n + j] = 0.0; }
            else if (j == i) { s[i * n + j] *= 0.5; }
        }
    }
    mj::trisolve_lower_t(l, s, y, n, n); // y = L⁻ᵀ·Φ(s)   (adjoint of dA↦L⁻¹dA L⁻ᵀ is N↦L⁻ᵀ N L⁻¹)
    for (int i = 0; i < n; ++i)          // w = yᵀ  (so L⁻ᵀ·yᵀ = (y·L⁻¹)ᵀ = Ā_rawᵀ)
    {
        for (int j = 0; j < n; ++j) { w[i * n + j] = y[j * n + i]; }
    }
    mj::trisolve_lower_t(l, w, s, n, n); // s = L⁻ᵀ·yᵀ = Ā_rawᵀ  (Ā_raw = L⁻ᵀ·Φ(Lᵀ·L̄)·L⁻¹)
    for (int i = 0; i < n; ++i)          // Ā = sym(Ā_raw) = ½(sᵀ + s)
    {
        for (int j = 0; j < n; ++j) { ga[i * n + j] = 0.5 * (s[j * n + i] + s[i * n + j]); }
    }
}

// ---- logdet(A) VALUE-ONLY, degeneracy-free (no F-matrix) -------------------------------------------------------
// SPD A = L·Lᵀ, scalar ḡ : Ā = ḡ·A⁻¹ (symmetric). scratch eye,y each n×n. (Ā = A⁻¹ is computed via the factor.)
inline void logdet_spd_vjp(const crd::f64* l, crd::f64 gbar, crd::f64* ga, int n, crd::f64* eye, crd::f64* y) noexcept
{
    for (int i = 0; i < n * n; ++i) { eye[i] = 0.0; }
    for (int i = 0; i < n; ++i) { eye[i * n + i] = 1.0; }
    mj::trisolve_lower(l, eye, y, n, n);  // y = L⁻¹
    mj::trisolve_lower_t(l, y, ga, n, n); // ga = A⁻¹
    for (int i = 0; i < n * n; ++i) { ga[i] *= gbar; }
}
// GENERAL A (LU factor a_lu/piv), scalar ḡ : Ā = ḡ·A⁻ᵀ. scratch e,x each n.
inline void logdet_lu_vjp(const crd::f64* a_lu, const int* piv, crd::f64 gbar, crd::f64* ga, int n, crd::f64* e,
                          crd::f64* x, crd::f64* tmp) noexcept
{
    for (int kcol = 0; kcol < n; ++kcol) // Ā[:,k] = ḡ·(A⁻ᵀ)_{:,k} = ḡ·solve(Aᵀ, e_k)
    {
        for (int i = 0; i < n; ++i) { e[i] = (i == kcol) ? 1.0 : 0.0; }
        sp::dense_lu_solve_t(a_lu, piv, e, x, n, tmp);
        for (int i = 0; i < n; ++i) { ga[i * n + kcol] = gbar * x[i]; }
    }
}

// ---- eigvals (symmetric A = Q·Λ·Qᵀ, Q eigenvectors in COLUMNS: q[t*n+i] = comp t of eigvec i) ------------------
// Given λ̄ : Ā = Q·diag(λ̄)·Qᵀ = Σ_i λ̄_i q_i q_iᵀ — VALUE-ONLY (no 1/(λ_i−λ_j)), finite at repeated eigenvalues.
// scratch tmp (n*n).
inline void eigvals_sym_vjp(const crd::f64* q, const crd::f64* lbar, crd::f64* ga, int n, crd::f64* tmp) noexcept
{
    for (int a = 0; a < n; ++a)
    {
        for (int i = 0; i < n; ++i) { tmp[a * n + i] = q[a * n + i] * lbar[i]; } // tmp = Q·diag(λ̄)
    }
    gemm_nt(tmp, q, ga, n, n, n); // Ā = tmp·Qᵀ
}

// ---- svdvals (A = U·Σ·Vᵀ, U m×n, V n×n thin) ------------------------------------------------------------------
// Given σ̄ : Ā = U·diag(σ̄)·Vᵀ (m×n) — VALUE-ONLY, finite at repeated/zero σ. scratch tmp (m*n).
inline void svdvals_vjp(const crd::f64* u, const crd::f64* v, const crd::f64* sbar, crd::f64* ga, int m, int n,
                        crd::f64* tmp) noexcept
{
    for (int a = 0; a < m; ++a)
    {
        for (int i = 0; i < n; ++i) { tmp[a * n + i] = u[a * n + i] * sbar[i]; } // tmp = U·diag(σ̄)
    }
    gemm_nt(tmp, v, ga, m, n, n); // Ā = tmp·Vᵀ
}

} // namespace crd::hesap::autodiff::reverse::matrix
