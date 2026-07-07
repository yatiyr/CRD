#pragma once

// sparse_reverse.hpp — Phase 3.1.6 v16-c: reverse-mode VJPs over the SPARSE (CSR) linear-algebra surface — spmv,
// spmm, and a sparse SOLVE, differentiated wrt BOTH the dense operand AND the sparse matrix ENTRIES (gradients come
// back in the same CSR pattern). PyTorch/TensorFlow LACK sparse-matrix autodiff (arXiv:2212.05159) — this is a clean
// CAPABILITY crush, feeding v9 sparse BDF Jacobians + the 3.1.12 FEA adjoint. Self-contained f64 over raw CSR
// (row_ptr[m+1] / col_idx[nnz] / values[nnz] — the hesap-sparse layout), the nn_reverse / matrix_jvp pattern:
// autodiff stays LOWER than the LA solvers (a self-contained inline dense LU factors the solve; the VALUE-reuse is
// the point, not the solve kernel). Deterministic (fixed order, crd::math), allocation-free (caller scratch).
// ADR-0097.
//
// The rules (each an exact transpose of the forward):
//   spmv   y = A·x            : Ā_ij = ȳ_i·x_j  (on the pattern) ; x̄ = Aᵀ·ȳ.
//   spmm   Y = A·X            : Ā_ij = Σ_p Ȳ_ip·X_jp             ; X̄ = Aᵀ·Ȳ.
//   solve  A·x = b            : b̄ = A⁻ᵀ·x̄ (one back-solve on the STORED factor) ; Ā_ij = −b̄_i·x_j (on the pattern).

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::reverse::sparse
{

// ---- spmv  y[i] = Σ_{e∈row i} vals[e]·x[col_idx[e]] --------------------------------------------------------
inline void csr_spmv(const int* row_ptr, const int* col_idx, const crd::f64* vals, const crd::f64* x, crd::f64* y,
                     int m) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        crd::f64 s = 0.0;
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e) { s += vals[e] * x[col_idx[e]]; }
        y[i] = s;
    }
}
// VJP: gvals[e] = ȳ_i·x_{col(e)} (per nonzero) ; gx[j] = (Aᵀ·ȳ)_j = Σ_{e: col(e)=j} vals[e]·ȳ_{row(e)} (scatter-add).
inline void csr_spmv_vjp(const int* row_ptr, const int* col_idx, const crd::f64* vals, const crd::f64* x,
                         const crd::f64* gy, crd::f64* gvals, crd::f64* gx, int m, int n) noexcept
{
    for (int j = 0; j < n; ++j) { gx[j] = 0.0; }
    for (int i = 0; i < m; ++i)
    {
        const crd::f64 gyi = gy[i];
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e)
        {
            const int j = col_idx[e];
            gvals[e]    = gyi * x[j];
            gx[j] += vals[e] * gyi;
        }
    }
}

// ---- spmm  Y[i,:] = Σ_{e∈row i} vals[e]·X[col_idx[e],:]  (X is n×p, Y is m×p, row-major) ---------------------
inline void csr_spmm(const int* row_ptr, const int* col_idx, const crd::f64* vals, const crd::f64* x, crd::f64* y,
                     int m, int p) noexcept
{
    for (int i = 0; i < m; ++i)
    {
        for (int c = 0; c < p; ++c) { y[i * p + c] = 0.0; }
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e)
        {
            const crd::f64  v  = vals[e];
            const crd::f64* xr = x + static_cast<crd::i64>(col_idx[e]) * p;
            crd::f64*       yr = y + static_cast<crd::i64>(i) * p;
            for (int c = 0; c < p; ++c) { yr[c] += v * xr[c]; }
        }
    }
}
// VJP: gvals[e] = Σ_c Ȳ_{row(e),c}·X_{col(e),c} ; gX[j,:] += Σ_{e: col=j} vals[e]·Ȳ_{row(e),:} (= Aᵀ·Ȳ).
inline void csr_spmm_vjp(const int* row_ptr, const int* col_idx, const crd::f64* vals, const crd::f64* x,
                         const crd::f64* gy, crd::f64* gvals, crd::f64* gx, int m, int n, int p) noexcept
{
    for (crd::i64 i = 0; i < static_cast<crd::i64>(n) * p; ++i) { gx[i] = 0.0; }
    for (int i = 0; i < m; ++i)
    {
        const crd::f64* gyr = gy + static_cast<crd::i64>(i) * p;
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e)
        {
            const int       j  = col_idx[e];
            const crd::f64* xr = x + static_cast<crd::i64>(j) * p;
            crd::f64*       gxr = gx + static_cast<crd::i64>(j) * p;
            crd::f64        gv = 0.0;
            const crd::f64  v  = vals[e];
            for (int c = 0; c < p; ++c)
            {
                gv += gyr[c] * xr[c];
                gxr[c] += v * gyr[c];
            }
            gvals[e] = gv;
        }
    }
}

