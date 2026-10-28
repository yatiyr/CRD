// The table of the hesap-dense kernels (SIMD-DISPATCH, ADR-0134 §5): the GEMM blocks, the BLAS-1 and BLAS-2 real
// kernels, the reflector, rotation and LDLT updates, the split-complex sweeps and the unblocked Cholesky, one table
// per CPU level the kernel unit is built for (engine/numerics/hesap-dense/kernels/dense_kernels.cpp), resolved by
// dense_kernels() in crd/hesap/dense/dense_dispatch.hpp. The header holds only data declarations, so a kernel unit
// compiles nothing from it.
//
// Determinism (ADR-0134 §7): each level is bit-exact with its declared arithmetic. Every element of a product is
// summed in depth order from +0; the portable and sse2 levels round each multiply and each add (they agree bit for
// bit), the avx2 and avx512 levels use a single-rounded fused multiply-add per step (they agree bit for bit), as
// hesap's GEMM has since ADR-0082's 2026-05-20 update. Across that boundary the family is tolerance-declared: an
// element differs by at most the
// classical inner-product bound, depth * eps * sum |a| |b|. The merge into C rounds alike on every level. A GEMM sums
// its depth in groups of kGemmKc (256, detail/gemm_pack.hpp) values, one group after another on every path (gemm, its
// parallel and small-GEMM paths, syrk): the first merges as beta c + alpha p (beta 0: +0 + alpha p, C unread), each
// later one as c + alpha p, alpha p being p at alpha 1, so neither the path nor the worker count nor a level's tile
// shape changes an element's bits.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::dense
{

// A level's register tile: mr rows by nr columns of C. It shapes the packed panels, which are per-call scratch, so each
// level packs with its own pack_a and pack_b (ADR-0082's backend-coupled packing) and the tile shape is free per level.
struct GemmShape
{
    crd::usize mr;
    crd::usize nr;
};

// op(X) read as a matrix: element (r, c) at data[r * row_stride + c * col_stride] (a layout and a transpose).
template <typename T> struct GemmSource
{
    const T* data;
    crd::usize row_stride;
    crd::usize col_stride;
};

// How a GEMM block writes element (i, j) of the product p into C.
enum class GemmUpdate : crd::u8
{
    Add,      // c + alpha * p (alpha == 1: c + p, the same bits)
    Subtract, // c - p
    Assign,   // p
    Scale,    // beta * c + alpha * p, a GEMM's first depth group; beta == 0 reads no c: +0 + alpha * p
};

// One block of C updated by the product of packed panels.
template <typename T> struct GemmBlock
{
    const T* a;     // ceil(rows / mr) row panels at a + panel * mr * lda: mr rows of depth values, row stride lda
    const T* b;     // ceil(cols / nr) column panels at b + panel * depth * nr: depth rows of nr values
    T* c;           // element (i, j) at c[i * ldc + j], or c[j * ldc + i] when col_major
    crd::usize lda; // the A row stride: depth for packed panels, wider for a resident row panel
    crd::usize ldc;
    crd::usize rows; // the elements written (rows by cols); the panels' padding is never stored
    crd::usize cols;
    crd::usize depth;
    crd::usize row0; // the block's first row and column in the matrix that lower refers to
    crd::usize col0;
    T alpha; // GemmUpdate::Add and Scale
    T beta;  // GemmUpdate::Scale only
    GemmUpdate update;
    bool col_major;
    bool lower; // only the elements with row0 + i >= col0 + j (a symmetric update's lower triangle)
};

// The vector and matrix-vector kernels sum like the GEMM blocks: below avx2 each vector step rounds its multiply and
// its add, from avx2 it is one fused multiply-add. The vectors are Vec4d (w = 4) and Vec8f (w = 8) below avx512 and
// Vec8d (w = 8) and Vec16f (w = 16) at avx512, so a reduction's shape is the level's, and avx2 and avx512 agree only
// within the inner-product bound there. The reductions have a fixed shape:
// - a dot product (dot, the Cholesky, triangular-solve and reflector rows): A accumulators (8 from avx2, 4 below);
//   whole vector v (elements v w to v w + w - 1) goes to accumulator v % A, the accumulators sum as the tree
//   ((0 + 1) + (2 + 3)) + ((4 + 5) + (6 + 7)), its lanes as horizontal_sum's pairwise tree (lanes 0 to w/2 - 1, then
//   the rest, each half the same way down to (0 + 1) + (2 + 3)), then the remaining elements in order, each rounding
//   twice;
// - a gemv row: one accumulator over the whole vectors, horizontal_sum, the remaining columns in order (rounding
//   twice), then alpha * s + beta * y;
// - symv: beta * y first; then row i (columns k < i of the lower triangle) takes the dot product of the row with x
//   as above and updates y[k] to y[k] + a_ik * (alpha * x[i]) at every column; then y[i] to
//   y[i] + alpha * (dot + a_ii * x[i]).
// The elementwise kernels (axpy, gemvt, ger, the reflector and LDLT updates) give every element, the last partial
// vector included, the level's step: y + a x rounded twice below avx2 and fused from avx2. rot rounds each multiply
// and add on every level, as LAPACK's xROT, so its levels agree bit for bit.

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

// y = y + A^T x over `rows` rows of `cols` values, row r at a + r * lda and x[r] at x[r * incx]: y takes the axpy of
// each row with x[r] in row order (element j: y[j] + x[r] a_rj, r ascending).
template <typename T> struct GemvtRows
{
    const T* a;
    const T* x;
    T* y;
    crd::usize lda;
    crd::usize incx;
    crd::usize rows;
    crd::usize cols;
};

// A = A + alpha x y^T over `rows` rows of `cols` values, row r at a + r * lda: row r takes the axpy of y with the
// scale alpha * x[r] (one rounding).
template <typename T> struct GerRows
{
    T* a;
    const T* x;
    const T* y;
    crd::usize lda;
    crd::usize rows;
    crd::usize cols;
    T alpha;
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

// The reflector H = I - tau v v^T, v = (1, v[0], ..., v[len - 1]), applied to `count` vectors of len + 1 values,
// vector r at c + r * ldc (LAPACK xLARF on rows): w = tau * (c_r[0] + dot(v, c_r + 1)) in the dot shape, then
// c_r[0] - w and the axpy of v with -w into the remaining values.
template <typename T> struct ReflectorRows
{
    const T* v;
    T* c;
    crd::usize ldc;
    crd::usize count;
    crd::usize len;
    T tau;
};

// A Bunch-Kaufman trailing update of LDL^T (LAPACK xSYTF2): the lower triangle of the order-n block loses
// U D^-1 U^T, for the one column u0 or the two columns u0 and u1 of U (n values each) and the pivot block D
// (1 by 1: inv_d = 1 / d11; 2 by 2: d11, d21, d22 and inv_det = 1 / (d11 d22 - d21^2)). Segment s is row s (columns
// 0 to s, at a + s * lda) of a row-major block, or with `trailing` column s (rows s to n - 1, at a + s * lda + s) of a
// column-major one; element t of it pairs with u[t] or u[s + t]. Segments 0 to rows - 1 are updated (rows <= n). The
// segment's factors are f0 = u0[s] inv_d, or f0 = (d22 u0[s] - d21 u1[s]) inv_det and
// f1 = (d11 u1[s] - d21 u0[s]) inv_det; the segment takes the axpy of u0 with -f0, then of u1 with -f1, a zero
// factor skipping its term.
template <typename T> struct LdltUpdate
{
    T* a;
    const T* u0;
    const T* u1; // null for a 1 by 1 pivot
    crd::usize lda;
    crd::usize n;
    crd::usize rows;
    T inv_d; // 1 by 1: 1 / d11; 2 by 2: 1 / det
    T d11;
    T d21;
    T d22;
    bool trailing;
};

// The split-complex kernels (the Hermitian tridiagonalization and the complex Hessenberg reduction) carry a complex
// vector as two real arrays, re and im, and step like the real kernels (two roundings below avx2, fused from avx2):
// - a complex dot product u = sum a_k v_k (no conjugation) sums its four real products (re re, im im, re im, im re)
//   in one pass, each in A accumulators (2 from avx2, 1 below; four products share the registers): whole vector q goes
//   to accumulator q % A, the accumulators sum (0 + 1), then horizontal_sum's tree, then the remaining elements in
//   order, each rounding twice; u = (s_rere - s_imim, s_reim + s_imre);
// - y + s x (or y + s conj(x)) gives each part two steps, the s_re term first: re takes s_re x_re, then -s_im x_im
//   (conj: +s_im x_im); im takes s_re x_im (conj: -s_re x_im), then s_im x_re;
// - a scalar complex product (tau u, alpha x_r) rounds each multiply and add: (a_re b_re - a_im b_im, a_re b_im +
//   a_im b_re).
// The elementwise kernels (creflect's row update, cgemvt, cger, cher2) give every element, the last partial vector
// included, the level's steps; chemv's column update, as symv's, rounds twice on the elements after the last whole
// vector.

// H = I - tau v v^H applied from the right to `count` rows of len values (row r at re + r * ld and im + r * ld; v
// explicit, its head included): u = row v in the complex dot shape, s = tau u, then row + (-s) conj(v).
template <typename T> struct SplitReflectorRows
{
    T* re;
    T* im;
    const T* vr;
    const T* vi;
    crd::usize ld;
    crd::usize count;
    crd::usize len;
    T tau_re;
    T tau_im;
};

// y = y + A^T x over `rows` rows of `cols` values (row r at re + r * ld and im + r * ld): y takes the complex axpy of
// each row with the scale x[r] (conj_x: conj(x[r])) in row order.
template <typename T> struct SplitGemvtRows
{
    const T* re;
    const T* im;
    const T* xr;
    const T* xi;
    T* yr;
    T* yi;
    crd::usize ld;
    crd::usize rows;
    crd::usize cols;
    bool conj_x;
};

// A = A + alpha x y^T over `rows` rows of `cols` values: row r takes the complex axpy of y with the scale alpha x[r].
template <typename T> struct SplitGerRows
{
    T* re;
    T* im;
    const T* xr;
    const T* xi;
    const T* yr;
    const T* yi;
    crd::usize ld;
    crd::usize rows;
    crd::usize cols;
    T alpha_re;
    T alpha_im;
};

// x = A v for a Hermitian A of order n read from its lower triangle ((i, k), k <= i, at re[i * ld + k] and
// im[i * ld + k]; the diagonal's real part only). x is written, never read: it starts at +0, then row i takes, as
// symv, the complex dot u of its first i values with v, updates x[k] to x[k] + v[i] conj(a_ik) at every column k < i,
// and sets x[i] to x[i] + (u + a_ii v[i]) (each part rounding the product and both adds).
template <typename T> struct SplitHemvLower
{
    const T* re;
    const T* im;
    const T* vr;
    const T* vi;
    T* xr;
    T* xi;
    crd::usize ld;
    crd::usize n;
};

// A = A - v w^H - w v^H on the lower triangle of a Hermitian A of order n: row i (columns 0 to i) takes the axpy of
// conj(w) with -v[i], then of conj(v) with -w[i], and the diagonal's imaginary part is set to 0.
template <typename T> struct SplitHer2Lower
{
    T* re;
    T* im;
    const T* vr;
    const T* vi;
    const T* wr;
    const T* wi;
    crd::usize ld;
    crd::usize n;
};

// The kernels of one element type.
template <typename T> struct DenseTypeKernels
{
    GemmShape gemm_shape; // the shape gemm_block's panels have and pack_a and pack_b write
    void (*gemm_block)(const GemmBlock<T>& block) noexcept;
    // Rows [0, rows) by columns [0, depth) of s as ceil(rows / mr) row panels (mr rows of depth values), the padding
    // rows +0.
    void (*pack_a)(const GemmSource<T>& s, crd::usize rows, crd::usize depth, T* out) noexcept;
    // Rows [0, depth) by columns [0, cols) of s as ceil(cols / nr) column panels (depth rows of nr values), the
    // padding columns +0.
    void (*pack_b)(const GemmSource<T>& s, crd::usize depth, crd::usize cols, T* out) noexcept;
    T (*dot)(const T* x, const T* y, crd::usize n) noexcept;
    void (*axpy)(T* y, const T* x, T alpha, crd::usize n) noexcept; // y + alpha x
    void (*gemv)(const GemvRows<T>& g) noexcept;
    void (*gemvt)(const GemvtRows<T>& g) noexcept;
    void (*ger)(const GerRows<T>& g) noexcept;
    void (*symv)(const SymvLower<T>& s) noexcept;
    void (*trsv)(const TrsvRows<T>& t) noexcept;
    // x' = c x + s y, y' = c y - s x over n values (LAPACK xROT).
    void (*rot)(T* x, T* y, T c, T s, crd::usize n) noexcept;
    void (*reflect)(const ReflectorRows<T>& r) noexcept;
    void (*ldlt_update)(const LdltUpdate<T>& u) noexcept;
    void (*creflect)(const SplitReflectorRows<T>& r) noexcept;
    void (*cgemvt)(const SplitGemvtRows<T>& g) noexcept;
    void (*cger)(const SplitGerRows<T>& g) noexcept;
    void (*chemv)(const SplitHemvLower<T>& h) noexcept;
    void (*cher2)(const SplitHer2Lower<T>& h) noexcept;
    // The unblocked lower Cholesky of the n by n block at a (row stride ld), in place: 0, or k + 1 when column k is
    // not positive definite (the columns before it are factored).
    crd::usize (*cholesky)(T* a, crd::usize n, crd::usize ld) noexcept;
    // L21 = A21 L11^-T for rows [first, end) of the panel at column k, width nb (L11 factored at (k, k)).
    void (*cholesky_trsm)(T* a, crd::usize ld, crd::usize k, crd::usize nb, crd::usize first, crd::usize end) noexcept;
};

struct DenseKernels
{
    DenseTypeKernels<crd::f64> f64_kernels;
    DenseTypeKernels<crd::f32> f32_kernels;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const DenseKernels kDenseKernelsPortable;
extern const DenseKernels kDenseKernelsSse2;
extern const DenseKernels kDenseKernelsAvx2;
extern const DenseKernels kDenseKernelsAvx512;

} // namespace crd::hesap::dense
