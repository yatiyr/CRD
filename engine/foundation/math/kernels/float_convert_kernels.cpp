// The batch small-float conversions of crd/math/float_convert.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), the scalar
// cores included, so no function of this unit is shared with general code or with another level. The portable body is
// the scalar loops. The vector bodies run one integer transcription of the scalar narrowing over the level's i32
// lanes (I32Lanes: sixteen on AVX-512 with opmask lane masks, eight on AVX2, four on SSE2 and SSE4.2, with all-ones
// lane masks); AVX-512 and AVX2 convert f16 with VCVTPS2PH and VCVTPH2PS, and SSE2 and SSE4.2 narrow f16 through the
// same transcription and widen it with integer operations. AVX-512 ends each call with one masked step instead of a
// scalar tail.

#include <crd/math/float_convert_kernels.hpp>
#include <crd/math/float_convert_scalar.hpp>
#include <crd/math/simd/backend.hpp>

#include <cstring>

namespace crd::math::inline CRD_MATH_LEVEL_NS
{
namespace
{

#if CRD_SIMD_HAS_AVX512
// The level's i32 lanes and their opmask lane masks. srlv and pow2 take shifts in [1, 31] and [0, 31] (other lanes get
// defined garbage).
struct I32Lanes
{
    using V = __m512i;
    using M = __mmask16;
    static constexpr crd::usize kLanes = 16U;
    static V set1(crd::u32 v) noexcept { return _mm512_set1_epi32(static_cast<int>(v)); }
    static V zero() noexcept { return _mm512_setzero_si512(); }
    static V add(V a, V b) noexcept { return _mm512_add_epi32(a, b); }
    static V sub(V a, V b) noexcept { return _mm512_sub_epi32(a, b); }
    static V bit_and(V a, V b) noexcept { return _mm512_and_si512(a, b); }
    static V bit_or(V a, V b) noexcept { return _mm512_or_si512(a, b); }
    static M cmpeq(V a, V b) noexcept { return _mm512_cmpeq_epi32_mask(a, b); }
    static M cmpgt(V a, V b) noexcept { return _mm512_cmpgt_epi32_mask(a, b); }
    static M mask_or(M a, M b) noexcept { return static_cast<M>(a | b); }
    static M mask_and(M a, M b) noexcept { return static_cast<M>(a & b); }
    static M mask_andnot(M a, M b) noexcept // ~a & b
    {
        return static_cast<M>(~a & b);
    }
    static M mask_not(M a) noexcept { return static_cast<M>(~a); }
    template <int N> static V srli(V a) noexcept { return _mm512_srli_epi32(a, N); }
    template <int N> static V slli(V a) noexcept { return _mm512_slli_epi32(a, N); }
    static V select(V a, V b, M m) noexcept // m ? b : a
    {
        return _mm512_mask_blend_epi32(m, a, b);
    }
    static V inc(V a, M m) noexcept // a + 1 where m
    {
        return _mm512_mask_add_epi32(a, m, a, _mm512_set1_epi32(1));
    }
    static V zero_where(M m, V a) noexcept { return _mm512_mask_mov_epi32(a, m, _mm512_setzero_si512()); }
    static V min(V a, V b) noexcept { return _mm512_min_epi32(a, b); }
    static V srlv(V a, V s) noexcept { return _mm512_srlv_epi32(a, s); }
    static V pow2(V s) noexcept { return _mm512_sllv_epi32(_mm512_set1_epi32(1), s); }
    static int movemask(M m) noexcept { return static_cast<int>(m); }
    // The first min(count, 16) lanes, and the masked loads and narrowing stores of a call's last step.
    static M first(crd::usize count) noexcept { return static_cast<M>((1U << (count < kLanes ? count : kLanes)) - 1U); }
    static V load(const void* p, M m) noexcept { return _mm512_maskz_loadu_epi32(m, p); }
    static void store_u16(V v, crd::u16* dst, M m) noexcept { _mm512_mask_cvtepi32_storeu_epi16(dst, m, v); }
    static void store_bytes(V v, crd::u8* dst, M m) noexcept { _mm512_mask_cvtepi32_storeu_epi8(dst, m, v); }
};
#elif CRD_SIMD_HAS_AVX2
// The level's i32 lanes, all-ones lane masks. srlv and pow2 take shifts in [1, 31] and [0, 31] (other lanes get defined
// garbage).
struct I32Lanes
{
    using V = __m256i;
    using M = V;
    static constexpr crd::usize kLanes = 8U;
    static V load(const void* p) noexcept { return _mm256_loadu_si256(static_cast<const __m256i*>(p)); }
    static V set1(crd::u32 v) noexcept { return _mm256_set1_epi32(static_cast<int>(v)); }
    static V zero() noexcept { return _mm256_setzero_si256(); }
    static V add(V a, V b) noexcept { return _mm256_add_epi32(a, b); }
    static V sub(V a, V b) noexcept { return _mm256_sub_epi32(a, b); }
    static V bit_and(V a, V b) noexcept { return _mm256_and_si256(a, b); }
    static V bit_or(V a, V b) noexcept { return _mm256_or_si256(a, b); }
    static M cmpeq(V a, V b) noexcept { return _mm256_cmpeq_epi32(a, b); }
    static M cmpgt(V a, V b) noexcept { return _mm256_cmpgt_epi32(a, b); }
    static M mask_or(M a, M b) noexcept { return _mm256_or_si256(a, b); }
    static M mask_and(M a, M b) noexcept { return _mm256_and_si256(a, b); }
    static M mask_andnot(M a, M b) noexcept // ~a & b
    {
        return _mm256_andnot_si256(a, b);
    }
    static M mask_not(M a) noexcept { return _mm256_xor_si256(a, _mm256_set1_epi32(-1)); }
    template <int N> static V srli(V a) noexcept { return _mm256_srli_epi32(a, N); }
    template <int N> static V slli(V a) noexcept { return _mm256_slli_epi32(a, N); }
    static V select(V a, V b, M m) noexcept // m ? b : a
    {
        return _mm256_blendv_epi8(a, b, m);
    }
    static V inc(V a, M m) noexcept // a + 1 where m (an all-ones lane is -1)
    {
        return _mm256_sub_epi32(a, m);
    }
    static V zero_where(M m, V a) noexcept { return _mm256_andnot_si256(m, a); }
    static V min(V a, V b) noexcept { return _mm256_min_epi32(a, b); }
    static V srlv(V a, V s) noexcept { return _mm256_srlv_epi32(a, s); }
    static V pow2(V s) noexcept { return _mm256_sllv_epi32(_mm256_set1_epi32(1), s); }
    static int movemask(M m) noexcept { return _mm256_movemask_ps(_mm256_castsi256_ps(m)); }
    // The low u16 of each lane to eight contiguous u16s.
    static void store_u16(V v, crd::u16* dst) noexcept
    {
        const __m256i p = _mm256_packus_epi32(v, _mm256_setzero_si256());
        const __m256i q = _mm256_permute4x64_epi64(p, 0xD8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(q));
    }
    // Each lane's byte value to eight contiguous bytes.
    static void store_bytes(V v, crd::u8* dst) noexcept
    {
        const __m256i shuf = _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 4, 8, 12,
                                              -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m256i p = _mm256_shuffle_epi8(v, shuf);
        const crd::u32 lo = static_cast<crd::u32>(_mm256_extract_epi32(p, 0));
        const crd::u32 hi = static_cast<crd::u32>(_mm256_extract_epi32(p, 4));
        std::memcpy(dst, &lo, 4U);
        std::memcpy(dst + 4, &hi, 4U);
    }
};
#elif CRD_SIMD_HAS_SSE2
// The level's i32 lanes. SSE4.2 selects with pblendvb and takes pminsd; SSE2 has neither, nor any per-lane shift:
// pow2 builds 2^(s & 15) through an exact f32 conversion and moves it up 16 bits where s & 16, and srlv takes the high
// half of the u64 products a * 2^(32 - s) (pmuludq on the even and odd lanes), which is a >> s for s in [1, 31].
struct I32Lanes
{
    using V = __m128i;
    using M = V;
    static constexpr crd::usize kLanes = 4U;
    static V load(const void* p) noexcept { return _mm_loadu_si128(static_cast<const __m128i*>(p)); }
    static V set1(crd::u32 v) noexcept { return _mm_set1_epi32(static_cast<int>(v)); }
    static V zero() noexcept { return _mm_setzero_si128(); }
    static V add(V a, V b) noexcept { return _mm_add_epi32(a, b); }
    static V sub(V a, V b) noexcept { return _mm_sub_epi32(a, b); }
    static V bit_and(V a, V b) noexcept { return _mm_and_si128(a, b); }
    static V bit_or(V a, V b) noexcept { return _mm_or_si128(a, b); }
    static V andnot(V a, V b) noexcept // ~a & b
    {
        return _mm_andnot_si128(a, b);
    }
    static M cmpeq(V a, V b) noexcept { return _mm_cmpeq_epi32(a, b); }
    static M cmpgt(V a, V b) noexcept { return _mm_cmpgt_epi32(a, b); }
    static M mask_or(M a, M b) noexcept { return _mm_or_si128(a, b); }
    static M mask_and(M a, M b) noexcept { return _mm_and_si128(a, b); }
    static M mask_andnot(M a, M b) noexcept // ~a & b
    {
        return _mm_andnot_si128(a, b);
    }
    static M mask_not(M a) noexcept { return _mm_xor_si128(a, _mm_set1_epi32(-1)); }
    template <int N> static V srli(V a) noexcept { return _mm_srli_epi32(a, N); }
    template <int N> static V slli(V a) noexcept { return _mm_slli_epi32(a, N); }
    static V select(V a, V b, M m) noexcept // m ? b : a
    {
#if CRD_SIMD_HAS_SSE42
        return _mm_blendv_epi8(a, b, m);
#else
        return _mm_or_si128(_mm_andnot_si128(m, a), _mm_and_si128(m, b));
#endif
    }
    static V min(V a, V b) noexcept
    {
#if CRD_SIMD_HAS_SSE42
        return _mm_min_epi32(a, b);
#else
        return select(a, b, _mm_cmpgt_epi32(a, b));
#endif
    }
    static V inc(V a, M m) noexcept // a + 1 where m (an all-ones lane is -1)
    {
        return _mm_sub_epi32(a, m);
    }
    static V zero_where(M m, V a) noexcept { return _mm_andnot_si128(m, a); }
    static V pow2(V s) noexcept
    {
        const __m128i e = _mm_slli_epi32(_mm_add_epi32(_mm_and_si128(s, _mm_set1_epi32(15)), _mm_set1_epi32(127)), 23);
        const __m128i low = _mm_cvttps_epi32(_mm_castsi128_ps(e)); // 2^(s & 15), exact
        const __m128i bit4 = _mm_set1_epi32(16);
        return select(low, _mm_slli_epi32(low, 16), _mm_cmpeq_epi32(_mm_and_si128(s, bit4), bit4));
    }
    static V srlv(V a, V s) noexcept
    {
        const __m128i m = pow2(_mm_sub_epi32(_mm_set1_epi32(32), s));
        const __m128i even = _mm_mul_epu32(a, m);
        const __m128i odd = _mm_mul_epu32(_mm_srli_epi64(a, 32), _mm_srli_epi64(m, 32));
        return _mm_or_si128(_mm_srli_epi64(even, 32), _mm_and_si128(odd, _mm_set_epi32(-1, 0, -1, 0)));
    }
    static int movemask(M m) noexcept { return _mm_movemask_ps(_mm_castsi128_ps(m)); }
    // The low u16 of each lane to four contiguous u16s: sign-extended from bit 15, packssdw keeps the 16 bits.
    static void store_u16(V v, crd::u16* dst) noexcept
    {
        const __m128i p = _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(v, 16), 16), _mm_setzero_si128());
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), p);
    }
    // Each lane's byte value (<= 255) to four contiguous bytes.
    static void store_bytes(V v, crd::u8* dst) noexcept
    {
        const __m128i p = _mm_packs_epi32(v, v);
        const crd::u32 b = static_cast<crd::u32>(_mm_cvtsi128_si32(_mm_packus_epi16(p, p)));
        std::memcpy(dst, &b, 4U);
    }
};
#endif

