#pragma once

#include "tensor.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/stats/philox.hpp> // include-only (header-only constexpr) — NO link edge (ADR-0096 §1)
#include <crd/math/float_convert.hpp> // the format primitives — MIGRATED to crd-math (owning module, SANITY #8)

#include <bit>

// ---------------------------------------------------------------------------
// crd-hesap-tensor dtypes — the low-precision storage POLICY layer (Phase
// 3.1.6 v14-a; ADR-0096 §2/§4).
//
// The format PRIMITIVES (scalar + F16C/AVX2 batch f16/bf16/FP8 converts, the
// random-supplied SR cores) live in `crd/math/float_convert.hpp` — migrated
// there 2026-07-02 per user direction so any module (asset cooker, GPU
// oracles, eylem replay, model I/O) reuses them without a tensor dependency.
// This header owns what is genuinely tensor policy:
//   - the ★deterministic SR KEYING contract: Philox4x32 keyed by
//     (seed, canonical destination index, step) with the PINNED lane-packed
//     layout (block = index >> 2, lane = index & 3) — a pure function of its
//     inputs, so SR is order/partition-independent and reproducible-by-seed;
//   - int8/int4 BLOCK-quantized storage (ggml-compatible Q8_0/Q4_0 — exact
//     transcriptions of quantize_row_{q8_0,q4_0}_ref; byte-interop with
//     llama.cpp/ggml weights) — an OPAQUE blocked representation;
//   - StorageTensor<Dtype> — element-addressable low-precision storage on the
//     bounded-rank shape header, compute-forbidden (convert/io only).
// ---------------------------------------------------------------------------

