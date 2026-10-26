// The batch small-float conversions of crd/math/float_convert.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), the scalar
// cores included, so no function of this unit is shared with general code or with another level. The portable body is
// the scalar loops; the AVX2 body converts eight lanes at a time with F16C and an AVX2 integer transcription of the
// scalar narrowing.

#include <crd/math/float_convert_kernels.hpp>
#include <crd/math/float_convert_scalar.hpp>

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
#include <cstring>
#include <immintrin.h>
#endif

namespace crd::math::inline CRD_MATH_LEVEL_NS
{
namespace
{

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
// 8-wide integer transcription of narrow_rne — same masks, shifts, and class
// blends, lane-for-lane. All intermediate values fit in signed 32-bit, so the
// signed AVX2 compares are exact.
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn> [[nodiscard]] __m256i narrow8_rne_avx2(__m256i fb) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1;
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;
    constexpr crd::u32 max_efield = Fn ? exp_all1 : exp_all1 - 1U;
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
    if constexpr (Fn)
    {
        const __m256i m_top = _mm256_cmpeq_epi32(te2, _mm256_set1_epi32(static_cast<int>(max_efield)));
        const __m256i m_mmax = _mm256_cmpeq_epi32(man, _mm256_set1_epi32(static_cast<int>(man_all1)));
        m_ovf = _mm256_or_si256(m_ovf, _mm256_and_si256(m_top, m_mmax));
    }
    __m256i res = _mm256_or_si256(_mm256_slli_epi32(te2, static_cast<int>(MBITS)), man);
    res = _mm256_blendv_epi8(res, _mm256_set1_epi32(static_cast<int>(Fn ? nan_fn : inf_bits)), m_ovf);

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
    if constexpr (Fn)
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
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn>
[[nodiscard]] __m256i narrow8_sr_avx2(__m256i fb, __m256i rnd, int& deep_mask) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1;
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;
    constexpr crd::u32 max_efield = Fn ? exp_all1 : exp_all1 - 1U;
    constexpr crd::u32 max_finite = Fn ? ((exp_all1 << MBITS) | (man_all1 - 1U)) : ((max_efield << MBITS) | man_all1);
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
    if constexpr (Fn)
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
    if constexpr (Fn)
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
void store8_u16(__m256i v, crd::u16* dst) noexcept
{
    const __m256i p = _mm256_packus_epi32(v, _mm256_setzero_si256());
    const __m256i q = _mm256_permute4x64_epi64(p, 0xD8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(q));
}

// Pack 8 per-lane u32 byte values to 8 contiguous bytes.
void store8_bytes(__m256i v, crd::u8* dst) noexcept
{
    const __m256i shuf = _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 4, 8, 12, -1,
                                          -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m256i p = _mm256_shuffle_epi8(v, shuf);
    const crd::u32 lo = static_cast<crd::u32>(_mm256_extract_epi32(p, 0));
    const crd::u32 hi = static_cast<crd::u32>(_mm256_extract_epi32(p, 4));
    std::memcpy(dst, &lo, 4U);
    std::memcpy(dst + 4, &hi, 4U);
}
#endif

// The batch kernels. Each is bit-identical to the scalar converts on every input; the vector parts end at a
// closed-form index, so a tail provably runs fewer than eight times (GCC's link-time ranges otherwise report
// -Waggressive-loop-optimizations on the tail of a constant-size call).

void f32_to_f16_n(const crd::f32* src, crd::u16* dst, crd::usize n) noexcept
{
    crd::usize i = 0;
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
    const crd::usize vector_end = n - (n % 8U);
    for (; i < vector_end; i += 8U)
    {
        const __m256 v = _mm256_loadu_ps(src + i);
        const __m128i h = _mm256_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT); // explicit RNE imm
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), h);
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = f32_to_f16_bits(src[i]);
    }
}

void f16_to_f32_n(const crd::u16* src, crd::f32* dst, crd::usize n) noexcept
{
    crd::usize i = 0;
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
    const crd::usize vector_end = n - (n % 8U);
    for (; i < vector_end; i += 8U)
    {
        const __m128i h = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        _mm256_storeu_ps(dst + i, _mm256_cvtph_ps(h));
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = f16_bits_to_f32(src[i]);
    }
}

void f32_to_bf16_n(const crd::f32* src, crd::u16* dst, crd::usize n) noexcept
{
    for (crd::usize i = 0; i < n; ++i) // branch-light carry trick: the compiler vectorizes it at the unit's level
    {
        dst[i] = f32_to_bf16_bits(src[i]);
    }
}