#if CRD_SIMD_HAS_SSE2
using I = I32Lanes;
using V = I32Lanes::V;
using M = I32Lanes::M;
constexpr crd::usize kLanes = I32Lanes::kLanes;

// Integer transcription of narrow_rne over the level's lanes: the same masks, shifts and class blends, lane for lane.
// All intermediate values fit in signed 32-bit, so the signed compares are exact.
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn> [[nodiscard]] V narrow_rne_lanes(V fb) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1;
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;
    constexpr crd::u32 max_efield = Fn ? exp_all1 : exp_all1 - 1U;
    constexpr int shift_n = static_cast<int>(23U - MBITS);
    constexpr crd::u32 half_n = 1U << (shift_n - 1);

    const V one = I::set1(1U);
    const V sign = I::slli<static_cast<int>(EBITS + MBITS)>(I::srli<31>(fb));
    const V fexp = I::bit_and(I::srli<23>(fb), I::set1(0xFFU));
    const V fman = I::bit_and(fb, I::set1(0x7FFFFFU));
    const M m_naninf = I::cmpeq(fexp, I::set1(0xFFU));
    const M m_fzero = I::cmpeq(fexp, I::zero());
    const V te = I::add(fexp, I::set1(bias - 127U));
    const V sig = I::bit_or(fman, I::set1(0x800000U));

    // ---- normal path (te >= 1) ----
    V keep = I::srli<shift_n>(sig);
    const V rem = I::bit_and(sig, I::set1((1U << shift_n) - 1U));
    const M m_gt = I::cmpgt(rem, I::set1(half_n));
    const M m_tie = I::cmpeq(rem, I::set1(half_n));
    const M m_odd = I::cmpeq(I::bit_and(keep, one), one);
    keep = I::inc(keep, I::mask_or(m_gt, I::mask_and(m_tie, m_odd)));
    const M m_carry = I::cmpeq(keep, I::set1(2U << MBITS));
    keep = I::select(keep, I::srli<1>(keep), m_carry);
    const V te2 = I::inc(te, m_carry);
    const V man = I::bit_and(keep, I::set1(man_all1));
    M m_ovf = I::cmpgt(te2, I::set1(max_efield));
    if constexpr (Fn)
    {
        const M m_top = I::cmpeq(te2, I::set1(max_efield));
        const M m_mmax = I::cmpeq(man, I::set1(man_all1));
        m_ovf = I::mask_or(m_ovf, I::mask_and(m_top, m_mmax));
    }
    V res = I::bit_or(I::slli<static_cast<int>(MBITS)>(te2), man);
    res = I::select(res, I::set1(Fn ? nan_fn : inf_bits), m_ovf);

    // ---- subnormal path (te <= 0) ----
    const M m_sub = I::cmpgt(one, te); // 1 > te  <=>  te <= 0
    V shift_s = I::sub(I::set1(static_cast<crd::u32>(shift_n) + 1U), te);
    const M m_under = I::cmpgt(shift_s, I::set1(24U));
    shift_s = I::min(shift_s, I::set1(31U)); // keep the variable shifts defined
    V keep_s = I::srlv(sig, shift_s);
    const V rem_s = I::bit_and(sig, I::sub(I::pow2(shift_s), one));
    const V half_s = I::pow2(I::sub(shift_s, one));
    const M m_gt_s = I::cmpgt(rem_s, half_s);
    const M m_tie_s = I::cmpeq(rem_s, half_s);
    const M m_odd_s = I::cmpeq(I::bit_and(keep_s, one), one);
    keep_s = I::inc(keep_s, I::mask_or(m_gt_s, I::mask_and(m_tie_s, m_odd_s)));
    keep_s = I::zero_where(m_under, keep_s);

    // ---- nan/inf lane values ----
    V res_ni;
    if constexpr (Fn)
    {
        res_ni = I::set1(nan_fn);
    }
    else
    {
        const V pay = I::bit_or(I::set1(inf_bits | (1U << (MBITS - 1U))), I::srli<shift_n>(fman));
        res_ni = I::select(I::set1(inf_bits), pay, I::mask_not(I::cmpeq(fman, I::zero())));
    }

    // ---- class selection (mirrors the scalar control flow) ----
    res = I::select(res, keep_s, m_sub);
    res = I::zero_where(m_fzero, res);
    res = I::select(res, res_ni, m_naninf);
    return I::bit_or(res, sign);
}