namespace crd::hesap::tensor
{

// Re-export the crd-math format primitives under the tensor namespace (the
// module's public dtype surface; implementations are crd::math's).
using crd::math::bf16_bits_to_f32;
using crd::math::convert_f16_to_f32;
using crd::math::convert_f32_to_bf16;
using crd::math::convert_f32_to_f16;
using crd::math::convert_f32_to_fp8_e4m3;
using crd::math::convert_f32_to_fp8_e5m2;
using crd::math::f16_bits_to_f32;
using crd::math::f32_to_bf16_bits;
using crd::math::f32_to_f16_bits;
using crd::math::f32_to_fp8_e4m3_bits;
using crd::math::f32_to_fp8_e5m2_bits;
using crd::math::fp8_e4m3_bits_to_f32;
using crd::math::fp8_e5m2_bits_to_f32;

// =======================================================================
// ★Deterministic SR keying (ADR-0096 §4) — the tensor-module contract.
// =======================================================================

namespace detail
{

inline constexpr crd::u32 kSrTag = 0x53525F31U; // 'SR_1' domain tag

// Lane-packed keying (PINNED — the SR bit-stream contract): element `index`
// reads lane (index & 3) of the Philox block for (index >> 2). Still a pure
// function of (seed, index, step) — order/partition-independent — but batch
// converts amortize one 10-round Philox block over FOUR elements.
[[nodiscard]] constexpr crd::u32 sr_draw(crd::u64 seed, crd::u64 index, crd::u32 step) noexcept
{
    const crd::u64 block = index >> 2U;
    const crd::u32 counter[4] = {static_cast<crd::u32>(block), static_cast<crd::u32>(block >> 32U), step, kSrTag};
    const crd::u32 key[2] = {static_cast<crd::u32>(seed), static_cast<crd::u32>(seed >> 32U)};
    return crd::hesap::stats::philox4x32(counter, key).v[static_cast<crd::u32>(index & 3U)];
}

} // namespace detail

[[nodiscard]] constexpr crd::u16 f32_to_f16_bits_sr(crd::f32 x, crd::u64 seed, crd::u64 index,
                                                    crd::u32 step = 0U) noexcept
{
    return crd::math::f32_to_f16_bits_sr(x, detail::sr_draw(seed, index, step));
}
[[nodiscard]] constexpr crd::u16 f32_to_bf16_bits_sr(crd::f32 x, crd::u64 seed, crd::u64 index,
                                                     crd::u32 step = 0U) noexcept
{
    return crd::math::f32_to_bf16_bits_sr(x, detail::sr_draw(seed, index, step));
}
[[nodiscard]] constexpr crd::u8 f32_to_fp8_e4m3_bits_sr(crd::f32 x, crd::u64 seed, crd::u64 index,
                                                        crd::u32 step = 0U) noexcept
{
    return crd::math::f32_to_fp8_e4m3_bits_sr(x, detail::sr_draw(seed, index, step));
}
[[nodiscard]] constexpr crd::u8 f32_to_fp8_e5m2_bits_sr(crd::f32 x, crd::u64 seed, crd::u64 index,
                                                        crd::u32 step = 0U) noexcept
{
    return crd::math::f32_to_fp8_e5m2_bits_sr(x, detail::sr_draw(seed, index, step));
}

// =======================================================================
// Batch SR converts — the crush path: draws come 32-at-a-time from the
// hesap-stats AVX2 8-block Philox kernel (one u32[32] chunk = blocks
// idx>>2 .. +7, lane-ordered exactly as the pinned keying), then the
// crd-math SIMD SR narrowers consume them. Bit-identical to the
// per-element scalar wrappers on every input (gated). Zero heap: a fixed
// stack chunk of draws.
// =======================================================================

namespace detail
{

// Fill draws for elements [base, base+count) under the pinned keying.
// base must be 4-aligned (block-aligned) for the vector path.
inline void fill_sr_draws(crd::u64 seed, crd::u64 base, crd::u32 step, crd::u32* draws, crd::usize count) noexcept
{
    crd::usize t = 0;
#if defined(__AVX2__)
    // Cross-module detail:: use, deliberate (SANITY #8 — reuse over a 40-line
    // Philox duplicate); the batch≡scalar bit-identity gate pins the contract.
    const auto key0 = static_cast<crd::u32>(seed);
    const auto key1 = static_cast<crd::u32>(seed >> 32U);
    for (; t + 32U <= count; t += 32U)
    {
        crd::u32 c0s[8];
        crd::u32 c1s[8];
        const crd::u64 b0 = (base + t) >> 2U;
        for (crd::u32 j = 0; j < 8U; ++j)
        {
            c0s[j] = static_cast<crd::u32>(b0 + j);
            c1s[j] = static_cast<crd::u32>((b0 + j) >> 32U);
        }
        crd::hesap::stats::detail::philox4x32_avx2_8(c0s, c1s, step, kSrTag, key0, key1,
                                                     reinterpret_cast<crd::u64*>(draws + t));
    }
#endif
    for (; t < count; ++t)
    {
        draws[t] = sr_draw(seed, base + t, step);
    }
}

inline constexpr crd::usize kSrChunk = 256U; // stack draw buffer (zero heap per call)

template <typename Bits, typename BatchFn>
inline void convert_sr_chunked(crd::containers::ConstSpan<crd::f32> src, crd::containers::Span<Bits> dst, crd::u64 seed,
                               crd::u64 base_index, crd::u32 step, BatchFn&& batch) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_sr: size mismatch");
    CRD_ASSERT_MSG((base_index & 3U) == 0U, "convert_sr: base_index must be 4-aligned (Philox block boundary)");
    crd::u32 draws[kSrChunk];
    crd::usize i = 0;
    while (i < src.size())
    {
        const crd::usize c = src.size() - i < kSrChunk ? src.size() - i : kSrChunk;
        fill_sr_draws(seed, base_index + i, step, draws, c);
        batch(src.subspan(i, c), dst.subspan(i, c), std::span<const crd::u32>{draws, c});
        i += c;
    }
}

} // namespace detail

