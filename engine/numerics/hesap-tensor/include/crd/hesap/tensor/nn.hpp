#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-m: the NN inference pack (the certified tiny-ML demo).
//
// Ops: conv2d (the direct 3x3/s1/p1 kernel with the fused 2x2/s2 maxpool, or
// im2col over the allocation-free v14-h tiny-tier GEMM, SANITY #8 reuse),
// max pooling (IEEE 754 maximum: a NaN wins, +0 over -0) and avg pooling, the
// convolution and pooling being per-level tensor kernels (tensor_kernels.hpp),
// activations (relu / exact erf-gelu / tanh / sigmoid — ALL
// transcendentals via crd::math, never std::), layernorm (torch semantics:
// biased variance, eps inside the sqrt), numerically-stable softmax
// (max-subtract; exp = the crd_exp1/crd_exp4 deterministic core), linear f32
// (x @ W^T + b over a load-time-transposed weight so the GEMM streams
// row-major), and the QUANTIZED Q8_0 linear (ggml block-32 layout from
// dtypes.hpp): int8 x int8 -> EXACT i32 lane partials per block, combined by
// a fixed per-lane fma with the f16-scale-pair product and a fixed horizontal
// tree — the INTEGER stage is bit-exact on every backend (the certification
// tier) and the whole sequence is a pinned op order (tensor_kernels.cpp).
//
// NnSequential — a fixed op list (bounded kNnMaxOps) with OWNED weights
// (loaders copy out of the call-scoped safetensors payload spans — never a
// borrowed-lifetime member in a returned object) and ALLOCATION-FREE
// inference: every intermediate lives in the caller workspace.
//
// Workspace-size formula (workspace_bytes(), fixed at finalize() for
// max_batch = input_shape[0]):
//     64                                  base-pointer 64-alignment slack
//   + 2 * align64(buf_elems * 4)          ping/pong activation buffers,
//                                         buf_elems = max over ops of the op's
//                                         output element count at max_batch
//   + align64(scratch_bytes)              the op scratch region, max over ops:
//       conv2d:    kNnConvScratchSlots * align64(scratch_floats * 4) where
//                  scratch_floats = max(C*KH*KW * OH*OW,               im2col
//                                       (C*OH + 17) * (OW + 10))  3x3 staging
//                  (zero row + C*H padded rows + 16 pool-fusion row buffers;
//                  one slot per worker; OH/OW are the CONV output dims)
//       Q8 linear: align64(max_batch * (K/32) * sizeof(BlockQ8Zero))
//                  + align64(max_batch * (K/32) * 4)
//                  (per-row quantized-activation blocks + their f32 scales)
//       i8 linear: align64(max_batch * K) + 2 * align64(max_batch * 4)
//                  (per-row u8 activations + their scales + zero points)
//       f32 linear, dense tier (2*B*K*N >= kNnDenseLinearFlops):
//                  kNnGemmScratchBytes (the hesap-dense GEMM pack-buffer
//                  arena — a LinearAllocator over this region, zero heap)
//
// Determinism: every kernel partitions ONLY across samples/planes/rows with
// disjoint outputs and a fixed within-sample operation order, so inference is
// bit-identical at any worker count (the {1,2,4,8,16} moat, gated). The
// dense-GEMM tier rides hesap-dense gemm_parallel's own disjoint-row-slab
// contract (ADR-0063, gated in test_blas3_parallel).
//
// D-v14m-1 (documented divergence): the frozen Q8_0 weight refs
// (scripts/v14m_nn_oracle.py) invert the f16-ROUNDED scale and round
// half-to-EVEN (np.rint) with an explicit +-127 clip; dtypes.hpp's
// quantize_q8_0 (the ggml quantize_row_q8_0_ref transcription) inverts the
// PRE-rounding f32 d and rounds half-away (roundf). Measured 2026-07-05 on
// the corpus weights: 150 / 16416 q values differ between the two. Weight
// quantize-on-load therefore uses nn_quantize_q8_0_rint (the oracle
// semantics, byte-exact vs the frozen refs, gate-enforced); ggml byte
// interop and ACTIVATION quantization keep riding dtypes.hpp quantize_q8_0.
//
// Error contract (ADR-0095 pillars): fallible ops return TensorStatus and are
// noexcept; programmer errors are CRD_ASSERT in debug. Bounded everything:
// op count, rank, conv scratch slots, dense-tier arena.
// ---------------------------------------------------------------------------

#include "dtypes.hpp" // BlockQ8Zero + f16 converts + quantize_q8_0 (activations)
#include "io.hpp"     // SafetensorsFile — the v14-l reader consumed by the builders
#include "tensor.hpp"
#include "tensor_dispatch.hpp" // dots, quantizers, softmax, layernorm, GEMM, conv, pool, relu: the level kernels

#include <crd/hesap/dense/blas3.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/cmath.hpp>         // crd::math::exp / tanh / sqrt / nearbyint — the deterministic surface
#include <crd/math/deterministic.hpp> // crd::math::deterministic::erf — the exact (erf-form) gelu
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

#include <cstring>
#include <utility> // std::move

namespace crd::hesap::tensor
{

inline constexpr crd::u32 kNnMaxOps = 24U;
inline constexpr crd::u32 kNnConvScratchSlots = 32U; // bounded per-worker conv scratch slots
// 2*B*K*N at/above this rides hesap-dense gemm; below it the v14-h direct
// register-tiled kernel wins (measured 2026-07-05: 150 vs 146 GF/s at 67 Mflop
// on 4096x64x128 — the direct tier holds through the tiny-model regime).
inline constexpr crd::u64 kNnDenseLinearFlops = 96ULL * 1024U * 1024U;
inline constexpr crd::u64 kNnGemmScratchBytes = 8ULL << 20U; // dense-tier pack-buffer arena
inline constexpr crd::f32 kNnLayerNormEps = 1e-5F;           // torch nn.LayerNorm default

[[nodiscard]] constexpr crd::u64 nn_align64(crd::u64 bytes) noexcept
{
    return (bytes + 63U) & ~crd::u64{63};
}

// =======================================================================
// Q8_0 weight quantization — the FROZEN-CORPUS semantics (D-v14m-1 above).
// Zero-pads the tail block (x.size() need not be a block multiple — conv
// weights flatten to non-multiples); out.size() must be ceil(x.size()/32).
// =======================================================================
inline void nn_quantize_q8_0_rint(crd::containers::ConstSpan<crd::f32> x,
                                  crd::containers::Span<BlockQ8Zero> out) noexcept
{
    CRD_ASSERT_MSG(out.size() * kQuantBlock >= x.size() &&
                       out.size() * kQuantBlock < x.size() + kQuantBlock,
                   "nn_quantize_q8_0_rint: out must hold ceil(n/32) blocks");
    const crd::usize n = x.size();
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        crd::f32 blk[kQuantBlock];
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::usize src = i * kQuantBlock + j;
            blk[j] = src < n ? x[src] : 0.0F;
        }
        crd::f32 amax = 0.0F;
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::f32 a = blk[j] < 0.0F ? -blk[j] : blk[j];
            amax = a > amax ? a : amax;
        }
        const crd::u16 dbits = f32_to_f16_bits(amax / 127.0F);
        const crd::f32 s = f16_bits_to_f32(dbits);       // the f16-ROUNDED scale (oracle semantics)
        const crd::f32 inv = s > 0.0F ? 1.0F / s : 0.0F; // np.where(scale > 0, 1/f32(scale), 0)
        out[i].d = dbits;
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::f32 v = crd::math::nearbyint(blk[j] * inv);  // np.rint — half-to-even
            const crd::f32 c = crd::math::clamp(v, -127.0F, 127.0F); // np.clip(-127, 127)
            out[i].qs[j] = static_cast<crd::i8>(static_cast<crd::i32>(c));
        }
    }
}

// Assemble BlockQ8Zero storage from the raw ref streams (.q8 = int8 values,
// .q8s = f16 scale bits) — the frozen-reference weight-load path.
[[nodiscard]] inline TensorStatus nn_q8_blocks_from_raw(crd::containers::ConstSpan<crd::i8> qs,
                                                        crd::containers::ConstSpan<crd::u16> scales,
                                                        crd::containers::Span<BlockQ8Zero> out) noexcept
{
    if (qs.size() != out.size() * kQuantBlock || scales.size() != out.size())
    {
        return TensorStatus::BadInput;
    }
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        out[i].d = scales[i];
        std::memcpy(out[i].qs, qs.data() + i * kQuantBlock, kQuantBlock);
    }
    return TensorStatus::Ok;
}

