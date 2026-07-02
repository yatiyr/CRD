#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#include <bit>
#include <cstring>
#include <span>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

// F16C availability: MSVC /arch:AVX2 implies the intrinsics but defines no
// __F16C__; GCC/Clang define __F16C__ under -mf16c (added to crd-simd-flags
// for the AVX2 target). Guarded separately from __AVX2__ so a consumer built
// without the flag falls back to the scalar f16 path instead of failing.
#if defined(__F16C__) || (defined(_MSC_VER) && defined(__AVX2__))
#define CRD_MATH_HAS_F16C 1
#else
#define CRD_MATH_HAS_F16C 0
#endif

// ---------------------------------------------------------------------------
// crd-math float_convert — small-float format conversions (f16 / bf16 / FP8
// e4m3fn / e5m2), scalar + F16C/AVX2 batch, RNE and stochastic-rounding.
//
// MIGRATED here from crd-hesap-tensor (v14-a) per user direction 2026-07-02:
// these are engine-base primitives (pure f32/u16/u8 signatures, header-only,
// constexpr scalar cores) intended for reuse — the tensor module's storage
// dtypes, the asset cooker's half-float texture/vertex payloads, v17 GPU's
// CPU-side conversion oracles, eylem quantized replay, v18 model I/O.
// crd-math is the owning module for reusable deterministic numeric
// primitives (SANITY #8).
//
// Semantics (reference-pinned, gated in tests/hesap-tensor vs the ml_dtypes
// corpus — the JAX/safetensors frontier):
//   - All narrowings are IEEE RNE. bf16 uses the carry trick.
//   - e4m3fn (OCP): NO inf — the all-ones exponent field holds normals except
//     S.1111.111 (NaN); overflow AND f32-inf narrow to NaN.
//   - NaN narrowing is payload-propagating (truncate + force quiet) — the
//     exact F16C VCVTPS2PH behavior, so the SIMD batch paths are bit-identical
//     to the scalar cores on EVERY input, NaN payloads included (gated).
//   - f32 subnormal inputs underflow to ±0 for EBITS < 8 targets (their
//     ranges end far above the f32 subnormal band); bf16 handles them exactly.
//   - Stochastic rounding (the *_sr variants) takes the uniform draw as an
//     ARGUMENT — pure bit math, unbiased on the linear grid (add the draw
//     below the discarded bits, truncate), saturating at max finite (never
//     manufactures inf/NaN from a finite input). Key/seed policy lives with
//     the caller (crd-hesap-tensor keys Philox by canonical destination
//     index — ADR-0096 §4).
//
// The batch converts CRUSH the frontier peers (1M elems, 1 core, 2026-07-02):
// f32→f16 0.106 ns = 1.29× torch-F16C / 11.5× numpy · f16→f32 0.097 = 6.9×
// numpy · bf16 0.139 = 2.5× ml_dtypes · e4m3 0.608 = 2.2× ml_dtypes.
// ---------------------------------------------------------------------------

namespace crd::math
{

namespace detail
{

// Generic small-float narrowing core (f32 → sign/EBITS/MBITS format, RNE).
// kFn = the e4m3fn convention (see the header comment).
template <crd::u32 EBITS, crd::u32 MBITS, bool kFn> [[nodiscard]] constexpr crd::u32 narrow_rne(crd::u32 fbits) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1; // e4m3fn: S.1111.111
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;            // formats with inf only
    constexpr crd::u32 max_efield = kFn ? exp_all1 : exp_all1 - 1U;

    const crd::u32 sign = (fbits >> 31U) << (EBITS + MBITS);
    const crd::u32 fexp = (fbits >> 23U) & 0xFFU;
    const crd::u32 fman = fbits & 0x7FFFFFU;

