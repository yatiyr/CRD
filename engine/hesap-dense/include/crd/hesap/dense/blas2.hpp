#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// BLAS Level 2 — matrix-vector and rank-1/2 updates. Phase 3.1.6 v0c.
//
// 17 operations spread across type variance:
//   General matrix:   gemv (4 types) / ger (real 2) / geru, gerc (complex 2)
//   Banded general:   gbmv (4 types)
//   Symmetric/Herm:   symv (real 2) / hemv (complex 2) / syr (real 2) /
//                     her (complex 2) / syr2 (real 2) / her2 (complex 2)
//   Symmetric banded: sbmv (real 2) / hbmv (complex 2)
//   Triangular:       trmv (4 types) / trsv (4 types)
//   Triangular band:  tbmv (4 types) / tbsv (4 types)
//
// Determinism: all reductions (gemv inner row dot, trsv back-substitution
// accumulators) route through `detail::pairwise_sum_produced`. Bit-exact
// across SIMD widths same contract as BLAS L1.
//
// Layout: every general / banded op is templated on `Layout L` per D21.
// Sub-views via `MatrixView<T, L>` preserve `ld`. Symmetric / Hermitian /
// Triangular ops only need square n × n storage; layout dispatch is moot.
// -----------------------------------------------------------------------

// ============ General matrix ============================================

// gemv: y = alpha * op(A) * x + beta * y, where op(A) ∈ {A, A^T, A^H}.
template <typename T, Layout L>
void gemv(
    T alpha,
    MatrixView<const T, L> a,
    crd::containers::ConstSpan<T> x,
    T beta,
    crd::containers::Span<T> y,
    Trans trans = Trans::None);

// Convenience overload for an owning Matrix.
template <typename T, Layout L>
inline void gemv(T alpha, const Matrix<T, L>& a, crd::containers::ConstSpan<T> x, T beta,
                 crd::containers::Span<T> y, Trans trans = Trans::None)
{
    gemv<T, L>(alpha, a.view(), x, beta, y, trans);
}

// ger: A += alpha * x * y^T  (real-only; complex variants are geru / gerc).
template <typename T, Layout L>
void ger(T alpha, crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, MatrixView<T, L> a);

// geru: A += alpha * x * y^T (UNCONJUGATED — complex only).
template <typename T, Layout L>
void geru(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, MatrixView<Complex<T>, L> a);

// gerc: A += alpha * x * conj(y)^T  (Hermitian rank-1 — complex only).
template <typename T, Layout L>
void gerc(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, MatrixView<Complex<T>, L> a);

// ============ Banded general ============================================

// gbmv: y = alpha * op(A) * x + beta * y, A banded.
template <typename T>
void gbmv(T alpha, const Banded<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y, Trans trans = Trans::None);

// ============ Symmetric / Hermitian =====================================

// symv: y = alpha * A * x + beta * y, A symmetric (real only).
template <typename T>
void symv(T alpha, const Symmetric<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y);

// hemv: y = alpha * A * x + beta * y, A Hermitian (complex only — real T is symv).
template <typename T>
void hemv(Complex<T> alpha, const Hermitian<Complex<T>>& a, crd::containers::ConstSpan<Complex<T>> x,
          Complex<T> beta, crd::containers::Span<Complex<T>> y);

// syr: A += alpha * x * x^T  (real, symmetric rank-1 update).
template <typename T>
void syr(T alpha, crd::containers::ConstSpan<T> x, Symmetric<T>& a);

// her: A += alpha * x * x^H  (Hermitian rank-1; alpha is REAL).
template <typename T>
void her(T alpha, crd::containers::ConstSpan<Complex<T>> x, Hermitian<Complex<T>>& a);

// syr2: A += alpha * x * y^T + alpha * y * x^T  (symmetric rank-2; real).
template <typename T>
void syr2(T alpha, crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, Symmetric<T>& a);

// her2: A += alpha * x * y^H + conj(alpha) * y * x^H  (Hermitian rank-2).
template <typename T>
void her2(Complex<T> alpha, crd::containers::ConstSpan<Complex<T>> x,
          crd::containers::ConstSpan<Complex<T>> y, Hermitian<Complex<T>>& a);

// ============ Symmetric / Hermitian banded ==============================

// sbmv: y = alpha * A * x + beta * y, A symmetric banded.
template <typename T>
void sbmv(T alpha, const Banded<T>& a, crd::containers::ConstSpan<T> x, T beta,
          crd::containers::Span<T> y);

// hbmv: y = alpha * A * x + beta * y, A Hermitian banded.
template <typename T>
void hbmv(Complex<T> alpha, const Banded<Complex<T>>& a, crd::containers::ConstSpan<Complex<T>> x,
          Complex<T> beta, crd::containers::Span<Complex<T>> y);

// ============ Triangular ================================================

// trmv: x = op(A) * x, A triangular.
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trmv(const Triangular<T, Side, Diag>& a, crd::containers::Span<T> x, Trans trans = Trans::None);

// trsv: solve op(A) * x = b in-place (x receives b on entry, solution on exit).
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trsv(const Triangular<T, Side, Diag>& a, crd::containers::Span<T> x, Trans trans = Trans::None);

// ============ Triangular banded =========================================

// tbmv: x = op(A) * x, A triangular banded. `k` is the number of off-diagonals
// on the canonical side (kl for Lower, ku for Upper).
template <typename T>
void tbmv(const Banded<T>& a, TriangularSide side, TriangularDiag diag, crd::containers::Span<T> x,
          Trans trans = Trans::None);

// tbsv: solve op(A) * x = b in-place, A triangular banded.
template <typename T>
void tbsv(const Banded<T>& a, TriangularSide side, TriangularDiag diag, crd::containers::Span<T> x,
          Trans trans = Trans::None);

} // namespace crd::hesap::dense
