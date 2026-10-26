// The scalar small-float conversions (f16, bf16, FP8 e4m3fn and e5m2; round to nearest even and stochastic
// rounding) of crd/math/float_convert.hpp: the API for single values and the bit-exact reference of every batch kernel.
//
// General code and each conversion kernel unit compile these functions into their own inline namespace
// (CRD_MATH_LEVEL_NS, crd/math/kernel_level.hpp), so a unit built for a higher ISA never provides general code's copy.
// For the same reason the bit casts use the compiler builtin: std::bit_cast's instantiation would be one shared body.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::math::inline CRD_MATH_LEVEL_NS
{

namespace small_float
{

template <typename To, typename From> [[nodiscard]] constexpr To bits_as(From from) noexcept
{
    return __builtin_bit_cast(To, from);
}

// Generic small-float narrowing core (f32 → sign/EBITS/MBITS format, RNE).
// kFn = the e4m3fn convention (see the header comment).
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn> [[nodiscard]] constexpr crd::u32 narrow_rne(crd::u32 fbits) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1; // e4m3fn: S.1111.111
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;            // formats with inf only
    constexpr crd::u32 max_efield = Fn ? exp_all1 : exp_all1 - 1U;

    const crd::u32 sign = (fbits >> 31U) << (EBITS + MBITS);
    const crd::u32 fexp = (fbits >> 23U) & 0xFFU;
    const crd::u32 fman = fbits & 0x7FFFFFU;

    if (fexp == 0xFFU) // f32 inf / nan
    {
        if (fman != 0U)
        {
            if constexpr (Fn)
            {
                return sign | nan_fn; // e4m3fn has a single NaN pattern
            }
            else
            {
                // Payload-propagating quiet NaN (truncate + force quiet) — the exact
                // F16C VCVTPS2PH semantics, so the SIMD batch path is bit-identical
                // to this scalar on every input including NaN payloads.
                return sign | (exp_all1 << MBITS) | (1U << (MBITS - 1U)) | (fman >> (23U - MBITS));
            }
        }
        return Fn ? (sign | nan_fn) : (sign | inf_bits); // inf → inf (or NaN for fn)
    }
    if (fexp == 0U) // f32 subnormal — below every EBITS<8 target's range
    {
        return sign; // ±0
    }

    const crd::i32 e = static_cast<crd::i32>(fexp) - 127; // unbiased
    const crd::i32 te = e + static_cast<crd::i32>(bias);  // target biased exponent
    const crd::u32 sig24 = fman | 0x800000U;              // implicit 1

    if (te <= 0) // target subnormal (or underflow)
    {
        // Discard (23 - MBITS) + (1 - te) bits with RNE. Shift ≥ 25 → |x| is
        // below half the min subnormal → ±0.
        const crd::u32 shift = (23U - MBITS) + static_cast<crd::u32>(1 - te);
        if (shift > 24U)
        {
            return sign;
        }
        const crd::u32 keep = sig24 >> shift;
        const crd::u32 rem = sig24 & ((1U << shift) - 1U);
        const crd::u32 half = 1U << (shift - 1U);
        const crd::u32 r = keep + ((rem > half || (rem == half && (keep & 1U))) ? 1U : 0U);
        // r may equal 1<<MBITS: that IS the first normal (exp field 1, man 0) —
        // the packed encoding is contiguous, so plain addition is correct.
        return sign | r;
    }

    // Normal path: RNE on the discarded (23 - MBITS) bits; carry may bump te.
    constexpr crd::u32 shift = 23U - MBITS;
    crd::u32 keep = sig24 >> shift;
    const crd::u32 rem = sig24 & ((1U << shift) - 1U);
    constexpr crd::u32 half = 1U << (shift - 1U);
    keep += (rem > half || (rem == half && (keep & 1U))) ? 1U : 0U;
    crd::u32 t = static_cast<crd::u32>(te);
    if (keep == (2U << MBITS)) // mantissa carry: 10.00…0 → renormalize
    {
        keep >>= 1U;
        ++t;
    }
    const crd::u32 man = keep & man_all1;
    if (t > max_efield || (Fn && t == max_efield && man == man_all1))
    {
        return Fn ? (sign | nan_fn) : (sign | inf_bits); // overflow
    }
    return sign | (t << MBITS) | man;
}

// Exact widening (target → f32); e4m3fn special-cases its NaN-only top code.
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn> [[nodiscard]] constexpr crd::f32 widen_exact(crd::u32 tbits) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;

    const crd::u32 sign = (tbits >> (EBITS + MBITS)) & 1U;
    const crd::u32 texp = (tbits >> MBITS) & exp_all1;
    const crd::u32 tman = tbits & man_all1;

    crd::u32 out = sign << 31U;
    if (texp == exp_all1 && (!Fn || tman == man_all1)) // inf/nan field
    {
        if constexpr (Fn)
        {
            out |= 0x7FC00000U; // e4m3fn's single NaN → canonical quiet f32 NaN
        }
        else if (tman != 0U)
        {
            // Payload-propagating quiet NaN (shift payload up + force quiet) —
            // the exact VCVTPH2PS semantics, so the F16C widening batch path is
            // bit-identical to this scalar on every input, NaN payloads included.
            out |= 0x7F800000U | (1U << 22U) | (tman << (23U - MBITS));
        }
        else
        {
            out |= 0x7F800000U; // inf
        }
        return bits_as<crd::f32>(out);
    }
    if (texp == 0U)
    {
        if (tman == 0U)
        {
            return bits_as<crd::f32>(out); // ±0
        }
        // Subnormal: value = tman × 2^(1 − bias − MBITS). Renormalize into f32.
        crd::u32 m = tman;
        crd::i32 e = static_cast<crd::i32>(1U - bias) - static_cast<crd::i32>(MBITS);
        while ((m & (1U << MBITS)) == 0U)
        {
            m <<= 1U;
            --e;
        }
        m &= man_all1;
        out |= static_cast<crd::u32>(e + static_cast<crd::i32>(MBITS) + 127) << 23U;
        out |= m << (23U - MBITS);
        return bits_as<crd::f32>(out);
    }
    out |= static_cast<crd::u32>(static_cast<crd::i32>(texp) - static_cast<crd::i32>(bias) + 127) << 23U;
    out |= tman << (23U - MBITS);
    return bits_as<crd::f32>(out);
}

