#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3c-1b — least-squares family: `lstsq` + `pinv`.
//
// Built on the shipped factorizations (which already beat Eigen + LAPACK):
//   - full-rank fast path → blocked Householder QR (`qr.hpp`),
//   - rank-deficient fast min-norm → complete orthogonal decomposition
//     (`cod.hpp`, LAPACK dgelsy),
//   - max-accuracy / complex min-norm → SVD (`svd.hpp`, D&C crush).
//
// `lstsq` solves  min‖A·X − B‖_F  for a (possibly multi-column) RHS B and a
// (possibly rank-deficient / non-square) A, returning the minimum-2-norm
// minimiser, the numerical rank, and the per-column residual norm. `pinv`
// returns the Moore-Penrose pseudoinverse via SVD with rcond truncation.
//
// rcond < 0 selects the LAPACK default  max(m,n)·eps.  The singular-value /
// diagonal cutoff is  rcond · (largest value)  (D(lstsq): strict `>`).
//
// 4 type variants (f32/f64/c32/c64); the COD/QR fast paths are real, complex
// routes through the complex SVD. Lower layer: raw scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

enum class LstSqMethod
{
    Auto,  // real → COD (rank-revealing, fast); complex → SVD
    QR,    // blocked Householder QR — assumes full column rank (LAPACK dgels)
    COD,   // complete orthogonal decomposition — rank-revealing min-norm (dgelsy)
    SVD    // SVD min-norm — most accurate, handles every rank/shape + complex (dgelsd)
};

enum class PinvMethod
{
    Auto,  // real → COD (matches Eigen pseudoInverse(); fast); complex → SVD
    COD,   // pseudoinverse via complete orthogonal decomposition (real)
    SVD    // pseudoinverse via SVD — most accurate, handles complex
};

template <typename T>
struct LstSq
{
    Matrix<T> x;                   // n × nrhs minimum-norm minimiser
    Vector<RealType<T>> residual;  // length nrhs: ‖A·x_c − b_c‖₂ per column
    crd::usize rank = 0;

    explicit LstSq(crd::memory::IAllocator* alloc) noexcept : x(alloc), residual(alloc) {}
    LstSq(LstSq&&) noexcept = default;
    LstSq& operator=(LstSq&&) noexcept = default;
    LstSq(const LstSq&) = delete;
    LstSq& operator=(const LstSq&) = delete;
};

// =======================================================================
// lstsq (matrix RHS) — solve min‖A·X − B‖ for A (m×n), B (m×nrhs).
// `with_residual` fills `residual` with the per-column ‖A·x_c − b_c‖₂ (an
// extra O(m·n) gemv per column); pass false when only the solution is needed.
// =======================================================================
template <typename T>
[[nodiscard]] LstSq<T> lstsq(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b,
                             LstSqMethod method = LstSqMethod::Auto, RealType<T> rcond = RealType<T>{-1},
                             bool with_residual = true);

// =======================================================================
// lstsq (vector RHS) — convenience for a single right-hand side b (length m).
// =======================================================================
template <typename T>
[[nodiscard]] LstSq<T> lstsq(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Vector<T>& b,
                             LstSqMethod method = LstSqMethod::Auto, RealType<T> rcond = RealType<T>{-1},
                             bool with_residual = true);

// =======================================================================
// pinv — Moore-Penrose pseudoinverse A⁺ (n×m) of A (m×n).
//   COD path (real, fast — matches Eigen completeOrthogonalDecomposition):
//     A·P = Q·[T 0; 0 0]·Z  ⟹  A⁺ = P·Zᵀ·[T⁻¹ 0; 0 0]·Qᵀ.
//   SVD path (complex / max-accuracy):
//     A⁺ = V·diag(σ_i > rcond·σ_max ? 1/σ_i : 0)·Uᴴ.
// =======================================================================
template <typename T>
[[nodiscard]] Matrix<T> pinv(crd::memory::IAllocator* alloc, const Matrix<T>& a,
                             PinvMethod method = PinvMethod::Auto, RealType<T> rcond = RealType<T>{-1});

} // namespace crd::hesap::dense