// Element k of src gets the SR draw for logical index base_index + k (the
// canonical-destination-index contract) — chunking/threading-independent.
inline void convert_f32_to_f16_sr(crd::containers::ConstSpan<crd::f32> src, crd::containers::Span<crd::u16> dst,
                                  crd::u64 seed, crd::u64 base_index = 0U, crd::u32 step = 0U) noexcept
{
    detail::convert_sr_chunked(src, dst, seed, base_index, step,
                               [](auto s, auto d, auto r) { crd::math::convert_f32_to_f16_sr(s, d, r); });
}
inline void convert_f32_to_bf16_sr(crd::containers::ConstSpan<crd::f32> src, crd::containers::Span<crd::u16> dst,
                                   crd::u64 seed, crd::u64 base_index = 0U, crd::u32 step = 0U) noexcept
{
    detail::convert_sr_chunked(src, dst, seed, base_index, step,
                               [](auto s, auto d, auto r) { crd::math::convert_f32_to_bf16_sr(s, d, r); });
}
inline void convert_f32_to_fp8_e4m3_sr(crd::containers::ConstSpan<crd::f32> src, crd::containers::Span<crd::u8> dst,
                                       crd::u64 seed, crd::u64 base_index = 0U, crd::u32 step = 0U) noexcept
{
    detail::convert_sr_chunked(src, dst, seed, base_index, step,
                               [](auto s, auto d, auto r) { crd::math::convert_f32_to_fp8_e4m3_sr(s, d, r); });
}
inline void convert_f32_to_fp8_e5m2_sr(crd::containers::ConstSpan<crd::f32> src, crd::containers::Span<crd::u8> dst,
                                       crd::u64 seed, crd::u64 base_index = 0U, crd::u32 step = 0U) noexcept
{
    detail::convert_sr_chunked(src, dst, seed, base_index, step,
                               [](auto s, auto d, auto r) { crd::math::convert_f32_to_fp8_e5m2_sr(s, d, r); });
}

// =======================================================================
// ggml-compatible block quantization (Q8_0 / Q4_0) — OPAQUE blocked storage
// (ADR-0096 §2: no element strides). Layouts + arithmetic are exact
// transcriptions of ggml's quantize_row_{q8_0,q4_0}_ref / dequantize_row_*
// (ggml-org/ggml src/ggml-quants.c) so cooked weights interop byte-for-byte.
// =======================================================================

inline constexpr crd::u32 kQuantBlock = 32U;

struct BlockQ8_0
{
    crd::u16 d;     // f16-encoded scale (amax/127)
    crd::i8 qs[32]; // q[j] = roundf(x[j]/d)
};
static_assert(sizeof(BlockQ8_0) == 34U, "BlockQ8_0 must match the ggml block_q8_0 layout");

struct BlockQ4_0
{
    crd::u16 d;     // f16-encoded scale (signed-max/-8)
    crd::u8 qs[16]; // low nibble = element j, high nibble = element j+16; stored value = q+8
};
static_assert(sizeof(BlockQ4_0) == 18U, "BlockQ4_0 must match the ggml block_q4_0 layout");

// x.size() must be a multiple of 32; out.size() == x.size()/32.
inline void quantize_q8_0(crd::containers::ConstSpan<crd::f32> x, crd::containers::Span<BlockQ8_0> out) noexcept
{
    CRD_ASSERT_MSG(x.size() % kQuantBlock == 0U, "quantize_q8_0: size must be a multiple of 32");
    CRD_ASSERT_MSG(out.size() == x.size() / kQuantBlock, "quantize_q8_0: bad output block count");
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        const crd::f32* blk = x.data() + i * kQuantBlock;
        crd::f32 amax = 0.0F;
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::f32 a = blk[j] < 0.0F ? -blk[j] : blk[j];
            amax = a > amax ? a : amax;
        }
        const crd::f32 d = amax / 127.0F;
        const crd::f32 id = d != 0.0F ? 1.0F / d : 0.0F;
        out[i].d = f32_to_f16_bits(d);
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::f32 v = blk[j] * id;
            // roundf (half away from zero), exact for |v| <= 127
            out[i].qs[j] = static_cast<crd::i8>(static_cast<crd::i32>(v >= 0.0F ? v + 0.5F : v - 0.5F));
        }
    }
}