// The stochastic-rounding narrower: narrow_sr lane for lane for discard shifts <= 31 (the shared direct 32-bit form).
// Lanes needing the 64-bit deep-underflow staging (shift > 31: 8+ binades below the target's subnormal floor) are
// reported in deep_mask for the caller's scalar patch (an f32 zero or subnormal is the vector's signed zero, as in the
// scalar core). Overflow SATURATES to max finite (the SR contract: never inf or
// nan from a finite input); nan/inf lanes get the RNE semantics.
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn> [[nodiscard]] V narrow_sr_lanes(V fb, V rnd, int& deep_mask) noexcept
{
    constexpr crd::u32 bias = (1U << (EBITS - 1U)) - 1U;
    constexpr crd::u32 exp_all1 = (1U << EBITS) - 1U;
    constexpr crd::u32 man_all1 = (1U << MBITS) - 1U;
    constexpr crd::u32 nan_fn = (exp_all1 << MBITS) | man_all1;
    constexpr crd::u32 inf_bits = exp_all1 << MBITS;
    constexpr crd::u32 max_efield = Fn ? exp_all1 : exp_all1 - 1U;
    constexpr crd::u32 max_finite = Fn ? ((exp_all1 << MBITS) | (man_all1 - 1U)) : ((max_efield << MBITS) | man_all1);
    constexpr int shift_n = static_cast<int>(23U - MBITS);

    const V one = I::set1(1U);
    const V sign = I::slli<static_cast<int>(EBITS + MBITS)>(I::srli<31>(fb));
    const V fexp = I::bit_and(I::srli<23>(fb), I::set1(0xFFU));
    const V fman = I::bit_and(fb, I::set1(0x7FFFFFU));
    const M m_naninf = I::cmpeq(fexp, I::set1(0xFFU));
    const M m_fzero = I::cmpeq(fexp, I::zero());
    const V te = I::add(fexp, I::set1(bias - 127U));
    const V sig = I::bit_or(fman, I::set1(0x800000U));

    // ---- normal path: keep = (sig + (rnd & mask_n)) >> shift_n ----
    V keep = I::srli<shift_n>(I::add(sig, I::bit_and(rnd, I::set1((1U << shift_n) - 1U))));
    const M m_carry = I::cmpeq(keep, I::set1(2U << MBITS));
    keep = I::select(keep, I::srli<1>(keep), m_carry);
    const V te2 = I::inc(te, m_carry);
    const V man = I::bit_and(keep, I::set1(man_all1));
    M m_ovf = I::cmpgt(te2, I::set1(max_efield));
    if constexpr (Fn)
    {
        const M m_top = I::cmpeq(te2, I::set1(max_efield));
        const M m_mmax = I::cmpeq(man, I::set1(man_all1));
        m_ovf = I::mask_or(m_ovf, I::mask_and(m_top, m_mmax));
    }
    V res = I::bit_or(I::slli<static_cast<int>(MBITS)>(te2), man);
    res = I::select(res, I::set1(max_finite), m_ovf); // SATURATE

    // ---- subnormal path: keep_s = (sig + (rnd & mask_s)) >> shift_s ----
    const M m_sub = I::cmpgt(one, te);
    V shift_s = I::sub(I::set1(static_cast<crd::u32>(shift_n) + 1U), te);
    const M m_deep = I::mask_andnot(m_fzero, I::mask_and(m_sub, I::cmpgt(shift_s, I::set1(31U))));
    shift_s = I::min(shift_s, I::set1(31U));
    const V mask_s = I::sub(I::pow2(shift_s), one);
    const V keep_s = I::srlv(I::add(sig, I::bit_and(rnd, mask_s)), shift_s);

    // ---- nan/inf lane values (RNE semantics) ----
    V res_ni;
    if constexpr (Fn)
    {
        res_ni = I::set1(nan_fn);
    }
    else
    {
        const V pay = I::bit_or(I::set1(inf_bits | (1U << (MBITS - 1U))), I::srli<shift_n>(fman));
        res_ni = I::select(I::set1(inf_bits), pay, I::mask_not(I::cmpeq(fman, I::zero())));
    }

    res = I::select(res, keep_s, m_sub);
    res = I::zero_where(m_fzero, res);
    res = I::select(res, res_ni, m_naninf);
    deep_mask = I::movemask(m_deep);
    return I::bit_or(res, sign);
}