namespace nndetail
{

// ---- the bounded parallel driver (the batched.hpp pattern) ----------------
// fn(ctx, lo, hi) over [0, count); serial when nw <= 1. Partition is across
// INDEPENDENT units (samples/planes/rows) with disjoint outputs, so results
// are bit-identical at every worker count by construction.
template <typename C>
inline void nn_parallel(crd::u32 count, crd::u32 nw, const C* ctx,
                        void (*fn)(const C&, crd::u32, crd::u32)) noexcept
{
    if (count == 0U)
    {
        return;
    }
    if (nw <= 1U || count < 2U)
    {
        fn(*ctx, 0U, count);
        return;
    }
    struct Task
    {
        const C* c;
        void (*f)(const C&, crd::u32, crd::u32);
    };
    Task t{ctx, fn};
    const Task* tp = &t;
    auto* const counter =
        crd::jobs::parallel_for(count, nw, [tp](crd::u32 lo, crd::u32 hi) { tp->f(*tp->c, lo, hi); });
    crd::jobs::wait(counter);
}

// ---- activations ----------------------------------------------------------

// relu: maxps(x, +0) per element (+0 for a NaN or -0, `x > 0 ? x : 0`), the tensor kernels' elementwise Max walk
// with a broadcast zero; y may be x.
inline void relu_span(const crd::f32* x, crd::f32* y, crd::u64 n) noexcept
{
    const crd::f32 zero = 0.0F;
    const crd::u64 shape[1] = {n};
    const crd::i64 sa[1] = {1};
    const crd::i64 sb[1] = {0};
    tensor_kernels().table->ew_binary_f32[static_cast<crd::u32>(BinaryOp::Max)]({y, x, &zero, shape, sa, sb, 1U});
}

// exact gelu (torch approximate='none'): 0.5 x (1 + erf(x / sqrt(2))) in f64.
[[nodiscard]] inline crd::f32 gelu_one(crd::f32 x) noexcept
{
    constexpr crd::f64 inv_sqrt2 = 0.70710678118654752440;
    const crd::f64 xd = static_cast<crd::f64>(x);
    return static_cast<crd::f32>(0.5 * xd * (1.0 + crd::math::deterministic::erf(xd * inv_sqrt2)));
}

[[nodiscard]] inline crd::f32 sigmoid_one(crd::f32 x) noexcept
{
    return static_cast<crd::f32>(1.0 / (1.0 + crd::math::exp(-static_cast<crd::f64>(x))));
}

[[nodiscard]] inline crd::f32 tanh_one(crd::f32 x) noexcept
{
    return crd::math::tanh(x);
}

enum class NnAct : crd::u8
{
    Relu,
    Gelu,
    Tanh,
    Sigmoid,
};

inline void act_span(NnAct act, const crd::f32* x, crd::f32* y, crd::u64 n) noexcept
{
    switch (act)
    {
    case NnAct::Relu:
        relu_span(x, y, n);
        return;
    case NnAct::Gelu:
        for (crd::u64 i = 0; i < n; ++i)
        {
            y[i] = gelu_one(x[i]);
        }
        return;
    case NnAct::Tanh:
        for (crd::u64 i = 0; i < n; ++i)
        {
            y[i] = tanh_one(x[i]);
        }
        return;
    case NnAct::Sigmoid:
        for (crd::u64 i = 0; i < n; ++i)
        {
            y[i] = sigmoid_one(x[i]);
        }
        return;
    }
}

struct EwCtx
{
    const crd::f32* x;
    crd::f32* y;
    crd::u64 per; // elements per partition unit (one sample)
    NnAct act;
};

inline void ew_run(const EwCtx& c, crd::u32 lo, crd::u32 hi)
{
    const crd::u64 n0 = static_cast<crd::u64>(lo) * c.per;
    const crd::u64 n1 = static_cast<crd::u64>(hi) * c.per;
    act_span(c.act, c.x + n0, c.y + n0, n1 - n0);
}

// ---- pooling and conv2d: the tensor kernels (NnPool2d, NnConv2d) ------------
// conv2d runs per image: the direct 3x3/s1/p1 kernel (the CNN hot shape, with the fused 2x2/s2 maxpool), or im2col
// and the tiny GEMM in a per-worker scratch slot.
struct ConvCtx
{
    NnConv2d conv;
    crd::f32* col_slots; // slot_floats per worker slot
    crd::u64 slot_floats;
    bool parallel; // slot = jobs::worker_index() when true, slot 0 otherwise
};

[[nodiscard]] inline bool conv_is_3x3s1p1(crd::u64 kh, crd::u64 kw, crd::u64 pad, crd::u64 stride) noexcept
{
    return kh == 3U && kw == 3U && pad == 1U && stride == 1U;
}

inline void conv_run(const ConvCtx& c, crd::u32 lo, crd::u32 hi)
{
    crd::f32* scratch = c.col_slots;
    if (c.parallel)
    {
        const crd::u32 slot = crd::jobs::worker_index();
        CRD_ASSERT_MSG(slot < kNnConvScratchSlots, "conv_run: worker slot exceeds the scratch bound");
        scratch = c.col_slots + static_cast<crd::u64>(slot) * c.slot_floats;
    }
    tensor_kernels().table->nn_conv2d_images(c.conv, scratch, lo, hi);
}

// ---- linear f32 (direct tier) ----------------------------------------------
// y = x @ Wt + bias with Wt PRE-TRANSPOSED to [K, N] row-major, so the tiny
// GEMM streams both operands (the v14-h register-tiled kernel).
struct LinF32Ctx
{
    const crd::f32* x;    // [M, K]
    const crd::f32* wt;   // [K, N]
    const crd::f32* bias; // [N]
    crd::f32* y;          // [M, N]
    crd::u64 k;
    crd::u64 n;
    bool relu; // fused epilogue (cache-hot; lane-identical to a separate pass)
};

inline void linear_f32_run(const LinF32Ctx& c, crd::u32 lo, crd::u32 hi)
{
    for (crd::u32 m = lo; m < hi; ++m)
    {
        std::memcpy(c.y + static_cast<crd::u64>(m) * c.n, c.bias, c.n * sizeof(crd::f32));
    }
    crd::f32* yblk = c.y + static_cast<crd::u64>(lo) * c.n;
    const crd::f32* xblk = c.x + static_cast<crd::u64>(lo) * c.k;
    const BatchedGemm<crd::f32> g{1.0F, 1.0F, xblk, c.wt, yblk, 0, 0, 0, static_cast<crd::u64>(hi) - lo, c.k, c.n};
    tensor_kernels().table->batched_f32.gemm_tiny(g, 0U, 1U);
    if (c.relu)
    {
        const crd::u64 nblk = (static_cast<crd::u64>(hi) - lo) * c.n;
        relu_span(yblk, yblk, nblk);
    }
}

struct PrefillCtx
{
    const crd::f32* bias;
    crd::f32* y;
    crd::u64 n;
};

inline void prefill_run(const PrefillCtx& c, crd::u32 lo, crd::u32 hi)
{
    for (crd::u32 m = lo; m < hi; ++m)
    {
        std::memcpy(c.y + static_cast<crd::u64>(m) * c.n, c.bias, c.n * sizeof(crd::f32));
    }
}

// ---- the quantized linears and softmax -------------------------------------
// The pinned lane-parallel Q8_0 dot, the int8 dots, the activation quantizers
// and softmax are the tensor kernels (tensor_kernels.hpp,
// kernels/tensor_kernels.cpp): one body per CPU level, every level
// bit-identical (ADR-0134 §7). The runner's int8 linear rides the packed
// kernel (nn_linear_i8_packed_rows) over this load-time layout:
// [ceil(N/8) blocks][K/4 slices][8 outputs x 4 k-bytes].

// pack row-major [n, k] i8 weights (k % 4 == 0) into [n8 * k] bytes; the
// n..n8-1 padding columns are zero (colsum/bias pad likewise).
inline void i8_pack_weights(const crd::i8* wq, crd::u64 n, crd::u64 k, crd::i8* wpack) noexcept
{
    const crd::u64 n8 = (n + 7U) & ~crd::u64{7};
    for (crd::u64 o = 0; o < n8; ++o)
    {
        for (crd::u64 c = 0; c < k; c += 4U)
        {
            for (crd::u64 j = 0; j < 4U; ++j)
            {
                wpack[(o / 8U) * (k * 8U) + (c / 4U) * 32U + (o % 8U) * 4U + j] =
                    o < n ? wq[o * k + c + j] : static_cast<crd::i8>(0);
            }
        }
    }
}

// resolve the effective worker count: explicit request clamped to the live
// pool; no pool (or no request) -> serial. Never oversubscribes.
[[nodiscard]] inline crd::u32 resolve_workers(crd::u32 requested) noexcept
{
    const crd::u32 pool = crd::jobs::num_workers();
    crd::u32 nw = requested;
    if (nw == 0U)
    {
        nw = pool != 0U ? pool : 1U;
    }
    if (nw > pool)
    {
        nw = pool != 0U ? pool : 1U;
    }
    return nw == 0U ? 1U : nw;
}

} // namespace nndetail

// =======================================================================
// Standalone op surface (unit-gated; the runner reuses the same kernels).
// All views must be C-contiguous (NotContiguous otherwise).
// =======================================================================

namespace nndetail
{
template <typename T> [[nodiscard]] inline bool contig(const TensorView<T>& v) noexcept
{
    return v.is_contiguous();
}
} // namespace nndetail

inline void nn_relu(crd::containers::ConstSpan<crd::f32> x, crd::containers::Span<crd::f32> y) noexcept
{
    CRD_ASSERT_MSG(x.size() == y.size(), "nn_relu: size mismatch");
    nndetail::relu_span(x.data(), y.data(), x.size());
}

inline void nn_gelu(crd::containers::ConstSpan<crd::f32> x, crd::containers::Span<crd::f32> y) noexcept
{
    CRD_ASSERT_MSG(x.size() == y.size(), "nn_gelu: size mismatch");
    nndetail::act_span(nndetail::NnAct::Gelu, x.data(), y.data(), x.size());
}

inline void nn_tanh(crd::containers::ConstSpan<crd::f32> x, crd::containers::Span<crd::f32> y) noexcept
{
    CRD_ASSERT_MSG(x.size() == y.size(), "nn_tanh: size mismatch");
    nndetail::act_span(nndetail::NnAct::Tanh, x.data(), y.data(), x.size());
}

inline void nn_sigmoid(crd::containers::ConstSpan<crd::f32> x, crd::containers::Span<crd::f32> y) noexcept
{
    CRD_ASSERT_MSG(x.size() == y.size(), "nn_sigmoid: size mismatch");
    nndetail::act_span(nndetail::NnAct::Sigmoid, x.data(), y.data(), x.size());
}

// softmax over the LAST dim of a contiguous view (any rank >= 1).
[[nodiscard]] inline TensorStatus nn_softmax(TensorView<const crd::f32> x, TensorView<crd::f32> y) noexcept
{
    if (x.rank() == 0U || x.rank() != y.rank())
    {
        return TensorStatus::ShapeMismatch;
    }
    for (crd::u32 dd = 0; dd < x.rank(); ++dd)
    {
        if (x.shape(dd) != y.shape(dd))
        {
            return TensorStatus::ShapeMismatch;
        }
    }
    if (!nndetail::contig(x) || !nndetail::contig(y))
    {
        return TensorStatus::NotContiguous;
    }
    const crd::u64 d = x.shape(x.rank() - 1U);
    if (d == 0U)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 rows = x.size() / d;
    const NnSoftmaxF32 ctx{x.data(), y.data(), d};
    nndetail::nn_parallel(static_cast<crd::u32>(rows), 1U, &ctx, tensor_kernels().table->nn_softmax_rows);
    return TensorStatus::Ok;
}

