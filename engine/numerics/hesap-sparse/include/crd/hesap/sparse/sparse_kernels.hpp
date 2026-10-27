// The table of the hesap-sparse kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-sparse/kernels/sparse_kernels.cpp), resolved by sparse_kernels() in
// crd/hesap/sparse/sparse_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing
// from it. Every routine is bit-exact across levels and with the CSR spmv (ADR-0063, D(sparse)-3), NaN payloads aside.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::sparse
{

// The SELL-C-σ slice heights (crd/hesap/sparse/sell.hpp): 256 bits of real values, four complex values of either type.
inline constexpr crd::u32 kSellSliceF64 = 4;
inline constexpr crd::u32 kSellSliceF32 = 8;
inline constexpr crd::u32 kSellSliceComplex = 4;

// The arrays of a SellMatrix a kernel reads, in its real scalar type T. Slice s holds rows [s C, s C + C) of the σ
// order, column-major: element (lane, w) is number slice_ptr[s] + w C + lane of vals and col_idx, padded with the
// value 0 at column 0 to slice_width[s]. A complex matrix stores each value as its (re, im) pair, element e at
// vals[2 e]. The padding of row lane, its columns from row_len[s C + lane] on, is never multiplied by x.
template <typename T> struct SellSlices
{
    const crd::u32* slice_ptr;
    const crd::u32* slice_width;
    const crd::u32* row_len; // stored values of each σ row, num_slices C long (0 past the last row)
    const T* vals;
    const crd::u32* col_idx;
    const crd::u32* perm; // perm[σ row] = original row; nullptr when the σ order is the identity
    crd::u32 rows;
};

// The complex alpha and beta of y = alpha A x + beta y as their parts.
template <typename T> struct SellComplexScales
{
    T alpha_re;
    T alpha_im;
    T beta_re;
    T beta_im;
};

struct SparseKernels
{
    // y[r] = alpha * sum_w vals * x[col] + beta * y[r] for the rows of slices [s_begin, s_end); each row sums its
    // columns in order, two roundings a step from +0, and beta == 0 never reads y.
    void (*sell_spmv_f64)(const SellSlices<crd::f64>& a, crd::f64 alpha, crd::f64 beta, const crd::f64* x, crd::f64* y,
                          crd::u32 s_begin, crd::u32 s_end) noexcept;
    void (*sell_spmv_f32)(const SellSlices<crd::f32>& a, crd::f32 alpha, crd::f32 beta, const crd::f32* x, crd::f32* y,
                          crd::u32 s_begin, crd::u32 s_end) noexcept;
    // The same over (re, im) pairs, x and y included, in the operators of Complex<T> (crd/hesap/complex.hpp): each step
    // adds (vr xr - vi xi, vr xi + vi xr) part by part, y = alpha sum + beta y, and beta == (0, 0) never reads y.
    void (*sell_spmv_c64)(const SellSlices<crd::f64>& a, const SellComplexScales<crd::f64>& scales, const crd::f64* x,
                          crd::f64* y, crd::u32 s_begin, crd::u32 s_end) noexcept;
    void (*sell_spmv_c32)(const SellSlices<crd::f32>& a, const SellComplexScales<crd::f32>& scales, const crd::f32* x,
                          crd::f32* y, crd::u32 s_begin, crd::u32 s_end) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const SparseKernels kSparseKernelsPortable;
extern const SparseKernels kSparseKernelsSse2;
extern const SparseKernels kSparseKernelsAvx2;

} // namespace crd::hesap::sparse
