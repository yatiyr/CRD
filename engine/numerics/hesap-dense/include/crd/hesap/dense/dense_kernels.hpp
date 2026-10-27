// The table of the hesap-dense kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
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

struct DenseKernels
{
    void (*gemm_block_f64)(const GemmBlock<crd::f64>& block) noexcept;
    void (*gemm_block_f32)(const GemmBlock<crd::f32>& block) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const DenseKernels kDenseKernelsPortable;
extern const DenseKernels kDenseKernelsSse2;
extern const DenseKernels kDenseKernelsAvx2;

} // namespace crd::hesap::dense