inline void dequantize_q8_0(crd::containers::ConstSpan<BlockQ8_0> in, crd::containers::Span<crd::f32> out) noexcept
{
    CRD_ASSERT_MSG(out.size() == in.size() * kQuantBlock, "dequantize_q8_0: bad output size");
    for (crd::usize i = 0; i < in.size(); ++i)
    {
        const crd::f32 d = f16_bits_to_f32(in[i].d);
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            out[i * kQuantBlock + j] = static_cast<crd::f32>(in[i].qs[j]) * d;
        }
    }
}

inline void quantize_q4_0(crd::containers::ConstSpan<crd::f32> x, crd::containers::Span<BlockQ4_0> out) noexcept
{
    CRD_ASSERT_MSG(x.size() % kQuantBlock == 0U, "quantize_q4_0: size must be a multiple of 32");
    CRD_ASSERT_MSG(out.size() == x.size() / kQuantBlock, "quantize_q4_0: bad output block count");
    for (crd::usize i = 0; i < out.size(); ++i)
    {
        const crd::f32* blk = x.data() + i * kQuantBlock;
        crd::f32 amax = 0.0F; // largest |v|
        crd::f32 vmax = 0.0F; // the SIGNED value carrying it (ggml semantics)
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::f32 a = blk[j] < 0.0F ? -blk[j] : blk[j];
            if (a > amax)
            {
                amax = a;
                vmax = blk[j];
            }
        }
        const crd::f32 d = vmax / -8.0F;
        const crd::f32 id = d != 0.0F ? 1.0F / d : 0.0F;
        out[i].d = f32_to_f16_bits(d);
        for (crd::u32 j = 0; j < kQuantBlock / 2U; ++j)
        {
            const crd::f32 x0 = blk[j] * id + 8.5F;
            const crd::f32 x1 = blk[j + kQuantBlock / 2U] * id + 8.5F;
            const crd::u8 xi0 = static_cast<crd::u8>(x0 < 15.0F ? static_cast<crd::i8>(x0) : 15);
            const crd::u8 xi1 = static_cast<crd::u8>(x1 < 15.0F ? static_cast<crd::i8>(x1) : 15);
            out[i].qs[j] = static_cast<crd::u8>(xi0 | (xi1 << 4U));
        }
    }
}

inline void dequantize_q4_0(crd::containers::ConstSpan<BlockQ4_0> in, crd::containers::Span<crd::f32> out) noexcept
{
    CRD_ASSERT_MSG(out.size() == in.size() * kQuantBlock, "dequantize_q4_0: bad output size");
    for (crd::usize i = 0; i < in.size(); ++i)
    {
        const crd::f32 d = f16_bits_to_f32(in[i].d);
        for (crd::u32 j = 0; j < kQuantBlock / 2U; ++j)
        {
            const crd::i32 x0 = static_cast<crd::i32>(in[i].qs[j] & 0x0FU) - 8;
            const crd::i32 x1 = static_cast<crd::i32>(in[i].qs[j] >> 4U) - 8;
            out[i * kQuantBlock + j] = static_cast<crd::f32>(x0) * d;
            out[i * kQuantBlock + j + kQuantBlock / 2U] = static_cast<crd::f32>(x1) * d;
        }
    }
}