// Stochastic narrowing on the same parameterization. `rand` supplies the
// uniform draw (only its low discard-width bits are consumed): exact
// linear-grid SR — P(round up) = remainder / 2^shift — saturating at max
// finite. nan/inf inputs behave exactly as in narrow_rne.
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn>
[[nodiscard]] constexpr crd::u32 narrow_sr(crd::u32 fbits, crd::u32 rand) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 max_efield = Fn ? exp_all1 : exp_all1 - 1U;
    // Max finite pattern: for fn the all-ones exponent holds normals up to man_all1-1.
    constexpr crd::u32 max_finite = Fn ? ((exp_all1 << MBITS) | (man_all1 - 1U)) : ((max_efield << MBITS) | man_all1);

    const crd::u32 fexp = (fbits >> 23U) & 0xFFU;
    const crd::u32 sign = (fbits >> 31U) << (EBITS + MBITS);
    if (fexp == 0xFFU) // nan/inf: defer to the RNE handler (identical semantics)
    {
        return narrow_rne<EBITS, MBITS, Fn>(fbits);
    }
    if (fexp == 0U)
    {
        return sign; // f32 subnormal underflows (EBITS < 8 targets)
    }

    const crd::i32 e = static_cast<crd::i32>(fexp) - 127;
    const crd::i32 te = e + static_cast<crd::i32>(bias);
    const crd::u32 sig24 = (fbits & 0x7FFFFFU) | 0x800000U;

    crd::u32 result;
    if (te <= 0) // target-subnormal grid
    {
        const crd::u32 shift = (23U - MBITS) + static_cast<crd::u32>(1 - te);
        if (shift > 31U)
        {
            // 8+ binades below the subnormal floor: P(min subnormal) = sig24/2^shift,
            // staged through 64 bits (rand supplies its full 32 bits of resolution).
            if (shift > 55U)
            {
                return sign; // probability < 2^-31 territory — round to 0
            }
            const crd::u64 wide = (static_cast<crd::u64>(sig24) << 32U) >> (shift - 24U);
            const crd::u64 r = static_cast<crd::u64>(rand) << 24U; // uniform in [0, 2^56)
            return sign | ((wide + r) >> 56U != 0U ? 1U : 0U);
        }
        // Direct 32-bit form for every shift <= 31: P(round up) = remainder/2^shift
        // EXACTLY (r uniform over 2^shift values) — shared lane-for-lane with the
        // AVX2 batch narrower. sig24 + r < 2^24 + 2^31 fits u32.
        const crd::u32 r = rand & ((1U << shift) - 1U);
        result = (sig24 + r) >> shift; // may carry into the first normal — encoding is contiguous
    }
    else
    {
        constexpr crd::u32 shift = 23U - MBITS;
        const crd::u32 r = rand & ((1U << shift) - 1U);
        crd::u32 keep = (sig24 + r) >> shift;
        crd::u32 t = static_cast<crd::u32>(te);
        if (keep == (2U << MBITS))
        {
            keep >>= 1U;
            ++t;
        }
        const crd::u32 man = keep & man_all1;
        if (t > max_efield || (Fn && t == max_efield && man == man_all1))
        {
            return sign | max_finite; // SR saturates — never rounds a finite into inf/nan
        }
        result = (t << MBITS) | man;
    }
    if (result > max_finite)
    {
        result = max_finite;
    }
    return sign | result;
}

} // namespace small_float