// layernorm over the last dim of a rank-2 [rows, D] view (torch semantics).
[[nodiscard]] inline TensorStatus nn_layernorm(TensorView<const crd::f32> x,
                                               crd::containers::ConstSpan<crd::f32> gamma,
                                               crd::containers::ConstSpan<crd::f32> beta, crd::f32 eps,
                                               TensorView<crd::f32> y) noexcept
{
    if (x.rank() != 2U || y.rank() != 2U || x.shape(0) != y.shape(0) || x.shape(1) != y.shape(1))
    {
        return TensorStatus::ShapeMismatch;
    }
    if (gamma.size() != x.shape(1) || beta.size() != x.shape(1))
    {
        return TensorStatus::ShapeMismatch;
    }
    if (!nndetail::contig(x) || !nndetail::contig(y))
    {
        return TensorStatus::NotContiguous;
    }
    const NnLayerNormF32 ctx{x.data(), gamma.data(), beta.data(), y.data(), x.shape(1), eps};
    nndetail::nn_parallel(static_cast<crd::u32>(x.shape(0)), 1U, &ctx, tensor_kernels().table->nn_layernorm_rows);
    return TensorStatus::Ok;
}

namespace nndetail
{
[[nodiscard]] inline TensorStatus pool_common(TensorView<const crd::f32> x, crd::u64 k, crd::u64 s,
                                              TensorView<crd::f32> y, bool is_max) noexcept
{
    if (x.rank() != 4U || y.rank() != 4U || k == 0U || s == 0U)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 h = x.shape(2);
    const crd::u64 w = x.shape(3);
    if (h < k || w < k)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 oh = (h - k) / s + 1U;
    const crd::u64 ow = (w - k) / s + 1U;
    if (y.shape(0) != x.shape(0) || y.shape(1) != x.shape(1) || y.shape(2) != oh || y.shape(3) != ow)
    {
        return TensorStatus::ShapeMismatch;
    }
    if (!contig(x) || !contig(y))
    {
        return TensorStatus::NotContiguous;
    }
    const NnPool2d ctx{x.data(), y.data(), h, w, oh, ow, k, s, is_max};
    nn_parallel(static_cast<crd::u32>(x.shape(0) * x.shape(1)), 1U, &ctx, tensor_kernels().table->nn_pool2d_planes);
    return TensorStatus::Ok;
}
} // namespace nndetail

[[nodiscard]] inline TensorStatus nn_maxpool2d(TensorView<const crd::f32> x, crd::u64 k, crd::u64 s,
                                               TensorView<crd::f32> y) noexcept
{
    return nndetail::pool_common(x, k, s, y, true);
}

[[nodiscard]] inline TensorStatus nn_avgpool2d(TensorView<const crd::f32> x, crd::u64 k, crd::u64 s,
                                               TensorView<crd::f32> y) noexcept
{
    return nndetail::pool_common(x, k, s, y, false);
}

// conv2d, stride/pad, x [N,C,H,W], w [OC,C,KH,KW], y [N,OC,OH,OW].
// col_scratch: >= nn_conv2d_scratch_floats(...) floats (serial path).
[[nodiscard]] inline TensorStatus nn_conv2d_f32(TensorView<const crd::f32> x, TensorView<const crd::f32> w,
                                                crd::containers::ConstSpan<crd::f32> bias, crd::u64 pad,
                                                crd::u64 stride, TensorView<crd::f32> y,
                                                crd::containers::Span<crd::f32> col_scratch) noexcept
{
    if (x.rank() != 4U || w.rank() != 4U || y.rank() != 4U || stride == 0U)
    {
        return TensorStatus::BadInput;
    }
    if (!nndetail::contig(x) || !nndetail::contig(w) || !nndetail::contig(y))
    {
        return TensorStatus::NotContiguous;
    }
    const crd::u64 n = x.shape(0);
    const crd::u64 chans = x.shape(1);
    const crd::u64 h = x.shape(2);
    const crd::u64 ww = x.shape(3);
    const crd::u64 oc = w.shape(0);
    const crd::u64 kh = w.shape(2);
    const crd::u64 kw = w.shape(3);
    if (w.shape(1) != chans || bias.size() != oc || kh == 0U || kw == 0U)
    {
        return TensorStatus::BadInput;
    }
    if (h + 2U * pad < kh || ww + 2U * pad < kw)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 oh = (h + 2U * pad - kh) / stride + 1U;
    const crd::u64 ow = (ww + 2U * pad - kw) / stride + 1U;
    if (y.shape(0) != n || y.shape(1) != oc || y.shape(2) != oh || y.shape(3) != ow)
    {
        return TensorStatus::ShapeMismatch;
    }
    const crd::u64 slot = nn_conv2d_scratch_floats(chans, kh, kw, oh, ow);
    if (col_scratch.size() < slot)
    {
        return TensorStatus::BadInput;
    }
    const NnConv2d conv{x.data(), w.data(), bias.data(), y.data(), chans, h,  ww,    kh,
                        kw,       pad,      stride,      oc,       oh,    ow, false, false};
    const nndetail::ConvCtx ctx{conv, col_scratch.data(), slot, false};
    nndetail::nn_parallel(static_cast<crd::u32>(n), 1U, &ctx, nndetail::conv_run);
    return TensorStatus::Ok;
}

// linear f32: y[m][o] = dot(x[m], Wt[:,o]) + bias[o]; wt is the PRE-TRANSPOSED
// weight, [K, N] row-major (K = in features, N = out features).
[[nodiscard]] inline TensorStatus nn_linear_f32(TensorView<const crd::f32> x,
                                                crd::containers::ConstSpan<crd::f32> wt,
                                                crd::containers::ConstSpan<crd::f32> bias,
                                                TensorView<crd::f32> y) noexcept
{
    if (x.rank() != 2U || y.rank() != 2U)
    {
        return TensorStatus::ShapeMismatch;
    }
    const crd::u64 m = x.shape(0);
    const crd::u64 k = x.shape(1);
    const crd::u64 n = y.shape(1);
    if (y.shape(0) != m || wt.size() != k * n || bias.size() != n)
    {
        return TensorStatus::ShapeMismatch;
    }
    if (!nndetail::contig(x) || !nndetail::contig(y))
    {
        return TensorStatus::NotContiguous;
    }
    const nndetail::LinF32Ctx ctx{x.data(), wt.data(), bias.data(), y.data(), k, n, false};
    nndetail::nn_parallel(static_cast<crd::u32>(m), 1U, &ctx, nndetail::linear_f32_run);
    return TensorStatus::Ok;
}

// the Q8_0 linear: wq = [N * (K/32)] blocks (row o's blocks contiguous);
// xq_scratch >= M * (K/32) blocks; scale_scratch >= (N + M) * (K/32) floats
// (the widened weight scales, then the per-row activation scales). K must be
// a multiple of 32.
[[nodiscard]] inline TensorStatus nn_linear_q8(TensorView<const crd::f32> x,
                                               crd::containers::ConstSpan<BlockQ8Zero> wq,
                                               crd::containers::ConstSpan<crd::f32> bias,
                                               TensorView<crd::f32> y,
                                               crd::containers::Span<BlockQ8Zero> xq_scratch,
                                               crd::containers::Span<crd::f32> scale_scratch) noexcept
{
    if (x.rank() != 2U || y.rank() != 2U)
    {
        return TensorStatus::ShapeMismatch;
    }
    const crd::u64 m = x.shape(0);
    const crd::u64 k = x.shape(1);
    const crd::u64 n = y.shape(1);
    if (k == 0U || k % kQuantBlock != 0U)
    {
        return TensorStatus::BadInput;
    }
    const crd::u64 nb = k / kQuantBlock;
    if (y.shape(0) != m || wq.size() != n * nb || bias.size() != n || xq_scratch.size() < m * nb ||
        scale_scratch.size() < (n + m) * nb)
    {
        return TensorStatus::ShapeMismatch;
    }
    if (!nndetail::contig(x) || !nndetail::contig(y))
    {
        return TensorStatus::NotContiguous;
    }
    crd::f32* wsc = scale_scratch.data();
    for (crd::u64 i = 0; i < n * nb; ++i) // widen the weight scales once per call
    {
        wsc[i] = crd::hesap::tensor::f16_bits_to_f32(wq[i].d);
    }
    const NnLinearQ8 ctx{x.data(), wq.data(), wsc, bias.data(), y.data(), xq_scratch.data(), wsc + n * nb, k, n, false};
    nndetail::nn_parallel(static_cast<crd::u32>(m), 1U, &ctx, tensor_kernels().table->nn_linear_q8_rows);
    return TensorStatus::Ok;
}

// the per-tensor int8 linear (the throughput tier): wq = [N, K] symmetric i8
// weights with ONE f32 scale + colsum = [N] per-output weight column sums
// (the zero-point correction, precomputed at load); xq_scratch >= M*K bytes
// (asymmetric u8 rows), xscale_scratch >= M floats, xzp_scratch >= M ints.
// K must be a multiple of 32.
[[nodiscard]] inline TensorStatus nn_linear_i8(TensorView<const crd::f32> x,
                                               crd::containers::ConstSpan<crd::i8> wq, crd::f32 wscale,
                                               crd::containers::ConstSpan<crd::i32> colsum,
                                               crd::containers::ConstSpan<crd::f32> bias,
                                               TensorView<crd::f32> y,
                                               crd::containers::Span<crd::u8> xq_scratch,
                                               crd::containers::Span<crd::f32> xscale_scratch,
                                               crd::containers::Span<crd::i32> xzp_scratch) noexcept
{
    if (x.rank() != 2U || y.rank() != 2U)
    {
        return TensorStatus::ShapeMismatch;
    }
    const crd::u64 m = x.shape(0);
    const crd::u64 k = x.shape(1);
    const crd::u64 n = y.shape(1);
    if (k == 0U || k % kQuantBlock != 0U)
    {
        return TensorStatus::BadInput;
    }
    if (y.shape(0) != m || wq.size() != n * k || colsum.size() != n || bias.size() != n ||
        xq_scratch.size() < m * k || xscale_scratch.size() < m || xzp_scratch.size() < m)
    {
        return TensorStatus::ShapeMismatch;
    }
    if (!nndetail::contig(x) || !nndetail::contig(y))
    {
        return TensorStatus::NotContiguous;
    }
    // the row-major reference path (the unit surface; the NnSequential runner rides the packed kernel, gated
    // bit-identical against this and the scalar formula)
    const NnLinearI8 ctx{x.data(),
                         wq.data(),
                         colsum.data(),
                         bias.data(),
                         y.data(),
                         xq_scratch.data(),
                         xscale_scratch.data(),
                         xzp_scratch.data(),
                         wscale,
                         k,
                         n,
                         false};
    nndetail::nn_parallel(static_cast<crd::u32>(m), 1U, &ctx, tensor_kernels().table->nn_linear_i8_rows);
    return TensorStatus::Ok;
}