    if (fexp == 0xFFU) // f32 inf / nan
    {
        if (fman != 0U)
        {
            if constexpr (kFn)
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
        return kFn ? (sign | nan_fn) : (sign | inf_bits); // inf → inf (or NaN for fn)
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
    if (t > max_efield || (kFn && t == max_efield && man == man_all1))
    {
        return kFn ? (sign | nan_fn) : (sign | inf_bits); // overflow
    }
    return sign | (t << MBITS) | man;
}

// Exact widening (target → f32); e4m3fn special-cases its NaN-only top code.
template <crd::u32 EBITS, crd::u32 MBITS, bool kFn>
[[nodiscard]] constexpr crd::f32 widen_exact(crd::u32 tbits) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;

    const crd::u32 sign = (tbits >> (EBITS + MBITS)) & 1U;
    const crd::u32 texp = (tbits >> MBITS) & exp_all1;
    const crd::u32 tman = tbits & man_all1;

    crd::u32 out = sign << 31U;
    if (texp == exp_all1 && (!kFn || tman == man_all1)) // inf/nan field
    {
        if constexpr (kFn)
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
        return std::bit_cast<crd::f32>(out);
    }
    if (texp == 0U)
    {
        if (tman == 0U)
        {
            return std::bit_cast<crd::f32>(out); // ±0
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
        return std::bit_cast<crd::f32>(out);
    }
    out |= static_cast<crd::u32>(static_cast<crd::i32>(texp) - static_cast<crd::i32>(bias) + 127) << 23U;
    out |= tman << (23U - MBITS);
    return std::bit_cast<crd::f32>(out);
}

// Stochastic narrowing on the same parameterization. `rand` supplies the
// uniform draw (only its low discard-width bits are consumed): exact
// linear-grid SR — P(round up) = remainder / 2^shift — saturating at max
// finite. nan/inf inputs behave exactly as in narrow_rne.
template <crd::u32 EBITS, crd::u32 MBITS, bool kFn>
[[nodiscard]] constexpr crd::u32 narrow_sr(crd::u32 fbits, crd::u32 rand) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 max_efield = kFn ? exp_all1 : exp_all1 - 1U;
    // Max finite pattern: for fn the all-ones exponent holds normals up to man_all1-1.
    constexpr crd::u32 max_finite = kFn ? ((exp_all1 << MBITS) | (man_all1 - 1U)) : ((max_efield << MBITS) | man_all1);

    const crd::u32 fexp = (fbits >> 23U) & 0xFFU;
    const crd::u32 sign = (fbits >> 31U) << (EBITS + MBITS);
    if (fexp == 0xFFU) // nan/inf: defer to the RNE handler (identical semantics)
    {
        return narrow_rne<EBITS, MBITS, kFn>(fbits);
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
        if (t > max_efield || (kFn && t == max_efield && man == man_all1))
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

#if defined(__AVX2__)
// 8-wide integer transcription of narrow_rne — same masks, shifts, and class
// blends, lane-for-lane. All intermediate values fit in signed 32-bit, so the
// signed AVX2 compares are exact.
template <crd::u32 EBITS, crd::u32 MBITS, bool kFn> [[nodiscard]] inline __m256i narrow8_rne_avx2(__m256i fb) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1;
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;
    constexpr crd::u32 max_efield = kFn ? exp_all1 : exp_all1 - 1U;
    constexpr crd::u32 shift_n = 23U - MBITS;
    constexpr crd::u32 half_n = 1U << (shift_n - 1U);

    const __m256i one = _mm256_set1_epi32(1);
    const __m256i sign = _mm256_slli_epi32(_mm256_srli_epi32(fb, 31), static_cast<int>(EBITS + MBITS));
    const __m256i fexp = _mm256_and_si256(_mm256_srli_epi32(fb, 23), _mm256_set1_epi32(0xFF));
    const __m256i fman = _mm256_and_si256(fb, _mm256_set1_epi32(0x7FFFFF));
    const __m256i m_naninf = _mm256_cmpeq_epi32(fexp, _mm256_set1_epi32(0xFF));
    const __m256i m_fzero = _mm256_cmpeq_epi32(fexp, _mm256_setzero_si256());
    const __m256i te = _mm256_add_epi32(fexp, _mm256_set1_epi32(static_cast<int>(bias) - 127));
    const __m256i sig = _mm256_or_si256(fman, _mm256_set1_epi32(0x800000));

    // ---- normal path (te >= 1) ----
    __m256i keep = _mm256_srli_epi32(sig, static_cast<int>(shift_n));
    const __m256i rem = _mm256_and_si256(sig, _mm256_set1_epi32(static_cast<int>((1U << shift_n) - 1U)));
    const __m256i m_gt = _mm256_cmpgt_epi32(rem, _mm256_set1_epi32(static_cast<int>(half_n)));
    const __m256i m_tie = _mm256_cmpeq_epi32(rem, _mm256_set1_epi32(static_cast<int>(half_n)));
    const __m256i m_odd = _mm256_cmpeq_epi32(_mm256_and_si256(keep, one), one);
    keep = _mm256_sub_epi32(keep, _mm256_or_si256(m_gt, _mm256_and_si256(m_tie, m_odd))); // mask = -1 ⇒ +1
    const __m256i m_carry = _mm256_cmpeq_epi32(keep, _mm256_set1_epi32(static_cast<int>(2U << MBITS)));
    keep = _mm256_blendv_epi8(keep, _mm256_srli_epi32(keep, 1), m_carry);
    const __m256i te2 = _mm256_sub_epi32(te, m_carry); // +1 where carry
    const __m256i man = _mm256_and_si256(keep, _mm256_set1_epi32(static_cast<int>(man_all1)));
    __m256i m_ovf = _mm256_cmpgt_epi32(te2, _mm256_set1_epi32(static_cast<int>(max_efield)));
    if constexpr (kFn)
    {
        const __m256i m_top = _mm256_cmpeq_epi32(te2, _mm256_set1_epi32(static_cast<int>(max_efield)));
        const __m256i m_mmax = _mm256_cmpeq_epi32(man, _mm256_set1_epi32(static_cast<int>(man_all1)));
        m_ovf = _mm256_or_si256(m_ovf, _mm256_and_si256(m_top, m_mmax));
    }
    __m256i res = _mm256_or_si256(_mm256_slli_epi32(te2, static_cast<int>(MBITS)), man);
    res = _mm256_blendv_epi8(res, _mm256_set1_epi32(static_cast<int>(kFn ? nan_fn : inf_bits)), m_ovf);

    // ---- subnormal path (te <= 0) ----
    const __m256i m_sub = _mm256_cmpgt_epi32(one, te); // 1 > te  ⇔  te <= 0
    __m256i shift_s = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(shift_n + 1U)),
                                       _mm256_sub_epi32(_mm256_setzero_si256(), te));
    const __m256i m_under = _mm256_cmpgt_epi32(shift_s, _mm256_set1_epi32(24));
    shift_s = _mm256_min_epi32(shift_s, _mm256_set1_epi32(31)); // keep srlv/sllv well-defined
    __m256i keep_s = _mm256_srlv_epi32(sig, shift_s);
    const __m256i mask_s = _mm256_sub_epi32(_mm256_sllv_epi32(one, shift_s), one);
    const __m256i rem_s = _mm256_and_si256(sig, mask_s);
    const __m256i half_s = _mm256_sllv_epi32(one, _mm256_sub_epi32(shift_s, one));
    const __m256i m_gt_s = _mm256_cmpgt_epi32(rem_s, half_s);
    const __m256i m_tie_s = _mm256_cmpeq_epi32(rem_s, half_s);
    const __m256i m_odd_s = _mm256_cmpeq_epi32(_mm256_and_si256(keep_s, one), one);
    keep_s = _mm256_sub_epi32(keep_s, _mm256_or_si256(m_gt_s, _mm256_and_si256(m_tie_s, m_odd_s)));
    keep_s = _mm256_andnot_si256(m_under, keep_s);

