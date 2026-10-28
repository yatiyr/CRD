// The table of the hesap-tensor kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-tensor/kernels/tensor_kernels.cpp), resolved by tensor_kernels() in
// crd/hesap/tensor/tensor_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing from
// it. Every routine is bit-exact across levels (ADR-0063, ADR-0134 §7): the permute is copies and the IEEE operations
// of a real or Complex<T> product, the binned sum is partition-independent by construction, the quantizers are
// per-element IEEE operations, the quantized dots are exact integers combined in one fixed f32 order (single-rounded
// FMAs), softmax's exp core has a bit-identical vector twin, the Philox blocks are integer rounds, the elementwise ops
// and the einsum products are per-element IEEE operations in one order, Tier D's reduction order is fixed by kEwLanes
// and kReduceBlock alone, each batched matrix runs its own lane in the scalar tier's operations, and each sparse
// walk's output element is one fma chain in the canonical nonzero order.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::tensor
{

namespace detail
{
struct BinnedRungs; // crd/hesap/tensor/binned_core.hpp

// Tier D's block (reduce.hpp): a function of the shape only, never of the workers.
inline constexpr crd::u64 kReduceBlock = 4096U;
} // namespace detail

struct BlockQ8Zero; // crd/hesap/tensor/quant_core.hpp

// The lane group of the elementwise, reduction and einsum kernels: eight f32 or four f64 values on every level. It
// fixes Tier D's block partial (four groups, ADR-0134 §7) and the einsum direct-kernel gates; a level with wider
// registers steps two groups at once.
template <typename T> inline constexpr crd::u64 kEwLanes = sizeof(T) == 4U ? 8U : 4U;

// The broadcast binary ops (elementwise.hpp ew_binary), each one IEEE operation per element; Min and Max are x86's
// minps and maxps (a < b ? a : b, a > b ? a : b: the second operand on a NaN or a signed-zero tie).
enum class BinaryOp : crd::u8
{
    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
};
inline constexpr crd::u32 kBinaryOps = 6U;

// The reductions (reduce.hpp, reduce_axes.hpp); the kernels run the first four (Mean is the sum's quotient).
enum class ReduceOp : crd::u8
{
    Sum,
    Prod,
    Min,
    Max,
    Mean,
};
inline constexpr crd::u32 kReduceKernelOps = 4U;

// One broadcast binary op over its collapsed dims (elementwise.hpp collapse): dst[w] = a op b for the row-major
// positions w of shape[0 .. rank), dst contiguous, a and b at their element strides sa and sb per dim (0 where they
// broadcast). Rank 0 is one element.
template <typename T> struct EwBinaryWalk
{
    T* dst;
    const T* a;
    const T* b;
    const crd::u64* shape;
    const crd::i64* sa;
    const crd::i64* sb;
    crd::u32 rank;
};

// The Tier D kernels of one ReduceOp.
template <typename T> struct ReduceKernels
{
    // A contiguous run's partial in the pinned order, folded onto init: four accumulators of kEwLanes lanes striped
    // over the run, combined ((a0 op a1) op (a2 op a3)), their lanes folded left to right, then the scalar tail.
    T (*block)(const T* p, crd::u64 n, T init) noexcept;
    // out[o] for o in [o0, o1): the Tier D tree of the run src[o*len, o*len + len), its blocks' partials folded left
    // to right, seeded by 0 (Sum), 1 (Prod) or the run's first value (Min, Max; len > 0).
    void (*rows)(T* out, const T* src, crd::u64 len, crd::u64 o0, crd::u64 o1) noexcept;
    // out[j] for j in [j0, j1): src[j] folded with src[i*w + j] for i = 1 .. rows - 1 in order (rows > 0).
    void (*columns)(T* out, const T* src, crd::u64 rows, crd::u64 w, crd::u64 j0, crd::u64 j1) noexcept;
};

// The batched dense linear algebra (batched.hpp) over matrices b of a batch, each row-major tight at element b * s of
// its operand. The lane tier factors kEwLanes matrices per group (one lane each) up to these n, the scalar tier
// (batched_core.hpp) above them, in the same operations, so a matrix's bits depend on nothing but its own values.
inline constexpr crd::u64 kBatchedCholMaxN = 32U; // Cholesky and LU
inline constexpr crd::u64 kBatchedSvdMaxN = 16U;

// c[b] = alpha * a[b] @ b[b] + beta * c[b], a m x k, b k x n: every element the k-ordered single-rounded fma chain,
// then alpha * acc and (beta != 0) fma(beta, c, alpha * acc).
template <typename T> struct BatchedGemm
{
    T alpha;
    T beta;
    const T* a;
    const T* b;
    T* c;
    crd::i64 sa;
    crd::i64 sb;
    crd::i64 sc;
    crd::u64 m;
    crd::u64 k;
    crd::u64 n;
};

// A batch of n x n matrices factored in place, with info[b] and (LU) the pivot rows piv[b * n + j].
template <typename T> struct BatchedFactor
{
    T* a;
    crd::i64 sa;
    crd::i32* info;
    crd::i32* piv;
    crd::u64 n;
};

// A batch of n x n one-sided Jacobi SVDs: u[b] diag(sigma[b * n ..]) v[b]^T = a[b], info[b] = 0 or 1 (still rotating
// after max_sweeps).
template <typename T> struct BatchedSvd
{
    const T* a;
    T* u;
    T* v;
    T* sigma;
    crd::i32* info;
    crd::i64 sa;
    crd::i64 su;
    crd::i64 sv;
    crd::u64 n;
    crd::u32 max_sweeps;
};

// The solves with a batched factor f (n x n) of the n x r right-hand sides x, in place; piv as BatchedFactor's (LU).
template <typename T> struct BatchedSolve
{
    const T* f;
    T* x;
    const crd::i32* piv;
    crd::i64 sf;
    crd::i64 sx;
    crd::u64 n;
    crd::u64 r;
};

// The batched kernels of one element type, each over the matrices [b0, b1).
template <typename T> struct BatchedKernels
{
    void (*gemm_tiny)(const BatchedGemm<T>& g, crd::u64 b0, crd::u64 b1) noexcept;
    // Cholesky (lower factor, strict upper zeroed; info the first non-positive pivot's 1-based column) and LU with
    // partial pivoting, both in batched_core.hpp's operations.
    void (*cholesky)(const BatchedFactor<T>& f, crd::u64 b0, crd::u64 b1) noexcept;
    void (*lu)(const BatchedFactor<T>& f, crd::u64 b0, crd::u64 b1) noexcept;
    void (*svd)(const BatchedSvd<T>& s, crd::u64 b0, crd::u64 b1) noexcept;
    // x = L^-T L^-1 x, and x = U^-1 L^-1 P x.
    void (*cholesky_solve)(const BatchedSolve<T>& s, crd::u64 b0, crd::u64 b1) noexcept;
    void (*lu_solve)(const BatchedSolve<T>& s, crd::u64 b0, crd::u64 b1) noexcept;
};

// The largest rank a sparse walk takes (kMaxRank of crd/hesap/tensor/tensor.hpp, which sparse.hpp asserts).
inline constexpr crd::u32 kSparseMaxRank = 8U;

// A sparse x dense mode-n product (sparse.hpp contract_mode) over a canonical COO's nonzeros e: the output element at
// sum over m != mode of planes[m][e] * ostride[m], plus q * ostride[mode], gains val[e] * u[planes[mode][e] * f + q]
// for q in [0, f).
template <typename T> struct SparseTtmWalk
{
    const crd::u32* planes[kSparseMaxRank]{};
    crd::u64 ostride[kSparseMaxRank]{};
    const T* val;
    const T* u;
    crd::u64 f;
    crd::u32 rank;
    crd::u32 mode;
};

// A CSF MTTKRP (sparse_mttkrp.hpp mttkrp): the fiber ids of every level, the child pointers of levels 0 .. rank - 2,
// the factor of each level's mode (level 0's unused), the leaf values and the [shape(order(0)), r] output.
template <typename T> struct MttkrpCsfWalk
{
    const crd::u32* fids[kSparseMaxRank]{};
    const crd::u32* fptr[kSparseMaxRank]{};
    const T* fac[kSparseMaxRank]{};
    const T* val;
    T* out;
    crd::u64 r;
    crd::u32 rank;
};

// A COO MTTKRP over mode 0 (sparse_mttkrp.hpp mttkrp_coo): the index planes and factors of every mode (mode 0's
// unused), the n values and the [shape(0), r] output.
template <typename T> struct MttkrpCooWalk
{
    const crd::u32* planes[kSparseMaxRank]{};
    const T* fac[kSparseMaxRank]{};
    const T* val;
    T* out;
    crd::u64 n;
    crd::u64 r;
    crd::u32 rank;
};

// The sparse tensor walks of one element type. Every output element is one chain of single-rounded fmas in the
// canonical nonzero order (an MTTKRP fiber's leaves seeded by 0 into a sum that multiplies its parent's factor row as
// fma(sum, row, dst)), or the parts' sums in ascending part order, so a CSF and a COO MTTKRP give the same bits.
template <typename T> struct SparseTensorKernels
{
    // The nonzeros [eb, ee) into dst (zeroed by the caller).
    void (*ttm_scatter)(const SparseTtmWalk<T>& w, crd::u64 eb, crd::u64 ee, T* dst) noexcept;
    // An inner mode (0 < mode < rank - 1): per run of equal modes 0 .. mode - 1 in [eb, ee), the nonzeros into the
    // zeroed [ostride[mode], f] tile, which is then transposed into its out slice.
    void (*ttm_staged)(const SparseTtmWalk<T>& w, crd::u64 eb, crd::u64 ee, T* tile, T* out) noexcept;
    // out[i] += parts[t * osize + i] for t = 0 .. nparts - 1 in order.
    void (*fold)(T* out, const T* parts, crd::u64 osize, crd::u32 nparts) noexcept;
    // The root fibers [rb, re) into their (zeroed) output rows; tmp holds (rank - 2) * r scratch values.
    void (*mttkrp_csf)(const MttkrpCsfWalk<T>& w, crd::u64 rb, crd::u64 re, T* tmp) noexcept;
    // The whole walk (out zeroed, n > 0); tmp as mttkrp_csf's.
    void (*mttkrp_coo)(const MttkrpCooWalk<T>& w, T* tmp) noexcept;
};

// The largest rank a permute walks (kMaxRank of crd/hesap/tensor/tensor.hpp, which permute.hpp asserts).
inline constexpr crd::u32 kPermuteMaxRank = 8U;

// The region (or tile) after this one, which the kernel prefetches while it copies this one: its origin in elements
// from this one's (src and dst) and its na x nb extents, 0 x 0 when there is none. The kernel prefetches only elements
// inside those extents, so it forms no pointer outside the arrays.
struct PermuteNext
{
    crd::i64 src;
    crd::i64 dst;
    crd::u64 na;
    crd::u64 nb;
};

// One (a, b) region of a permute of f32 or f64 elements, or (complex) of Complex<T> elements (crd/hesap/complex.hpp)
// as their (re, im) pairs: dst(ia, ib) = src(ia, ib) (scale: alpha * src(ia, ib), alpha = alpha_re + i alpha_im in
// Complex<T>'s operator*; alpha_re alone for a real element) over [0, na) x [0, nb), with dst(ia, ib) at element
// ia*ra + ib of dst and src(ia, ib) at element ia*sa + ib*sb of src, in tile_a x tile_b tiles, a-tiles outer. stream
// asks for non-temporal stores (the region origin dst 32-byte aligned, ra and tile_b elements a multiple of 32
// bytes), published before the call returns; a level without them stores normally.
template <typename T> struct PermuteRegion
{
    T* dst;
    crd::i64 ra;
    const T* src;
    crd::i64 sa;
    crd::i64 sb;
    crd::u64 na;
    crd::u64 nb;
    crd::u64 tile_a;
    crd::u64 tile_b;
    T alpha_re;
    T alpha_im;
    bool complex;
    bool scale;
    bool stream;
    PermuteNext next{};
};

// A whole serial tiled permute (permute.hpp permute_tiled), one call per transform, of the elements of PermuteRegion:
// the m collapsed dims in dst order (dst dim d has extent shp[d], src stride s_in[d] and dst stride s_out[d], in
// elements; m <= kPermuteMaxRank), a the src-fastest dim and m - 1 the dst-fastest. Every (a, m - 1) plane is one
// region in tile x tile tiles; the planes follow permute_outer_order (permute_tiles.hpp), and each region prefetches
// the next plane.
template <typename T> struct PermuteWalk
{
    T* dst;
    const T* src;
    const crd::u64* shp;
    const crd::i64* s_in;
    const crd::i64* s_out;
    crd::u32 m;
    crd::u32 a;
    crd::u64 tile;
    T alpha_re;
    T alpha_im;
    bool complex;
    bool scale;
};

// The rows [lo, hi) of a last-dimension softmax over [rows, d] (nn.hpp nn_softmax): max-subtract, exp, sum in index
// order, divide.
struct NnSoftmaxF32
{
    const crd::f32* x;
    crd::f32* y;
    crd::u64 d;
};

// The Q8_0 linear y = x @ W^T + b (optionally ReLU) for the rows [lo, hi): each row's activations quantize to Q8_0
// blocks (quant_core.hpp quantize_q8_0_blocks) and every output is the pinned lane-parallel dot (nn.hpp).
struct NnLinearQ8
{
    const crd::f32* x;      // [M, K], K % 32 == 0
    const BlockQ8Zero* wq;  // [N * (K/32)]
    const crd::f32* wscale; // [N * (K/32)] the weight block scales widened to f32 at load
    const crd::f32* bias;   // [N]
    crd::f32* y;            // [M, N]
    BlockQ8Zero* xq;        // scratch: [M * (K/32)] quantized activation rows
    crd::f32* xscale;       // scratch: [M * (K/32)] activation block scales
    crd::u64 k;
    crd::u64 n;
    bool relu;
};

// The per-tensor int8 linear for the rows [lo, hi): each row's activations quantize to asymmetric u8 (dtypes.hpp
// nn_quantize_u8_asym) and every output is fma(sx * sw, f32(dot - zp * colsum), bias) on the exact i32 dot.
struct NnLinearI8
{
    const crd::f32* x;      // [M, K], K % 32 == 0
    const crd::i8* wq;      // [N, K] row-major, or packed [n8 * K] (nn.hpp i8_pack_weights) for the packed entry
    const crd::i32* colsum; // [N] per-output weight column sums (n8-padded for the packed entry)
    const crd::f32* bias;   // [N] (n8-padded for the packed entry)
    crd::f32* y;            // [M, N]
    crd::u8* xq;            // scratch: [M, K] u8 activation rows
    crd::f32* xscale;       // scratch: [M] row scales
    crd::i32* xzp;          // scratch: [M] row zero points
    crd::f32 wscale;        // the one weight scale
    crd::u64 k;
    crd::u64 n;
    bool relu;
};

struct TensorKernels
{
    // The region of a permute of f32 or Complex<f32> (f64, Complex<f64>) elements: one multithreaded permute task.
    void (*permute_region_f32)(const PermuteRegion<crd::f32>& region) noexcept;
    void (*permute_region_f64)(const PermuteRegion<crd::f64>& region) noexcept;
    // The serial tiled permute of those elements (PermuteWalk).
    void (*permute_walk_f32)(const PermuteWalk<crd::f32>& walk) noexcept;
    void (*permute_walk_f64)(const PermuteWalk<crd::f64>& walk) noexcept;
    // The Tier R sum: deposit p[0..n) into acc (the binnedBLAS dbdsum protocol), bit-identical to the scalar
    // deposits in any partition.
    void (*binned_sum_f64)(detail::BinnedRungs& acc, const crd::f64* p, crd::u64 n) noexcept;
    // Q8_0 over nblocks whole blocks of x (dtypes.hpp quantize_q8_0), bit-identical to the scalar reference
    // detail::quantize_q8_0_blocks (quant_core.hpp).
    void (*quantize_q8_0)(const crd::f32* x, BlockQ8Zero* out, crd::u64 nblocks) noexcept;
    // Asymmetric u8 over one row of k values (k % 32 == 0), bit-identical to nn_quantize_u8_asym.
    void (*quantize_u8_row)(const crd::f32* x, crd::u64 k, crd::u8* out, crd::f32& scale,
                            crd::i32& zero_point) noexcept;
    void (*nn_softmax_rows)(const NnSoftmaxF32& c, crd::u32 lo, crd::u32 hi) noexcept;
    void (*nn_linear_q8_rows)(const NnLinearQ8& c, crd::u32 lo, crd::u32 hi) noexcept;
    // The row-major weights of nn_linear_i8 and the packed weights of the NnSequential runner; the same bits.
    void (*nn_linear_i8_rows)(const NnLinearI8& c, crd::u32 lo, crd::u32 hi) noexcept;
    void (*nn_linear_i8_packed_rows)(const NnLinearI8& c, crd::u32 lo, crd::u32 hi) noexcept;
    // Philox4x32-10 blocks first_block .. first_block + blocks - 1 of (seed, stream), their u32 words in block order at
    // out (any alignment): the hesap-stats block loop (crd/hesap/stats/philox_blocks.hpp) compiled at this level, so
    // the SR draws (dtypes.hpp) and the decomp sketches keep Philox include-only (ADR-0096 §1).
    void (*philox4x32_blocks)(crd::u64 seed, crd::u64 stream, crd::u64 first_block, void* out,
                              crd::usize blocks) noexcept;
    // The broadcast binary ops, indexed by BinaryOp.
    void (*ew_binary_f32[kBinaryOps])(const EwBinaryWalk<crd::f32>& walk) noexcept;
    void (*ew_binary_f64[kBinaryOps])(const EwBinaryWalk<crd::f64>& walk) noexcept;
    // Tier D, indexed by ReduceOp.
    ReduceKernels<crd::f32> reduce_f32[kReduceKernelOps];
    ReduceKernels<crd::f64> reduce_f64[kReduceKernelOps];
    // einsum's pack-free row-major products c[m x n] = a[m x k] b[k x n] (einsum_exec.hpp), each output a sum of
    // a*b products in k order, rounded after every multiply and add. thin: n <= 8 * kEwLanes. smallm: a is k x m when
    // a_trans; n in pairs of lane groups in registers over 8 rows, the rest as scalar dots.
    void (*einsum_thin_f32)(crd::u64 m, crd::u64 k, crd::u64 n, const crd::f32* a, const crd::f32* b,
                            crd::f32* c) noexcept;
    void (*einsum_thin_f64)(crd::u64 m, crd::u64 k, crd::u64 n, const crd::f64* a, const crd::f64* b,
                            crd::f64* c) noexcept;
    void (*einsum_smallm_f32)(crd::u64 m, crd::u64 k, crd::u64 n, const crd::f32* a, const crd::f32* b, crd::f32* c,
                              bool a_trans) noexcept;
    void (*einsum_smallm_f64)(crd::u64 m, crd::u64 k, crd::u64 n, const crd::f64* a, const crd::f64* b, crd::f64* c,
                              bool a_trans) noexcept;
    // The batched dense linear algebra.
    BatchedKernels<crd::f32> batched_f32;
    BatchedKernels<crd::f64> batched_f64;
    // The sparse tensor TTM and MTTKRP walks.
    SparseTensorKernels<crd::f32> sparse_f32;
    SparseTensorKernels<crd::f64> sparse_f64;
};

// The levels with a body and the VNNI variants of avx2 (AVX-VNNI) and avx512 (AVX512-VNNI), the integer dots on
// dpbusd; the dispatcher refuses a build that compiles any other.
extern const TensorKernels kTensorKernelsPortable;
extern const TensorKernels kTensorKernelsSse2;
extern const TensorKernels kTensorKernelsSse42;
extern const TensorKernels kTensorKernelsAvx2;
extern const TensorKernels kTensorKernelsAvx2AvxVnni;
extern const TensorKernels kTensorKernelsAvx512;
extern const TensorKernels kTensorKernelsAvx512Avx512Vnni;

} // namespace crd::hesap::tensor