// =======================================================================
// NnSequential — the tiny graph runner (fixed op list, caller workspace,
// zero allocations at infer time; weights OWNED).
// =======================================================================

enum class NnOpKind : crd::u8
{
    LinearF32,
    LinearQ8,  // Q8_0 block-32 — the certified / ggml-interop tier (DEFAULT quantized tier)
    LinearI8,  // per-tensor weight scale + per-row activation scale — the throughput tier
    Conv2dF32, // also carries dequantized-Q8/I8 conv weights (the quantized model paths)
    MaxPool2d,
    AvgPool2d,
    Relu,
    Gelu,
    Tanh,
    Sigmoid,
    LayerNorm,
    Softmax,
    Flatten,
};

struct NnOpRec
{
    NnOpKind kind = NnOpKind::Relu;
    crd::u64 dim_in = 0;  // linear: K; layernorm: D
    crd::u64 dim_out = 0; // linear: N
    crd::u64 in_c = 0, out_c = 0, kh = 0, kw = 0, pad = 0, stride = 1; // conv
    crd::u64 pool_k = 0, pool_s = 0;
    crd::f32 eps = 0.0F;
    bool has_q8 = false;
    bool fuse_relu = false; // this op absorbs the relu that follows it (finalize scan)
    bool fuse_pool = false; // conv absorbs the 2x2/s2 maxpool that follows (direct 3x3 only)
    bool fused = false;     // this relu/pool was absorbed — the runner skips it
    // finalized (max_batch in dim 0):
    crd::u64 in_shape[4] = {};
    crd::u64 out_shape[4] = {};
    crd::u32 in_rank = 0;
    crd::u32 out_rank = 0;
    crd::u64 in_elems = 0;  // per sample
    crd::u64 out_elems = 0; // per sample
};

class NnSequential
{
public:
    explicit NnSequential(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc)
    {
        CRD_ASSERT_MSG(alloc != nullptr, "NnSequential: null allocator");
    }

    NnSequential(const NnSequential&) = delete;
    NnSequential& operator=(const NnSequential&) = delete;

    // ---- graph assembly (BUILD time — the only allocating phase) ---------

    // w: [N, K] (torch nn.Linear layout); stored transposed [K, N].
    [[nodiscard]] TensorStatus add_linear_f32(TensorView<const crd::f32> w, TensorView<const crd::f32> b)
    {
        if (w.rank() != 2U || b.rank() != 1U || b.shape(0) != w.shape(0) || !w.is_contiguous() ||
            !b.is_contiguous())
        {
            return TensorStatus::BadInput;
        }
        NnOpRec rec;
        rec.kind = NnOpKind::LinearF32;
        rec.dim_out = w.shape(0);
        rec.dim_in = w.shape(1);
        Tensor<crd::f32> wt(m_alloc);
        const crd::u64 shp[2] = {rec.dim_in, rec.dim_out};
        TensorStatus st = wt.resize({shp, 2});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        for (crd::u64 i = 0; i < rec.dim_in; ++i) // load-time transpose (never at infer)
        {
            for (crd::u64 o = 0; o < rec.dim_out; ++o)
            {
                wt.data()[i * rec.dim_out + o] = w.data()[o * rec.dim_in + i];
            }
        }
        Tensor<crd::f32> bias(m_alloc);
        st = copy_span(b.data(), b.shape(0), bias);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        return push(rec, static_cast<Tensor<crd::f32>&&>(wt), static_cast<Tensor<crd::f32>&&>(bias),
                    Tensor<BlockQ8Zero>{});
    }