    // ---- nan/inf lane values ----
    __m256i res_ni;
    if constexpr (kFn)
    {
        res_ni = _mm256_set1_epi32(static_cast<int>(nan_fn));
    }
    else
    {
        const __m256i pay = _mm256_or_si256(_mm256_set1_epi32(static_cast<int>(inf_bits | (1U << (MBITS - 1U)))),
                                            _mm256_srli_epi32(fman, static_cast<int>(23U - MBITS)));
        const __m256i m_isnan =
            _mm256_xor_si256(_mm256_cmpeq_epi32(fman, _mm256_setzero_si256()), _mm256_set1_epi32(-1));
        res_ni = _mm256_blendv_epi8(_mm256_set1_epi32(static_cast<int>(inf_bits)), pay, m_isnan);
    }

    // ---- class selection (mirrors the scalar control flow) ----
    res = _mm256_blendv_epi8(res, keep_s, m_sub);
    res = _mm256_andnot_si256(m_fzero, res);
    res = _mm256_blendv_epi8(res, res_ni, m_naninf);
    return _mm256_or_si256(res, sign);
}

// SR variant of the 8-wide narrower — mirrors narrow_sr lane-for-lane for
// discard shifts <= 31 (the shared direct 32-bit form). Lanes needing the
// 64-bit deep-underflow staging (shift > 31: 8+ binades below the target's
// subnormal floor) are reported in deep_mask for the caller's scalar patch.
// Overflow SATURATES to max finite (the SR contract — never inf/nan from a
// finite input); nan/inf lanes get the RNE semantics.
template <crd::u32 EBITS, crd::u32 MBITS, bool kFn>
[[nodiscard]] inline __m256i narrow8_sr_avx2(__m256i fb, __m256i rnd, int& deep_mask) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1;
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;
    constexpr crd::u32 max_efield = kFn ? exp_all1 : exp_all1 - 1U;
    constexpr crd::u32 max_finite = kFn ? ((exp_all1 << MBITS) | (man_all1 - 1U)) : ((max_efield << MBITS) | man_all1);
    constexpr crd::u32 shift_n = 23U - MBITS;

