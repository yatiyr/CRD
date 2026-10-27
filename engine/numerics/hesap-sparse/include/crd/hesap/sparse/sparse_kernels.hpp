// The table of the hesap-sparse kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-sparse/kernels/sparse_kernels.cpp), resolved by sparse_kernels() in
// crd/hesap/sparse/sparse_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing
// from it. Every routine is bit-exact across levels and with the CSR spmv (ADR-0063, D(sparse)-3).

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::sparse
{

// The SELL-C-σ slice heights (crd/hesap/sparse/sell.hpp): one 256-bit register of each type.
inline constexpr crd::u32 kSellSliceF64 = 4;
inline constexpr crd::u32 kSellSliceF32 = 8;

// The arrays of a SellMatrix<T> a kernel reads. Slice s holds rows [s C, s C + C) of the σ order, column-major:
// element (lane, w) at vals[slice_ptr[s] + w C + lane], padded with the value 0 at column 0 to slice_width[s].
template <typename T> struct SellSlices
{
    const crd::u32* slice_ptr;
    const crd::u32* slice_width;
    const T* vals;
    const crd::u32* col_idx;
    const crd::u32* perm; // perm[σ row] = original row; nullptr when the σ order is the identity
    crd::u32 rows;
};

struct SparseKernels
{
    // y[r] = alpha * sum_w vals * x[col] + beta * y[r] for the rows of slices [s_begin, s_end); each row sums its
    // columns in order, two roundings a step from +0, and beta == 0 never reads y.
    void (*sell_spmv_f64)(const SellSlices<crd::f64>& a, crd::f64 alpha, crd::f64 beta, const crd::f64* x, crd::f64* y,
                          crd::u32 s_begin, crd::u32 s_end) noexcept;
    void (*sell_spmv_f32)(const SellSlices<crd::f32>& a, crd::f32 alpha, crd::f32 beta, const crd::f32* x, crd::f32* y,
                          crd::u32 s_begin, crd::u32 s_end) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const SparseKernels kSparseKernelsPortable;
extern const SparseKernels kSparseKernelsSse2;
extern const SparseKernels kSparseKernelsAvx2;

} // namespace crd::hesap::sparse
