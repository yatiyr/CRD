#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — v14-m: the NN inference pack (the certified tiny-ML demo).
//
// Ops: conv2d (im2col over the engine's OWN dense GEMM — the allocation-free
// v14-h direct tiny-tier kernel per im2col matrix, SANITY #8 reuse), max/avg
// pooling, activations (relu / exact erf-gelu / tanh / sigmoid — ALL
// transcendentals via crd::math, never std::), layernorm (torch semantics:
// biased variance, eps inside the sqrt), numerically-stable softmax
// (max-subtract; exp = the crd_exp1/crd_exp4 deterministic core), linear f32
// (x @ W^T + b over a load-time-transposed weight so the GEMM streams
// row-major), and the QUANTIZED Q8_0 linear (ggml block-32 layout from
// dtypes.hpp): int8 x int8 -> EXACT i32 lane partials per block, combined by
// a fixed per-lane fma with the f16-scale-pair product and a fixed horizontal
// tree — the INTEGER stage is bit-exact on every backend (the certification
// tier) and the whole sequence is a pinned op order (see q8_dot_tile).
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
//       Q8 linear: align64(max_batch * (K/32) * sizeof(BlockQ8_0))
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

#include "batched.hpp" // batcheddetail::direct_gemm_tiny — the v14-h allocation-free tiny GEMM (+ jobs + simd)
#include "dtypes.hpp"  // BlockQ8_0 + f16 converts + quantize_q8_0 (activations)
#include "io.hpp"      // SafetensorsFile — the v14-l reader consumed by the builders
#include "tensor.hpp"

#include <crd/math/cmath.hpp>         // crd::math::exp / tanh / sqrt / nearbyint — the deterministic surface
#include <crd/math/deterministic.hpp> // crd::math::deterministic::erf — the exact (erf-form) gelu
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

#include <cmath> // std::fma — single-rounded accumulation (the codelet bit contract)
#include <cstring>

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
                                  crd::containers::Span<BlockQ8_0> out) noexcept
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

// Assemble BlockQ8_0 storage from the raw ref streams (.q8 = int8 values,
// .q8s = f16 scale bits) — the frozen-reference weight-load path.
[[nodiscard]] inline TensorStatus nn_q8_blocks_from_raw(crd::containers::ConstSpan<crd::i8> qs,
                                                        crd::containers::ConstSpan<crd::u16> scales,
                                                        crd::containers::Span<BlockQ8_0> out) noexcept
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

// =======================================================================
// Per-tensor symmetric i8 quantization — the THROUGHPUT tier's scalar
// reference semantics (ONE f32 scale = amax/127, roundf-style half-away
// rounding, +-127 clamp; NO f16 round-trip). Used for weight quantization
// at load; the vectorized per-row twin (nndetail::quantize_row_i8) is
// gate-enforced bit-identical.
// HOME-> crd/hesap/tensor/dtypes.hpp: this quantizer pair belongs beside
// quantize_q8_0 when dtypes is next touched.
// =======================================================================
inline void nn_quantize_i8_per_tensor(crd::containers::ConstSpan<crd::f32> x,
                                      crd::containers::Span<crd::i8> out, crd::f32& scale) noexcept
{
    CRD_ASSERT_MSG(x.size() == out.size(), "nn_quantize_i8_per_tensor: size mismatch");
    crd::f32 amax = 0.0F;
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        const crd::f32 a = x[i] < 0.0F ? -x[i] : x[i];
        amax = a > amax ? a : amax;
    }
    const crd::f32 s = amax / 127.0F;
    const crd::f32 inv = s != 0.0F ? 1.0F / s : 0.0F;
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        const crd::f32 t = x[i] * inv;
        const crd::f32 r = t >= 0.0F ? t + 0.5F : t - 0.5F; // roundf half-away (the dtypes form)
        const crd::f32 c = crd::math::clamp(r, -127.0F, 127.0F);
        out[i] = static_cast<crd::i8>(static_cast<crd::i32>(c));
    }
    scale = s;
}

// Per-tensor ASYMMETRIC u8 quantization — the throughput tier's ACTIVATION
// reference semantics (the ort DynamicQuantizeLinear convention): the range
// is 0-extended (rmin = min(0, min x), rmax = max(0, max x)), one f32 scale
// (rmax - rmin)/255 + an integer zero point (RNE of -rmin/s, clamped to
// [0, 255]); values round half-away, add the zero point, clamp to [0, 255].
// The vectorized per-row twin (nndetail::quantize_row_u8) is gate-enforced
// bit-identical.
inline void nn_quantize_u8_asym(crd::containers::ConstSpan<crd::f32> x, crd::containers::Span<crd::u8> out,
                                crd::f32& scale, crd::i32& zero_point) noexcept
{
    CRD_ASSERT_MSG(x.size() == out.size(), "nn_quantize_u8_asym: size mismatch");
    crd::f32 rmin = 0.0F;
    crd::f32 rmax = 0.0F;
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        rmin = x[i] < rmin ? x[i] : rmin;
        rmax = x[i] > rmax ? x[i] : rmax;
    }
    const crd::f32 s = (rmax - rmin) / 255.0F;
    const crd::f32 inv = s != 0.0F ? 1.0F / s : 0.0F;
    const crd::f32 zp_f = s != 0.0F ? crd::math::clamp(-rmin / s, 0.0F, 255.0F) : 0.0F;
    const crd::i32 zp = static_cast<crd::i32>(crd::math::nearbyint(zp_f));
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        const crd::f32 t = x[i] * inv;
        const crd::f32 r = t >= 0.0F ? t + 0.5F : t - 0.5F; // half-away (the house quantizer form)
        const crd::i32 q = static_cast<crd::i32>(r) + zp;
        out[i] = static_cast<crd::u8>(crd::math::clamp(q, 0, 255));
    }
    scale = s;
    zero_point = zp;
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