    const __m256i one = _mm256_set1_epi32(1);
    const __m256i sign = _mm256_slli_epi32(_mm256_srli_epi32(fb, 31), static_cast<int>(EBITS + MBITS));
    const __m256i fexp = _mm256_and_si256(_mm256_srli_epi32(fb, 23), _mm256_set1_epi32(0xFF));
    const __m256i fman = _mm256_and_si256(fb, _mm256_set1_epi32(0x7FFFFF));
    const __m256i m_naninf = _mm256_cmpeq_epi32(fexp, _mm256_set1_epi32(0xFF));
    const __m256i m_fzero = _mm256_cmpeq_epi32(fexp, _mm256_setzero_si256());
    const __m256i te = _mm256_add_epi32(fexp, _mm256_set1_epi32(static_cast<int>(bias) - 127));
    const __m256i sig = _mm256_or_si256(fman, _mm256_set1_epi32(0x800000));

    // ---- normal path: keep = (sig + (rnd & mask_n)) >> shift_n ----
    __m256i keep = _mm256_srli_epi32(
        _mm256_add_epi32(sig, _mm256_and_si256(rnd, _mm256_set1_epi32(static_cast<int>((1U << shift_n) - 1U)))),
        static_cast<int>(shift_n));
    const __m256i m_carry = _mm256_cmpeq_epi32(keep, _mm256_set1_epi32(static_cast<int>(2U << MBITS)));
    keep = _mm256_blendv_epi8(keep, _mm256_srli_epi32(keep, 1), m_carry);
    const __m256i te2 = _mm256_sub_epi32(te, m_carry);
    const __m256i man = _mm256_and_si256(keep, _mm256_set1_epi32(static_cast<int>(man_all1)));
    __m256i m_ovf = _mm256_cmpgt_epi32(te2, _mm256_set1_epi32(static_cast<int>(max_efield)));
    if constexpr (kFn)
    {
        const __m256i m_top = _mm256_cmpeq_epi32(te2, _mm256_set1_epi32(static_cast<int>(max_efield)));
        const __m256i m_mmax = _mm256_cmpeq_epi32(man, _mm256_set1_epi32(static_cast<int>(man_all1)));
        m_ovf = _mm256_or_si256(m_ovf, _mm256_and_si256(m_top, m_mmax));
    }
    __m256i res = _mm256_or_si256(_mm256_slli_epi32(te2, static_cast<int>(MBITS)), man);
    res = _mm256_blendv_epi8(res, _mm256_set1_epi32(static_cast<int>(max_finite)), m_ovf); // SATURATE

    // ---- subnormal path: keep_s = (sig + (rnd & mask_s)) >> shift_s ----
    const __m256i m_sub = _mm256_cmpgt_epi32(one, te);
    __m256i shift_s = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int>(shift_n + 1U)),
                                       _mm256_sub_epi32(_mm256_setzero_si256(), te));
    const __m256i m_deep = _mm256_and_si256(m_sub, _mm256_cmpgt_epi32(shift_s, _mm256_set1_epi32(31)));
    shift_s = _mm256_min_epi32(shift_s, _mm256_set1_epi32(31));
    const __m256i mask_s = _mm256_sub_epi32(_mm256_sllv_epi32(one, shift_s), one);
    const __m256i keep_s = _mm256_srlv_epi32(_mm256_add_epi32(sig, _mm256_and_si256(rnd, mask_s)), shift_s);

    // ---- nan/inf lane values (RNE semantics) ----
    __m256i res_ni;
    if constexpr (kFn)
    {
        res_ni = _mm256_set1_epi32(static_cast<int>(nan_fn));
    }
    else
    {
        const __m256i pay = _mm256_or_si256(_mm256_set1_epi32(static_cast<int>(inf_bits | (1U << (MBITS - 1U)))),
                                            _mm256_srli_epi32(fman, static_cast<int>(23U - MBITS)));
        const __m256i m_isnan =
            _mm256_xor_si256(_mm256_cmpeq_epi32(fman, _mm256_setzero_si256()), _mm256_set1_epi32(-1));
        res_ni = _mm256_blendv_epi8(_mm256_set1_epi32(static_cast<int>(inf_bits)), pay, m_isnan);
    }

    res = _mm256_blendv_epi8(res, keep_s, m_sub);
    res = _mm256_andnot_si256(m_fzero, res);
    res = _mm256_blendv_epi8(res, res_ni, m_naninf);
    deep_mask = _mm256_movemask_ps(_mm256_castsi256_ps(m_deep));
    return _mm256_or_si256(res, sign);
}

