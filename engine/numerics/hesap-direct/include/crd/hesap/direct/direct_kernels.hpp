// The table of the hesap-direct kernels (SIMD-DISPATCH, ADR-0134 §5): the triangular solves of the supernodal Cholesky
// panels, one table per CPU level the kernel unit (../../../../kernels/direct_kernels.cpp) is built for, resolved by
// direct_kernels() in crd/hesap/direct/direct_dispatch.hpp. The header holds only data declarations, so a kernel unit
// compiles nothing from it. The backward single-RHS solve runs the dense gemv and trsv entries.
//
// A panel is column-major: column k at panel + k * ld, its diagonal block in rows 0 to nc - 1. Determinism
// (ADR-0134 §7): each level is bit-exact with the arithmetic declared here. A step is the dense kernels' step (a
// multiply and an add rounded below avx2, one fused multiply-add from avx2), and every element, the last partial vector
// included, takes it. Each element's chain depends on the column count only, never on the row count or offset, so the
// serial right-looking and the level-parallel left-looking solves give the same bits. A subtraction x - l y takes the
// step with -l (or -y), which gives the same bits. Across the avx2 boundary the family is tolerance-declared.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/dense_kernels.hpp>

namespace crd::hesap::direct
{

// out (rows by nrhs, column c at out + c * rows) = sum over k < nc of column k of the panel rows times y(k, c), y(k, c)
// at y[k + c * ldy]: every element starts at +0 and takes the columns k ascending.
template <typename T> struct PanelProduct
{
    T* out;
    const T* panel;
    const T* y;
    crd::usize ld;
    crd::usize ldy;
    crd::usize nc;
    crd::usize rows;
    crd::usize nrhs;
};

// y(k, c) = y(k, c) - acc(k, c) for k < nc, y(k, c) at y[k + c * ldy], acc(k, c) at acc[k * nrhs + c] (the caller's
// scratch) the sum from +0 over the panel rows r ascending of column k at r times wt[r * nrhs + c].
template <typename T> struct PanelReduce
{
    T* y;
    T* acc;
    const T* panel;
    const T* wt;
    crd::usize ldy;
    crd::usize ld;
    crd::usize nc;
    crd::usize rows;
    crd::usize nrhs;
};

// The kernels of one element type.
template <typename T> struct DirectTypeKernels
{
    // The forward below-update: dense::GemvtRows' semantics (y + A^T x, every element taking the rows in order) walked
    // four rows (panel columns) at a time over the whole of y, four concurrent read streams.
    void (*forward_below)(const dense::GemvtRows<T>& g) noexcept;
    // L x = b for the panel's nc columns: in blocks of four columns, x_j = x_j / l_jj, then each later row of the
    // block x_i - l_ij x_j rounding twice on every level; the rows below the block take its columns in order.
    void (*forward_diagonal)(T* x, const T* panel, crd::usize ld, crd::usize nc) noexcept;
    void (*block_forward_below)(const PanelProduct<T>& p) noexcept;
    void (*block_backward_below)(const PanelReduce<T>& p) noexcept;
    // forward_diagonal on each column c of d (nc by nrhs, row j at d + j * nrhs): column c gives its bits.
    void (*block_forward_diagonal)(T* d, crd::usize nrhs, const T* panel, crd::usize ld, crd::usize nc) noexcept;
    // L^T x = y on each column of d, in blocks of four columns descending: each block row first loses the sum from +0
    // over the rows k below the block ascending of l_kj d(k, c), then, descending, d_j - l_kj d_k (k ascending in the
    // block, rounding twice) and d_j / l_jj.
    void (*block_backward_diagonal)(T* d, crd::usize nrhs, const T* panel, crd::usize ld, crd::usize nc) noexcept;
};

struct DirectKernels
{
    DirectTypeKernels<crd::f64> f64_kernels;
    DirectTypeKernels<crd::f32> f32_kernels;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const DirectKernels kDirectKernelsPortable;
extern const DirectKernels kDirectKernelsSse2;
extern const DirectKernels kDirectKernelsAvx2;

} // namespace crd::hesap::direct
