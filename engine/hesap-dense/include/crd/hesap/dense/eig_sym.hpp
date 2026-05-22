#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3a-1 — symmetric eigenvalue decomposition (real f32/f64).
//
// A = V * diag(values) * V^T, A symmetric. Computed by:
//   1. blocked Householder tridiagonalization  Q^T A Q = T  (dsytrd)
//   2. implicit-shift QL/QR on the tridiagonal (dsteqr), accumulating
//      the tridiagonal eigenvectors into Z
//   3. back-transform  V = Q * Z  (dormtr)
//   4. ascending sort of `values` + matching column permutation of V
//      (permutation-array applied once — D(dense-eig)-3)
//   5. eigenvector sign fixed: lowest-index largest-magnitude component
//      positive (D(dense-eig)-4) — reproducible + testable.
//
// Determinism (D(dense-eig)-1..5, → ADR-0065 §17): RNG-free; fixed
// Wilkinson shift; faithful LAPACK split threshold; two-rounded reductions.
//
// Interfaces are complex-aware so v3a-1b (complex Hermitian) drops in:
//   - `tau` carries T (complex reflectors for Hermitian)
//   - `D`/`E` are RealType<T> (the tridiagonal is real even for Hermitian)
//   - `steqr` templates the eigenvector scalar Z separately from the real
//     tridiagonal scalar R (real Givens applied to complex columns).
// -----------------------------------------------------------------------

template <typename T>
struct EigSym
{
    Vector<RealType<T>> values;  // ascending
    Matrix<T> vectors;           // column k is the eigenvector for values[k]

    explicit EigSym(crd::memory::IAllocator* alloc) noexcept : values(alloc), vectors(alloc) {}
    EigSym(crd::memory::IAllocator* alloc, crd::usize n) : values(alloc, n), vectors(alloc, n, n) {}

    EigSym(EigSym&&) noexcept = default;
    EigSym& operator=(EigSym&&) noexcept = default;
    EigSym(const EigSym&) = delete;
    EigSym& operator=(const EigSym&) = delete;
};

// =======================================================================
// eig_sym — full symmetric eigensolve. `a` is not modified (cloned into a
// working buffer internally). Scratch + outputs use `alloc`.
// =======================================================================
template <typename T>
[[nodiscard]] EigSym<T> eig_sym(crd::memory::IAllocator* alloc, const Symmetric<T>& a);

// =======================================================================
// Lower-level pieces (exposed for testing + reuse by v3a-2/v3a-1b).
// =======================================================================

// tridiagonalize — reduce the symmetric matrix stored in the lower triangle
// of `a` (n×n, leading dimension `lda`, RowMajor) to tridiagonal form by an
// orthogonal similarity transform Q^T A Q = T. PRODUCTION = blocked dsytrd.
//
// On exit:
//   - d[0..n-1]   = diagonal of T
//   - e[0..n-2]   = sub-diagonal of T
//   - the strict lower triangle of `a` below the sub-diagonal holds the
//     Householder vectors; tau[0..n-2] their scalar factors (LAPACK 'L'
//     convention: H(i) annihilates A(i+2:n, i)).
template <typename T>
void tridiagonalize(T* a, crd::usize n, crd::usize lda, RealType<T>* d, RealType<T>* e, T* tau,
                    crd::memory::IAllocator* scratch);

// steqr — implicit-shift QL/QR on the real symmetric tridiagonal (d, e).
// d has length n, e has length n-1 (e[i] couples rows i,i+1). If
// `want_vectors`, the accumulated rotations are applied to z (n×ldz,
// column-major-of-eigenvectors stored RowMajor: z[r*ldz + c]); caller seeds
// z = I (tridiagonal eigenvectors) or z = Q (to fold the back-transform).
// R is the real scalar (f32/f64); Z is the eigenvector scalar (R or
// Complex<R>). Returns the number of unconverged eigenvalues (0 = success).
template <typename R, typename Z>
[[nodiscard]] int steqr(R* d, R* e, crd::usize n, Z* z, crd::usize ldz, bool want_vectors);

// =======================================================================
// v3a-2.2 — rank1_eigensolve: eigendecomposition of  diag(d) + rho*z*z^T
// (the Cuppen D&C "conquer" step). Deflation (negligible-weight + equal-pole
// Givens, faithful dlaed2) + secular roots (detail::secular_root) + Löwner /
// Gu-Eisenstat eigenvectors, back-transformed via gemm_parallel.
//
// On exit: lambda_out[0..n-1] ascending; v_out is n*n RowMajor with column k
// the eigenvector for lambda_out[k]. T real (f32/f64). Sign per D(dense-eig)-4.
//
// `q_in` (nullable, n*n RowMajor): the subproblem eigenvector matrix Q for the
// D&C merge. When given, the eigenvectors are produced as Q*V in a SINGLE
// back-transform (deflation Givens + Löwner gemm operate on Q's columns) — the
// fused dlaed1-style path. When null, Q = I (standalone rank-1 solve). `rho`
// may be negative (handled by a one-level negate-and-reverse).
// =======================================================================
template <typename T>
void rank1_eigensolve(crd::memory::IAllocator* alloc, crd::usize n, const T* d_in, const T* z_in,
                      T rho, const T* q_in, T* lambda_out, T* v_out);

// =======================================================================
// v3a-2.3 — dc_tridiag_eig: Cuppen divide-and-conquer on a symmetric
// tridiagonal (d[0..n-1], e[0..n-2]). lambda_out ascending, z_out n*n
// RowMajor (col k = eigenvector for lambda_out[k]). Base case = steqr.
// =======================================================================
template <typename T>
void dc_tridiag_eig(crd::memory::IAllocator* alloc, crd::usize n, const T* d, const T* e,
                    T* lambda_out, T* z_out);

// =======================================================================
// v3a-2.5 — eig_herm: complex Hermitian eigendecomposition. zhetd2-style
// reduction (complex reflectors → REAL tridiagonal) → reuse the real D&C
// solver on (D,E) → complex back-transform V = Q*Z (Q complex, Z real, done
// as two real gemms Qr*Z, Qi*Z). C = Complex<f32|f64>; values are real
// (RealType<C>), vectors complex. Phase convention: lowest-index largest-
// magnitude component made real-positive (complex analog of D(dense-eig)-4).
// =======================================================================
template <typename C>
[[nodiscard]] EigSym<C> eig_herm(crd::memory::IAllocator* alloc, const Hermitian<C>& a);

} // namespace crd::hesap::dense