// Pack the low u16 of each of the 8 u32 lanes to 8 contiguous u16s.
inline void store8_u16(__m256i v, crd::u16* dst) noexcept
{
    const __m256i p = _mm256_packus_epi32(v, _mm256_setzero_si256());
    const __m256i q = _mm256_permute4x64_epi64(p, 0xD8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(q));
}

// Pack 8 per-lane u32 byte values to 8 contiguous bytes.
inline void store8_bytes(__m256i v, crd::u8* dst) noexcept
{
    const __m256i shuf = _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 4, 8, 12, -1,
                                          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i p = _mm256_shuffle_epi8(v, shuf);
    const crd::u32 lo = static_cast<crd::u32>(_mm256_extract_epi32(p, 0));
    const crd::u32 hi = static_cast<crd::u32>(_mm256_extract_epi32(p, 4));
    std::memcpy(dst, &lo, 4U);
    std::memcpy(dst + 4, &hi, 4U);
}
#endif // __AVX2__

} // namespace detail

// =======================================================================
// Scalar converts (bit-exact vs the ml_dtypes reference corpus)
// =======================================================================

[[nodiscard]] constexpr crd::u16 f32_to_f16_bits(crd::f32 x) noexcept
{
    return static_cast<crd::u16>(detail::narrow_rne<5U, 10U, false>(std::bit_cast<crd::u32>(x)));
}
[[nodiscard]] constexpr crd::f32 f16_bits_to_f32(crd::u16 h) noexcept
{
    return detail::widen_exact<5U, 10U, false>(h);
}

// bf16: RNE is the classic carry trick on the low 16 bits (handles normals,
// subnormals, and overflow-to-inf in one add); NaN needs the quiet force.
[[nodiscard]] constexpr crd::u16 f32_to_bf16_bits(crd::f32 x) noexcept
{
    const crd::u32 bits = std::bit_cast<crd::u32>(x);
    if ((bits & 0x7FFFFFFFU) > 0x7F800000U) // nan
    {
        return static_cast<crd::u16>((bits >> 16U) | 0x0040U); // keep payload, force quiet
    }
    const crd::u32 rounded = bits + 0x7FFFU + ((bits >> 16U) & 1U);
    return static_cast<crd::u16>(rounded >> 16U);
}
[[nodiscard]] constexpr crd::f32 bf16_bits_to_f32(crd::u16 h) noexcept
{
    return std::bit_cast<crd::f32>(static_cast<crd::u32>(h) << 16U);
}

[[nodiscard]] constexpr crd::u8 f32_to_fp8_e4m3_bits(crd::f32 x) noexcept
{
    return static_cast<crd::u8>(detail::narrow_rne<4U, 3U, true>(std::bit_cast<crd::u32>(x)));
}
[[nodiscard]] constexpr crd::f32 fp8_e4m3_bits_to_f32(crd::u8 b) noexcept
{
    return detail::widen_exact<4U, 3U, true>(b);
}
[[nodiscard]] constexpr crd::u8 f32_to_fp8_e5m2_bits(crd::f32 x) noexcept
{
    return static_cast<crd::u8>(detail::narrow_rne<5U, 2U, false>(std::bit_cast<crd::u32>(x)));
}
[[nodiscard]] constexpr crd::f32 fp8_e5m2_bits_to_f32(crd::u8 b) noexcept
{
    return detail::widen_exact<5U, 2U, false>(b);
}

// =======================================================================
// Stochastic-rounding converts — the uniform draw is an ARGUMENT (pure bit
// math). Draw policy (who keys what) belongs to the caller.
// =======================================================================