// relu: vector max(x, +0) with a scalar tail — lane-for-lane bit-identical
// (maxps returns the SECOND operand on equal/NaN, matching `x > 0 ? x : 0`).
inline void relu_span(const crd::f32* x, crd::f32* y, crd::u64 n) noexcept
{
    namespace simd = crd::math::simd;
    const simd::Vec8f zero(0.0F);
    crd::u64 i = 0;
    for (; i + 8U <= n; i += 8U)
    {
        simd::max(simd::Vec8f::load(x + i), zero).store(y + i);
    }
    for (; i < n; ++i)
    {
        y[i] = x[i] > 0.0F ? x[i] : 0.0F;
    }
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

// ---- softmax (last dim, stable) --------------------------------------------
// exp rides the deterministic crd_exp1 core; the AVX2 4-lane widen path uses
// crd_exp4 which is BIT-IDENTICAL to crd_exp1 on every input, so the
// vector/tail split cannot perturb results.
inline void softmax_row(const crd::f32* x, crd::f32* y, crd::u64 d) noexcept
{
    crd::f32 m = x[0];
    for (crd::u64 j = 1; j < d; ++j)
    {
        m = x[j] > m ? x[j] : m;
    }
    crd::u64 j = 0;
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    for (; j + 4U <= d; j += 4U)
    {
        const __m128 xf = _mm_sub_ps(_mm_loadu_ps(x + j), _mm_set1_ps(m));
        const __m128 ef = _mm256_cvtpd_ps(crd::math::crd_exp4(_mm256_cvtps_pd(xf)));
        _mm_storeu_ps(y + j, ef);
    }
#endif
    for (; j < d; ++j)
    {
        y[j] = static_cast<crd::f32>(crd::math::exp(static_cast<crd::f64>(x[j] - m)));
    }
    crd::f32 s = 0.0F;
    for (crd::u64 t = 0; t < d; ++t)
    {
        s += y[t];
    }
    for (crd::u64 t = 0; t < d; ++t)
    {
        y[t] /= s;
    }
}

struct SoftmaxCtx
{
    const crd::f32* x;
    crd::f32* y;
    crd::u64 d;
};

inline void softmax_run(const SoftmaxCtx& c, crd::u32 lo, crd::u32 hi)
{
    for (crd::u32 r = lo; r < hi; ++r)
    {
        softmax_row(c.x + static_cast<crd::u64>(r) * c.d, c.y + static_cast<crd::u64>(r) * c.d, c.d);
    }
}

// ---- layernorm (torch semantics: biased variance, affine) ------------------
inline void layernorm_row(const crd::f32* x, const crd::f32* g, const crd::f32* b, crd::f32 eps, crd::f32* y,
                          crd::u64 d) noexcept
{
    crd::f32 mean = 0.0F;
    for (crd::u64 jj = 0; jj < d; ++jj)
    {
        mean += x[jj];
    }
    mean /= static_cast<crd::f32>(d);
    crd::f32 var = 0.0F;
    for (crd::u64 jj = 0; jj < d; ++jj)
    {
        const crd::f32 t = x[jj] - mean;
        var = std::fma(t, t, var);
    }
    var /= static_cast<crd::f32>(d);
    const crd::f32 rstd = 1.0F / crd::math::sqrt(var + eps);
    for (crd::u64 jj = 0; jj < d; ++jj)
    {
        y[jj] = std::fma((x[jj] - mean) * rstd, g[jj], b[jj]);
    }
}

struct LayerNormCtx
{
    const crd::f32* x;
    const crd::f32* g;
    const crd::f32* b;
    crd::f32* y;
    crd::u64 d;
    crd::f32 eps;
};

inline void layernorm_run(const LayerNormCtx& c, crd::u32 lo, crd::u32 hi)
{
    for (crd::u32 r = lo; r < hi; ++r)
    {
        layernorm_row(c.x + static_cast<crd::u64>(r) * c.d, c.g, c.b, c.eps,
                      c.y + static_cast<crd::u64>(r) * c.d, c.d);
    }
}

// ---- pooling ---------------------------------------------------------------
// scalar ternary max / sum chains per window (the dtypes.hpp amax idiom —
// no conditional two-array updates near vector code, SANITY 2026-07-05).
struct PoolCtx
{
    const crd::f32* x;
    crd::f32* y;
    crd::u64 h;
    crd::u64 w;
    crd::u64 oh;
    crd::u64 ow;
    crd::u64 k;
    crd::u64 s;
    bool is_max;
};

inline void pool_run(const PoolCtx& c, crd::u32 lo, crd::u32 hi)
{
    namespace simd = crd::math::simd;
    const crd::f32 inv = 1.0F / static_cast<crd::f32>(c.k * c.k);
    if (c.is_max && c.k == 2U && c.s == 2U && c.w <= 256U) // the CNN hot shape, vectorized
    {
        for (crd::u32 p = lo; p < hi; ++p)
        {
            const crd::f32* xp = c.x + static_cast<crd::u64>(p) * c.h * c.w;
            crd::f32* yp = c.y + static_cast<crd::u64>(p) * c.oh * c.ow;
            crd::f32 vm[256];
            for (crd::u64 oi = 0; oi < c.oh; ++oi)
            {
                const crd::f32* r0 = xp + (2U * oi) * c.w;
                const crd::f32* r1 = r0 + c.w;
                crd::u64 j = 0;
                for (; j + 8U <= c.w; j += 8U) // vertical max, 8 lanes at a time
                {
                    simd::max(simd::Vec8f::load(r0 + j), simd::Vec8f::load(r1 + j)).store(vm + j);
                }
                for (; j < c.w; ++j)
                {
                    vm[j] = r0[j] > r1[j] ? r0[j] : r1[j];
                }
                crd::f32* yrow = yp + oi * c.ow;
                for (crd::u64 oj = 0; oj < c.ow; ++oj) // horizontal pair max
                {
                    const crd::f32 a = vm[2U * oj];
                    const crd::f32 b = vm[2U * oj + 1U];
                    yrow[oj] = a > b ? a : b;
                }
            }
        }
        return;
    }
    for (crd::u32 p = lo; p < hi; ++p) // p = plane index (sample*channels + channel)
    {
        const crd::f32* xp = c.x + static_cast<crd::u64>(p) * c.h * c.w;
        crd::f32* yp = c.y + static_cast<crd::u64>(p) * c.oh * c.ow;
        for (crd::u64 oi = 0; oi < c.oh; ++oi)
        {
            for (crd::u64 oj = 0; oj < c.ow; ++oj)
            {
                const crd::f32* win = xp + oi * c.s * c.w + oj * c.s;
                if (c.is_max)
                {
                    crd::f32 m = win[0];
                    for (crd::u64 ki = 0; ki < c.k; ++ki)
                    {
                        for (crd::u64 kj = 0; kj < c.k; ++kj)
                        {
                            const crd::f32 v = win[ki * c.w + kj];
                            m = v > m ? v : m;
                        }
                    }
                    yp[oi * c.ow + oj] = m;
                }
                else
                {
                    crd::f32 acc = 0.0F;
                    for (crd::u64 ki = 0; ki < c.k; ++ki)
                    {
                        for (crd::u64 kj = 0; kj < c.k; ++kj)
                        {
                            acc += win[ki * c.w + kj];
                        }
                    }
                    yp[oi * c.ow + oj] = acc * inv;
                }
            }
        }
    }
}

// ---- conv2d: im2col + the engine's own dense GEMM ---------------------------
// One column matrix per image: col[(c*KH+ki)*KW+kj][oh*OW+ow]; stride-1 rows
// fill via memset edges + memcpy middle (the hot corpus path), generic
// strides via a branchless bounds ternary.
inline void im2col_one(const crd::f32* x, crd::u64 chans, crd::u64 h, crd::u64 w, crd::u64 kh, crd::u64 kw,
                       crd::u64 pad, crd::u64 stride, crd::u64 oh, crd::u64 ow, crd::f32* col) noexcept
{
    const crd::u64 ohw = oh * ow;
    crd::u64 r = 0;
    for (crd::u64 ci = 0; ci < chans; ++ci)
    {
        const crd::f32* plane = x + ci * h * w;
        for (crd::u64 ki = 0; ki < kh; ++ki)
        {
            for (crd::u64 kj = 0; kj < kw; ++kj)
            {
                crd::f32* dst = col + r * ohw;
                ++r;
                for (crd::u64 oi = 0; oi < oh; ++oi)
                {
                    const crd::i64 ih =
                        static_cast<crd::i64>(oi * stride + ki) - static_cast<crd::i64>(pad);
                    crd::f32* drow = dst + oi * ow;
                    if (ih < 0 || ih >= static_cast<crd::i64>(h))
                    {
                        std::memset(drow, 0, ow * sizeof(crd::f32));
                        continue;
                    }
                    const crd::f32* srow = plane + static_cast<crd::u64>(ih) * w;
                    if (stride == 1U)
                    {
                        const crd::i64 shift = static_cast<crd::i64>(kj) - static_cast<crd::i64>(pad);
                        const crd::u64 valid_lo = shift < 0 ? static_cast<crd::u64>(-shift) : 0U;
                        const crd::i64 hi_signed = static_cast<crd::i64>(w) - shift;
                        crd::u64 valid_hi = hi_signed < 0 ? 0U : static_cast<crd::u64>(hi_signed);
                        valid_hi = valid_hi > ow ? ow : valid_hi;
                        if (valid_lo > 0U)
                        {
                            std::memset(drow, 0, valid_lo * sizeof(crd::f32));
                        }
                        if (valid_hi > valid_lo)
                        {
                            std::memcpy(drow + valid_lo, srow + static_cast<crd::i64>(valid_lo) + shift,
                                        (valid_hi - valid_lo) * sizeof(crd::f32));
                        }
                        if (valid_hi < ow)
                        {
                            std::memset(drow + valid_hi, 0, (ow - valid_hi) * sizeof(crd::f32));
                        }
                    }
                    else
                    {
                        for (crd::u64 oj = 0; oj < ow; ++oj)
                        {
                            const crd::i64 iw =
                                static_cast<crd::i64>(oj * stride + kj) - static_cast<crd::i64>(pad);
                            drow[oj] =
                                (iw >= 0 && iw < static_cast<crd::i64>(w)) ? srow[iw] : 0.0F;
                        }
                    }
                }
            }
        }
    }
}

struct ConvCtx
{
    const crd::f32* x;   // [N, C, H, W]
    const crd::f32* w;   // [OC, C*KH*KW] row-major flat
    const crd::f32* bias; // [OC]
    crd::f32* y;         // [N, OC, OH, OW]
    crd::f32* col_slots; // slot_floats per worker slot
    crd::u64 slot_floats;
    crd::u64 chans, h, w_in, kh, kw, pad, stride, oc, oh, ow; // oh/ow = CONV output dims
    bool parallel; // slot = jobs::worker_index() when true, slot 0 otherwise
    bool relu;     // fused epilogue
    bool pool;     // fused 2x2/s2 maxpool (direct 3x3 path only) — y carries POOLED planes
};

// ---- the direct 3x3 / stride-1 / pad-1 kernel (the CNN hot shape) ----------
// Same per-element fma CHAIN as im2col+GEMM — the accumulation order over
// (c, ki, kj) and the single-rounded bias add are identical, vector lanes are
// output columns — so the conv bit gate holds across both paths. Input rows
// are staged once per image into a zero-padded buffer (left pad + right slack)
// so every kj shift is a plain 8-lane load; OOB rows read a shared zero row
// (the fma with 0 executes — never skipped — preserving the chain).
// One conv output row for R output channels into dst + r*dst_stride (+ j0).
template <crd::u32 R>
inline void conv3x3_rowblock(const crd::f32* rows, const crd::f32* zrow, crd::u64 chans, crd::u64 h,
                             crd::u64 w, crd::u64 rs, const crd::f32* wblk, const crd::f32* bias,
                             crd::f32* dst, crd::u64 dst_stride, crd::u64 oi, bool relu) noexcept
{
    namespace simd = crd::math::simd;
    using V = simd::Vec8f;
    const V zero(0.0F);
    for (crd::u64 j0 = 0; j0 < w; j0 += 8U)
    {
        const crd::u64 cw = w - j0 < 8U ? w - j0 : 8U;
        V acc[R];
        for (crd::u32 r = 0; r < R; ++r)
        {
            acc[r] = V(0.0F);
        }
        for (crd::u64 ci = 0; ci < chans; ++ci)
        {
            for (crd::u64 ki = 0; ki < 3U; ++ki)
            {
                const crd::i64 ih = static_cast<crd::i64>(oi + ki) - 1;
                const bool oob = ih < 0 || ih >= static_cast<crd::i64>(h);
                const crd::f32* srow = oob ? zrow : rows + (ci * h + static_cast<crd::u64>(ih)) * rs;
                for (crd::u64 kj = 0; kj < 3U; ++kj)
                {
                    const V xv = V::load(srow + j0 + kj); // staged: full loads are always in-bounds
                    const crd::u64 wo = (ci * 3U + ki) * 3U + kj;
                    for (crd::u32 r = 0; r < R; ++r)
                    {
                        acc[r] = simd::fma(V(wblk[r * chans * 9U + wo]), xv, acc[r]);
                    }
                }
            }
        }
        for (crd::u32 r = 0; r < R; ++r)
        {
            V out = V(bias[r]) + acc[r]; // fl(bias + chain) — the GEMM's beta=1 form
            if (relu)
            {
                out = simd::max(out, zero);
            }
            crd::f32* d = dst + static_cast<crd::u64>(r) * dst_stride + j0;
            if (cw == 8U)
            {
                out.store(d);
            }
            else
            {
                out.store_partial(d, static_cast<crd::usize>(cw));
            }
        }
    }
}

// Dispatch over the residual oc-block size (the batched.hpp switch pattern).
inline void conv3x3_rowblock_any(crd::u32 r, const crd::f32* rows, const crd::f32* zrow, crd::u64 chans,
                                 crd::u64 h, crd::u64 w, crd::u64 rs, const crd::f32* wblk,
                                 const crd::f32* bias, crd::f32* dst, crd::u64 dst_stride, crd::u64 oi,
                                 bool relu) noexcept
{
    switch (r)
    {
    case 8U:
        conv3x3_rowblock<8U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    case 7U:
        conv3x3_rowblock<7U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    case 6U:
        conv3x3_rowblock<6U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    case 5U:
        conv3x3_rowblock<5U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    case 4U:
        conv3x3_rowblock<4U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    case 3U:
        conv3x3_rowblock<3U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    case 2U:
        conv3x3_rowblock<2U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    case 1U:
        conv3x3_rowblock<1U>(rows, zrow, chans, h, w, rs, wblk, bias, dst, dst_stride, oi, relu);
        return;
    default:
        return;
    }
}

// fuse_pool: the conv(+relu) rows are pooled 2x2/s2 in-register-file and only
// the POOLED rows hit memory — the CNN pipeline's DRAM-traffic killer (the
// unfused conv output never exists as a buffer). Pooling of identical values
// is exact (max), so the fused pipeline is bit-identical to conv->relu->pool.
inline void conv3x3_image(const crd::f32* xi, crd::u64 chans, crd::u64 h, crd::u64 w, const crd::f32* wflat,
                          const crd::f32* bias, crd::f32* yi, crd::u64 oc, crd::f32* stage, bool relu,
                          bool fuse_pool) noexcept
{
    const crd::u64 rs = w + 10U; // 1 left pad + w + right zero slack for full 8-lane loads
    crd::f32* zrow = stage;
    crd::f32* rows = stage + rs;
    std::memset(zrow, 0, rs * sizeof(crd::f32));
    for (crd::u64 ci = 0; ci < chans; ++ci)
    {
        for (crd::u64 ih = 0; ih < h; ++ih)
        {
            crd::f32* dst = rows + (ci * h + ih) * rs;
            dst[0] = 0.0F;
            std::memcpy(dst + 1, xi + (ci * h + ih) * w, w * sizeof(crd::f32));
            std::memset(dst + 1 + w, 0, 9U * sizeof(crd::f32));
        }
    }
    if (!fuse_pool)
    {
        for (crd::u64 oi = 0; oi < h; ++oi)
        {
            for (crd::u64 o0 = 0; o0 < oc; o0 += 8U)
            {
                const crd::u32 r = oc - o0 < 8U ? static_cast<crd::u32>(oc - o0) : 8U;
                conv3x3_rowblock_any(r, rows, zrow, chans, h, w, rs, wflat + o0 * chans * 9U, bias + o0,
                                     yi + o0 * h * w + oi * w, h * w, oi, relu);
            }
        }
        return;
    }
    // fused 2x2/s2 maxpool: two conv rows land in the scratch row buffer, the
    // pooled row goes to memory. rowbuf = 16 rows of rs floats after staging.
    const crd::u64 oh2 = (h - 2U) / 2U + 1U;
    const crd::u64 ow2 = (w - 2U) / 2U + 1U;
    crd::f32* rowbuf = rows + chans * h * rs;
    for (crd::u64 oi2 = 0; oi2 < oh2; ++oi2)
    {
        for (crd::u64 o0 = 0; o0 < oc; o0 += 8U)
        {
            const crd::u32 r = oc - o0 < 8U ? static_cast<crd::u32>(oc - o0) : 8U;
            crd::f32* rb0 = rowbuf;            // conv row 2*oi2, R rows of rs
            crd::f32* rb1 = rowbuf + 8U * rs;  // conv row 2*oi2+1
            conv3x3_rowblock_any(r, rows, zrow, chans, h, w, rs, wflat + o0 * chans * 9U, bias + o0, rb0,
                                 rs, 2U * oi2, relu);
            conv3x3_rowblock_any(r, rows, zrow, chans, h, w, rs, wflat + o0 * chans * 9U, bias + o0, rb1,
                                 rs, 2U * oi2 + 1U, relu);
            for (crd::u32 rr = 0; rr < r; ++rr)
            {
                const crd::f32* a = rb0 + static_cast<crd::u64>(rr) * rs;
                const crd::f32* b = rb1 + static_cast<crd::u64>(rr) * rs;
                crd::f32* yrow = yi + (o0 + rr) * oh2 * ow2 + oi2 * ow2;
                for (crd::u64 oj = 0; oj < ow2; ++oj)
                {
                    const crd::f32 v0 = a[2U * oj] > b[2U * oj] ? a[2U * oj] : b[2U * oj];
                    const crd::f32 v1 =
                        a[2U * oj + 1U] > b[2U * oj + 1U] ? a[2U * oj + 1U] : b[2U * oj + 1U];
                    yrow[oj] = v0 > v1 ? v0 : v1;
                }
            }
        }
    }
}

[[nodiscard]] inline bool conv_is_3x3s1p1(crd::u64 kh, crd::u64 kw, crd::u64 pad, crd::u64 stride) noexcept
{
    return kh == 3U && kw == 3U && pad == 1U && stride == 1U;
}

inline void conv_run(const ConvCtx& c, crd::u32 lo, crd::u32 hi)
{
    namespace simd = crd::math::simd;
    const crd::u64 ckk = c.chans * c.kh * c.kw;
    const crd::u64 ohw = c.oh * c.ow;
    crd::f32* scratch = c.col_slots;
    if (c.parallel)
    {
        const crd::u32 slot = crd::jobs::worker_index();
        CRD_ASSERT_MSG(slot < kNnConvScratchSlots, "conv_run: worker slot exceeds the scratch bound");
        scratch = c.col_slots + static_cast<crd::u64>(slot) * c.slot_floats;
    }
    const bool direct3x3 = conv_is_3x3s1p1(c.kh, c.kw, c.pad, c.stride);
    const crd::u64 yohw = c.pool ? ((c.oh - 2U) / 2U + 1U) * ((c.ow - 2U) / 2U + 1U) : ohw;
    for (crd::u32 img = lo; img < hi; ++img)
    {
        const crd::f32* xi = c.x + static_cast<crd::u64>(img) * c.chans * c.h * c.w_in;
        crd::f32* yi = c.y + static_cast<crd::u64>(img) * c.oc * yohw;
        if (direct3x3)
        {
            conv3x3_image(xi, c.chans, c.h, c.w_in, c.w, c.bias, yi, c.oc, scratch, c.relu, c.pool);
            continue;
        }
        im2col_one(xi, c.chans, c.h, c.w_in, c.kh, c.kw, c.pad, c.stride, c.oh, c.ow, scratch);
        for (crd::u64 o = 0; o < c.oc; ++o) // bias prefill (beta = 1 in the GEMM)
        {
            const simd::Vec8f bv(c.bias[o]);
            crd::f32* row = yi + o * ohw;
            crd::u64 p = 0;
            for (; p + 8U <= ohw; p += 8U)
            {
                bv.store(row + p);
            }
            if (p < ohw)
            {
                bv.store_partial(row + p, static_cast<crd::usize>(ohw - p));
            }
        }
        batcheddetail::direct_gemm_tiny(1.0F, c.w, scratch, 1.0F, yi, c.oc, ckk, ohw);
        if (c.relu)
        {
            relu_span(yi, yi, c.oc * ohw);
        }
    }
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
    batcheddetail::direct_gemm_tiny(1.0F, c.x + static_cast<crd::u64>(lo) * c.k, c.wt, 1.0F, yblk,
                                    static_cast<crd::u64>(hi) - lo, c.k, c.n);
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

// ---- the Q8_0 linear --------------------------------------------------------
// The lane-parallel Q8_0 kernel (PINNED semantics, both backends identical):
// each block's int8 x int8 products land in EIGHT exact i32 lane partials
// (lane k = products 4k..4k+3 — the maddubs/madd pair structure; every lane
// value is exact integer arithmetic, |lane| <= 64516 so the f32 convert is
// also exact). The per-block f32 combine is one fma per lane
// (acc_k = fma(dx*dw, f32(lane_k), acc_k)), and the per-output horizontal sum
// is the fixed hadd tree ((l0+l1)+(l2+l3)) + ((l4+l5)+(l6+l7)) — one 4-output
// hadd reduction per tile. Everything is a fixed operation sequence ->
// bit-exact across worker counts, runs, and backends; the INTEGER stage is
// bit-exact across all hardware (the certification tier). The scalar fallback
// mirrors the lane structure and the sum tree exactly.
inline constexpr crd::u32 kNnQ8OutTile = 8U; // outputs sharing one activation-block load
                                             // (8 independent acc chains hide the fma latency)

// y[o0..o0+T) outputs over nb blocks. T is a COMPILE-TIME tile size so the
// inner loop is fully unrolled straight-line (a runtime bound costs 2-3 loop
// micro-ops per 3-instruction iteration — measured as the issue-width wall).
template <crd::u32 T>
inline void q8_dot_tile(const BlockQ8_0* xrow, const crd::f32* xsc, const BlockQ8_0* wq,
                        const crd::f32* wscale, crd::u64 nb, crd::u64 o0, const crd::f32* bias,
                        crd::f32* yrow, bool relu) noexcept
{
    constexpr crd::u32 t = T;
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    __m256 acc[kNnQ8OutTile];
    for (crd::u32 r = 0; r < kNnQ8OutTile; ++r)
    {
        acc[r] = _mm256_setzero_ps();
    }
#if !defined(__AVXVNNI__)
    const __m256i ones16 = _mm256_set1_epi16(1);
#endif
    for (crd::u64 bi = 0; bi < nb; ++bi)
    {
        const __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow[bi].qs));
        const __m256i ax = _mm256_sign_epi8(vx, vx); // |x| (u8 <= 127), shared by the tile
        const __m256 vdx = _mm256_set1_ps(xsc[bi]);
        for (crd::u32 r = 0; r < t; ++r)
        {
            const BlockQ8_0* wrow = wq + (o0 + r) * nb;
            const __m256i vw = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wrow[bi].qs));
            const __m256i sw = _mm256_sign_epi8(vw, vx); // w * sign(x)
#if defined(__AVXVNNI__)
            // dpbusd accumulates the same 4-product i32 lanes exactly (no
            // intermediate saturation is reachable at |q| <= 127) — the
            // integer lane values are BIT-IDENTICAL to the maddubs/madd pair
            const __m256i p32 = _mm256_dpbusd_avx_epi32(_mm256_setzero_si256(), ax, sw);
#else
            const __m256i p16 = _mm256_maddubs_epi16(ax, sw); // pairs -> i16 (no saturation reachable)
            const __m256i p32 = _mm256_madd_epi16(p16, ones16); // 4-product i32 lanes
#endif
            const __m256 scale = _mm256_mul_ps(vdx, _mm256_broadcast_ss(wscale + (o0 + r) * nb + bi));
            acc[r] = _mm256_fmadd_ps(scale, _mm256_cvtepi32_ps(p32), acc[r]);
        }
    }
    // 4-output horizontal reductions (the pinned hadd tree, unchanged per
    // output — outputs group by (index mod 4) exactly as the 4-wide tile did)
    for (crd::u32 g = 0; g < t; g += 4U)
    {
        const crd::u32 tg = t - g < 4U ? t - g : 4U;
        const __m256 h0 = _mm256_hadd_ps(acc[g], acc[g + 1U]);
        const __m256 h1 = _mm256_hadd_ps(acc[g + 2U], acc[g + 3U]);
        const __m256 hh = _mm256_hadd_ps(h0, h1);
        const __m128 r4 = _mm_add_ps(_mm256_castps256_ps128(hh), _mm256_extractf128_ps(hh, 1));
        if (tg == 4U)
        {
            __m128 v = _mm_add_ps(r4, _mm_loadu_ps(bias + o0 + g));
            if (relu)
            {
                v = _mm_max_ps(v, _mm_setzero_ps()); // lane semantics == the ternary form
            }
            _mm_storeu_ps(yrow + o0 + g, v);
            continue;
        }
        alignas(16) crd::f32 d4[4];
        _mm_store_ps(d4, r4);
        for (crd::u32 r = 0; r < tg; ++r)
        {
            const crd::f32 v = d4[r] + bias[o0 + g + r];
            yrow[o0 + g + r] = relu ? (v > 0.0F ? v : 0.0F) : v;
        }
    }
