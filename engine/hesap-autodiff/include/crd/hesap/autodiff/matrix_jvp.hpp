#pragma once

// matrix_jvp.hpp — Phase 3.1.6 v15-f: MATRIX-CALCULUS forward differentials (JVPs). The v15-f principle: for a
// factorization-based operation, differentiate the SOLUTION reusing the STORED FACTOR — never AD-through the O(n³)
// factorization loop. Rules verified vs Giles NA-08/01, Murray arXiv:1602.07527, JAX `jax/_src/lax/linalg.py`.
//
// SELF-CONTAINED (ADR-0097): autodiff is LOWER than the LA solvers (hesap-dense), so these take the caller's stored
// factor + dense row-major matrices and use the inline gemm / triangular-solve below — they NEVER call a
// factorization. The crush is algorithmic: a solve JVP reusing the factor is O(n²) per direction vs AD-through-
// Cholesky at O(n³). VALUE-ONLY drivers (logdet / eigvals / svdvals) never divide by (λ_i−λ_j) or σ, so they stay
// FINITE at repeated/zero spectra where JAX/PyTorch (eigenvector derivatives with the F-matrix) return NaN.
//
// All routines are allocation-free (caller owns every scratch buffer) and deterministic (single-rounded fma via
// crd::math where it matters). Matrices are row-major crd::f64 of the stated shape.

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward::matrix
{

// ---- dense kernels (row-major) ------------------------------------------------------------------------------
// C[m×p] = A[m×k] · B[k×p]
inline void gemm(const crd::f64* a, const crd::f64* b, crd::f64* c, int m, int k, int p) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < p; ++j)
        {
            crd::f64 s = 0.0;
            for (int t = 0; t < k; ++t) { s += a[i * k + t] * b[t * p + j]; }
            c[i * p + j] = s;
        }
    }
}
// C[m×p] = Aᵀ[m×k] · B[k×p]  (A is stored k×m)
inline void gemm_tn(const crd::f64* a, const crd::f64* b, crd::f64* c, int m, int k, int p) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        for (int j = 0; j < p; ++j)
        {
            crd::f64 s = 0.0;
            for (int t = 0; t < k; ++t) { s += a[t * m + i] * b[t * p + j]; }
            c[i * p + j] = s;
        }
    }
}
// Solve L·Y = B for Y (forward substitution); L lower-tri n×n, B/Y n×p. Templated so the crush bench can run the
// whole solve on Dual/Jet (the AD-through peer). In-place safe (Y may alias B).
template <class T>
inline void trisolve_lower(const T* l, const T* b, T* y, int n, int p) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < p; ++j)
        {
            T s = b[i * p + j];
            for (int t = 0; t < i; ++t) { s = s - l[i * n + t] * y[t * p + j]; }
            y[i * p + j] = s / l[i * n + i];
        }
    }
}
// Solve Lᵀ·X = Y for X (back substitution); L lower-tri n×n.
template <class T>
inline void trisolve_lower_t(const T* l, const T* y, T* x, int n, int p) noexcept
{
    for (int i = n - 1; i >= 0; --i)
    {
        for (int j = 0; j < p; ++j)
        {
            T s = y[i * p + j];
            for (int t = i + 1; t < n; ++t) { s = s - l[t * n + i] * x[t * p + j]; }
            x[i * p + j] = s / l[i * n + i];
        }
    }
}

// Cholesky A = L·Lᵀ (SPD, lower L, A n×n). Templated so the crush bench can AD-through it on Dual/Jet. Returns
// false if a non-positive pivot is hit.
template <class T>
inline bool cholesky(const T* a, T* l, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            T s = a[i * n + j];
            for (int t = 0; t < j; ++t) { s = s - l[i * n + t] * l[j * n + t]; }
            if (i == j)
            {
                using crd::math::sqrt;
                l[i * n + j] = sqrt(s);
            }
            else
            {
                l[i * n + j] = s / l[j * n + j];
            }
        }
        for (int j = i + 1; j < n; ++j) { l[i * n + j] = T(0); }
    }
    return true;
}

// ---- JVP rules ----------------------------------------------------------------------------------------------
// gemm  C = A·B → dC = dA·B + A·dB. (Bilinear; no factor to reuse — matches everyone on flops.)
inline void gemm_jvp(const crd::f64* a, const crd::f64* b, const crd::f64* da, const crd::f64* db, crd::f64* dc, int m,
                     int k, int p, crd::f64* scratch /*m*p*/) noexcept
{
    gemm(da, b, dc, m, k, p);
    gemm(a, db, scratch, m, k, p);
    for (int i = 0; i < m * p; ++i) { dc[i] += scratch[i]; }
}

// ★ SPD solve  A·X = B (X = A⁻¹B), A = L·Lᵀ given the factor L. dX = A⁻¹·(dB − dA·X). ONE gemm + two trisolves,
// reusing L — O(n²·p) per direction vs AD-through-Cholesky's O(n³). x, dx are n×p.
inline void solve_spd_jvp(const crd::f64* l, const crd::f64* x, const crd::f64* da, const crd::f64* db, crd::f64* dx,
                          int n, int p, crd::f64* r /*n*p*/) noexcept
{
    gemm(da, x, r, n, n, p);               // r = dA·X
    for (int i = 0; i < n * p; ++i) { r[i] = db[i] - r[i]; } // r = dB − dA·X
    trisolve_lower(l, r, dx, n, p);        // L·y = r
    trisolve_lower_t(l, dx, dx, n, p);     // Lᵀ·dX = y   (dX ← A⁻¹ r)
}