#if !CRD_SIMD_HAS_AVX2
// widen_exact<5, 10, false> lane for lane: a normal or infinite field rebiases, a NaN keeps its payload shifted up and
// set quiet (VCVTPH2PS), and a subnormal tman * 2^-24 is exact in f32, so the f32 product is its encoding.
V widen_f16_lanes(V h) noexcept
{
    const V sign = I::slli<31>(I::srli<15>(h));
    const V texp = I::bit_and(I::srli<10>(h), I::set1(0x1FU));
    const V tman = I::bit_and(h, I::set1(0x3FFU));
    const V normal = I::bit_or(I::slli<23>(I::add(texp, I::set1(127U - 15U))), I::slli<13>(tman));
    const V m_nan = I::andnot(I::cmpeq(tman, I::zero()), I::set1(0xFFFFFFFFU));
    const V naninf =
        I::bit_or(I::set1(0x7F800000U), I::bit_and(m_nan, I::bit_or(I::set1(1U << 22U), I::slli<13>(tman))));
    const V sub = _mm_castps_si128(_mm_mul_ps(_mm_cvtepi32_ps(tman), _mm_castsi128_ps(I::set1(0x33800000U)))); // 2^-24
    V res = I::select(normal, sub, I::cmpeq(texp, I::zero()));
    res = I::select(res, naninf, I::cmpeq(texp, I::set1(0x1FU)));
    return I::bit_or(res, sign);
}
#endif
#endif