#else
    for (crd::u32 r = 0; r < t; ++r)
    {
        crd::f32 accl[8] = {};
        const BlockQ8_0* wrow = wq + (o0 + r) * nb;
        for (crd::u64 bi = 0; bi < nb; ++bi)
        {
            const crd::f32 scale = xsc[bi] * wscale[(o0 + r) * nb + bi];
            for (crd::u32 k = 0; k < 8U; ++k) // the exact i32 lane partials (maddubs/madd structure)
            {
                crd::i32 lane = 0;
                for (crd::u32 j = 0; j < 4U; ++j)
                {
                    lane += static_cast<crd::i32>(xrow[bi].qs[4U * k + j]) *
                            static_cast<crd::i32>(wrow[bi].qs[4U * k + j]);
                }
                accl[k] = std::fma(scale, static_cast<crd::f32>(lane), accl[k]);
            }
        }
        // the fixed horizontal tree — mirrors the AVX2 hadd reduction exactly
        const crd::f32 q0 = (accl[0] + accl[1]) + (accl[2] + accl[3]);
        const crd::f32 q1 = (accl[4] + accl[5]) + (accl[6] + accl[7]);
        const crd::f32 v = (q0 + q1) + bias[o0 + r];
        yrow[o0 + r] = relu ? (v > 0.0F ? v : 0.0F) : v;
    }