// =======================================================================
// StorageTensor<Dtype> — element-addressable low-precision storage carrying
// the same bounded-rank shape header (ADR-0096 §2). Compute-forbidden:
// reachable only via convert_from / convert_to (+ io/DLPack at v14-l).
// Contiguous canonical row-major; conversion from a STRIDED f32 view
// enumerates the destination's canonical logical order — which is exactly
// the SR key domain, so strided sources and any chunking produce identical
// bits.
// =======================================================================

enum class StorageDtype : crd::u8
{
    F16,
    Bf16,
    Fp8E4m3,
    Fp8E5m2,
};

namespace detail
{
template <StorageDtype D> struct StorageTraits;
template <> struct StorageTraits<StorageDtype::F16>
{
    using Bits = crd::u16;
    static constexpr crd::u16 narrow(crd::f32 x) noexcept { return f32_to_f16_bits(x); }
    static constexpr crd::u16 narrow_sr(crd::f32 x, crd::u64 s, crd::u64 i) noexcept
    {
        return f32_to_f16_bits_sr(x, s, i);
    }
    static constexpr crd::f32 widen(crd::u16 b) noexcept { return f16_bits_to_f32(b); }
};
template <> struct StorageTraits<StorageDtype::Bf16>
{
    using Bits = crd::u16;
    static constexpr crd::u16 narrow(crd::f32 x) noexcept { return f32_to_bf16_bits(x); }
    static constexpr crd::u16 narrow_sr(crd::f32 x, crd::u64 s, crd::u64 i) noexcept
    {
        return f32_to_bf16_bits_sr(x, s, i);
    }
    static constexpr crd::f32 widen(crd::u16 b) noexcept { return bf16_bits_to_f32(b); }
};
template <> struct StorageTraits<StorageDtype::Fp8E4m3>
{
    using Bits = crd::u8;
    static constexpr crd::u8 narrow(crd::f32 x) noexcept { return f32_to_fp8_e4m3_bits(x); }
    static constexpr crd::u8 narrow_sr(crd::f32 x, crd::u64 s, crd::u64 i) noexcept
    {
        return f32_to_fp8_e4m3_bits_sr(x, s, i);
    }
    static constexpr crd::f32 widen(crd::u8 b) noexcept { return fp8_e4m3_bits_to_f32(b); }
};
template <> struct StorageTraits<StorageDtype::Fp8E5m2>
{
    using Bits = crd::u8;
    static constexpr crd::u8 narrow(crd::f32 x) noexcept { return f32_to_fp8_e5m2_bits(x); }
    static constexpr crd::u8 narrow_sr(crd::f32 x, crd::u64 s, crd::u64 i) noexcept
    {
        return f32_to_fp8_e5m2_bits_sr(x, s, i);
    }
    static constexpr crd::f32 widen(crd::u8 b) noexcept { return fp8_e5m2_bits_to_f32(b); }
};
} // namespace detail

