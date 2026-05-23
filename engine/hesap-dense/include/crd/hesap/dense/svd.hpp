#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/eig_sym.hpp>  // EigSym (rsyev return)
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3b — singular value decomposition  A = U * diag(S) * V^T.
//
// v3b-1a (this slice): Golub-Kahan Householder bidiagonalization (dgebrd).
// Singular values (later) come from the dqds engine (detail/dqds.hpp,
// `dlasq2`) — its native purpose; singular vectors from Demmel-Kahan QR
// (`dbdsqr`, v3b-1b). Reuses `detail/householder.hpp::make_householder`.
// Lower layer: raw f32/f64 (ADR-0078).
// -----------------------------------------------------------------------

template <typename T>
struct SVD
{
    Matrix<T> u;            // m x m (or m x min) orthogonal
    Vector<RealType<T>> s;  // min(m,n) singular values, descending
    Matrix<T> v;            // n x n orthogonal (A = U diag(S) V^T)

    explicit SVD(crd::memory::IAllocator* alloc) noexcept : u(alloc), s(alloc), v(alloc) {}
    SVD(SVD&&) noexcept = default;
    SVD& operator=(SVD&&) noexcept = default;
    SVD(const SVD&) = delete;
    SVD& operator=(const SVD&) = delete;
};

// =======================================================================
// bidiagonalize — reduce A (m x n, m >= n, RowMajor, leading dim `lda`) to
// UPPER bidiagonal form  Q^T A P = B  by Householder reflectors from both
// sides (Golub-Kahan, faithful unblocked `dgebd2`; the blocked `dlabrd`
// BLAS-3 path is v3b-1a-perf). On exit:
//   - d[0..n-1]   = diagonal of B
//   - e[0..n-2]   = super-diagonal of B
//   - the left reflectors H(i) (annihilating A(i+1:m,i)) are stored in the
//     sub-diagonal columns of A; tauq[0..n-1] their scalars.
//   - the right reflectors G(i) (annihilating A(i,i+2:n)) are stored in the
//     super-diagonal rows of A; taup[0..n-2] their scalars.
// `a` is overwritten. Real f32/f64. (m < n: caller transposes; v3b-1b.)
// =======================================================================
template <typename T>
void bidiagonalize(T* a, crd::usize m, crd::usize n, crd::usize lda, RealType<T>* d, RealType<T>* e, T* tauq,
                   T* taup, crd::memory::IAllocator* scratch);

// =======================================================================
// svd (v3b-1b) — full singular value decomposition  A = U * diag(S) * V^T
// of a general real matrix A (m x n). Pipeline: Golub-Kahan bidiagonalization
// (`bidiagonalize`) -> Demmel-Kahan implicit-zero-shift QR on the bidiagonal
// (`detail/bdsqr.hpp::dbdsqr`) accumulating the rotations into U and V^T ->
// V = (V^T)^T. m < n is handled by transposing internally (D(svd)-3). The
// returned thin factors: U is m x min(m,n), V is n x min(m,n), S length
// min(m,n) in DESCENDING order (>= 0). Eigenvector signs pinned for
// reproducibility (D(svd)-2): the largest-magnitude entry of each V column is
// made positive, the matching U column flipped to preserve A = U S V^T.
// Real f32/f64 (complex is v3b-1c). `a_in` is not modified.
// =======================================================================
template <typename T>
[[nodiscard]] SVD<T> svd(crd::memory::IAllocator* alloc, const Matrix<T>& a_in);

// =======================================================================
// svdvals (v3b-1b) — singular values ONLY (descending) of a general real
// matrix A (m x n). Bidiagonalize then feed B's squared qd array to the dqds
// engine (`detail/dqds.hpp::dlasq2` — its native purpose), with dlasq1-style
// smax scaling (D(svd)-5). No singular vectors are formed. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] Vector<RealType<T>> svdvals(crd::memory::IAllocator* alloc, const Matrix<T>& a_in);

// =======================================================================
// rsvd (v3b-3) — randomized truncated SVD (Halko-Martinsson-Tropp 2011) of a
// general real matrix A (m x n): a rank-`rank` approximation A ≈ U diag(S) V^T
// via a Gaussian sketch + range finder + `power_iters` subspace iterations
// (re-orthonormalized between). Returns thin U (m x k), S (k, descending,
// >= 0), V (n x k), where k = min(rank, min(m,n)). Built entirely on the
// deterministic gemm / Householder-QR / dense svd already shipped — Eigen has
// no randomized path; the gate is low-rank accuracy + speed on rank-deficient
// inputs. Deterministic given `seed`. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] SVD<T> rsvd(crd::memory::IAllocator* alloc, const Matrix<T>& a_in, crd::usize rank,
                          crd::usize oversampling = 8, crd::usize power_iters = 2,
                          crd::u64 seed = 0x5EED5D11ULL);

// =======================================================================
// rsyev (v3b-3) — randomized symmetric eigendecomposition: the top-`rank`
// eigenpairs of a symmetric A (n x n) via a Gaussian range finder + a small
// dense eig of B = Q^T A Q (Rayleigh-Ritz). Returns EigSym<T> with values in
// DESCENDING magnitude order (top-k) and eigenvectors as columns. For low-rank
// / spectrally-decaying symmetric A. D-pin: Rayleigh-Ritz chosen over the
// Nyström C^{-T} variant — more general (any symmetric A, not just PSD) and
// reuses the eig_sym already shipped (which beats Eigen + LAPACK). Built on the
// deterministic gemm / Householder-QR / eig_sym. Deterministic given `seed`.
// Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] EigSym<T> rsyev(crd::memory::IAllocator* alloc, const Symmetric<T>& a, crd::usize rank,
                              crd::usize oversampling = 8, crd::usize power_iters = 2,
                              crd::u64 seed = 0x5EE59EULL);

} // namespace crd::hesap::dense