#endif
}

inline void q8_dot_tile_any(crd::u32 t, const BlockQ8_0* xrow, const crd::f32* xsc, const BlockQ8_0* wq,
                            const crd::f32* wscale, crd::u64 nb, crd::u64 o0, const crd::f32* bias,
                            crd::f32* yrow, bool relu) noexcept
{
    switch (t)
    {
    case 8U:
        q8_dot_tile<8U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    case 7U:
        q8_dot_tile<7U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    case 6U:
        q8_dot_tile<6U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    case 5U:
        q8_dot_tile<5U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    case 4U:
        q8_dot_tile<4U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    case 3U:
        q8_dot_tile<3U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    case 2U:
        q8_dot_tile<2U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    case 1U:
        q8_dot_tile<1U>(xrow, xsc, wq, wscale, nb, o0, bias, yrow, relu);
        return;
    default:
        return;
    }
}

// Vectorized Q8_0 activation quantizer — BIT-IDENTICAL to dtypes.hpp's scalar
// quantize_q8_0 on every input (gate-enforced): amax is exact (order-free),
// the per-lane f32 mul matches, and roundf's half-away form is reproduced as
// trunc(t + copysign(0.5, t)) — the scalar's `t >= 0 ? t+0.5 : t-0.5` cast,
// lane for lane (including -0, which is >= 0 in both forms).
// HOME-> crd/hesap/tensor/dtypes.hpp: this AVX2 batch quantizer belongs next
// to quantize_q8_0 itself; move it there on the next dtypes touch.
inline void quantize_q8_0_fast(const crd::f32* x, BlockQ8_0* out, crd::u64 nblocks) noexcept
{
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    const __m256 sign_mask = _mm256_castsi256_ps(_mm256_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m256 half = _mm256_set1_ps(0.5F);
    for (crd::u64 i = 0; i < nblocks; ++i)
    {
        const crd::f32* blk = x + i * kQuantBlock;
        const __m256 v0 = _mm256_loadu_ps(blk);
        const __m256 v1 = _mm256_loadu_ps(blk + 8);
        const __m256 v2 = _mm256_loadu_ps(blk + 16);
        const __m256 v3 = _mm256_loadu_ps(blk + 24);
        __m256 am = _mm256_max_ps(_mm256_max_ps(_mm256_and_ps(v0, abs_mask), _mm256_and_ps(v1, abs_mask)),
                                  _mm256_max_ps(_mm256_and_ps(v2, abs_mask), _mm256_and_ps(v3, abs_mask)));
        __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(am), _mm256_extractf128_ps(am, 1));
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0x4E));
        m4 = _mm_max_ps(m4, _mm_shuffle_ps(m4, m4, 0xB1));
        const crd::f32 amax = _mm_cvtss_f32(m4);
        const crd::f32 d = amax / 127.0F;
        const crd::f32 id = d != 0.0F ? 1.0F / d : 0.0F;
        out[i].d = f32_to_f16_bits(d);
        const __m256 vid = _mm256_set1_ps(id);
        const __m256 t0 = _mm256_mul_ps(v0, vid);
        const __m256 t1 = _mm256_mul_ps(v1, vid);
        const __m256 t2 = _mm256_mul_ps(v2, vid);
        const __m256 t3 = _mm256_mul_ps(v3, vid);
        const auto away = [&](__m256 t)
        { return _mm256_cvttps_epi32(_mm256_add_ps(t, _mm256_or_ps(_mm256_and_ps(t, sign_mask), half))); };
        const __m256i q01 = _mm256_packs_epi32(away(t0), away(t1)); // i16, 128-lane interleaved
        const __m256i q23 = _mm256_packs_epi32(away(t2), away(t3));
        const __m256i q = _mm256_packs_epi16(q01, q23); // i8, order 0,1|2,3 per 128 half
        const __m256i fixed = _mm256_permutevar8x32_epi32(q, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out[i].qs), fixed);
    }
