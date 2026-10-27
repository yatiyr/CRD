// The table of the hesap-dense kernels (SIMD-DISPATCH, ADR-0134 §5): the GEMM blocks, the BLAS-1 and BLAS-2 real
// kernels and the unblocked Cholesky, one table per CPU level the kernel unit is built for
// (engine/numerics/hesap-dense/kernels/dense_kernels.cpp), resolved by dense_kernels() in
// crd/hesap/dense/dense_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing
// from it.
//
// Determinism (ADR-0134 §7): each level is bit-exact with its declared arithmetic. Every element of a product is
// summed in depth order from +0; the portable and sse2 levels round each multiply and each add (they agree bit for
// bit), the avx2 level uses a single-rounded fused multiply-add per step, as hesap's GEMM has since ADR-0082's
// 2026-05-20 update. Across that boundary the family is tolerance-declared: an element differs by at most the
// classical inner-product bound, depth * eps * sum |a| |b|. The merge into C rounds alike on every level.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::dense
{

namespace detail
{

// The register tile (MR rows by NR columns) of each type. It shapes the packed panels (detail/gemm_pack.hpp), so it is
// a data layout and the same on every level: f64 6 by 8 (twelve AVX2 accumulators), the others 8 by 8.
// NOLINTBEGIN(readability-identifier-naming): MR and NR are the BLIS register-blocking names the packing code follows.
template <typename T> struct GemmTraits
{
    static constexpr crd::usize MR = 8;
    static constexpr crd::usize NR = 8;
};
template <> struct GemmTraits<crd::f64>
{
    static constexpr crd::usize MR = 6;
    static constexpr crd::usize NR = 8;
};
// NOLINTEND(readability-identifier-naming)

} // namespace detail

// How a GEMM block writes element (i, j) of the product p into C.
enum class GemmUpdate : crd::u8
{
    Add,      // c + alpha * p (alpha == 1: c + p, the same bits)
    Subtract, // c - p
    Assign,   // p
};

// One block of C updated by the product of packed panels.
template <typename T> struct GemmBlock
{
    const T* a;     // ceil(rows / MR) row panels at a + panel * MR * lda: MR rows of depth values, row stride lda
    const T* b;     // ceil(cols / NR) column panels at b + panel * depth * NR: depth rows of NR values
    T* c;           // element (i, j) at c[i * ldc + j], or c[j * ldc + i] when col_major
    crd::usize lda; // the A row stride: depth for packed panels, wider for a resident row panel
    crd::usize ldc;
    crd::usize rows; // the elements written (rows by cols); the panels' padding is never stored
    crd::usize cols;
    crd::usize depth;
    crd::usize row0; // the block's first row and column in the matrix that lower refers to
    crd::usize col0;
    T alpha; // GemmUpdate::Add only
    GemmUpdate update;
    bool col_major;
    bool lower; // only the elements with row0 + i >= col0 + j (a symmetric update's lower triangle)
};

// The vector and matrix-vector kernels sum like the GEMM blocks: below avx2 each vector step rounds its multiply and
// its add, from avx2 it is one fused multiply-add; the scalar tails round twice on every level. The vectors are
// Vec4d (w = 4) and Vec8f (w = 8), and the reductions have a fixed shape:
// - a dot product (dot_*, the Cholesky and triangular-solve rows): A accumulators (8 from avx2, 4 below); whole
//   vector v (elements v w to v w + w - 1) goes to accumulator v % A, the accumulators sum as the tree
//   ((0 + 1) + (2 + 3)) + ((4 + 5) + (6 + 7)), its lanes as horizontal_sum's tree, then the remaining elements in
//   order;
// - a gemv row: one accumulator over the whole vectors, horizontal_sum, the remaining columns in order, then
//   alpha * s + beta * y;
// - symv: beta * y first; then row i (columns k < i of the lower triangle) takes the dot product of the row with x
//   as above and updates y[k] to y[k] + a_ik * (alpha * x[i]) at every column; then y[i] to
//   y[i] + alpha * (dot + a_ii * x[i]).

// y = alpha * A x + beta * y over `rows` rows of `cols` values, row i at a + i * lda.
template <typename T> struct GemvRows
{
    const T* a;
    const T* x;
    T* y;
    crd::usize lda;
    crd::usize rows;
    crd::usize cols;
    T alpha;
    T beta;
};

// y = alpha * A x + beta * y for a symmetric A of order n read from its lower triangle: (i, k), k <= i, at
// a[i * lda + k].
template <typename T> struct SymvLower
{
    const T* a;
    const T* x;
    T* y;
    crd::usize lda;
    crd::usize n;
    T alpha;
    T beta;
};

// x = A^-1 x for a triangular A of order n read from its rows ((i, k) at a[i * lda + k]): forward substitution for a
// lower A, back substitution for an upper one; a unit diagonal is not read.
template <typename T> struct TrsvRows
{
    const T* a;
    T* x;
    crd::usize lda;
    crd::usize n;
    bool lower;
    bool unit;
};

struct DenseKernels
{
    void (*gemm_block_f64)(const GemmBlock<crd::f64>& block) noexcept;
    void (*gemm_block_f32)(const GemmBlock<crd::f32>& block) noexcept;
    crd::f64 (*dot_f64)(const crd::f64* x, const crd::f64* y, crd::usize n) noexcept;
    crd::f32 (*dot_f32)(const crd::f32* x, const crd::f32* y, crd::usize n) noexcept;
    void (*gemv_f64)(const GemvRows<crd::f64>& g) noexcept;
    void (*gemv_f32)(const GemvRows<crd::f32>& g) noexcept;
    void (*symv_f64)(const SymvLower<crd::f64>& s) noexcept;
    void (*symv_f32)(const SymvLower<crd::f32>& s) noexcept;
    void (*trsv_f64)(const TrsvRows<crd::f64>& t) noexcept;
    void (*trsv_f32)(const TrsvRows<crd::f32>& t) noexcept;
    // The unblocked lower Cholesky of the n by n block at a (row stride ld), in place: 0, or k + 1 when column k is
    // not positive definite (the columns before it are factored).
    crd::usize (*cholesky_f64)(crd::f64* a, crd::usize n, crd::usize ld) noexcept;
    crd::usize (*cholesky_f32)(crd::f32* a, crd::usize n, crd::usize ld) noexcept;
    // L21 = A21 L11^-T for rows [first, end) of the panel at column k, width nb (L11 factored at (k, k)).
    void (*cholesky_trsm_f64)(crd::f64* a, crd::usize ld, crd::usize k, crd::usize nb, crd::usize first,
                              crd::usize end) noexcept;
    void (*cholesky_trsm_f32)(crd::f32* a, crd::usize ld, crd::usize k, crd::usize nb, crd::usize first,
                              crd::usize end) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const DenseKernels kDenseKernelsPortable;
extern const DenseKernels kDenseKernelsSse2;
extern const DenseKernels kDenseKernelsAvx2;

} // namespace crd::hesap::dense
