#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/qr_colpiv.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3c-1a — complete orthogonal decomposition (COD).
//
//     A * P = Q * [ T11  0 ] * Z
//                 [  0   0 ]
//
// where (col-piv QR) A*P = Q*R reveals rank `r`, then the leading r×n
// upper-trapezoidal block [R11 R12] is reduced from the RIGHT by RZ
// Householder reflectors (LAPACK `dtzrzf`/`dlatrz`) to an r×r upper-triangular
// T11, with Z = H(0)·H(1)···H(r-1) the orthogonal accumulation. This is the
// FAST rank-deficient least-squares path (LAPACK `dgelsy`): the min-norm
// solution to min‖Ax−b‖ costs one col-piv QR + one trapezoidal reduction +
// triangular solves — far cheaper than SVD when only the solution (not the
// spectrum) is needed.
//
// Real f32/f64. Lower layer: raw scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

template <typename T, Layout L = Layout::RowMajor>
struct COD
{
    QRColPiv<T, L> qr;             // A*P = Q*R (full min reflectors) + rank + jpvt
    Matrix<T, L> t11;              // rank × rank upper-triangular T11
    Matrix<T, L> z;                // rank × (n-rank) RZ reflector tails (row i = z_i)
    crd::containers::Array<T> tau_z;  // rank RZ reflector scalars
    crd::usize m = 0;
    crd::usize n = 0;
    crd::usize rank = 0;

    explicit COD(crd::memory::IAllocator* alloc) noexcept
        : qr(alloc), t11(alloc), z(alloc), tau_z(alloc)
    {
    }

    COD(COD&&) noexcept = default;
    COD& operator=(COD&&) noexcept = default;
    COD(const COD&) = delete;
    COD& operator=(const COD&) = delete;
};

// =======================================================================
// factor_cod — build the complete orthogonal decomposition of A (m×n).
// `rcond < 0` selects the default rank tolerance (max(m,n)·eps). The result
// supports `solve_cod` (min-norm least squares). Real f32/f64.
// =======================================================================
template <typename T, Layout L>
[[nodiscard]] COD<T, L> factor_cod(crd::memory::IAllocator* alloc, const Matrix<T, L>& a,
                                   RealType<T> rcond = RealType<T>{-1});

// =======================================================================
// solve_cod — minimum-norm least-squares solve  min‖A·x − b‖₂  (LAPACK
// dgelsy). `b` has length m (input RHS); `x` has length n (output solution).
// For a rank-deficient A this returns the unique minimum-2-norm minimiser.
// =======================================================================
template <typename T, Layout L>
void solve_cod(const COD<T, L>& cod, crd::containers::ConstSpan<T> b, crd::containers::Span<T> x);

} // namespace crd::hesap::dense