#else
    quantize_q8_0({x, static_cast<crd::usize>(nblocks * kQuantBlock)}, {out, static_cast<crd::usize>(nblocks)});
#endif
}

struct LinQ8Ctx
{
    const crd::f32* x;      // [M, K], K % 32 == 0
    const BlockQ8_0* wq;    // [N * (K/32)]
    const crd::f32* wscale; // [N * (K/32)] the weight block scales pre-widened to f32 at LOAD
    const crd::f32* bias;   // [N]
    crd::f32* y;            // [M, N]
    BlockQ8_0* xq;          // scratch: [M * (K/32)] quantized activation rows
    crd::f32* xscale;       // scratch: [M * (K/32)] activation block scales (f32, per row)
    crd::u64 k;
    crd::u64 n;
    bool relu;
};

inline void linear_q8_run(const LinQ8Ctx& c, crd::u32 lo, crd::u32 hi)
{
    const crd::u64 nb = c.k / kQuantBlock;
    for (crd::u32 m = lo; m < hi; ++m)
    {
        BlockQ8_0* xrow = c.xq + static_cast<crd::u64>(m) * nb;
        crd::f32* xsc = c.xscale + static_cast<crd::u64>(m) * nb;
        // activation quantization = dtypes.hpp's ggml transcription semantics
        // (the vector path is gate-enforced bit-identical to the scalar)
        quantize_q8_0_fast(c.x + static_cast<crd::u64>(m) * c.k, xrow, nb);
        for (crd::u64 bi = 0; bi < nb; ++bi) // widen the row scales ONCE (never in the o loop)
        {
            xsc[bi] = f16_bits_to_f32(xrow[bi].d);
        }
        crd::f32* yrow = c.y + static_cast<crd::u64>(m) * c.n;
        for (crd::u64 o0 = 0; o0 < c.n; o0 += kNnQ8OutTile)
        {
            const crd::u32 t = c.n - o0 < kNnQ8OutTile ? static_cast<crd::u32>(c.n - o0)
                                                       : kNnQ8OutTile;
            q8_dot_tile_any(t, xrow, xsc, c.wq, c.wscale, nb, o0, c.bias, yrow, c.relu);
        }
    }
}

// ---- the per-tensor int8 linear (the THROUGHPUT tier) ----------------------
// Scheme (the ort DynamicQuantizeLinear + MatMulInteger class): weights are
// per-tensor SYMMETRIC i8 (one f32 scale, quantized at load, with precomputed
// per-output column sums); activations are per-row ASYMMETRIC u8 (dynamic
// scale + zero point over the 0-extended row range, computed inside the row
// task — single pass, partition-local, so the moat holds by construction; u8
// captures the full range of post-relu rows). The inner kernel is a pure
// u8 x i8 -> i32 dot over the whole row (ONE dpbusd per 32 values under
// AVX-VNNI — no sign fixup, no per-block scale application), and the epilogue
// is integer zero-point correction (idot - zp*colsum, exact i32) + ONE fma:
// y = fma(sx*sw, f32(idot - zp*colsum), bias). Integer stages are EXACT in
// any summation order on every backend (vector, VNNI, and scalar produce
// identical integers by arithmetic identity); the f32 convert is exact for
// K <= 516 worst-case and correctly-rounded (still deterministic) above.
// Q8_0 block-32 stays the DEFAULT tier (certified / ggml byte-interop).

