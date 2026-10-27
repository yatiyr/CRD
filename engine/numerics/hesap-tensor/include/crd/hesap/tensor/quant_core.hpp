#pragma once

// The quantized storage blocks and the scalar activation quantizers (ADR-0096 §2). dtypes.hpp's public quantizers and
// the per-level tensor kernels (../../../../kernels/tensor_kernels.cpp, whose portable bodies these are) both compile
// the quantizers, so they live in the unit's level namespace (CRD_MATH_LEVEL_NS, ADR-0134 §5) and call only
// crd/math/libm.hpp. The blocks are one plain type on every level.
//
// A NaN element quantizes as zero (Q8_0: q = 0; u8: q = the zero point) and never enters a range or an amax, so every
// level gives the same bytes for every input; an infinite range gives the scale infinity and every element zero.

#include <crd/core/types.hpp>
#include <crd/math/float_convert_scalar.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/libm.hpp>

namespace crd::hesap::tensor
{

// ggml-compatible block quantization (Q8_0 / Q4_0): layouts and arithmetic are exact transcriptions of ggml's
// quantize_row_{q8_0,q4_0}_ref / dequantize_row_* (ggml-org/ggml src/ggml-quants.c), so cooked weights interop
// byte for byte. Opaque blocked storage (ADR-0096 §2: no element strides).
inline constexpr crd::u32 kQuantBlock = 32U;

struct BlockQ8Zero
{
    crd::u16 d;     // f16-encoded scale (amax/127)
    crd::i8 qs[32]; // q[j] = roundf(x[j]/d)
};
static_assert(sizeof(BlockQ8Zero) == 34U, "BlockQ8Zero must match the ggml block_q8_0 layout");

struct BlockQ4Zero
{
    crd::u16 d;     // f16-encoded scale (signed-max/-8)
    crd::u8 qs[16]; // low nibble = element j, high nibble = element j+16; stored value = q+8
};
static_assert(sizeof(BlockQ4Zero) == 18U, "BlockQ4Zero must match the ggml block_q4_0 layout");

} // namespace crd::hesap::tensor

namespace crd::hesap::tensor::detail::inline CRD_MATH_LEVEL_NS
{

// Q8_0 over nblocks whole blocks of x: d = amax/127 (f16-encoded), q = roundf(x/d), the half-away form
// t >= 0 ? t + 0.5 : t - 0.5 truncated (exact for |t| <= 127).
inline void quantize_q8_0_blocks(const crd::f32* x, BlockQ8Zero* out, crd::u64 nblocks) noexcept
{
    for (crd::u64 i = 0; i < nblocks; ++i)
    {
        const crd::f32* blk = x + i * kQuantBlock;
        crd::f32 amax = 0.0F;
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::f32 a = blk[j] < 0.0F ? -blk[j] : blk[j];
            amax = a > amax ? a : amax;
        }
        const crd::f32 d = amax / 127.0F;
        const crd::f32 id = d != 0.0F ? 1.0F / d : 0.0F;
        out[i].d = crd::math::f32_to_f16_bits(d);
        for (crd::u32 j = 0; j < kQuantBlock; ++j)
        {
            const crd::f32 v = blk[j] * id;
            crd::i8 q = 0;
            if (!crd::math::libm::is_nan(v))
            {
                q = static_cast<crd::i8>(static_cast<crd::i32>(v >= 0.0F ? v + 0.5F : v - 0.5F));
            }
            out[i].qs[j] = q;
        }
    }
}

// The u8 zero point of the range [rmin, rmax] at scale s: round-to-nearest-even of -rmin/s in [0, 255]; 0 for an
// empty or undefined range.
[[nodiscard]] inline crd::i32 quantize_u8_zero_point(crd::f32 rmin, crd::f32 s) noexcept
{
    const crd::f32 z = s != 0.0F ? -rmin / s : 0.0F;
    crd::f32 c = 0.0F; // a NaN stays 0
    if (z > 0.0F)
    {
        c = z < 255.0F ? z : 255.0F;
    }
    return static_cast<crd::i32>(crd::math::libm::rint(c));
}

// Asymmetric u8 over n values (the ort DynamicQuantizeLinear convention): the range is 0-extended, s =
// (rmax - rmin)/255, the zero point as above; q = half-away(x/s) + zp clamped to [0, 255].
inline void quantize_u8_asym_row(const crd::f32* x, crd::u64 n, crd::u8* out, crd::f32& scale,
                                 crd::i32& zero_point) noexcept
{
    crd::f32 rmin = 0.0F;
    crd::f32 rmax = 0.0F;
    for (crd::u64 i = 0; i < n; ++i)
    {
        rmin = x[i] < rmin ? x[i] : rmin;
        rmax = x[i] > rmax ? x[i] : rmax;
    }
    const crd::f32 s = (rmax - rmin) / 255.0F;
    const crd::f32 inv = s != 0.0F ? 1.0F / s : 0.0F;
    const crd::i32 zp = quantize_u8_zero_point(rmin, s);
    for (crd::u64 i = 0; i < n; ++i)
    {
        const crd::f32 t = x[i] * inv;
        crd::i32 q = zp;
        if (!crd::math::libm::is_nan(t))
        {
            q += static_cast<crd::i32>(t >= 0.0F ? t + 0.5F : t - 0.5F);
        }
        q = q < 0 ? 0 : q;
        out[i] = static_cast<crd::u8>(q > 255 ? 255 : q);
    }
    scale = s;
    zero_point = zp;
}

} // namespace crd::hesap::tensor::detail::inline CRD_MATH_LEVEL_NS