// The batch kernels. Each is bit-identical to the scalar converts on every input. AVX-512 runs every step, the last
// one included, on the first min(n - i, 16) lanes; the other levels' vector parts end at a closed-form index, so a tail
// provably runs fewer than eight times (GCC's link-time ranges otherwise report -Waggressive-loop-optimizations on the
// tail of a constant-size call).

void f32_to_f16_n(const crd::f32* src, crd::u16* dst, crd::usize n) noexcept
{
#if CRD_SIMD_HAS_AVX512
    for (crd::usize i = 0; i < n; i += kLanes)
    {
        const M m = I::first(n - i);
        const __m512 v = _mm512_maskz_loadu_ps(m, src + i);
        _mm256_mask_storeu_epi16(dst + i, m, _mm512_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT)); // explicit RNE imm
    }
#else
    crd::usize i = 0;
#if CRD_SIMD_HAS_AVX2
    const crd::usize vector_end = n - (n % 8U);
    for (; i < vector_end; i += 8U)
    {
        const __m256 v = _mm256_loadu_ps(src + i);
        const __m128i h = _mm256_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT); // explicit RNE imm
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), h);
    }
#elif CRD_SIMD_HAS_SSE2
    const crd::usize vector_end = n - (n % kLanes);
    for (; i < vector_end; i += kLanes)
    {
        I::store_u16(narrow_rne_lanes<5U, 10U, false>(I::load(src + i)), dst + i);
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = f32_to_f16_bits(src[i]);
    }