[[nodiscard]] constexpr crd::u16 f32_to_f16_bits_sr(crd::f32 x, crd::u32 rand) noexcept
{
    return static_cast<crd::u16>(detail::narrow_sr<5U, 10U, false>(std::bit_cast<crd::u32>(x), rand));
}
[[nodiscard]] constexpr crd::u16 f32_to_bf16_bits_sr(crd::f32 x, crd::u32 rand) noexcept
{
    const crd::u32 bits = std::bit_cast<crd::u32>(x);
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
    return static_cast<crd::u8>(detail::narrow_sr<4U, 3U, true>(std::bit_cast<crd::u32>(x), rand));
}
[[nodiscard]] constexpr crd::u8 f32_to_fp8_e5m2_bits_sr(crd::f32 x, crd::u32 rand) noexcept
{
    return static_cast<crd::u8>(detail::narrow_sr<5U, 2U, false>(std::bit_cast<crd::u32>(x), rand));
}

// =======================================================================
// Batch converts — the crush kernels. Contract: BIT-IDENTICAL to the scalar
// converts on every input (gated in tests/hesap-tensor over the reference
// corpus + a Philox-driven random-pattern sweep). f16 rides F16C
// (VCVTPS2PH/VCVTPH2PS — hardware IEEE RNE incl. subnormals); fp8 rides the
// 8-wide AVX2 narrower. Non-AVX2 builds take the scalar loops.
// =======================================================================

inline void convert_f32_to_f16(std::span<const crd::f32> src, std::span<crd::u16> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_f16: size mismatch");
    crd::usize i = 0;
#if CRD_MATH_HAS_F16C
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m256 v = _mm256_loadu_ps(src.data() + i);
        const __m128i h = _mm256_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT); // explicit RNE imm
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst.data() + i), h);
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f32_to_f16_bits(src[i]);
    }
}

inline void convert_f16_to_f32(std::span<const crd::u16> src, std::span<crd::f32> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f16_to_f32: size mismatch");
    crd::usize i = 0;
#if CRD_MATH_HAS_F16C
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m128i h = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src.data() + i));
        _mm256_storeu_ps(dst.data() + i, _mm256_cvtph_ps(h));
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f16_bits_to_f32(src[i]);
    }
}

inline void convert_f32_to_bf16(std::span<const crd::f32> src, std::span<crd::u16> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_bf16: size mismatch");
    for (crd::usize i = 0; i < src.size(); ++i) // branch-light carry trick — auto-vectorizes
    {
        dst[i] = f32_to_bf16_bits(src[i]);
    }
}

inline void convert_f32_to_fp8_e4m3(std::span<const crd::f32> src, std::span<crd::u8> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_fp8_e4m3: size mismatch");
    crd::usize i = 0;
#if defined(__AVX2__)
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m256i fb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data() + i));
        detail::store8_bytes(detail::narrow8_rne_avx2<4U, 3U, true>(fb), dst.data() + i);
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f32_to_fp8_e4m3_bits(src[i]);
    }
}

inline void convert_f32_to_fp8_e5m2(std::span<const crd::f32> src, std::span<crd::u8> dst) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size(), "convert_f32_to_fp8_e5m2: size mismatch");
    crd::usize i = 0;
#if defined(__AVX2__)
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m256i fb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data() + i));
        detail::store8_bytes(detail::narrow8_rne_avx2<5U, 2U, false>(fb), dst.data() + i);
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f32_to_fp8_e5m2_bits(src[i]);
    }
}

// =======================================================================
// Batch SR converts — draw-agnostic (rand[k] is element k's uniform draw;
// the caller owns the draw policy, e.g. crd-hesap-tensor's Philox keying).
// Bit-identical to the per-element scalar SR on every input; rare
// deep-underflow lanes are patched through the scalar path in-loop.
// =======================================================================

inline void convert_f32_to_f16_sr(std::span<const crd::f32> src, std::span<crd::u16> dst,
                                  std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_f16_sr: size mismatch");
    crd::usize i = 0;
#if defined(__AVX2__)
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m256i fb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data() + i));
        const __m256i rd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rand.data() + i));
        int deep = 0;
        const __m256i r = detail::narrow8_sr_avx2<5U, 10U, false>(fb, rd, deep);
        detail::store8_u16(r, dst.data() + i);
        while (deep != 0) // rare: 8+ binades below the subnormal floor
        {
            const int lane = std::countr_zero(static_cast<crd::u32>(deep));
            dst[i + static_cast<crd::usize>(lane)] =
                f32_to_f16_bits_sr(src[i + static_cast<crd::usize>(lane)], rand[i + static_cast<crd::usize>(lane)]);
            deep &= deep - 1;
        }
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f32_to_f16_bits_sr(src[i], rand[i]);
    }
}