// Vectorized per-row asymmetric u8 quantizer — BIT-IDENTICAL to
// nn_quantize_u8_asym (the pinned scalar reference) on every input: exact
// min/max over the 0-extended range, the same f32 ops, the half-away
// trunc(t + copysign(0.5, t)) form lane for lane, packus saturation == the
// scalar [0, 255] clamp. k must be a multiple of 32.
inline void quantize_row_u8(const crd::f32* x, crd::u64 k, crd::u8* out, crd::f32& scale,
                            crd::i32& zero_point) noexcept
{
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    __m256 vmin = _mm256_setzero_ps(); // the range is 0-extended (the ort convention)
    __m256 vmax = _mm256_setzero_ps();
    for (crd::u64 i = 0; i < k; i += 8U)
    {
        const __m256 v = _mm256_loadu_ps(x + i);
        vmin = _mm256_min_ps(vmin, v);
        vmax = _mm256_max_ps(vmax, v);
    }
    __m128 lo4 = _mm_min_ps(_mm256_castps256_ps128(vmin), _mm256_extractf128_ps(vmin, 1));
    lo4 = _mm_min_ps(lo4, _mm_shuffle_ps(lo4, lo4, 0x4E));
    lo4 = _mm_min_ps(lo4, _mm_shuffle_ps(lo4, lo4, 0xB1));
    __m128 hi4 = _mm_max_ps(_mm256_castps256_ps128(vmax), _mm256_extractf128_ps(vmax, 1));
    hi4 = _mm_max_ps(hi4, _mm_shuffle_ps(hi4, hi4, 0x4E));
    hi4 = _mm_max_ps(hi4, _mm_shuffle_ps(hi4, hi4, 0xB1));
    const crd::f32 rmin = _mm_cvtss_f32(lo4);
    const crd::f32 rmax = _mm_cvtss_f32(hi4);
    const crd::f32 s = (rmax - rmin) / 255.0F;
    const crd::f32 inv = s != 0.0F ? 1.0F / s : 0.0F;
    const crd::f32 zp_f = s != 0.0F ? crd::math::clamp(-rmin / s, 0.0F, 255.0F) : 0.0F;
    const crd::i32 zp = static_cast<crd::i32>(crd::math::nearbyint(zp_f));
    const __m256 vinv = _mm256_set1_ps(inv);
    const __m256i vzp = _mm256_set1_epi32(zp);
    const __m256 sign_mask = _mm256_castsi256_ps(_mm256_set1_epi32(static_cast<crd::i32>(0x80000000U)));
    const __m256 half = _mm256_set1_ps(0.5F);
    const auto away_zp = [&](__m256 t)
    {
        const __m256i q = _mm256_cvttps_epi32(_mm256_add_ps(t, _mm256_or_ps(_mm256_and_ps(t, sign_mask), half)));
        return _mm256_add_epi32(q, vzp);
    };
    for (crd::u64 i = 0; i < k; i += 32U)
    {
        const __m256i q0 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i), vinv));
        const __m256i q1 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i + 8), vinv));
        const __m256i q2 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i + 16), vinv));
        const __m256i q3 = away_zp(_mm256_mul_ps(_mm256_loadu_ps(x + i + 24), vinv));
        const __m256i q01 = _mm256_packus_epi32(q0, q1); // unsigned saturate == clamp [0, 255]
        const __m256i q23 = _mm256_packus_epi32(q2, q3);
        const __m256i q = _mm256_packus_epi16(q01, q23);
        const __m256i fixed = _mm256_permutevar8x32_epi32(q, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), fixed);
    }
    scale = s;
    zero_point = zp;
#else
    nn_quantize_u8_asym({x, static_cast<crd::usize>(k)}, {out, static_cast<crd::usize>(k)}, scale,
                        zero_point);
#endif
}

// y[o0..o0+T) over the whole K in exact i32; integer zero-point correction +
// ONE fma per output. 8 independent i32 accumulator chains hide the dpbusd /
// madd latency; T is COMPILE-TIME so the inner loop is fully unrolled.
template <crd::u32 T>
inline void i8_dot_tile(const crd::u8* xrow, const crd::i8* wq, const crd::i32* colsum, crd::u64 k,
                        crd::u64 o0, crd::f32 scale_m, crd::i32 zp, const crd::f32* bias, crd::f32* yrow,
                        bool relu) noexcept
{
    constexpr crd::u32 t = T;
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    __m256i acc[8];
    for (crd::u32 r = 0; r < 8U; ++r)
    {
        acc[r] = _mm256_setzero_si256();
    }
#if !defined(__AVXVNNI__)
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i low7 = _mm256_set1_epi8(0x7F);
    const __m256i one8 = _mm256_set1_epi8(1);
#endif
    for (crd::u64 c = 0; c < k; c += 32U)
    {
        const __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow + c)); // u8
#if !defined(__AVXVNNI__)
        // u8 x i8 without VNNI: split x = xl + 128*xh (xl <= 127, xh in {0,1})
        // so the maddubs i16 pair sums can never saturate; exact i32 either way
        const __m256i xl = _mm256_and_si256(vx, low7);
        const __m256i xh = _mm256_and_si256(_mm256_srli_epi16(vx, 7), one8);
#endif
        for (crd::u32 r = 0; r < t; ++r)
        {
            const __m256i vw =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wq + (o0 + r) * k + c));
#if defined(__AVXVNNI__)
            acc[r] = _mm256_dpbusd_avx_epi32(acc[r], vx, vw); // u8 x i8, exact i32 accumulate
#else
            const __m256i pl = _mm256_madd_epi16(_mm256_maddubs_epi16(xl, vw), ones16);
            const __m256i ph = _mm256_madd_epi16(_mm256_maddubs_epi16(xh, vw), ones16);
            acc[r] = _mm256_add_epi32(acc[r], _mm256_add_epi32(pl, _mm256_slli_epi32(ph, 7)));
#endif
        }
    }
    // 4-output integer horizontal reductions + zero-point correction (exact)
    for (crd::u32 g = 0; g < t; g += 4U)
    {
        const crd::u32 tg = t - g < 4U ? t - g : 4U;
        const __m256i h0 = _mm256_hadd_epi32(acc[g], acc[g + 1U]);
        const __m256i h1 = _mm256_hadd_epi32(acc[g + 2U], acc[g + 3U]);
        const __m256i hh = _mm256_hadd_epi32(h0, h1);
        const __m128i r4 = _mm_add_epi32(_mm256_castsi256_si128(hh), _mm256_extracti128_si256(hh, 1));
        if (tg == 4U)
        {
            const __m128i corr = _mm_mullo_epi32(
                _mm_set1_epi32(zp), _mm_loadu_si128(reinterpret_cast<const __m128i*>(colsum + o0 + g)));
            __m128 v = _mm_fmadd_ps(_mm_set1_ps(scale_m), _mm_cvtepi32_ps(_mm_sub_epi32(r4, corr)),
                                    _mm_loadu_ps(bias + o0 + g));
            if (relu)
            {
                v = _mm_max_ps(v, _mm_setzero_ps()); // lane semantics == the ternary form
            }
            _mm_storeu_ps(yrow + o0 + g, v);
            continue;
        }
        alignas(16) crd::i32 d4[4]; // tail: scalar correction (no 4-wide colsum read past N)
        _mm_store_si128(reinterpret_cast<__m128i*>(d4), r4);
        for (crd::u32 r = 0; r < tg; ++r)
        {
            const crd::i32 dc = d4[r] - zp * colsum[o0 + g + r];
            const crd::f32 v = std::fma(scale_m, static_cast<crd::f32>(dc), bias[o0 + g + r]);
            yrow[o0 + g + r] = relu ? (v > 0.0F ? v : 0.0F) : v;
        }
    }
#else
    for (crd::u32 r = 0; r < t; ++r)
    {
        crd::i32 idot = 0; // exact integer — order-free, identical to the vector path by arithmetic
        const crd::i8* wrow = wq + (o0 + r) * k;
        for (crd::u64 j = 0; j < k; ++j)
        {
            idot += static_cast<crd::i32>(xrow[j]) * static_cast<crd::i32>(wrow[j]);
        }
        idot -= zp * colsum[o0 + r]; // exact zero-point correction
        const crd::f32 v = std::fma(scale_m, static_cast<crd::f32>(idot), bias[o0 + r]);
        yrow[o0 + r] = relu ? (v > 0.0F ? v : 0.0F) : v;
    }
#endif
}

inline void i8_dot_tile_any(crd::u32 t, const crd::u8* xrow, const crd::i8* wq, const crd::i32* colsum,
                            crd::u64 k, crd::u64 o0, crd::f32 scale_m, crd::i32 zp, const crd::f32* bias,
                            crd::f32* yrow, bool relu) noexcept
{
    switch (t)
    {
    case 8U:
        i8_dot_tile<8U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    case 7U:
        i8_dot_tile<7U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    case 6U:
        i8_dot_tile<6U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    case 5U:
        i8_dot_tile<5U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    case 4U:
        i8_dot_tile<4U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    case 3U:
        i8_dot_tile<3U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    case 2U:
        i8_dot_tile<2U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    case 1U:
        i8_dot_tile<1U>(xrow, wq, colsum, k, o0, scale_m, zp, bias, yrow, relu);
        return;
    default:
        return;
    }
}

// ---- the PACKED runner kernel (the MLAS structure) --------------------------
// Weights are repacked at LOAD to [ceil(N/8) blocks][K/4 slices][8 outputs x
// 4 k-bytes] so ONE dpbusd accumulates 4 k-steps for 8 OUTPUTS at once — the
// accumulator vector IS 8 outputs (no horizontal reduction, no per-output
// accumulator chains), activations broadcast 4 bytes at a time, and R rows
// share every weight load. Integer results are the exact whole-row dots —
// identical to the row-major reference kernel and the scalar formula by
// arithmetic identity (gate-enforced).

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