// ---- a self-contained dense LU (partial pivoting) — the SOLVE's stored factor (reused for A and Aᵀ) -----------
inline void dense_lu_factor(crd::f64* a, int* piv, int n) noexcept
{
    for (int i = 0; i < n; ++i) { piv[i] = i; }
    for (int k = 0; k < n; ++k)
    {
        int      p  = k;
        crd::f64 mx = crd::math::abs(a[k * n + k]);
        for (int i = k + 1; i < n; ++i)
        {
            const crd::f64 v = crd::math::abs(a[i * n + k]);
            if (v > mx) { mx = v; p = i; }
        }
        if (p != k)
        {
            for (int j = 0; j < n; ++j)
            {
                const crd::f64 t = a[k * n + j];
                a[k * n + j]     = a[p * n + j];
                a[p * n + j]     = t;
            }
            const int tp = piv[k];
            piv[k]       = piv[p];
            piv[p]       = tp;
        }
        const crd::f64 akk = a[k * n + k];
        for (int i = k + 1; i < n; ++i)
        {
            const crd::f64 f = a[i * n + k] / akk;
            a[i * n + k]     = f;
            for (int j = k + 1; j < n; ++j) { a[i * n + j] -= f * a[k * n + j]; }
        }
    }
}
// Solve A·x = b using the stored factor (P·A = L·U). x may not alias b.
inline void dense_lu_solve(const crd::f64* a, const int* piv, const crd::f64* b, crd::f64* x, int n) noexcept
{
    for (int i = 0; i < n; ++i) { x[i] = b[piv[i]]; }               // apply P
    for (int i = 0; i < n; ++i) { for (int j = 0; j < i; ++j) { x[i] -= a[i * n + j] * x[j]; } } // L·y = Pb (unit diag)
    for (int i = n - 1; i >= 0; --i)
    {
        for (int j = i + 1; j < n; ++j) { x[i] -= a[i * n + j] * x[j]; }
        x[i] /= a[i * n + i]; // U·x = y
    }
}
// Solve Aᵀ·x = b using the SAME factor (Aᵀ = Uᵀ·Lᵀ·P). `tmp` (length n) is caller scratch.
inline void dense_lu_solve_t(const crd::f64* a, const int* piv, const crd::f64* b, crd::f64* x, int n,
                             crd::f64* tmp) noexcept
{
    for (int i = 0; i < n; ++i) // Uᵀ·v = b (forward; Uᵀ lower-tri, diag = U_ii)
    {
        crd::f64 s = b[i];
        for (int j = 0; j < i; ++j) { s -= a[j * n + i] * tmp[j]; }
        tmp[i] = s / a[i * n + i];
    }
    for (int i = n - 1; i >= 0; --i) // Lᵀ·w = v (back; Lᵀ upper-tri, unit diag) — in place
    {
        for (int j = i + 1; j < n; ++j) { tmp[i] -= a[j * n + i] * tmp[j]; }
    }
    for (int i = 0; i < n; ++i) { x[piv[i]] = tmp[i]; } // x = Pᵀ·w
}

// ---- sparse solve  A·x = b  (A n×n CSR) --------------------------------------------------------------------
// Forward: densify → LU → solve. `a`(n*n) is overwritten with the factor, `piv`(n) the pivots (both reused by the VJP).
inline void csr_solve(const int* row_ptr, const int* col_idx, const crd::f64* vals, const crd::f64* b, crd::f64* x,
                      int n, crd::f64* a, int* piv) noexcept
{
    for (crd::i64 i = 0; i < static_cast<crd::i64>(n) * n; ++i) { a[i] = 0.0; }
    for (int i = 0; i < n; ++i)
    {
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e) { a[static_cast<crd::i64>(i) * n + col_idx[e]] = vals[e]; }
    }
    dense_lu_factor(a, piv, n);
    dense_lu_solve(a, piv, b, x, n);
}
// VJP wrt the sparse entries + b, given the forward solution x and the output cotangent cx (= ∂L/∂x):
//   b̄ = A⁻ᵀ·cx (one back-solve on the stored factor) ; Ā_ij = −b̄_i·x_j (per nonzero). Reuses the factor a/piv from
//   csr_solve — the v16-d factor-reuse crush. `z`,`tmp` (length n) are caller scratch.
inline void csr_solve_vjp(const int* row_ptr, const int* col_idx, const crd::f64* x, const crd::f64* cx,
                          crd::f64* gvals, crd::f64* gb, int n, const crd::f64* a, const int* piv, crd::f64* z,
                          crd::f64* tmp) noexcept
{
    dense_lu_solve_t(a, piv, cx, z, n, tmp); // z = A⁻ᵀ·cx = b̄
    for (int j = 0; j < n; ++j) { gb[j] = z[j]; }
    for (int i = 0; i < n; ++i)
    {
        for (int e = row_ptr[i]; e < row_ptr[i + 1]; ++e) { gvals[e] = -z[i] * x[col_idx[e]]; }
    }
}

} // namespace crd::hesap::autodiff::reverse::sparse
