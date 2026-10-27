#pragma once

// The Tier R binned accumulator (ADR-0096 §4): a faithful fold-3 transcription of ReproBLAS v2.1.0 (binned.h +
// src/binned/{dindex,dmbins,dmdupdate,dmddeposit,dmrenorm,dmdmadd,ddmconv}.c, external/reproblas, SHA dfb8150).
// Deposits are order-free (the sticky-bit two-sum ladder), the representation is partition-independent, and the
// read-out rounds once at the end.
//
// General code (reduce.hpp) and the per-level tensor kernels (../../../../kernels/tensor_kernels.cpp) both compile
// these operations, so they live in the unit's level namespace (CRD_MATH_LEVEL_NS, ADR-0134 §5) and use the bit-cast
// builtin, not std::bit_cast's shared instantiation. BinnedRungs is one plain type on every level.

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::tensor::detail
{

inline constexpr crd::i32 kBinWidth = 40;                 // DBWIDTH
inline constexpr crd::i32 kBinFold = 3;                   // the default fold
inline constexpr crd::i32 kBinMaxIndex = 51;              // ((1024+1021+52)/40)-1
inline constexpr crd::i32 kBinMaxFold = kBinMaxIndex + 1; // binned_DBMAXFOLD
inline constexpr crd::u64 kBinEndurance = 1ULL << 11U;    // 1<<(53-40-2)
inline constexpr crd::f64 kBinCompression = 1.0 / (1 << 14);
inline constexpr crd::f64 kBinExpansion = 1.0 * (1 << 14);

// The fold-3 accumulator: a primary and a carry per rung. All zero (value-initialised) is the empty sum.
struct BinnedRungs
{
    crd::f64 pri[kBinFold];
    crd::f64 car[kBinFold];
};

inline namespace CRD_MATH_LEVEL_NS
{

// 0.75 × 2^k as bits (1.5 × 2^(k-1): exponent field k-1+1023, mantissa 0.5).
[[nodiscard]] constexpr crd::f64 pow2_075(crd::i32 k) noexcept
{
    const crd::u64 bits = (static_cast<crd::u64>(k - 1 + 1023) << 52U) | 0x8000000000000ULL;
    return __builtin_bit_cast(crd::f64, bits);
}

// The bin ladder (binned_dmbins): bins[0] = 1.5×2^1023; bins[i>=1] = 0.75×2^(1038 - 40i); the tail repeats the last
// finite rung.
[[nodiscard]] inline crd::f64 bin_value(crd::i32 index) noexcept
{
    if (index <= 0)
    {
        return pow2_075(1024); // 2·0.75·2^1023
    }
    const crd::i32 i = index > kBinMaxIndex ? kBinMaxIndex : index;
    return pow2_075(1024 + 53 - kBinWidth + 1 - i * kBinWidth);
}

[[nodiscard]] inline crd::u32 f64_biased_exp(crd::f64 x) noexcept
{
    return static_cast<crd::u32>((__builtin_bit_cast(crd::u64, x) >> 52U) & 0x7FFU);
}

[[nodiscard]] inline bool f64_nan_or_inf(crd::f64 x) noexcept
{
    return f64_biased_exp(x) == 0x7FFU;
}

// x with its lowest mantissa bit set: the sticky bit of the deposit ladder.
[[nodiscard]] inline crd::f64 f64_sticky(crd::f64 x) noexcept
{
    return __builtin_bit_cast(crd::f64, __builtin_bit_cast(crd::u64, x) | 1ULL);
}

// binned_dindex: the ladder bucket of |X|.
[[nodiscard]] inline crd::i32 bin_index_of(crd::f64 x) noexcept
{
    const crd::u32 e = f64_biased_exp(x);
    if (e == 0U)
    {
        return kBinMaxIndex; // zero/denormal: the bottom bucket (their MIN() clamp)
    }
    return (2047 - static_cast<crd::i32>(e)) / kBinWidth; // (DBL_MAX_EXP + EXP_BIAS - exp)/W
}

// binned_dmindex: the index OF an accumulator, from its top primary.
[[nodiscard]] inline crd::i32 bin_index_of_acc(crd::f64 pri0) noexcept
{
    return (1024 + 53 - kBinWidth + 1 + 1023 - static_cast<crd::i32>(f64_biased_exp(pri0))) / kBinWidth;
}

// binned_dmdupdate: make the index suitable for |x| <= max_abs.
inline void binned_update(BinnedRungs& b, crd::f64 max_abs) noexcept
{
    if (f64_nan_or_inf(b.pri[0]))
    {
        return;
    }
    const crd::i32 x_index = bin_index_of(max_abs);
    if (b.pri[0] == 0.0)
    {
        for (crd::i32 i = 0; i < kBinFold; ++i)
        {
            b.pri[i] = bin_value(x_index + i);
            b.car[i] = 0.0;
        }
        return;
    }
    const crd::i32 shift = bin_index_of_acc(b.pri[0]) - x_index;
    if (shift > 0)
    {
        crd::i32 i = kBinFold - 1;
        for (; i >= shift; --i)
        {
            b.pri[i] = b.pri[i - shift];
            b.car[i] = b.car[i - shift];
        }
        for (crd::i32 j = 0; j < i + 1; ++j)
        {
            b.pri[j] = bin_value(x_index + j);
            b.car[j] = 0.0;
        }
    }
}

// binned_dmddeposit: the sticky-bit two-sum ladder (order-free).
inline void binned_deposit(BinnedRungs& b, crd::f64 x) noexcept
{
    if (f64_nan_or_inf(x) || f64_nan_or_inf(b.pri[0]))
    {
        b.pri[0] += x;
        return;
    }
    crd::i32 first = 0;
    if (bin_index_of_acc(b.pri[0]) == 0) // the index-0 compression branch
    {
        crd::f64 m = b.pri[0];
        const crd::f64 qd = f64_sticky(x * kBinCompression) + m;
        b.pri[0] = qd;
        m -= qd;
        m *= kBinExpansion * 0.5;
        x += m;
        x += m;
        first = 1;
    }
    for (crd::i32 i = first; i < kBinFold - 1; ++i)
    {
        crd::f64 m = b.pri[i];
        const crd::f64 qd = f64_sticky(x) + m;
        b.pri[i] = qd;
        m -= qd;
        x += m;
    }
    b.pri[kBinFold - 1] += f64_sticky(x);
}

// binned_dmrenorm (the bit-twiddled form): pull the top 2 mantissa bits into the carry and re-center the primary.
// Call at least every kBinEndurance deposits and before add/convert.
inline void binned_renorm(BinnedRungs& b) noexcept
{
    if (b.pri[0] == 0.0 || f64_nan_or_inf(b.pri[0]))
    {
        return;
    }
    for (crd::i32 i = 0; i < kBinFold; ++i)
    {
        crd::u64 bits = __builtin_bit_cast(crd::u64, b.pri[i]);
        b.car[i] += static_cast<crd::f64>(static_cast<crd::i32>((bits >> 50U) & 3ULL) - 2);
        bits &= ~(1ULL << 50U);
        bits |= 1ULL << 51U;
        b.pri[i] = __builtin_bit_cast(crd::f64, bits);
    }
}

// binned_dmdmadd: b += other (index-aligned merge, then renorm).
inline void binned_add(BinnedRungs& b, const BinnedRungs& other) noexcept
{
    if (other.pri[0] == 0.0)
    {
        return;
    }
    if (b.pri[0] == 0.0)
    {
        b = other;
        return;
    }
    if (f64_nan_or_inf(other.pri[0]) || f64_nan_or_inf(b.pri[0]))
    {
        b.pri[0] += other.pri[0];
        return;
    }
    const crd::i32 x_index = bin_index_of_acc(other.pri[0]);
    const crd::i32 y_index = bin_index_of_acc(b.pri[0]);
    const crd::i32 shift = y_index - x_index;
    if (shift > 0)
    {
        // shift b upwards and add other
        for (crd::i32 i = kBinFold - 1; i >= shift; --i)
        {
            b.pri[i] = other.pri[i] + (b.pri[i - shift] - bin_value(y_index + (i - shift)));
            b.car[i] = other.car[i] + b.car[i - shift];
        }
        for (crd::i32 i = 0; i < shift && i < kBinFold; ++i)
        {
            b.pri[i] = other.pri[i];
            b.car[i] = other.car[i];
        }
    }
    else
    {
        // shift other upwards and add onto b
        for (crd::i32 i = -shift; i < kBinFold; ++i)
        {
            b.pri[i] += other.pri[i + shift] - bin_value(x_index + (i + shift));
            b.car[i] += other.car[i + shift];
        }
    }
    binned_renorm(b);
}

// binned_ddmconv: read out the represented value (rounds ONCE, here).
[[nodiscard]] inline crd::f64 binned_convert(const BinnedRungs& b) noexcept
{
    if (f64_nan_or_inf(b.pri[0]))
    {
        // A NaN's payload and sign depend on which deposit or merge met it first: one quiet NaN keeps the read-out
        // independent of the partition and of the kernel level. Infinities are exact.
        const bool nan = (__builtin_bit_cast(crd::u64, b.pri[0]) & 0x000FFFFFFFFFFFFFULL) != 0U;
        return nan ? __builtin_bit_cast(crd::f64, 0x7FF8000000000000ULL) : b.pri[0];
    }
    if (b.pri[0] == 0.0)
    {
        return 0.0;
    }
    crd::f64 y = 0.0;
    const crd::i32 x_index = bin_index_of_acc(b.pri[0]);
    if (x_index <= (3 * 53) / kBinWidth)
    {
        // near the overflow end: scaled accumulation (their scale branch)
        const crd::f64 scale_down = __builtin_bit_cast(crd::f64, static_cast<crd::u64>(-65 + 1023) << 52U); // 2^-65
        const crd::f64 scale_up = __builtin_bit_cast(crd::f64, static_cast<crd::u64>(65 + 1023) << 52U);    // 2^65
        const crd::i32 want = (3 * 53) / kBinWidth - x_index;
        crd::i32 scaled = want > kBinFold ? kBinFold : want;
        scaled = scaled < 0 ? 0 : scaled;
        crd::i32 i;
        if (x_index == 0)
        {
            y += b.car[0] * ((bin_value(x_index + 0) / 6.0) * scale_down * kBinExpansion);
            y += b.car[1] * ((bin_value(x_index + 1) / 6.0) * scale_down);
            y += (b.pri[0] - bin_value(x_index + 0)) * scale_down * kBinExpansion;
            i = 2;
        }
        else
        {
            y += b.car[0] * ((bin_value(x_index + 0) / 6.0) * scale_down);
            i = 1;
        }
        for (; i < scaled; ++i)
        {
            y += b.car[i] * ((bin_value(x_index + i) / 6.0) * scale_down);
            y += (b.pri[i - 1] - bin_value(x_index + i - 1)) * scale_down;
        }
        if (i == kBinFold)
        {
            y += (b.pri[kBinFold - 1] - bin_value(x_index + kBinFold - 1)) * scale_down;
            return y * scale_up;
        }
        if (f64_nan_or_inf(y * scale_up))
        {
            return y * scale_up;
        }
        y *= scale_up;
        for (; i < kBinFold; ++i)
        {
            y += b.car[i] * (bin_value(x_index + i) / 6.0);
            y += b.pri[i - 1] - bin_value(x_index + i - 1);
        }
        y += b.pri[kBinFold - 1] - bin_value(x_index + kBinFold - 1);
        return y;
    }
    y += b.car[0] * (bin_value(x_index + 0) / 6.0);
    for (crd::i32 i = 1; i < kBinFold; ++i)
    {
        y += b.car[i] * (bin_value(x_index + i) / 6.0);
        y += b.pri[i - 1] - bin_value(x_index + i - 1);
    }
    y += b.pri[kBinFold - 1] - bin_value(x_index + kBinFold - 1);
    return y;
}

// The scalar sum of a contiguous run: per-endurance-block max update, deposits and renorm (the binnedBLAS dbdsum
// protocol).
inline void binned_sum_run_scalar(BinnedRungs& acc, const crd::f64* p, crd::u64 n) noexcept
{
    crd::u64 i = 0;
    while (i < n)
    {
        const crd::u64 len = n - i < kBinEndurance ? n - i : kBinEndurance;
        crd::f64 amax = 0.0;
        for (crd::u64 k = 0; k < len; ++k)
        {
            const crd::f64 a = p[i + k] < 0.0 ? -p[i + k] : p[i + k];
            amax = a > amax ? a : amax;
        }
        binned_update(acc, amax);
        for (crd::u64 k = 0; k < len; ++k)
        {
            binned_deposit(acc, p[i + k]);
        }
        binned_renorm(acc);
        i += len;
    }
}

} // namespace CRD_MATH_LEVEL_NS

} // namespace crd::hesap::tensor::detail