alignas(32) inline constexpr crd::i32 kNnTailMask[16] = {-1, -1, -1, -1, -1, -1, -1, -1,
                                                         0,  0,  0,  0,  0,  0,  0,  0};

// R rows (R <= 4) against the packed weights; colsum/bias are n8-padded.
template <crd::u32 R>
inline void i8_packed_rows(const crd::u8* xrows, const crd::i8* wpack, const crd::i32* colsum, crd::u64 k,
                           crd::u64 n, const crd::f32* scale_m, const crd::i32* zp, const crd::f32* bias,
                           crd::f32* yrows, bool relu) noexcept
{
    const crd::u64 n8 = (n + 7U) & ~crd::u64{7};
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
#if !defined(__AVXVNNI__)
    const __m256i ones16 = _mm256_set1_epi16(1);
    const __m256i low7 = _mm256_set1_epi8(0x7F);
    const __m256i one8 = _mm256_set1_epi8(1);
#endif
    const __m256 zero = _mm256_setzero_ps();
    for (crd::u64 b = 0; b < n8; b += 8U)
    {
        __m256i acc[R];
        for (crd::u32 r = 0; r < R; ++r)
        {
            acc[r] = _mm256_setzero_si256();
        }
        const crd::i8* wp = wpack + b * k; // this block: k/4 slices of 8x4 bytes
        for (crd::u64 c = 0; c < k; c += 4U)
        {
            const __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(wp + c * 8U));
            for (crd::u32 r = 0; r < R; ++r)
            {
                crd::i32 xw = 0; // 4 u8 activation bytes, broadcast to every lane
                std::memcpy(&xw, xrows + static_cast<crd::u64>(r) * k + c, 4U);
                const __m256i va = _mm256_set1_epi32(xw);
#if defined(__AVXVNNI__)
                acc[r] = _mm256_dpbusd_avx_epi32(acc[r], va, vb); // u8 x i8, exact i32
#else
                // u8 x i8 without VNNI: split x = xl + 128*xh so the maddubs
                // i16 pair sums can never saturate; exact i32 either way
                const __m256i xl = _mm256_and_si256(va, low7);
                const __m256i xh = _mm256_and_si256(_mm256_srli_epi16(va, 7), one8);
                const __m256i pl = _mm256_madd_epi16(_mm256_maddubs_epi16(xl, vb), ones16);
                const __m256i ph = _mm256_madd_epi16(_mm256_maddubs_epi16(xh, vb), ones16);
                acc[r] = _mm256_add_epi32(acc[r], _mm256_add_epi32(pl, _mm256_slli_epi32(ph, 7)));
#endif
            }
        }
        const crd::u64 valid = n - b < 8U ? n - b : 8U;
        const __m256i cs8 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(colsum + b));
        const __m256 b8 = _mm256_loadu_ps(bias + b);
        for (crd::u32 r = 0; r < R; ++r)
        {
            const __m256i corr = _mm256_mullo_epi32(_mm256_set1_epi32(zp[r]), cs8);
            __m256 v = _mm256_fmadd_ps(_mm256_set1_ps(scale_m[r]),
                                       _mm256_cvtepi32_ps(_mm256_sub_epi32(acc[r], corr)), b8);
            if (relu)
            {
                v = _mm256_max_ps(v, zero); // lane semantics == the ternary form
            }
            crd::f32* dst = yrows + static_cast<crd::u64>(r) * n + b;
            if (valid == 8U)
            {
                _mm256_storeu_ps(dst, v);
            }
            else
            {
                const __m256i vmask = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(kNnTailMask + (8U - valid)));
                _mm256_maskstore_ps(dst, vmask, v);
            }
        }
    }
#else
    for (crd::u32 r = 0; r < R; ++r)
    {
        for (crd::u64 o = 0; o < n; ++o)
        {
            crd::i32 idot = 0; // exact integer over the packed layout — order-free
            for (crd::u64 c = 0; c < k; c += 4U)
            {
                for (crd::u64 j = 0; j < 4U; ++j)
                {
                    const crd::i8 wv =
                        wpack[(o / 8U) * (k * 8U) + (c / 4U) * 32U + (o % 8U) * 4U + j];
                    idot += static_cast<crd::i32>(xrows[static_cast<crd::u64>(r) * k + c + j]) *
                            static_cast<crd::i32>(wv);
                }
            }
            idot -= zp[r] * colsum[o];
            const crd::f32 v = std::fma(scale_m[r], static_cast<crd::f32>(idot), bias[o]);
            yrows[static_cast<crd::u64>(r) * n + o] = relu ? (v > 0.0F ? v : 0.0F) : v;
        }
    }
    (void)n8;
#endif
}

struct LinI8Ctx
{
    const crd::f32* x;      // [M, K], K % 32 == 0
    const crd::i8* wq;      // PACKED [n8 * K] weights (i8_pack_weights layout)
    const crd::i32* colsum; // [n8] per-output weight column sums (zero-point correction)
    const crd::f32* bias;   // [n8] (padded)
    crd::f32* y;            // [M, N]
    crd::u8* xq;            // scratch: [M, K] asymmetric u8 activation rows
    crd::f32* xscale;       // scratch: [M] per-row activation scales
    crd::i32* xzp;          // scratch: [M] per-row activation zero points
    crd::f32 wscale;        // the ONE weight scale
    crd::u64 k;
    crd::u64 n;
    bool relu;
};

inline void linear_i8_run(const LinI8Ctx& c, crd::u32 lo, crd::u32 hi)
{
    // 4-row groups amortize the weight-block loads; grouping cannot perturb
    // results (each row's accumulator and math are its own — bit-identical for
    // any grouping, so task partitions stay moat-safe)
    crd::u32 m0 = lo;
    while (m0 < hi)
    {
        const crd::u32 rr = hi - m0 < 4U ? hi - m0 : 4U;
        crd::f32 scale_m[4];
        crd::i32 zp[4];
        for (crd::u32 r = 0; r < rr; ++r)
        {
            const crd::u32 m = m0 + r;
            quantize_row_u8(c.x + static_cast<crd::u64>(m) * c.k, c.k,
                            c.xq + static_cast<crd::u64>(m) * c.k, c.xscale[m], c.xzp[m]);
            scale_m[r] = c.xscale[m] * c.wscale;
            zp[r] = c.xzp[m];
        }
        const crd::u8* xrows = c.xq + static_cast<crd::u64>(m0) * c.k;
        crd::f32* yrows = c.y + static_cast<crd::u64>(m0) * c.n;
        switch (rr)
        {
        case 4U:
            i8_packed_rows<4U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
            break;
        case 3U:
            i8_packed_rows<3U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
            break;
        case 2U:
            i8_packed_rows<2U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
            break;
        case 1U:
            i8_packed_rows<1U>(xrows, c.wq, c.colsum, c.k, c.n, scale_m, zp, c.bias, yrows, c.relu);
            break;
        default:
            break;
        }
        m0 += rr;
    }
}