template <crd::u32 EBITS, crd::u32 MBITS, bool Fn>
void f32_to_fp8_n(const crd::f32* src, crd::u8* dst, crd::usize n) noexcept
{
    crd::usize i = 0;
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
    const crd::usize vector_end = n - (n % 8U);
    for (; i < vector_end; i += 8U)
    {
        const __m256i fb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        store8_bytes(narrow8_rne_avx2<EBITS, MBITS, Fn>(fb), dst + i);
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] =
            static_cast<crd::u8>(small_float::narrow_rne<EBITS, MBITS, Fn>(small_float::bits_as<crd::u32>(src[i])));
    }
}

// Stochastic rounding: rand[k] is element k's uniform draw (the caller owns the draw policy).
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn, typename Bits>
void f32_to_small_sr_n(const crd::f32* src, Bits* dst, const crd::u32* rand, crd::usize n) noexcept
{
    crd::usize i = 0;
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
    const crd::usize vector_end = n - (n % 8U);
    for (; i < vector_end; i += 8U)
    {
        const __m256i fb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        const __m256i rd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rand + i));
        int deep = 0;
        const __m256i r = narrow8_sr_avx2<EBITS, MBITS, Fn>(fb, rd, deep);
        if constexpr (sizeof(Bits) == 2U)
        {
            store8_u16(r, dst + i);
        }
        else
        {
            store8_bytes(r, dst + i);
        }
        while (deep != 0) // rare: 8+ binades below the subnormal floor, staged through 64 bits by the scalar core
        {
            const crd::usize lane = i + _tzcnt_u32(static_cast<crd::u32>(deep));
            dst[lane] = static_cast<Bits>(
                small_float::narrow_sr<EBITS, MBITS, Fn>(small_float::bits_as<crd::u32>(src[lane]), rand[lane]));
            deep &= deep - 1;
        }
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = static_cast<Bits>(
            small_float::narrow_sr<EBITS, MBITS, Fn>(small_float::bits_as<crd::u32>(src[i]), rand[i]));
    }
}

void f32_to_bf16_sr_n(const crd::f32* src, crd::u16* dst, const crd::u32* rand, crd::usize n) noexcept
{
    crd::usize i = 0;
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
    const crd::usize vector_end = n - (n % 8U);
    for (; i < vector_end; i += 8U)
    {
        const __m256i bits = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        const __m256i rd = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rand + i));
        const __m256i mag = _mm256_and_si256(bits, _mm256_set1_epi32(0x7FFFFFFF));
        const __m256i m_ni = _mm256_cmpgt_epi32(mag, _mm256_set1_epi32(0x7F7FFFFF)); // >= inf
        __m256i h = _mm256_srli_epi32(_mm256_add_epi32(mag, _mm256_and_si256(rd, _mm256_set1_epi32(0xFFFF))), 16);
        const __m256i m_sat = _mm256_cmpgt_epi32(h, _mm256_set1_epi32(0x7F7F));
        h = _mm256_blendv_epi8(h, _mm256_set1_epi32(0x7F7F), m_sat);
        const __m256i s = _mm256_and_si256(_mm256_srli_epi32(bits, 16), _mm256_set1_epi32(0x8000));
        store8_u16(_mm256_or_si256(s, h), dst + i);
        int ni = _mm256_movemask_ps(_mm256_castsi256_ps(m_ni));
        while (ni != 0) // nan/inf lanes: RNE semantics through the scalar path
        {
            const crd::usize lane = i + _tzcnt_u32(static_cast<crd::u32>(ni));
            dst[lane] = f32_to_bf16_bits_sr(src[lane], rand[lane]);
            ni &= ni - 1;
        }
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = f32_to_bf16_bits_sr(src[i], rand[i]);
    }
}

} // namespace

} // namespace crd::math::inline CRD_MATH_LEVEL_NS

namespace crd::math
{

const FloatConvertKernels CRD_KERNEL_TABLE(kFloatConvertKernels) = {
    &CRD_MATH_LEVEL_NS::f32_to_f16_n,
    &CRD_MATH_LEVEL_NS::f16_to_f32_n,
    &CRD_MATH_LEVEL_NS::f32_to_bf16_n,
    &CRD_MATH_LEVEL_NS::f32_to_fp8_n<4U, 3U, true>,
    &CRD_MATH_LEVEL_NS::f32_to_fp8_n<5U, 2U, false>,
    &CRD_MATH_LEVEL_NS::f32_to_small_sr_n<5U, 10U, false, crd::u16>,
    &CRD_MATH_LEVEL_NS::f32_to_bf16_sr_n,
    &CRD_MATH_LEVEL_NS::f32_to_small_sr_n<4U, 3U, true, crd::u8>,
    &CRD_MATH_LEVEL_NS::f32_to_small_sr_n<5U, 2U, false, crd::u8>,
};

} // namespace crd::math