#endif
}

void f16_to_f32_n(const crd::u16* src, crd::f32* dst, crd::usize n) noexcept
{
#if CRD_SIMD_HAS_AVX512
    for (crd::usize i = 0; i < n; i += kLanes)
    {
        const M m = I::first(n - i);
        _mm512_mask_storeu_ps(dst + i, m, _mm512_cvtph_ps(_mm256_maskz_loadu_epi16(m, src + i)));
    }
#else
    crd::usize i = 0;
#if CRD_SIMD_HAS_AVX2
    const crd::usize vector_end = n - (n % 8U);
    for (; i < vector_end; i += 8U)
    {
        const __m128i h = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        _mm256_storeu_ps(dst + i, _mm256_cvtph_ps(h));
    }
#elif CRD_SIMD_HAS_SSE2
    const crd::usize vector_end = n - (n % kLanes);
    for (; i < vector_end; i += kLanes)
    {
        const __m128i h = _mm_unpacklo_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + i)), I::zero());
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), widen_f16_lanes(h));
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = f16_bits_to_f32(src[i]);
    }
#endif
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
#if CRD_SIMD_HAS_AVX512
    for (crd::usize i = 0; i < n; i += kLanes)
    {
        const M m = I::first(n - i);
        I::store_bytes(narrow_rne_lanes<EBITS, MBITS, Fn>(I::load(src + i, m)), dst + i, m);
    }
#else
    crd::usize i = 0;
#if CRD_SIMD_HAS_SSE2
    const crd::usize vector_end = n - (n % kLanes);
    for (; i < vector_end; i += kLanes)
    {
        I::store_bytes(narrow_rne_lanes<EBITS, MBITS, Fn>(I::load(src + i)), dst + i);
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] =
            static_cast<crd::u8>(small_float::narrow_rne<EBITS, MBITS, Fn>(small_float::bits_as<crd::u32>(src[i])));
    }
#endif
}

