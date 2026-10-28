// The table of the hesap-sparse kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-sparse/kernels/sparse_kernels.cpp), resolved by sparse_kernels() in
// crd/hesap/sparse/sparse_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing
// from it. Every routine is bit-exact across levels (ADR-0063, D(sparse)-3), NaN payloads aside: the SELL-C-σ spmv
// across column forms and with the CSR spmv too.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::sparse
{

// The SELL-C-σ slice heights (crd/hesap/sparse/sell.hpp): 256 bits of real values, four complex values of either type.
inline constexpr crd::u32 kSellSliceF64 = 4;
inline constexpr crd::u32 kSellSliceF32 = 8;
inline constexpr crd::u32 kSellSliceComplex = 4;

// The arrays of a SellMatrix a kernel reads, in its real scalar type T. Slice s holds rows [s C, s C + C) of the σ
// order, column-major: element (lane, w) is number e = slice_ptr[s] + w C + lane of vals, padded with the value 0 to
// slice_width[s]. A complex matrix stores each value as its (re, im) pair, element e at vals[2 e]. The padding of row
// lane, its columns from row_len[s C + lane] on, is never multiplied by x.
//
// A slice's columns take one of two forms. A U32 slice (delta_ptr[s + 1] == delta_ptr[s]) stores them as u32 at
// col_idx[e - delta_ptr[s]]. A Delta16 slice stores each row's first column at col_first[s C + lane] and the gap to
// each column from the one before as u16 at col_delta[delta_ptr[s] + w C + lane] (0 at w = 0 and in the padding), so
// column w is col_first plus the gaps up to w (mod 2^32); a slice falls back to U32 when a gap needs more than 16 bits.
template <typename T> struct SellSlices
{
    const crd::u32* slice_ptr;
    const crd::u32* slice_width;
    const crd::u32* row_len; // stored values of each σ row, num_slices C long (0 past the last row)
    const T* vals;
    const crd::u32* col_idx;   // the U32 slices' columns
    const crd::u32* delta_ptr; // num_slices + 1: the Delta16 elements before each slice
    const crd::u32* col_first; // num_slices C when a slice is Delta16: each σ row's first column (0 for an empty row)
    const crd::u16* col_delta; // the Delta16 slices' gaps
    const crd::u32* perm;      // perm[σ row] = original row; nullptr when the σ order is the identity
    crd::u32 rows;
    crd::u32 cols; // of the matrix: x's length
};

// Where a call's data sits (crd/hesap/sparse/sell.hpp decides it from the range's bytes and the L2): each level and
// type picks its slice pairing and x gather per sweep, and every pick gives the same bits.
enum class SellSweep : crd::u8
{
    Resident,  // the range's matrix, x and y fit the L2
    Streaming, // they come from beyond it
};

// The complex alpha and beta of y = alpha A x + beta y as their parts.
template <typename T> struct SellComplexScales
{
    T alpha_re;
    T alpha_im;
    T beta_re;
    T beta_im;
};

// One SDDMM (crd/hesap/sparse/sddmm.hpp): a CSR mask's row pointers and columns, X and Y row-major with leading
// dimensions ldx and ldy >= r, and the mask's value slots. Each dot runs two roundings a term in one fixed order on
// every level: f64 in two four-lane sums over stripes of 8 columns, added lane by lane and folded pairwise, then each
// remaining stripe of 4 folded pairwise and added, then column by column; f32 in one eight-lane sum over stripes of 8,
// folded pairwise, then column by column. An empty dot is +0.
template <typename T> struct SddmmRows
{
    const crd::u32* outer;
    const crd::u32* inner;
    const T* x;
    const T* y;
    T* out;
    crd::u32 ldx;
    crd::u32 ldy;
    crd::u32 r;
    T alpha;
};

struct SparseKernels
{
    // y[r] = alpha * sum_w vals * x[col] + beta * y[r] for the rows of slices [s_begin, s_end); each row sums its
    // columns in order, two roundings a step from +0, and beta == 0 never reads y.
    void (*sell_spmv_f64)(const SellSlices<crd::f64>& a, crd::f64 alpha, crd::f64 beta, const crd::f64* x, crd::f64* y,
                          crd::u32 s_begin, crd::u32 s_end, SellSweep sweep) noexcept;
    void (*sell_spmv_f32)(const SellSlices<crd::f32>& a, crd::f32 alpha, crd::f32 beta, const crd::f32* x, crd::f32* y,
                          crd::u32 s_begin, crd::u32 s_end, SellSweep sweep) noexcept;
    // The same over (re, im) pairs, x and y included, in the operators of Complex<T> (crd/hesap/complex.hpp): each step
    // adds (vr xr - vi xi, vr xi + vi xr) part by part, y = alpha sum + beta y, and beta == (0, 0) never reads y.
    void (*sell_spmv_c64)(const SellSlices<crd::f64>& a, const SellComplexScales<crd::f64>& scales, const crd::f64* x,
                          crd::f64* y, crd::u32 s_begin, crd::u32 s_end, SellSweep sweep) noexcept;
    void (*sell_spmv_c32)(const SellSlices<crd::f32>& a, const SellComplexScales<crd::f32>& scales, const crd::f32* x,
                          crd::f32* y, crd::u32 s_begin, crd::u32 s_end, SellSweep sweep) noexcept;
    // out[k] = alpha * dot(x row i, y row inner[k]) for the nonzeros k of mask rows [i_begin, i_end) (SddmmRows).
    void (*sddmm_rows_f64)(const SddmmRows<crd::f64>& s, crd::u32 i_begin, crd::u32 i_end) noexcept;
    void (*sddmm_rows_f32)(const SddmmRows<crd::f32>& s, crd::u32 i_begin, crd::u32 i_end) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const SparseKernels kSparseKernelsPortable;
extern const SparseKernels kSparseKernelsSse2;
extern const SparseKernels kSparseKernelsAvx2;
extern const SparseKernels kSparseKernelsAvx512;

} // namespace crd::hesap::sparse