template <StorageDtype D> class StorageTensor
{
public:
    using Bits = typename detail::StorageTraits<D>::Bits;

    explicit StorageTensor(crd::memory::IAllocator* alloc) noexcept : m_bits(alloc) {}

    [[nodiscard]] TensorStatus resize(crd::containers::ConstSpan<crd::u64> shape) { return m_bits.resize(shape); }

    // RNE convert from any (possibly strided/broadcast) f32 view; shapes must match.
    // The contiguous fast path rides the crd-math batch (F16C/AVX2) kernels.
    [[nodiscard]] TensorStatus convert_from(const TensorView<const crd::f32>& src)
    {
        const TensorStatus st = reshape_to(src);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        if (src.is_contiguous())
        {
            batch_narrow({src.data(), static_cast<crd::usize>(src.size())},
                         {m_bits.data(), static_cast<crd::usize>(m_bits.size())});
            return TensorStatus::Ok;
        }
        Bits* out = m_bits.data();
        crd::u64 k = 0;
        src.for_each([&](const crd::u64*, const crd::f32& v) { out[k++] = detail::StorageTraits<D>::narrow(v); });
        return TensorStatus::Ok;
    }

    // ★Deterministic SR convert: the SR key is the canonical destination index —
    // identical bits for any source striding, chunking, or thread partition.
    [[nodiscard]] TensorStatus convert_from_sr(const TensorView<const crd::f32>& src, crd::u64 seed)
    {
        const TensorStatus st = reshape_to(src);
        if (st != TensorStatus::Ok)
        {
            return st;
        }
        if (src.is_contiguous()) // batch path: AVX2 Philox draws + SIMD SR narrowers
        {
            batch_narrow_sr({src.data(), static_cast<crd::usize>(src.size())},
                            {m_bits.data(), static_cast<crd::usize>(m_bits.size())}, seed);
            return TensorStatus::Ok;
        }
        Bits* out = m_bits.data();
        crd::u64 k = 0;
        src.for_each(
            [&](const crd::u64*, const crd::f32& v)
            {
                out[k] = detail::StorageTraits<D>::narrow_sr(v, seed, k);
                ++k;
            });
        return TensorStatus::Ok;
    }

    // Widen into a caller-provided f32 view of the same shape.
    [[nodiscard]] TensorStatus convert_to(const TensorView<crd::f32>& dst) const
    {
        if (dst.rank() != m_bits.rank())
        {
            return TensorStatus::ShapeMismatch;
        }
        for (crd::u32 d = 0; d < dst.rank(); ++d)
        {
            if (dst.shape(d) != m_bits.shape(d))
            {
                return TensorStatus::ShapeMismatch;
            }
        }
        const Bits* in = m_bits.data();
        crd::u64 k = 0;
        dst.for_each([&](const crd::u64*, crd::f32& v) { v = detail::StorageTraits<D>::widen(in[k++]); });
        return TensorStatus::Ok;
    }

    [[nodiscard]] crd::containers::ConstSpan<crd::u64> shape() const noexcept { return m_bits.shape(); }
    [[nodiscard]] crd::u64 size() const noexcept { return m_bits.size(); }
    [[nodiscard]] const Bits* bits() const noexcept { return m_bits.data(); }
    [[nodiscard]] Bits* bits() noexcept { return m_bits.data(); }

private:
    static void batch_narrow(std::span<const crd::f32> src, std::span<Bits> dst) noexcept
    {
        if constexpr (D == StorageDtype::F16)
        {
            crd::math::convert_f32_to_f16(src, dst);
        }
        else if constexpr (D == StorageDtype::Bf16)
        {
            crd::math::convert_f32_to_bf16(src, dst);
        }
        else if constexpr (D == StorageDtype::Fp8E4m3)
        {
            crd::math::convert_f32_to_fp8_e4m3(src, dst);
        }
        else
        {
            crd::math::convert_f32_to_fp8_e5m2(src, dst);
        }
    }

    static void batch_narrow_sr(std::span<const crd::f32> src, std::span<Bits> dst, crd::u64 seed) noexcept
    {
        if constexpr (D == StorageDtype::F16)
        {
            convert_f32_to_f16_sr(src, dst, seed);
        }
        else if constexpr (D == StorageDtype::Bf16)
        {
            convert_f32_to_bf16_sr(src, dst, seed);
        }
        else if constexpr (D == StorageDtype::Fp8E4m3)
        {
            convert_f32_to_fp8_e4m3_sr(src, dst, seed);
        }
        else
        {
            convert_f32_to_fp8_e5m2_sr(src, dst, seed);
        }
    }

    [[nodiscard]] TensorStatus reshape_to(const TensorView<const crd::f32>& src)
    {
        crd::u64 shp[kMaxRank];
        for (crd::u32 d = 0; d < src.rank(); ++d)
        {
            shp[d] = src.shape(d);
        }
        return m_bits.resize({shp, src.rank()});
    }

    Tensor<Bits> m_bits; // contiguous canonical row-major payload + the shape header
};

} // namespace crd::hesap::tensor