// resolve the effective worker count: explicit request clamped to the live
// pool; no pool (or no request) -> serial. Never oversubscribes.
[[nodiscard]] inline crd::u32 resolve_workers(crd::u32 requested) noexcept
{
    const crd::u32 pool = crd::jobs::num_workers();
    crd::u32 nw = requested != 0U ? requested : (pool != 0U ? pool : 1U);
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
    const nndetail::SoftmaxCtx ctx{x.data(), y.data(), d};
    nndetail::nn_parallel(static_cast<crd::u32>(rows), 1U, &ctx, nndetail::softmax_run);
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
    const nndetail::LayerNormCtx ctx{x.data(), gamma.data(), beta.data(), y.data(), x.shape(1), eps};
    nndetail::nn_parallel(static_cast<crd::u32>(x.shape(0)), 1U, &ctx, nndetail::layernorm_run);
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
    const PoolCtx ctx{x.data(), y.data(), h, w, oh, ow, k, s, is_max};
    nn_parallel(static_cast<crd::u32>(x.shape(0) * x.shape(1)), 1U, &ctx, pool_run);
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

// Per-call conv2d scratch size (ONE slot, floats): the im2col column matrix,
// or the padded row staging of the direct 3x3/s1/p1 kernel — whichever is
// larger. The NnSequential workspace reserves kNnConvScratchSlots of these.
[[nodiscard]] constexpr crd::u64 nn_conv2d_scratch_floats(crd::u64 chans, crd::u64 kh, crd::u64 kw,
                                                          crd::u64 oh, crd::u64 ow) noexcept
{
    const crd::u64 col = chans * kh * kw * oh * ow;
    // zero row + C*H padded rows + 16 pooled-fusion row buffers
    const crd::u64 stage = (chans * oh + 1U + 16U) * (ow + 10U);
    return col > stage ? col : stage;
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
    const nndetail::ConvCtx ctx{x.data(), w.data(),  bias.data(), y.data(), col_scratch.data(), slot,
                                chans,    h,         ww,          kh,       kw,                 pad,
                                stride,   oc,        oh,          ow,       false,              false,
                                false};
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
                                               crd::containers::ConstSpan<BlockQ8_0> wq,
                                               crd::containers::ConstSpan<crd::f32> bias,
                                               TensorView<crd::f32> y,
                                               crd::containers::Span<BlockQ8_0> xq_scratch,
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
    const nndetail::LinQ8Ctx ctx{x.data(), wq.data(),         wsc, bias.data(), y.data(),
                                 xq_scratch.data(), wsc + n * nb, k,   n,           false};
    nndetail::nn_parallel(static_cast<crd::u32>(m), 1U, &ctx, nndetail::linear_q8_run);
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
    // the ROW-MAJOR reference path (unit surface; the NnSequential runner rides
    // the packed kernel, gated bit-identical against this and the scalar formula)
    for (crd::u64 r = 0; r < m; ++r)
    {
        crd::u8* xrow = xq_scratch.data() + r * k;
        nndetail::quantize_row_u8(x.data() + r * k, k, xrow, xscale_scratch[static_cast<crd::usize>(r)],
                                  xzp_scratch[static_cast<crd::usize>(r)]);
        const crd::f32 scale_m = xscale_scratch[static_cast<crd::usize>(r)] * wscale;
        crd::f32* yrow = y.data() + r * n;
        for (crd::u64 o0 = 0; o0 < n; o0 += 8U)
        {
            const crd::u32 t = n - o0 < 8U ? static_cast<crd::u32>(n - o0) : 8U;
            nndetail::i8_dot_tile_any(t, xrow, wq.data(), colsum.data(), k, o0, scale_m,
                                      xzp_scratch[static_cast<crd::usize>(r)], bias.data(), yrow, false);
        }
    }
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
                    Tensor<BlockQ8_0>{});
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
        Tensor<BlockQ8_0> q(m_alloc);
        const crd::u64 qshp[1] = {nblocks};
        TensorStatus st = q.resize({qshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        nn_quantize_q8_0_rint({w.data(), static_cast<crd::usize>(w.size())},
                              {q.data(), static_cast<crd::usize>(nblocks)});
        return add_linear_q8_blocks(static_cast<Tensor<BlockQ8_0>&&>(q), w.shape(0), w.shape(1), b);
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
        Tensor<BlockQ8_0> q(m_alloc);
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
        return add_linear_q8_blocks(static_cast<Tensor<BlockQ8_0>&&>(q), out, in, b);
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
                  Tensor<BlockQ8_0>{});
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
                  Tensor<BlockQ8_0>{});
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
                    Tensor<BlockQ8_0>{});
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
        Tensor<BlockQ8_0> q(m_alloc);
        const crd::u64 qshp[1] = {nblocks};
        const TensorStatus st = q.resize({qshp, 1});
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        nn_quantize_q8_0_rint({w.data(), static_cast<crd::usize>(numel)},
                              {q.data(), static_cast<crd::usize>(nblocks)});
        return add_conv2d_from_blocks(static_cast<Tensor<BlockQ8_0>&&>(q), w.shape(0), w.shape(1),
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
        Tensor<BlockQ8_0> q(m_alloc);
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
        return add_conv2d_from_blocks(static_cast<Tensor<BlockQ8_0>&&>(q), oc, chans, kh, kw, b, pad,
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
                    Tensor<BlockQ8_0>{});
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
            crd::f32* dst = i == last_write ? y.data() : (cur == buf0 ? buf1 : buf0);
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
    [[nodiscard]] crd::containers::ConstSpan<BlockQ8_0> q8_blocks(crd::u32 i) const noexcept
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
                                    Tensor<BlockQ8_0>&& q)
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
        m_w[m_count] = static_cast<Tensor<crd::f32>&&>(w);
        m_b[m_count] = static_cast<Tensor<crd::f32>&&>(b);
        m_q[m_count] = static_cast<Tensor<BlockQ8_0>&&>(q);
        ++m_count;
        return TensorStatus::Ok;
    }

    [[nodiscard]] TensorStatus push_simple(NnOpKind kind)
    {
        NnOpRec rec;
        rec.kind = kind;
        return push(rec, Tensor<crd::f32>{}, Tensor<crd::f32>{}, Tensor<BlockQ8_0>{});
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
        return push(rec, Tensor<crd::f32>{}, Tensor<crd::f32>{}, Tensor<BlockQ8_0>{});
    }

    [[nodiscard]] TensorStatus add_linear_q8_blocks(Tensor<BlockQ8_0>&& q, crd::u64 out, crd::u64 in,
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
        return push(rec, static_cast<Tensor<crd::f32>&&>(wscale), static_cast<Tensor<crd::f32>&&>(bias),
                    static_cast<Tensor<BlockQ8_0>&&>(q));
    }

    [[nodiscard]] TensorStatus add_conv2d_from_blocks(Tensor<BlockQ8_0>&& q, crd::u64 oc, crd::u64 chans,
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
        return push(rec, static_cast<Tensor<crd::f32>&&>(wf), static_cast<Tensor<crd::f32>&&>(bias),
                    static_cast<Tensor<BlockQ8_0>&&>(q));
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
            return nn_align64(max_batch * nb * sizeof(BlockQ8_0)) +
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
            const crd::u64 xq_bytes = nn_align64(m_in_shape[0] * nb * sizeof(BlockQ8_0));
            const nndetail::LinQ8Ctx ctx{in,
                                         q8_ptr(op),
                                         weight_ptr(op), // the pre-widened f32 block scales
                                         bias_ptr(op),
                                         out,
                                         reinterpret_cast<BlockQ8_0*>(scratch),
                                         reinterpret_cast<crd::f32*>(scratch + xq_bytes),
                                         op.dim_in,
                                         op.dim_out,
                                         op.fuse_relu};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, nndetail::linear_q8_run);
            return;
        }
        case NnOpKind::LinearI8:
        {
            // scratch layout (fixed offsets — max_batch): [xq rows][row scales][row zero points]
            const crd::u64 xq_bytes = nn_align64(m_in_shape[0] * op.dim_in);
            const crd::u64 xsc_bytes = nn_align64(m_in_shape[0] * sizeof(crd::f32));
            const nndetail::LinI8Ctx ctx{in,
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
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, nndetail::linear_i8_run);
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
            const nndetail::ConvCtx ctx{in,
                                        weight_ptr(op),
                                        bias_ptr(op),
                                        out,
                                        reinterpret_cast<crd::f32*>(scratch),
                                        slot_floats,
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
                                        parallel,
                                        op.fuse_relu,
                                        op.fuse_pool};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), parallel ? nw : 1U, &ctx,
                                  nndetail::conv_run);
            return;
        }
        case NnOpKind::MaxPool2d:
        case NnOpKind::AvgPool2d:
        {
            const nndetail::PoolCtx ctx{in,
                                        out,
                                        op.in_shape[2],
                                        op.in_shape[3],
                                        op.out_shape[2],
                                        op.out_shape[3],
                                        op.pool_k,
                                        op.pool_s,
                                        op.kind == NnOpKind::MaxPool2d};
            nndetail::nn_parallel(static_cast<crd::u32>(batch * op.in_shape[1]), nw, &ctx,
                                  nndetail::pool_run);
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
            const nndetail::LayerNormCtx ctx{in, weight_ptr(op), bias_ptr(op), out, op.dim_in, op.eps};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, nndetail::layernorm_run);
            return;
        }
        case NnOpKind::Softmax:
        {
            const nndetail::SoftmaxCtx ctx{in, out, op.in_shape[op.in_rank - 1U]};
            nndetail::nn_parallel(static_cast<crd::u32>(batch), nw, &ctx, nndetail::softmax_run);
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
    [[nodiscard]] const BlockQ8_0* q8_ptr(const NnOpRec& op) const noexcept
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
    Tensor<BlockQ8_0> m_q[kNnMaxOps];
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