// Stochastic rounding: rand[k] is element k's uniform draw (the caller owns the draw policy). The lanes narrow_sr_lanes
// reports deep (rare: staged through 64 bits by the scalar core) are patched after the vector store.
template <crd::u32 EBITS, crd::u32 MBITS, bool Fn, typename Bits>
void f32_to_small_sr_n(const crd::f32* src, Bits* dst, const crd::u32* rand, crd::usize n) noexcept
{
    [[maybe_unused]] const auto patch = [&](crd::usize i, int deep)
    {
        for (crd::usize lane = 0; deep != 0; ++lane, deep >>= 1)
        {
            if ((deep & 1) != 0)
            {
                dst[i + lane] = static_cast<Bits>(small_float::narrow_sr<EBITS, MBITS, Fn>(
                    small_float::bits_as<crd::u32>(src[i + lane]), rand[i + lane]));
            }
        }
    };
#if CRD_SIMD_HAS_AVX512
    for (crd::usize i = 0; i < n; i += kLanes)
    {
        const M m = I::first(n - i);
        int deep = 0;
        const V r = narrow_sr_lanes<EBITS, MBITS, Fn>(I::load(src + i, m), I::load(rand + i, m), deep);
        if constexpr (sizeof(Bits) == 2U)
        {
            I::store_u16(r, dst + i, m);
        }
        else
        {
            I::store_bytes(r, dst + i, m);
        }
        patch(i, deep & static_cast<int>(m)); // a lane past n loads as +0, never deep
    }
#else
    crd::usize i = 0;
#if CRD_SIMD_HAS_SSE2
    const crd::usize vector_end = n - (n % kLanes);
    for (; i < vector_end; i += kLanes)
    {
        int deep = 0;
        const V r = narrow_sr_lanes<EBITS, MBITS, Fn>(I::load(src + i), I::load(rand + i), deep);
        if constexpr (sizeof(Bits) == 2U)
        {
            I::store_u16(r, dst + i);
        }
        else
        {
            I::store_bytes(r, dst + i);
        }
        patch(i, deep);
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = static_cast<Bits>(
            small_float::narrow_sr<EBITS, MBITS, Fn>(small_float::bits_as<crd::u32>(src[i]), rand[i]));
    }
#endif
}

#if CRD_SIMD_HAS_SSE2
// The bf16 stochastic narrowing of one register: the 16-bit carry add and the saturation below inf, lane for lane;
// the nan/inf lanes (RNE semantics) are reported in ni_mask for the caller's scalar patch.
V bf16_sr_lanes(V bits, V rnd, int& ni_mask) noexcept
{
    const V mag = I::bit_and(bits, I::set1(0x7FFFFFFFU));
    ni_mask = I::movemask(I::cmpgt(mag, I::set1(0x7F7FFFFFU))); // >= inf
    V h = I::srli<16>(I::add(mag, I::bit_and(rnd, I::set1(0xFFFFU))));
    h = I::select(h, I::set1(0x7F7FU), I::cmpgt(h, I::set1(0x7F7FU)));
    return I::bit_or(I::bit_and(I::srli<16>(bits), I::set1(0x8000U)), h);
}
#endif

void f32_to_bf16_sr_n(const crd::f32* src, crd::u16* dst, const crd::u32* rand, crd::usize n) noexcept
{
    [[maybe_unused]] const auto patch = [&](crd::usize i, int ni)
    {
        for (crd::usize lane = 0; ni != 0; ++lane, ni >>= 1)
        {
            if ((ni & 1) != 0)
            {
                dst[i + lane] = f32_to_bf16_bits_sr(src[i + lane], rand[i + lane]);
            }
        }
    };
#if CRD_SIMD_HAS_AVX512
    for (crd::usize i = 0; i < n; i += kLanes)
    {
        const M m = I::first(n - i);
        int ni = 0;
        I::store_u16(bf16_sr_lanes(I::load(src + i, m), I::load(rand + i, m), ni), dst + i, m);
        patch(i, ni); // a lane past n loads as +0, never nan/inf
    }
#else
    crd::usize i = 0;
#if CRD_SIMD_HAS_SSE2
    const crd::usize vector_end = n - (n % kLanes);
    for (; i < vector_end; i += kLanes)
    {
        int ni = 0;
        I::store_u16(bf16_sr_lanes(I::load(src + i), I::load(rand + i), ni), dst + i);
        patch(i, ni);
    }
#endif
    for (; i < n; ++i)
    {
        dst[i] = f32_to_bf16_bits_sr(src[i], rand[i]);
    }
#endif
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
