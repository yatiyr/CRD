// The table of the hesap-tensor kernels (SIMD-DISPATCH, ADR-0134 §5): one per CPU level the kernel unit is built for
// (engine/numerics/hesap-tensor/kernels/tensor_kernels.cpp), resolved by tensor_kernels() in
// crd/hesap/tensor/tensor_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing from
// it. Every routine is bit-exact across levels (ADR-0063, ADR-0134 §7): the permute is copies and IEEE multiplies, the
// binned sum is partition-independent by construction, the quantizers are per-element IEEE operations, the quantized
// dots are exact integers combined in one fixed f32 order (single-rounded FMAs), and softmax's exp core has a
// bit-identical vector twin.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::tensor
{

namespace detail
{
struct BinnedRungs; // crd/hesap/tensor/binned_core.hpp
} // namespace detail

struct BlockQ8Zero; // crd/hesap/tensor/quant_core.hpp

// One (a, b) region of an f32 permute: dst(ia, ib) = src(ia, ib) (scale: alpha * src(ia, ib)) over [0, na) x [0, nb),
// with dst(ia, ib) at dst + ia*ra + ib and src(ia, ib) at src + ia*sa + ib*sb, in tile_a x tile_b tiles, a-tiles outer.
// stream asks for non-temporal stores (the region origin dst 32-byte aligned, ra and tile_b multiples of 8),
// published before the call returns; a level without them stores normally.
struct PermuteRegionF32
{
    crd::f32* dst;
    crd::i64 ra;
    const crd::f32* src;
    crd::i64 sa;
    crd::i64 sb;
    crd::u64 na;
    crd::u64 nb;
    crd::u64 tile_a;
    crd::u64 tile_b;
    crd::f32 alpha;
    bool scale;
    bool stream;
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
// blocks (dtypes.hpp quantize_q8_0) and every output is the pinned lane-parallel dot (nn.hpp).
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
    // The f32 region of a permute (PermuteRegionF32).
    void (*permute_region_f32)(const PermuteRegionF32& region) noexcept;
    // The Tier R sum: deposit p[0..n) into acc (the binnedBLAS dbdsum protocol), bit-identical to the scalar
    // deposits in any partition.
    void (*binned_sum_f64)(detail::BinnedRungs& acc, const crd::f64* p, crd::u64 n) noexcept;
    // Q8_0 over nblocks whole blocks of x, bit-identical to quantize_q8_0.
    void (*quantize_q8_0)(const crd::f32* x, BlockQ8Zero* out, crd::u64 nblocks) noexcept;
    // Asymmetric u8 over one row of k values (k % 32 == 0), bit-identical to nn_quantize_u8_asym.
    void (*quantize_u8_row)(const crd::f32* x, crd::u64 k, crd::u8* out, crd::f32& scale,
                            crd::i32& zero_point) noexcept;
    void (*nn_softmax_rows)(const NnSoftmaxF32& c, crd::u32 lo, crd::u32 hi) noexcept;
    void (*nn_linear_q8_rows)(const NnLinearQ8& c, crd::u32 lo, crd::u32 hi) noexcept;
    // The row-major weights of nn_linear_i8 and the packed weights of the NnSequential runner; the same bits.
    void (*nn_linear_i8_rows)(const NnLinearI8& c, crd::u32 lo, crd::u32 hi) noexcept;
    void (*nn_linear_i8_packed_rows)(const NnLinearI8& c, crd::u32 lo, crd::u32 hi) noexcept;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const TensorKernels kTensorKernelsPortable;
extern const TensorKernels kTensorKernelsAvx2;

} // namespace crd::hesap::tensor