// =======================================================================
// Scalar converts (bit-exact vs the ml_dtypes reference corpus)
// =======================================================================

[[nodiscard]] constexpr crd::u16 f32_to_f16_bits(crd::f32 x) noexcept
{
    return static_cast<crd::u16>(small_float::narrow_rne<5U, 10U, false>(small_float::bits_as<crd::u32>(x)));
}
[[nodiscard]] constexpr crd::f32 f16_bits_to_f32(crd::u16 h) noexcept
{
    return small_float::widen_exact<5U, 10U, false>(h);
}

// bf16: RNE is the classic carry trick on the low 16 bits (handles normals,
// subnormals, and overflow-to-inf in one add); NaN needs the quiet force.
[[nodiscard]] constexpr crd::u16 f32_to_bf16_bits(crd::f32 x) noexcept
{
    const crd::u32 bits = small_float::bits_as<crd::u32>(x);
    if ((bits & 0x7FFFFFFFU) > 0x7F800000U) // nan
    {
        return static_cast<crd::u16>((bits >> 16U) | 0x0040U); // keep payload, force quiet
    }
    const crd::u32 rounded = bits + 0x7FFFU + ((bits >> 16U) & 1U);
    return static_cast<crd::u16>(rounded >> 16U);
}
[[nodiscard]] constexpr crd::f32 bf16_bits_to_f32(crd::u16 h) noexcept
{
    return small_float::bits_as<crd::f32>(static_cast<crd::u32>(h) << 16U);
}

[[nodiscard]] constexpr crd::u8 f32_to_fp8_e4m3_bits(crd::f32 x) noexcept
{
    return static_cast<crd::u8>(small_float::narrow_rne<4U, 3U, true>(small_float::bits_as<crd::u32>(x)));
}
[[nodiscard]] constexpr crd::f32 fp8_e4m3_bits_to_f32(crd::u8 b) noexcept
{
    return small_float::widen_exact<4U, 3U, true>(b);
}
[[nodiscard]] constexpr crd::u8 f32_to_fp8_e5m2_bits(crd::f32 x) noexcept
{
    return static_cast<crd::u8>(small_float::narrow_rne<5U, 2U, false>(small_float::bits_as<crd::u32>(x)));
}
[[nodiscard]] constexpr crd::f32 fp8_e5m2_bits_to_f32(crd::u8 b) noexcept
{
    return small_float::widen_exact<5U, 2U, false>(b);
}

// =======================================================================
// Stochastic-rounding converts — the uniform draw is an ARGUMENT (pure bit
// math). Draw policy (who keys what) belongs to the caller.
// =======================================================================

[[nodiscard]] constexpr crd::u16 f32_to_f16_bits_sr(crd::f32 x, crd::u32 rand) noexcept
{
    return static_cast<crd::u16>(small_float::narrow_sr<5U, 10U, false>(small_float::bits_as<crd::u32>(x), rand));
}
[[nodiscard]] constexpr crd::u16 f32_to_bf16_bits_sr(crd::f32 x, crd::u32 rand) noexcept
{
    const crd::u32 bits = small_float::bits_as<crd::u32>(x);
    if ((bits & 0x7FFFFFFFU) >= 0x7F800000U)
    {
        return f32_to_bf16_bits(x); // nan/inf: RNE semantics
    }
    // bf16 grid = truncation of the low 16 bits; SR = add uniform-16-bit, truncate,
    // saturating at max finite (0x7F7F magnitude).
    const crd::u32 r = rand & 0xFFFFU;
    const crd::u32 sign = bits & 0x80000000U;
    const crd::u32 mag = (bits & 0x7FFFFFFFU) + r;
    crd::u32 h = mag >> 16U;
    if (h >= 0x7F80U)
    {
        h = 0x7F7FU; // saturate below inf
    }
    return static_cast<crd::u16>((sign >> 16U) | h);
}
[[nodiscard]] constexpr crd::u8 f32_to_fp8_e4m3_bits_sr(crd::f32 x, crd::u32 rand) noexcept
{
    return static_cast<crd::u8>(small_float::narrow_sr<4U, 3U, true>(small_float::bits_as<crd::u32>(x), rand));
}
[[nodiscard]] constexpr crd::u8 f32_to_fp8_e5m2_bits_sr(crd::f32 x, crd::u32 rand) noexcept
{
    return static_cast<crd::u8>(small_float::narrow_sr<5U, 2U, false>(small_float::bits_as<crd::u32>(x), rand));
}

} // namespace crd::math::inline CRD_MATH_LEVEL_NS