    // Q8 linear, quantize-on-load from f32 (the frozen-corpus semantics —
    // see D-v14m-1). K must be a multiple of 32.
    [[nodiscard]] TensorStatus add_linear_q8(TensorView<const crd::f32> w, TensorView<const crd::f32> b)
    {
        if (w.rank() != 2U || b.rank() != 1U || b.shape(0) != w.shape(0) || !w.is_contiguous() ||
            !b.is_contiguous() || w.shape(1) % kQuantBlock != 0U)
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 nblocks = w.shape(0) * w.shape(1) / kQuantBlock;
        Tensor<BlockQ8Zero> q(m_alloc);
        const crd::u64 qshp[1] = {nblocks};
        TensorStatus st = q.resize({qshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        nn_quantize_q8_0_rint({w.data(), static_cast<crd::usize>(w.size())},
                              {q.data(), static_cast<crd::usize>(nblocks)});
        return add_linear_q8_blocks(static_cast<Tensor<BlockQ8Zero>&&>(q), w.shape(0), w.shape(1), b);
    }

    // Q8 linear from the raw frozen refs (.q8 int8 values + .q8s f16 scales).
    [[nodiscard]] TensorStatus add_linear_q8_raw(crd::containers::ConstSpan<crd::i8> qs,
                                                 crd::containers::ConstSpan<crd::u16> scales, crd::u64 out,
                                                 crd::u64 in, TensorView<const crd::f32> b)
    {
        if (in == 0U || in % kQuantBlock != 0U || out == 0U)
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 nblocks = out * in / kQuantBlock;
        Tensor<BlockQ8Zero> q(m_alloc);
        const crd::u64 qshp[1] = {nblocks};
        TensorStatus st = q.resize({qshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        st = nn_q8_blocks_from_raw(qs, scales, {q.data(), static_cast<crd::usize>(nblocks)});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        return add_linear_q8_blocks(static_cast<Tensor<BlockQ8Zero>&&>(q), out, in, b);
    }

    // per-tensor int8 linear (the throughput tier): ONE weight scale,
    // quantize-on-load with the nn_quantize_i8_per_tensor semantics.
    [[nodiscard]] TensorStatus add_linear_i8(TensorView<const crd::f32> w, TensorView<const crd::f32> b)
    {
        if (w.rank() != 2U || b.rank() != 1U || b.shape(0) != w.shape(0) || !w.is_contiguous() ||
            !b.is_contiguous() || w.shape(1) % kQuantBlock != 0U)
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 n = w.shape(0);
        const crd::u64 k = w.shape(1);
        const crd::u64 n8 = (n + 7U) & ~crd::u64{7};
        Tensor<crd::i8> wq(m_alloc);
        const crd::u64 qshp[2] = {n, k};
        TensorStatus st = wq.resize({qshp, 2});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        crd::f32 sw = 0.0F;
        nn_quantize_i8_per_tensor({w.data(), static_cast<crd::usize>(w.size())},
                                  {wq.data(), static_cast<crd::usize>(w.size())}, sw);
        // the runner's PACKED weight layout (i8_pack_weights; padding columns 0)
        Tensor<crd::i8> wpack(m_alloc);
        const crd::u64 pshp[1] = {n8 * k};
        st = wpack.resize({pshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        nndetail::i8_pack_weights(wq.data(), n, k, wpack.data());
        Tensor<crd::f32> wsc(m_alloc);
        const crd::u64 sshp[1] = {1};
        st = wsc.resize({sshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        wsc.data()[0] = sw;
        // per-output weight column sums (zero-point correction), n8-padded
        Tensor<crd::i32> colsum(m_alloc);
        const crd::u64 cshp[1] = {n8};
        st = colsum.resize({cshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        for (crd::u64 o = 0; o < n8; ++o)
        {
            crd::i32 cs = 0;
            for (crd::u64 j = 0; o < n && j < k; ++j)
            {
                cs += static_cast<crd::i32>(wq.data()[o * k + j]);
            }
            colsum.data()[o] = cs;
        }
        // bias, n8-padded (the packed epilogue loads 8-wide)
        Tensor<crd::f32> bias(m_alloc);
        const crd::u64 bshp[1] = {n8};
        st = bias.resize({bshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        std::memcpy(bias.data(), b.data(), static_cast<crd::usize>(n) * sizeof(crd::f32));
        for (crd::u64 o = n; o < n8; ++o)
        {
            bias.data()[o] = 0.0F;
        }
        NnOpRec rec;
        rec.kind = NnOpKind::LinearI8;
        rec.dim_out = n;
        rec.dim_in = k;
        st = push(rec, static_cast<Tensor<crd::f32>&&>(wsc), static_cast<Tensor<crd::f32>&&>(bias),
                  Tensor<BlockQ8Zero>{});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        m_wi8[m_count - 1U] = static_cast<Tensor<crd::i8>&&>(wpack);
        m_wcolsum[m_count - 1U] = static_cast<Tensor<crd::i32>&&>(colsum);
        return TensorStatus::Ok;
    }

    // conv2d for the per-tensor int8 tier: quantize-on-load (one scale for the
    // whole kernel tensor), conv runs on the DEQUANTIZED f32 weights (mirrors
    // the Q8 conv path — 3x3 rows are not vector-dot shaped).
    [[nodiscard]] TensorStatus add_conv2d_i8(TensorView<const crd::f32> w, TensorView<const crd::f32> b,
                                             crd::u64 pad, crd::u64 stride = 1U)
    {
        if (w.rank() != 4U || b.rank() != 1U || b.shape(0) != w.shape(0) || !w.is_contiguous() ||
            !b.is_contiguous() || stride == 0U)
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 numel = w.size();
        Tensor<crd::i8> q(m_alloc);
        const crd::u64 qshp[1] = {numel};
        TensorStatus st = q.resize({qshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        crd::f32 sw = 0.0F;
        nn_quantize_i8_per_tensor({w.data(), static_cast<crd::usize>(numel)},
                                  {q.data(), static_cast<crd::usize>(numel)}, sw);
        Tensor<crd::f32> wf(m_alloc);
        const crd::u64 shp[2] = {w.shape(0), w.shape(1) * w.shape(2) * w.shape(3)};
        st = wf.resize({shp, 2});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        for (crd::u64 i = 0; i < numel; ++i) // dequantize (exact f32 ops, deterministic)
        {
            wf.data()[i] = static_cast<crd::f32>(q.data()[i]) * sw;
        }
        Tensor<crd::f32> bias(m_alloc);
        st = copy_span(b.data(), b.shape(0), bias);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        NnOpRec rec;
        rec.kind = NnOpKind::Conv2dF32;
        rec.out_c = w.shape(0);
        rec.in_c = w.shape(1);
        rec.kh = w.shape(2);
        rec.kw = w.shape(3);
        rec.pad = pad;
        rec.stride = stride;
        st = push(rec, static_cast<Tensor<crd::f32>&&>(wf), static_cast<Tensor<crd::f32>&&>(bias),
                  Tensor<BlockQ8Zero>{});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        m_wi8[m_count - 1U] = static_cast<Tensor<crd::i8>&&>(q);
        return TensorStatus::Ok;
    }

    // conv2d f32: w [OC, C, KH, KW] stored flat [OC, C*KH*KW].
    [[nodiscard]] TensorStatus add_conv2d_f32(TensorView<const crd::f32> w, TensorView<const crd::f32> b,
                                              crd::u64 pad, crd::u64 stride = 1U)
    {
        if (w.rank() != 4U || b.rank() != 1U || b.shape(0) != w.shape(0) || !w.is_contiguous() ||
            !b.is_contiguous() || stride == 0U)
        {
            return TensorStatus::BadInput;
        }
        Tensor<crd::f32> wf(m_alloc);
        const crd::u64 shp[2] = {w.shape(0), w.shape(1) * w.shape(2) * w.shape(3)};
        TensorStatus st = wf.resize({shp, 2});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        std::memcpy(wf.data(), w.data(), static_cast<crd::usize>(w.size()) * sizeof(crd::f32));
        Tensor<crd::f32> bias(m_alloc);
        st = copy_span(b.data(), b.shape(0), bias);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        NnOpRec rec;
        rec.kind = NnOpKind::Conv2dF32;
        rec.out_c = w.shape(0);
        rec.in_c = w.shape(1);
        rec.kh = w.shape(2);
        rec.kw = w.shape(3);
        rec.pad = pad;
        rec.stride = stride;
        return push(rec, static_cast<Tensor<crd::f32>&&>(wf), static_cast<Tensor<crd::f32>&&>(bias),
                    Tensor<BlockQ8Zero>{});
    }

    // conv2d for the QUANTIZED model: quantize-on-load (whole-tensor flatten,
    // zero-padded tail block — the frozen-corpus layout), then run the conv on
    // the DEQUANTIZED f32 weights (3x3 rows are not block-aligned, so the
    // integer block-dot form applies to linears only; documented).
    [[nodiscard]] TensorStatus add_conv2d_q8(TensorView<const crd::f32> w, TensorView<const crd::f32> b,
                                             crd::u64 pad, crd::u64 stride = 1U)
    {
        if (w.rank() != 4U || !w.is_contiguous())
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 numel = w.size();
        const crd::u64 nblocks = (numel + kQuantBlock - 1U) / kQuantBlock;
        Tensor<BlockQ8Zero> q(m_alloc);
        const crd::u64 qshp[1] = {nblocks};
        const TensorStatus st = q.resize({qshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        nn_quantize_q8_0_rint({w.data(), static_cast<crd::usize>(numel)},
                              {q.data(), static_cast<crd::usize>(nblocks)});
        return add_conv2d_from_blocks(static_cast<Tensor<BlockQ8Zero>&&>(q), w.shape(0), w.shape(1),
                                      w.shape(2), w.shape(3), b, pad, stride);
    }

    [[nodiscard]] TensorStatus add_conv2d_q8_raw(crd::containers::ConstSpan<crd::i8> qs,
                                                 crd::containers::ConstSpan<crd::u16> scales, crd::u64 oc,
                                                 crd::u64 chans, crd::u64 kh, crd::u64 kw,
                                                 TensorView<const crd::f32> b, crd::u64 pad,
                                                 crd::u64 stride = 1U)
    {
        const crd::u64 numel = oc * chans * kh * kw;
        const crd::u64 nblocks = (numel + kQuantBlock - 1U) / kQuantBlock;
        Tensor<BlockQ8Zero> q(m_alloc);
        const crd::u64 qshp[1] = {nblocks};
        TensorStatus st = q.resize({qshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        st = nn_q8_blocks_from_raw(qs, scales, {q.data(), static_cast<crd::usize>(nblocks)});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        return add_conv2d_from_blocks(static_cast<Tensor<BlockQ8Zero>&&>(q), oc, chans, kh, kw, b, pad,
                                      stride);
    }

    [[nodiscard]] TensorStatus add_relu() { return push_simple(NnOpKind::Relu); }
    [[nodiscard]] TensorStatus add_gelu() { return push_simple(NnOpKind::Gelu); }
    [[nodiscard]] TensorStatus add_tanh() { return push_simple(NnOpKind::Tanh); }
    [[nodiscard]] TensorStatus add_sigmoid() { return push_simple(NnOpKind::Sigmoid); }
    [[nodiscard]] TensorStatus add_softmax() { return push_simple(NnOpKind::Softmax); }
    [[nodiscard]] TensorStatus add_flatten() { return push_simple(NnOpKind::Flatten); }

    [[nodiscard]] TensorStatus add_maxpool2d(crd::u64 k, crd::u64 s)
    {
        return push_pool(NnOpKind::MaxPool2d, k, s);
    }
    [[nodiscard]] TensorStatus add_avgpool2d(crd::u64 k, crd::u64 s)
    {
        return push_pool(NnOpKind::AvgPool2d, k, s);
    }

    [[nodiscard]] TensorStatus add_layernorm(TensorView<const crd::f32> gamma, TensorView<const crd::f32> beta,
                                             crd::f32 eps = kNnLayerNormEps)
    {
        if (gamma.rank() != 1U || beta.rank() != 1U || gamma.shape(0) != beta.shape(0) ||
            !gamma.is_contiguous() || !beta.is_contiguous())
        {
            return TensorStatus::BadInput;
        }
        Tensor<crd::f32> g(m_alloc);
        TensorStatus st = copy_span(gamma.data(), gamma.shape(0), g);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        Tensor<crd::f32> bt(m_alloc);
        st = copy_span(beta.data(), beta.shape(0), bt);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        NnOpRec rec;
        rec.kind = NnOpKind::LayerNorm;
        rec.dim_in = gamma.shape(0);
        rec.eps = eps;
        return push(rec, static_cast<Tensor<crd::f32>&&>(g), static_cast<Tensor<crd::f32>&&>(bt),
                    Tensor<BlockQ8Zero>{});
    }

    // ---- finalize: shape propagation + the workspace formula ---------------
    // input_shape carries max_batch in dim 0 (rank 2 for MLPs, 4 for CNNs).
    [[nodiscard]] TensorStatus finalize(crd::containers::ConstSpan<crd::u64> input_shape) noexcept
    {
        if (m_finalized || m_count == 0U || (input_shape.size() != 2U && input_shape.size() != 4U) ||
            input_shape[0] == 0U)
        {
            return TensorStatus::BadInput;
        }
        // relu fusion: a linear/conv absorbs an immediately-following relu
        // (identical lane semantics applied cache-hot inside the same task)
        for (crd::u32 i = 0; i + 1U < m_count; ++i)
        {
            NnOpRec& producer = m_ops[i];
            if ((producer.kind == NnOpKind::LinearF32 || producer.kind == NnOpKind::LinearQ8 ||
                 producer.kind == NnOpKind::LinearI8 || producer.kind == NnOpKind::Conv2dF32) &&
                m_ops[i + 1U].kind == NnOpKind::Relu)
            {
                producer.fuse_relu = true;
                m_ops[i + 1U].fused = true;
            }
        }
        m_in_rank = static_cast<crd::u32>(input_shape.size());
        for (crd::u32 dd = 0; dd < m_in_rank; ++dd)
        {
            m_in_shape[dd] = input_shape[dd];
        }
        crd::u64 cur[4] = {};
        crd::u32 cur_rank = m_in_rank;
        for (crd::u32 dd = 0; dd < m_in_rank; ++dd)
        {
            cur[dd] = m_in_shape[dd];
        }
        const crd::u64 max_batch = cur[0];
        m_buf_elems = 0;
        m_scratch_bytes = 0;
        for (crd::u32 i = 0; i < m_count; ++i)
        {
            NnOpRec& op = m_ops[i];
            op.in_rank = cur_rank;
            for (crd::u32 dd = 0; dd < cur_rank; ++dd)
            {
                op.in_shape[dd] = cur[dd];
            }
            // maxpool fusion: a direct-3x3 conv absorbs the 2x2/s2 maxpool that
            // follows it (looking past already-fused relus) — needs the live
            // shape (row-buffer bound), so it is decided here in the walk
            if (op.kind == NnOpKind::Conv2dF32 && cur_rank == 4U &&
                nndetail::conv_is_3x3s1p1(op.kh, op.kw, op.pad, op.stride))
            {
                crd::u32 j = i + 1U;
                while (j < m_count && m_ops[j].fused)
                {
                    ++j;
                }
                if (j < m_count && m_ops[j].kind == NnOpKind::MaxPool2d && m_ops[j].pool_k == 2U &&
                    m_ops[j].pool_s == 2U && cur[2] >= 2U && cur[3] >= 2U)
                {
                    op.fuse_pool = true;
                    m_ops[j].fused = true;
                }
            }
            const TensorStatus st = op.fused ? TensorStatus::Ok : shape_op(op, cur, cur_rank);
            if (st != TensorStatus::Ok)
            {
                return st;
            }
            op.out_rank = cur_rank;
            crd::u64 total = 1;
            for (crd::u32 dd = 0; dd < cur_rank; ++dd)
            {
                op.out_shape[dd] = cur[dd];
                total *= cur[dd];
            }
            op.out_elems = total / max_batch;
            crd::u64 in_total = 1;
            for (crd::u32 dd = 0; dd < op.in_rank; ++dd)
            {
                in_total *= op.in_shape[dd];
            }
            op.in_elems = in_total / max_batch;
            if (total > m_buf_elems)
            {
                m_buf_elems = total;
            }
            m_scratch_bytes = m_scratch_bytes > op_scratch_bytes(op, max_batch)
                                  ? m_scratch_bytes
                                  : op_scratch_bytes(op, max_batch);
        }
        m_finalized = true;
        return TensorStatus::Ok;
    }

    // The documented workspace formula (header comment): base slack + two
    // ping/pong activation buffers + the op scratch region.
    [[nodiscard]] crd::u64 workspace_bytes() const noexcept
    {
        CRD_ASSERT_MSG(m_finalized, "NnSequential::workspace_bytes: finalize() first");
        return 64U + 2U * nn_align64(m_buf_elems * sizeof(crd::f32)) + nn_align64(m_scratch_bytes);
    }

    // ---- inference: ZERO allocations, bit-identical at any worker count ----
    [[nodiscard]] TensorStatus infer(TensorView<const crd::f32> x, TensorView<crd::f32> y,
                                     crd::containers::Span<crd::u8> workspace,
                                     crd::u32 num_workers = 0U) const noexcept
    {
        if (!m_finalized)
        {
            return TensorStatus::BadInput;
        }
        if (x.rank() != m_in_rank || x.shape(0) == 0U || x.shape(0) > m_in_shape[0])
        {
            return TensorStatus::ShapeMismatch;
        }
        for (crd::u32 dd = 1; dd < m_in_rank; ++dd)
        {
            if (x.shape(dd) != m_in_shape[dd])
            {
                return TensorStatus::ShapeMismatch;
            }
        }
        const crd::u64 batch = x.shape(0);
        const NnOpRec& last = m_ops[m_count - 1U];
        if (y.rank() != last.out_rank || y.shape(0) != batch)
        {
            return TensorStatus::ShapeMismatch;
        }
        for (crd::u32 dd = 1; dd < last.out_rank; ++dd)
        {
            if (y.shape(dd) != last.out_shape[dd])
            {
                return TensorStatus::ShapeMismatch;
            }
        }
        if (!x.is_contiguous() || !y.is_contiguous())
        {
            return TensorStatus::NotContiguous;
        }
        if (workspace.size() < workspace_bytes())
        {
            return TensorStatus::BadInput;
        }
        const crd::u32 nw = nndetail::resolve_workers(num_workers);
        // region layout (fixed offsets — the formula above)
        crd::u8* base = workspace.data();
        const crd::u64 mis = reinterpret_cast<crd::u64>(base) & 63U;
        base += mis == 0U ? 0U : 64U - mis;
        const crd::u64 buf_bytes = nn_align64(m_buf_elems * sizeof(crd::f32));
        crd::f32* buf0 = reinterpret_cast<crd::f32*>(base);
        crd::f32* buf1 = reinterpret_cast<crd::f32*>(base + buf_bytes);
        crd::u8* scratch = base + 2U * buf_bytes;

        // the last op that WRITES (flatten is shape-only, fused relus are
        // absorbed by their producer) targets y directly
        crd::u32 last_write = m_count;
        for (crd::u32 i = m_count; i-- > 0U;)
        {
            if (m_ops[i].kind != NnOpKind::Flatten && !m_ops[i].fused)
            {
                last_write = i;
                break;
            }
        }
        if (last_write == m_count)
        {
            return TensorStatus::BadInput; // flatten-only graphs are not a model
        }

        const crd::f32* cur = x.data();
        for (crd::u32 i = 0; i < m_count; ++i)
        {
            const NnOpRec& op = m_ops[i];
            if (op.kind == NnOpKind::Flatten || op.fused)
            {
                continue; // shape-only / absorbed into the producing op
            }
            crd::f32* const spare = cur == buf0 ? buf1 : buf0; // the ping-pong buffer not being read
            crd::f32* dst = i == last_write ? y.data() : spare;
            run_op(op, cur, dst, scratch, batch, nw);
            cur = dst;
        }
        return TensorStatus::Ok;
    }

    // ---- introspection (gates) ---------------------------------------------
    [[nodiscard]] crd::u32 op_count() const noexcept { return m_count; }
    [[nodiscard]] const NnOpRec& op(crd::u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_count, "NnSequential::op: index out of range");
        return m_ops[i];
    }
    [[nodiscard]] crd::containers::ConstSpan<BlockQ8Zero> q8_blocks(crd::u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_count, "NnSequential::q8_blocks: index out of range");
        return {m_q[i].data(), static_cast<crd::usize>(m_q[i].size())};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::i8> i8_weights(crd::u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_count, "NnSequential::i8_weights: index out of range");
        return {m_wi8[i].data(), static_cast<crd::usize>(m_wi8[i].size())};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::f32> weight_f32(crd::u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_count, "NnSequential::weight_f32: index out of range");
        return {m_w[i].data(), static_cast<crd::usize>(m_w[i].size())};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::f32> bias_f32(crd::u32 i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_count, "NnSequential::bias_f32: index out of range");
        return {m_b[i].data(), static_cast<crd::usize>(m_b[i].size())};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u64> output_shape() const noexcept
    {
        CRD_ASSERT_MSG(m_finalized, "NnSequential::output_shape: finalize() first");
        const NnOpRec& last = m_ops[m_count - 1U];
        return {last.out_shape, last.out_rank};
    }

private:
    [[nodiscard]] TensorStatus copy_span(const crd::f32* src, crd::u64 n, Tensor<crd::f32>& out)
    {
        Tensor<crd::f32> t(m_alloc);
        const crd::u64 shp[1] = {n};
        const TensorStatus st = t.resize({shp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        std::memcpy(t.data(), src, static_cast<crd::usize>(n) * sizeof(crd::f32));
        out = static_cast<Tensor<crd::f32>&&>(t);
        return TensorStatus::Ok;
    }

    [[nodiscard]] TensorStatus push(const NnOpRec& rec, Tensor<crd::f32>&& w, Tensor<crd::f32>&& b,
                                    Tensor<BlockQ8Zero>&& q)
    {
        if (m_count >= kNnMaxOps)
        {
            return TensorStatus::BadInput;
        }
        if (m_finalized)
        {
            return TensorStatus::BadInput; // append-then-finalize, never mutate a finalized graph
        }
        m_ops[m_count] = rec;
        m_w[m_count] = std::move(w);
        m_b[m_count] = std::move(b);
        m_q[m_count] = std::move(q);
        ++m_count;
        return TensorStatus::Ok;
    }

    [[nodiscard]] TensorStatus push_simple(NnOpKind kind)
    {
        NnOpRec rec;
        rec.kind = kind;
        return push(rec, Tensor<crd::f32>{}, Tensor<crd::f32>{}, Tensor<BlockQ8Zero>{});
    }

    [[nodiscard]] TensorStatus push_pool(NnOpKind kind, crd::u64 k, crd::u64 s)
    {
        if (k == 0U || s == 0U)
        {
            return TensorStatus::BadInput;
        }
        NnOpRec rec;
        rec.kind = kind;
        rec.pool_k = k;
        rec.pool_s = s;
        return push(rec, Tensor<crd::f32>{}, Tensor<crd::f32>{}, Tensor<BlockQ8Zero>{});
    }

    [[nodiscard]] TensorStatus add_linear_q8_blocks(Tensor<BlockQ8Zero>&& q, crd::u64 out, crd::u64 in,
                                                    TensorView<const crd::f32> b)
    {
        if (b.rank() != 1U || b.shape(0) != out || !b.is_contiguous())
        {
            return TensorStatus::BadInput;
        }
        Tensor<crd::f32> bias(m_alloc);
        TensorStatus st = copy_span(b.data(), b.shape(0), bias);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        // pre-widen the f16 block scales ONCE (never inside the o loop at infer)
        Tensor<crd::f32> wscale(m_alloc);
        const crd::u64 sshp[1] = {q.size()};
        st = wscale.resize({sshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        for (crd::u64 i = 0; i < q.size(); ++i)
        {
            wscale.data()[i] = f16_bits_to_f32(q.data()[i].d);
        }
        NnOpRec rec;
        rec.kind = NnOpKind::LinearQ8;
        rec.dim_out = out;
        rec.dim_in = in;
        rec.has_q8 = true;
        return push(rec, static_cast<Tensor<crd::f32>&&>(wscale), static_cast<Tensor<crd::f32>&&>(bias), std::move(q));
    }

    [[nodiscard]] TensorStatus add_conv2d_from_blocks(Tensor<BlockQ8Zero>&& q, crd::u64 oc, crd::u64 chans,
                                                      crd::u64 kh, crd::u64 kw, TensorView<const crd::f32> b,
                                                      crd::u64 pad, crd::u64 stride)
    {
        if (b.rank() != 1U || b.shape(0) != oc || !b.is_contiguous() || stride == 0U || kh == 0U || kw == 0U)
        {
            return TensorStatus::BadInput;
        }
        const crd::u64 numel = oc * chans * kh * kw;
        const crd::u64 nblocks = q.size();
        // dequantize (dtypes.hpp — the pinned ggml arithmetic) into the owned f32 weight
        Tensor<crd::f32> padded(m_alloc);
        const crd::u64 pshp[1] = {nblocks * kQuantBlock};
        TensorStatus st = padded.resize({pshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        dequantize_q8_0({q.data(), static_cast<crd::usize>(nblocks)},
                        {padded.data(), static_cast<crd::usize>(nblocks * kQuantBlock)});
        Tensor<crd::f32> wf(m_alloc);
        const crd::u64 shp[2] = {oc, chans * kh * kw};
        st = wf.resize({shp, 2});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        std::memcpy(wf.data(), padded.data(), static_cast<crd::usize>(numel) * sizeof(crd::f32));
        Tensor<crd::f32> bias(m_alloc);
        st = copy_span(b.data(), b.shape(0), bias);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        NnOpRec rec;
        rec.kind = NnOpKind::Conv2dF32;
        rec.out_c = oc;
        rec.in_c = chans;
        rec.kh = kh;
        rec.kw = kw;
        rec.pad = pad;
        rec.stride = stride;
        rec.has_q8 = true;
        return push(rec, static_cast<Tensor<crd::f32>&&>(wf), static_cast<Tensor<crd::f32>&&>(bias), std::move(q));
    }

    [[nodiscard]] static TensorStatus shape_op(const NnOpRec& op, crd::u64 (&cur)[4],
                                               crd::u32& cur_rank) noexcept
    {
        switch (op.kind)
        {
        case NnOpKind::LinearF32:
        case NnOpKind::LinearQ8:
        case NnOpKind::LinearI8:
            if (cur_rank != 2U || cur[1] != op.dim_in)
            {
                return TensorStatus::ShapeMismatch;
            }
            cur[1] = op.dim_out;
            return TensorStatus::Ok;
        case NnOpKind::Conv2dF32:
        {
            if (cur_rank != 4U || cur[1] != op.in_c)
            {
                return TensorStatus::ShapeMismatch;
            }
            if (cur[2] + 2U * op.pad < op.kh || cur[3] + 2U * op.pad < op.kw)
            {
                return TensorStatus::BadInput;
            }
            cur[1] = op.out_c;
            cur[2] = (cur[2] + 2U * op.pad - op.kh) / op.stride + 1U;
            cur[3] = (cur[3] + 2U * op.pad - op.kw) / op.stride + 1U;
            if (op.fuse_pool) // the absorbed 2x2/s2 maxpool
            {
                cur[2] = (cur[2] - 2U) / 2U + 1U;
                cur[3] = (cur[3] - 2U) / 2U + 1U;
            }
            return TensorStatus::Ok;
        }
        case NnOpKind::MaxPool2d:
        case NnOpKind::AvgPool2d:
            if (cur_rank != 4U)
            {
                return TensorStatus::ShapeMismatch;
            }
            if (cur[2] < op.pool_k || cur[3] < op.pool_k)
            {
                return TensorStatus::BadInput;
            }
            cur[2] = (cur[2] - op.pool_k) / op.pool_s + 1U;
            cur[3] = (cur[3] - op.pool_k) / op.pool_s + 1U;
            return TensorStatus::Ok;
        case NnOpKind::LayerNorm:
            if (cur_rank != 2U || cur[1] != op.dim_in)
            {
                return TensorStatus::ShapeMismatch;
            }
            return TensorStatus::Ok;
        case NnOpKind::Flatten:
            if (cur_rank == 2U)
            {
                return TensorStatus::Ok; // no-op on [B, D]
            }
            if (cur_rank != 4U)
            {
                return TensorStatus::ShapeMismatch;
            }
            cur[1] = cur[1] * cur[2] * cur[3];
            cur_rank = 2U;
            return TensorStatus::Ok;
        case NnOpKind::Relu:
        case NnOpKind::Gelu:
        case NnOpKind::Tanh:
        case NnOpKind::Sigmoid:
            return TensorStatus::Ok;
        case NnOpKind::Softmax:
            if (cur_rank != 2U)
            {
                return TensorStatus::ShapeMismatch; // the runner softmax is row-wise [B, D]
            }
            return TensorStatus::Ok;
        }
        return TensorStatus::BadInput;
    }

    [[nodiscard]] static crd::u64 op_scratch_bytes(const NnOpRec& op, crd::u64 max_batch) noexcept
    {
        switch (op.kind)
        {
        case NnOpKind::Conv2dF32:
        {
            // CONV output dims (with a fused pool, out_shape carries the POOLED dims;
            // the fused path is 3x3/s1/p1 where conv oh/ow == the input h/w)
            const crd::u64 oh = op.fuse_pool ? op.in_shape[2] : op.out_shape[2];
            const crd::u64 ow = op.fuse_pool ? op.in_shape[3] : op.out_shape[3];
            const crd::u64 slot =
                nn_align64(nn_conv2d_scratch_floats(op.in_c, op.kh, op.kw, oh, ow) * sizeof(crd::f32));
            return static_cast<crd::u64>(kNnConvScratchSlots) * slot;
        }
        case NnOpKind::LinearQ8:
        {
            const crd::u64 nb = op.dim_in / kQuantBlock;
            return nn_align64(max_batch * nb * sizeof(BlockQ8Zero)) +
                   nn_align64(max_batch * nb * sizeof(crd::f32));
        }
        case NnOpKind::LinearI8:
            return nn_align64(max_batch * op.dim_in) + nn_align64(max_batch * sizeof(crd::f32)) +
                   nn_align64(max_batch * sizeof(crd::i32));
        case NnOpKind::LinearF32:
        {
            const crd::u64 flops = 2U * max_batch * op.dim_in * op.dim_out;
            return flops >= kNnDenseLinearFlops ? kNnGemmScratchBytes : 0U;
        }
        default:
            return 0U;
        }
    }

    void run_op(const NnOpRec& op, const crd::f32* in, crd::f32* out, crd::u8* scratch, crd::u64 batch,
                crd::u32 nw) const noexcept
    {
        switch (op.kind)
        {
        case NnOpKind::LinearF32:
        {
            const crd::u64 flops = 2U * batch * op.dim_in * op.dim_out;
            if (flops >= kNnDenseLinearFlops)
            {
                run_linear_dense(op, in, out, scratch, batch, nw);
                return;
            }
            const nndetail::LinF32Ctx ctx{in,        weight_ptr(op), bias_ptr(op), out,
                                          op.dim_in, op.dim_out,     op.fuse_relu};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, nndetail::linear_f32_run);
            return;
        }
        case NnOpKind::LinearQ8:
        {
            // scratch layout (fixed offsets — max_batch): [xq blocks][x scales]
            const crd::u64 nb = op.dim_in / kQuantBlock;
            const crd::u64 xq_bytes = nn_align64(m_in_shape[0] * nb * sizeof(BlockQ8Zero));
            const NnLinearQ8 ctx{in,
                                 q8_ptr(op),
                                 weight_ptr(op), // the pre-widened f32 block scales
                                 bias_ptr(op),
                                 out,
                                 reinterpret_cast<BlockQ8Zero*>(scratch),
                                 reinterpret_cast<crd::f32*>(scratch + xq_bytes),
                                 op.dim_in,
                                 op.dim_out,
                                 op.fuse_relu};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, tensor_kernels().table->nn_linear_q8_rows);
            return;
        }
        case NnOpKind::LinearI8:
        {
            // scratch layout (fixed offsets — max_batch): [xq rows][row scales][row zero points]
            const crd::u64 xq_bytes = nn_align64(m_in_shape[0] * op.dim_in);
            const crd::u64 xsc_bytes = nn_align64(m_in_shape[0] * sizeof(crd::f32));
            const NnLinearI8 ctx{in,
                                 m_wi8[op_index(op)].data(),
                                 m_wcolsum[op_index(op)].data(),
                                 bias_ptr(op),
                                 out,
                                 scratch, // u8 activation rows
                                 reinterpret_cast<crd::f32*>(scratch + xq_bytes),
                                 reinterpret_cast<crd::i32*>(scratch + xq_bytes + xsc_bytes),
                                 weight_ptr(op)[0], // the ONE weight scale
                                 op.dim_in,
                                 op.dim_out,
                                 op.fuse_relu};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx,
                                  tensor_kernels().table->nn_linear_i8_packed_rows);
            return;
        }
        case NnOpKind::Conv2dF32:
        {
            const crd::u64 oh = op.fuse_pool ? op.in_shape[2] : op.out_shape[2]; // CONV dims
            const crd::u64 ow = op.fuse_pool ? op.in_shape[3] : op.out_shape[3];
            const crd::u64 slot_floats =
                nn_align64(nn_conv2d_scratch_floats(op.in_c, op.kh, op.kw, oh, ow) * sizeof(crd::f32)) /
                sizeof(crd::f32);
            // bounded per-worker slots: fall back to serial if the live pool
            // exceeds the reserved slot count (moat pools are <= 16). batch >= 2
            // mirrors nn_parallel's serial short-circuit so the slot logic and
            // the execution mode can never disagree.
            const bool parallel =
                nw > 1U && batch >= 2U && crd::jobs::num_workers() <= kNnConvScratchSlots;
            const NnConv2d conv{in,
                                weight_ptr(op),
                                bias_ptr(op),
                                out,
                                op.in_c,
                                op.in_shape[2],
                                op.in_shape[3],
                                op.kh,
                                op.kw,
                                op.pad,
                                op.stride,
                                op.out_c,
                                oh,
                                ow,
                                op.fuse_relu,
                                op.fuse_pool};
            const nndetail::ConvCtx ctx{conv, reinterpret_cast<crd::f32*>(scratch), slot_floats, parallel};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), parallel ? nw : 1U, &ctx,
                                  nndetail::conv_run);
            return;
        }
        case NnOpKind::MaxPool2d:
        case NnOpKind::AvgPool2d:
        {
            const NnPool2d ctx{in,
                               out,
                               op.in_shape[2],
                               op.in_shape[3],
                               op.out_shape[2],
                               op.out_shape[3],
                               op.pool_k,
                               op.pool_s,
                               op.kind == NnOpKind::MaxPool2d};
            nndetail::nn_parallel(static_cast<crd::u32>(batch * op.in_shape[1]), nw, &ctx,
                                  tensor_kernels().table->nn_pool2d_planes);
            return;
        }
        case NnOpKind::Relu:
        case NnOpKind::Gelu:
        case NnOpKind::Tanh:
        case NnOpKind::Sigmoid:
        {
            nndetail::NnAct act = nndetail::NnAct::Relu;
            if (op.kind == NnOpKind::Gelu)
            {
                act = nndetail::NnAct::Gelu;
            }
            else if (op.kind == NnOpKind::Tanh)
            {
                act = nndetail::NnAct::Tanh;
            }
            else if (op.kind == NnOpKind::Sigmoid)
            {
                act = nndetail::NnAct::Sigmoid;
            }
            const nndetail::EwCtx ctx{in, out, op.in_elems, act};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, nndetail::ew_run);
            return;
        }
        case NnOpKind::LayerNorm:
        {
            const NnLayerNormF32 ctx{in, weight_ptr(op), bias_ptr(op), out, op.dim_in, op.eps};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, tensor_kernels().table->nn_layernorm_rows);
            return;
        }
        case NnOpKind::Softmax:
        {
            const NnSoftmaxF32 ctx{in, out, op.in_shape[op.in_rank - 1U]};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, tensor_kernels().table->nn_softmax_rows);
            return;
        }
        case NnOpKind::Flatten:
            return; // handled by the runner (shape-only)
        }
    }

    // dense tier: hesap-dense GEMM (the engine's frontier sgemm) with pack
    // buffers from a LinearAllocator over the workspace scratch — zero heap.
    void run_linear_dense(const NnOpRec& op, const crd::f32* in, crd::f32* out, crd::u8* scratch,
                          crd::u64 batch, crd::u32 nw) const noexcept
    {
        const nndetail::PrefillCtx pc{bias_ptr(op), out, op.dim_out};
        nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &pc, nndetail::prefill_run);
        // (the fused relu, when present, runs as an in-place pass after the GEMM below)
        crd::memory::LinearAllocator arena(scratch, static_cast<crd::usize>(kNnGemmScratchBytes),
                                           "NnGemmArena");
        crd::memory::ThreadSafeAllocator ts(&arena, "NnGemmArenaTs");
        using L = crd::hesap::dense::Layout;
        using MVC = crd::hesap::dense::MatrixView<const crd::f32, L::RowMajor>;
        using MV = crd::hesap::dense::MatrixView<crd::f32, L::RowMajor>;
        const MVC a{in, static_cast<crd::usize>(batch), static_cast<crd::usize>(op.dim_in),
                    static_cast<crd::usize>(op.dim_in)};
        const MVC b{weight_ptr(op), static_cast<crd::usize>(op.dim_in), static_cast<crd::usize>(op.dim_out),
                    static_cast<crd::usize>(op.dim_out)};
        MV c{out, static_cast<crd::usize>(batch), static_cast<crd::usize>(op.dim_out),
             static_cast<crd::usize>(op.dim_out)};
        if (nw > 1U)
        {
            crd::hesap::dense::gemm_parallel<crd::f32, L::RowMajor>(nw, 1.0F, a, b, 1.0F, c,
                                                                    crd::hesap::dense::Trans::None,
                                                                    crd::hesap::dense::Trans::None, &ts);
        }
        else
        {
            crd::hesap::dense::gemm<crd::f32, L::RowMajor>(1.0F, a, b, 1.0F, c,
                                                           crd::hesap::dense::Trans::None,
                                                           crd::hesap::dense::Trans::None, &ts);
        }
        if (op.fuse_relu)
        {
            const nndetail::EwCtx rc{out, out, op.dim_out, nndetail::NnAct::Relu};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &rc, nndetail::ew_run);
        }
    }

    [[nodiscard]] const crd::f32* weight_ptr(const NnOpRec& op) const noexcept
    {
        return m_w[op_index(op)].data();
    }
    [[nodiscard]] const crd::f32* bias_ptr(const NnOpRec& op) const noexcept
    {
        return m_b[op_index(op)].data();
    }
    [[nodiscard]] const BlockQ8Zero* q8_ptr(const NnOpRec& op) const noexcept
    {
        return m_q[op_index(op)].data();
    }
    [[nodiscard]] crd::u32 op_index(const NnOpRec& op) const noexcept
    {
        const auto idx = static_cast<crd::u32>(&op - m_ops);
        CRD_ASSERT_MSG(idx < m_count, "NnSequential: foreign op record");
        return idx;
    }

    crd::memory::IAllocator* m_alloc = nullptr;
    NnOpRec m_ops[kNnMaxOps];
    Tensor<crd::f32> m_w[kNnMaxOps];
    Tensor<crd::f32> m_b[kNnMaxOps];
    Tensor<BlockQ8Zero> m_q[kNnMaxOps];
    Tensor<crd::i8> m_wi8[kNnMaxOps];      // per-tensor int8 weights (the throughput tier)
    Tensor<crd::i32> m_wcolsum[kNnMaxOps]; // per-output weight column sums (zero-point correction)
    crd::u32 m_count = 0;
    bool m_finalized = false;
    crd::u64 m_in_shape[4] = {};
    crd::u32 m_in_rank = 0;
    crd::u64 m_buf_elems = 0;
    crd::u64 m_scratch_bytes = 0;
};

// =======================================================================
// Model builders — safetensors -> NnSequential (weights COPIED to owned
// storage; the file's zero-copy payload spans are call-scoped only).
// =======================================================================

namespace nndetail
{

[[nodiscard]] inline TensorStatus st_read_f32(const SafetensorsFile& f, crd::memory::IAllocator* alloc,
                                              crd::containers::StringView name, Tensor<crd::f32>& out) noexcept
{
    const crd::i64 idx = f.find(name);
    if (idx < 0)
    {
        return TensorStatus::BadInput;
    }
    return f.read<crd::f32>(static_cast<crd::usize>(idx), alloc, out);
}

} // namespace nndetail

// The quantization tier of a built network. Block-32 Q8_0 is the DEFAULT
// quantized tier (byte-interop with the frozen refs / ggml — the certified
// tier); per-tensor int8 is the max-throughput tier (per-tensor weight scale,
// per-row dynamic activation scale — the fbgemm/MatMulInteger scheme class).
enum class NnQuantTier : crd::u8
{
    F32,
    Q8Block32,
    I8PerTensor,
};

// The frozen tiny-MLP: fc1(64->128) relu fc2(128->32) relu layernorm(32)
// fc3(32->10) softmax. The tier selects every linear's kernel
// (quantize-on-load; Q8Block32 = D-v14m-1 semantics).
[[nodiscard]] inline TensorStatus build_mlp_from_safetensors(crd::memory::IAllocator* alloc,
                                                             const SafetensorsFile& f, crd::u64 max_batch,
                                                             NnQuantTier tier, NnSequential& net) noexcept
{
    if (net.op_count() != 0U)
    {
        return TensorStatus::BadInput;
    }
    Tensor<crd::f32> w1(alloc);
    Tensor<crd::f32> b1(alloc);
    Tensor<crd::f32> w2(alloc);
    Tensor<crd::f32> b2(alloc);
    Tensor<crd::f32> w3(alloc);
    Tensor<crd::f32> b3(alloc);
    Tensor<crd::f32> lng(alloc);
    Tensor<crd::f32> lnb(alloc);
    TensorStatus st = nndetail::st_read_f32(f, alloc, "fc1.weight", w1);
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "fc1.bias", b1) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "fc2.weight", w2) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "fc2.bias", b2) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "fc3.weight", w3) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "fc3.bias", b3) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "ln.weight", lng) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "ln.bias", lnb) : st;
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    const auto add_lin = [&](const Tensor<crd::f32>& w, const Tensor<crd::f32>& b) -> TensorStatus
    {
        switch (tier)
        {
        case NnQuantTier::F32:
            return net.add_linear_f32(w.view(), b.view());
        case NnQuantTier::Q8Block32:
            return net.add_linear_q8(w.view(), b.view());
        case NnQuantTier::I8PerTensor:
            return net.add_linear_i8(w.view(), b.view());
        }
        return TensorStatus::BadInput;
    };
    st = add_lin(w1, b1);
    st = st == TensorStatus::Ok ? net.add_relu() : st;
    st = st == TensorStatus::Ok ? add_lin(w2, b2) : st;
    st = st == TensorStatus::Ok ? net.add_relu() : st;
    st = st == TensorStatus::Ok ? net.add_layernorm(lng.view(), lnb.view()) : st;
    st = st == TensorStatus::Ok ? add_lin(w3, b3) : st;
    st = st == TensorStatus::Ok ? net.add_softmax() : st;
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    const crd::u64 shp[2] = {max_batch, w1.shape(1)};
    return net.finalize({shp, 2});
}

// bool back-compat overload: quantized -> the DEFAULT quantized tier (Q8_0).
[[nodiscard]] inline TensorStatus build_mlp_from_safetensors(crd::memory::IAllocator* alloc,
                                                             const SafetensorsFile& f, crd::u64 max_batch,
                                                             bool quantized, NnSequential& net) noexcept
{
    return build_mlp_from_safetensors(alloc, f, max_batch,
                                      quantized ? NnQuantTier::Q8Block32 : NnQuantTier::F32, net);
}

// The frozen tiny-CNN: conv(1->8,3x3,pad1) relu maxpool2 conv(8->16,3x3,pad1)
// relu maxpool2 flatten fc(256->10) softmax. Quantized tiers: convs run on
// dequantized (Q8_0 / per-tensor-i8) weights, the fc rides the integer linear.
[[nodiscard]] inline TensorStatus build_cnn_from_safetensors(crd::memory::IAllocator* alloc,
                                                             const SafetensorsFile& f, crd::u64 max_batch,
                                                             NnQuantTier tier, NnSequential& net) noexcept
{
    if (net.op_count() != 0U)
    {
        return TensorStatus::BadInput;
    }
    Tensor<crd::f32> c1w(alloc);
    Tensor<crd::f32> c1b(alloc);
    Tensor<crd::f32> c2w(alloc);
    Tensor<crd::f32> c2b(alloc);
    Tensor<crd::f32> fcw(alloc);
    Tensor<crd::f32> fcb(alloc);
    TensorStatus st = nndetail::st_read_f32(f, alloc, "c1.weight", c1w);
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "c1.bias", c1b) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "c2.weight", c2w) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "c2.bias", c2b) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "fc.weight", fcw) : st;
    st = st == TensorStatus::Ok ? nndetail::st_read_f32(f, alloc, "fc.bias", fcb) : st;
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    const auto add_conv = [&](const Tensor<crd::f32>& w, const Tensor<crd::f32>& b) -> TensorStatus
    {
        switch (tier)
        {
        case NnQuantTier::F32:
            return net.add_conv2d_f32(w.view(), b.view(), 1U);
        case NnQuantTier::Q8Block32:
            return net.add_conv2d_q8(w.view(), b.view(), 1U);
        case NnQuantTier::I8PerTensor:
            return net.add_conv2d_i8(w.view(), b.view(), 1U);
        }
        return TensorStatus::BadInput;
    };
    st = add_conv(c1w, c1b);
    st = st == TensorStatus::Ok ? net.add_relu() : st;
    st = st == TensorStatus::Ok ? net.add_maxpool2d(2U, 2U) : st;
    st = st == TensorStatus::Ok ? add_conv(c2w, c2b) : st;
    st = st == TensorStatus::Ok ? net.add_relu() : st;
    st = st == TensorStatus::Ok ? net.add_maxpool2d(2U, 2U) : st;
    st = st == TensorStatus::Ok ? net.add_flatten() : st;
    if (st == TensorStatus::Ok)
    {
        switch (tier)
        {
        case NnQuantTier::F32:
            st = net.add_linear_f32(fcw.view(), fcb.view());
            break;
        case NnQuantTier::Q8Block32:
            st = net.add_linear_q8(fcw.view(), fcb.view());
            break;
        case NnQuantTier::I8PerTensor:
            st = net.add_linear_i8(fcw.view(), fcb.view());
            break;
        }
    }
    st = st == TensorStatus::Ok ? net.add_softmax() : st;
    if (st != TensorStatus::Ok)
    {
        return st;
    }
    const crd::u64 shp[4] = {max_batch, c1w.shape(1), 16U, 16U};
    return net.finalize({shp, 4});
}

// bool back-compat overload: quantized -> the DEFAULT quantized tier (Q8_0).
[[nodiscard]] inline TensorStatus build_cnn_from_safetensors(crd::memory::IAllocator* alloc,
                                                             const SafetensorsFile& f, crd::u64 max_batch,
                                                             bool quantized, NnSequential& net) noexcept
{
    return build_cnn_from_safetensors(alloc, f, max_batch,
                                      quantized ? NnQuantTier::Q8Block32 : NnQuantTier::F32, net);
}

} // namespace crd::hesap::tensor
