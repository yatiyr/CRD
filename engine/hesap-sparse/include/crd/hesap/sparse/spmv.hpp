#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// spmv -- sparse matrix-vector product: y = alpha * op(A) * x + beta * y.
//
// v1b-1 ships the CSR scalar baseline (the deterministic reference the SELL
// SIMD primary in v1b-2 must match bit-for-bit). Per D(sparse)-3:
//   - per-row reduction is left-to-right in stored (column-ascending) order
//     via `acc = acc + val * x[col]` (two roundings, never a fused single-
//     rounded FMA) -> width-independent + reproducible.
//   - beta == 0 does NOT read y (BLAS convention) so an uninitialised /
//     NaN-filled y is safe as a pure output.
//   - the matrix must be COMPRESSED CSR (assert); CSC-spmv + uncompressed
//     land with the v1c conversion graph.
// -----------------------------------------------------------------------

enum class Trans : crd::u8
{
    None = 0,
    Transpose = 1,
    ConjTranspose = 2,
};

namespace detail
{
[[nodiscard]] inline constexpr crd::f32 spmv_conj(crd::f32 v) noexcept { return v; }
[[nodiscard]] inline constexpr crd::f64 spmv_conj(crd::f64 v) noexcept { return v; }
template <typename U>
[[nodiscard]] inline constexpr Complex<U> spmv_conj(const Complex<U>& v) noexcept
{
    return crd::hesap::conj(v);
}

[[nodiscard]] inline constexpr bool spmv_is_zero(crd::f32 v) noexcept { return v == 0.0F; }
[[nodiscard]] inline constexpr bool spmv_is_zero(crd::f64 v) noexcept { return v == 0.0; }
template <typename U>
[[nodiscard]] inline constexpr bool spmv_is_zero(const Complex<U>& v) noexcept
{
    return v.is_zero();
}
} // namespace detail

// y = alpha * op(A) * x + beta * y, where op is None / Transpose /
// ConjTranspose. A is a compressed CSR matrix.
//   None:           x has cols() entries, y has rows() entries.
//   (Conj)Transpose: x has rows() entries, y has cols() entries.
template <typename T>
void spmv(T alpha, const SparseMatrix<T, SparseFormat::Csr>& matrix, Trans trans,
          crd::containers::ConstSpan<T> x, T beta, crd::containers::Span<T> y)
{
    const SparsePattern& pat = matrix.pattern();
    CRD_ASSERT_MSG(pat.is_compressed(), "spmv requires a compressed CSR matrix (call make_compressed)");

    const T*        vals  = matrix.values().values.data();
    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const crd::u32  rows  = pat.rows;
    const crd::u32  cols  = pat.cols;
    const bool      bzero = detail::spmv_is_zero(beta);

    if (trans == Trans::None)
    {
        CRD_ASSERT_MSG(x.size() == cols && y.size() == rows, "spmv: x must be cols(), y must be rows()");
        for (crd::u32 r = 0; r < rows; ++r)
        {
            T acc{};  // zero
            for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
            {
                acc = acc + vals[k] * x[inner[k]];  // two-rounded, fixed order (D(sparse)-3)
            }
            y[r] = bzero ? (alpha * acc) : (alpha * acc + beta * y[r]);
        }
    }
    else
    {
        CRD_ASSERT_MSG(x.size() == rows && y.size() == cols, "spmv^T: x must be rows(), y must be cols()");
        // beta scale first, then scatter-accumulate (deterministic write order).
        for (crd::u32 j = 0; j < cols; ++j)
        {
            y[j] = bzero ? T{} : (beta * y[j]);
        }
        const bool do_conj = (trans == Trans::ConjTranspose);
        for (crd::u32 r = 0; r < rows; ++r)
        {
            const T xr = alpha * x[r];
            for (crd::u32 k = outer[r]; k < outer[r + 1]; ++k)
            {
                const T a = do_conj ? detail::spmv_conj(vals[k]) : vals[k];
                y[inner[k]] = y[inner[k]] + a * xr;
            }
        }
    }
}

} // namespace crd::hesap::sparse