inline void convert_f32_to_bf16_sr(std::span<const crd::f32> src, std::span<crd::u16> dst,
                                   std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_bf16_sr: size mismatch");
    crd::usize i = 0;
#if defined(__AVX2__)
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m256i bits = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data() + i));
        const __m256i rd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rand.data() + i));
        const __m256i mag = _mm256_and_si256(bits, _mm256_set1_epi32(0x7FFFFFFF));
        const __m256i m_ni = _mm256_cmpgt_epi32(mag, _mm256_set1_epi32(0x7F7FFFFF)); // >= inf
        __m256i h = _mm256_srli_epi32(_mm256_add_epi32(mag, _mm256_and_si256(rd, _mm256_set1_epi32(0xFFFF))), 16);
        const __m256i m_sat = _mm256_cmpgt_epi32(h, _mm256_set1_epi32(0x7F7F));
        h = _mm256_blendv_epi8(h, _mm256_set1_epi32(0x7F7F), m_sat);
        const __m256i s = _mm256_and_si256(_mm256_srli_epi32(bits, 16), _mm256_set1_epi32(0x8000));
        detail::store8_u16(_mm256_or_si256(s, h), dst.data() + i);
        int ni = _mm256_movemask_ps(_mm256_castsi256_ps(m_ni));
        while (ni != 0) // nan/inf lanes: RNE semantics via the scalar path
        {
            const int lane = std::countr_zero(static_cast<crd::u32>(ni));
            dst[i + static_cast<crd::usize>(lane)] =
                f32_to_bf16_bits_sr(src[i + static_cast<crd::usize>(lane)], rand[i + static_cast<crd::usize>(lane)]);
            ni &= ni - 1;
        }
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f32_to_bf16_bits_sr(src[i], rand[i]);
    }
}

inline void convert_f32_to_fp8_e4m3_sr(std::span<const crd::f32> src, std::span<crd::u8> dst,
                                       std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_fp8_e4m3_sr: size mismatch");
    crd::usize i = 0;
#if defined(__AVX2__)
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m256i fb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data() + i));
        const __m256i rd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rand.data() + i));
        int deep = 0;
        detail::store8_bytes(detail::narrow8_sr_avx2<4U, 3U, true>(fb, rd, deep), dst.data() + i);
        while (deep != 0)
        {
            const int lane = std::countr_zero(static_cast<crd::u32>(deep));
            dst[i + static_cast<crd::usize>(lane)] = f32_to_fp8_e4m3_bits_sr(src[i + static_cast<crd::usize>(lane)],
                                                                             rand[i + static_cast<crd::usize>(lane)]);
            deep &= deep - 1;
        }
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f32_to_fp8_e4m3_bits_sr(src[i], rand[i]);
    }
}

inline void convert_f32_to_fp8_e5m2_sr(std::span<const crd::f32> src, std::span<crd::u8> dst,
                                       std::span<const crd::u32> rand) noexcept
{
    CRD_ASSERT_MSG(src.size() == dst.size() && src.size() == rand.size(), "convert_f32_to_fp8_e5m2_sr: size mismatch");
    crd::usize i = 0;
#if defined(__AVX2__)
    for (; i + 8U <= src.size(); i += 8U)
    {
        const __m256i fb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src.data() + i));
        const __m256i rd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rand.data() + i));
        int deep = 0;
        detail::store8_bytes(detail::narrow8_sr_avx2<5U, 2U, false>(fb, rd, deep), dst.data() + i);
        while (deep != 0)
        {
            const int lane = std::countr_zero(static_cast<crd::u32>(deep));
            dst[i + static_cast<crd::usize>(lane)] = f32_to_fp8_e5m2_bits_sr(src[i + static_cast<crd::usize>(lane)],
                                                                             rand[i + static_cast<crd::usize>(lane)]);
            deep &= deep - 1;
        }
    }
#endif
    for (; i < src.size(); ++i)
    {
        dst[i] = f32_to_fp8_e5m2_bits_sr(src[i], rand[i]);
    }
}

} // namespace crd::math