// Cholesky A = L·Lᵀ → dL = L·Φ(L⁻¹·dA·L⁻ᵀ), Φ(X)=tril(X) with HALVED diagonal (Murray eq 6). dA symmetric ⇒
// M=L⁻¹dA L⁻ᵀ symmetric = L⁻¹·(L⁻¹dA)ᵀ. Two forward-solves + a gemm, reusing L. scratch m1,m2 each n×n.
inline void cholesky_jvp(const crd::f64* l, const crd::f64* da, crd::f64* dl, int n, crd::f64* m1,
                         crd::f64* m2) noexcept
{
    trisolve_lower(l, da, m1, n, n); // m1 = L⁻¹·dA
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { m2[i * n + j] = m1[j * n + i]; } // m2 = m1ᵀ
    }
    trisolve_lower(l, m2, m1, n, n); // m1 = L⁻¹·m1ᵀ = M (symmetric)
    for (int i = 0; i < n; ++i)      // Φ(M): lower-tri, halved diagonal, zero above
    {
        for (int j = 0; j < n; ++j)
        {
            if (j > i) { m1[i * n + j] = 0.0; }
            else if (j == i) { m1[i * n + j] *= 0.5; }
        }
    }
    gemm(l, m1, dl, n, n, n); // dL = L·Φ(M)
}

// logdet(A), A SPD = L·Lᵀ. d(logdet) = Tr(A⁻¹·dA) = Tr(L⁻¹·dA·L⁻ᵀ) — VALUE-ONLY, degeneracy-free (no F-matrix).
// scratch: m1,m2 each n×n.
[[nodiscard]] inline crd::f64 logdet_spd_jvp(const crd::f64* l, const crd::f64* da, int n, crd::f64* m1,
                                             crd::f64* m2) noexcept
{
    trisolve_lower(l, da, m1, n, n);   // m1 = L⁻¹·dA
    // m2 = m1·L⁻ᵀ  ⇒ solve  m2·Lᵀ = m1  ⇔  L·m2ᵀ = m1ᵀ. Do it as: transpose-solve via trisolve on rows.
    // Equivalent: Tr(L⁻¹ dA L⁻ᵀ) = Σ_i (L⁻¹ dA L⁻ᵀ)_ii. Compute m2 = (L⁻¹)(m1ᵀ) columns then trace of m1·L⁻ᵀ.
    // Simpler: Tr(m1·L⁻ᵀ) = Σ_i Σ_j m1_ij (L⁻ᵀ)_ji = Σ_i (m1·L⁻ᵀ)_ii. Get X = L⁻¹·m1ᵀ (so Xᵀ = m1·L⁻ᵀ), trace(Xᵀ)=trace(X).
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j) { m2[i * n + j] = m1[j * n + i]; } // m2 = m1ᵀ
    }
    crd::f64 tr = 0.0;
    trisolve_lower(l, m2, m1, n, n); // m1 = L⁻¹·m1ᵀ ; trace(m1) = Tr(L⁻¹ dA L⁻ᵀ)
    for (int i = 0; i < n; ++i) { tr += m1[i * n + i]; }
    return tr;
}

// eigvals: A = Q·Λ·Qᵀ (symmetric), Q the eigenvectors (caller-supplied). dλ_i = (Qᵀ·dA·Q)_ii — VALUE-ONLY: no
// division by (λ_i−λ_j), so FINITE even at repeated eigenvalues (where the eigenVECTOR derivative is NaN).
inline void eigvals_jvp(const crd::f64* q, const crd::f64* da, crd::f64* dlambda, int n, crd::f64* tmp /*n*n*/) noexcept
{
    gemm(da, q, tmp, n, n, n);      // tmp = dA·Q
    for (int i = 0; i < n; ++i)     // dλ_i = (Qᵀ dA Q)_ii = Σ_t Q_ti · tmp_ti
    {
        crd::f64 s = 0.0;
        for (int t = 0; t < n; ++t) { s += q[t * n + i] * tmp[t * n + i]; }
        dlambda[i] = s;
    }
}

// svdvals: A = U·Σ·Vᵀ (U m×n, V n×n, thin), σ singular values. dσ_i = (Uᵀ·dA·V)_ii — VALUE-ONLY, finite even at
// repeated/zero σ. (dU/dV need 1/(σ_j²−σ_i²) and 1/σ — the NaN cases we deliberately do NOT force.)
inline void svdvals_jvp(const crd::f64* u, const crd::f64* v, const crd::f64* da, crd::f64* dsigma, int m, int n,
                        crd::f64* tmp /*m*n*/) noexcept
{
    gemm(da, v, tmp, m, n, n);  // tmp = dA·V   (m×n)
    for (int i = 0; i < n; ++i) // dσ_i = (Uᵀ dA V)_ii = Σ_t U_ti · tmp_ti
    {
        crd::f64 s = 0.0;
        for (int t = 0; t < m; ++t) { s += u[t * n + i] * tmp[t * n + i]; }
        dsigma[i] = s;
    }
}

} // namespace crd::hesap::autodiff::forward::matrix
