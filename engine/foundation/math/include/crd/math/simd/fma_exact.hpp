// The single-rounded fused multiply-add of the SSE2 and SSE4.2 backends (SIMD-DISPATCH, ADR-0063, ADR-0134 §7).
//
// Neither level has an FMA instruction, and the C library's fma is one call per lane. These bodies compute
// RN(a*b + c) exactly with error-free transforms and rounding to odd (S. Boldo, G. Melquiond, "Emulation of FMA and
// correctly rounded sums: proved algorithms using rounding to odd", IEEE Trans. Computers 57(4), 2008), so every level
// gives the bits of the hardware instruction and of libm::fma, NaN payloads aside.
//
// f32: the f64 product of two f32 values is exact and lies in [2^-298, 2^256], so TwoSum(ab, c) is exact in f64; the
// f64 sum rounded to odd, then rounded to f32, is RN_24(ab + c) because 53 >= 24 + 2 (rounding the f64 sum to nearest
// first would round twice). Every input takes this path.
//
// f64 (Algorithm 5.4 of the paper): (uh, ul) = ab exactly (Veltkamp split, Dekker product), (th, tl) = TwoSum(c, uh),
// v = RO(tl + ul), z = RN(th + v). The transforms are exact where every operand is finite, |a| and |b| lie in
// [2^-1022, 2^995) (the split by 2^27 + 1 neither overflows nor meets a subnormal), |ab| in [2^-900, 2^1020) (the
// product's error is representable) and |c| < 2^1020 (no sum overflows). A result below 2^-1022 inside that region
// comes from a Sterbenz cancellation of c and uh, so it is exact before its one rounding. A zero a or b makes ab an
// exact signed zero, so c + uh is the result for any c. The other lanes call libm::fma.

#pragma once

#include <crd/math/libm.hpp>
#include <crd/math/simd/backend.hpp>

#if CRD_SIMD_HAS_SSE2 && !CRD_SIMD_HAS_AVX2

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS::detail
{

// s = RN(a + b) and e = a + b - s exactly (Knuth's TwoSum), for operands whose sum does not overflow.
CRD_FORCEINLINE void two_sum_pd(__m128d a, __m128d b, __m128d& s, __m128d& e) noexcept
{
    s = _mm_add_pd(a, b);
    const __m128d bb = _mm_sub_pd(s, a);
    e = _mm_add_pd(_mm_sub_pd(a, _mm_sub_pd(s, bb)), _mm_sub_pd(b, bb));
}

// a + b rounded to odd: RN(a + b) when exact, else the neighbour of a + b whose last significand bit is one. Built as
// the sum truncated toward zero with the last bit set: an inexact RN result that the error says rounded away from zero
// steps one unit down in magnitude, and every inexact one then sets bit 0. A non-finite sum keeps its value, as its
// TwoSum error is NaN and fails both ordered compares.
CRD_FORCEINLINE __m128d add_round_odd_pd(__m128d a, __m128d b) noexcept
{
    __m128d s;
    __m128d e;
    two_sum_pd(a, b, s, e);
    const __m128d zero = _mm_setzero_pd();
    const __m128i inexact = _mm_castpd_si128(_mm_or_pd(_mm_cmplt_pd(e, zero), _mm_cmpgt_pd(e, zero)));
    // The sign of s ^ e, broadcast from each lane's high dword: all ones where s rounded away from zero.
    const __m128i sign = _mm_srai_epi32(_mm_castpd_si128(_mm_xor_pd(s, e)), 31);
    const __m128i away = _mm_shuffle_epi32(sign, _MM_SHUFFLE(3, 3, 1, 1));
    __m128i bits = _mm_add_epi64(_mm_castpd_si128(s), _mm_and_si128(away, inexact)); // adds -1 to truncate
    bits = _mm_or_si128(bits, _mm_and_si128(inexact, _mm_set_epi32(0, 1, 0, 1)));
    return _mm_castsi128_pd(bits);
}

CRD_FORCEINLINE __m128d split_high_pd(__m128d a) noexcept
{
    const __m128d t = _mm_mul_pd(a, _mm_set1_pd(134217729.0)); // 2^27 + 1
    return _mm_sub_pd(t, _mm_sub_pd(t, a));
}

// The lanes outside the region the transforms prove take the C library's fma.
CRD_NOINLINE inline __m128d fma_pd_lanes(__m128d a, __m128d b, __m128d c, __m128d z, int exact_lanes) noexcept
{
    alignas(16) f64 x[2];
    alignas(16) f64 y[2];
    alignas(16) f64 w[2];
    alignas(16) f64 r[2];
    _mm_store_pd(x, a);
    _mm_store_pd(y, b);
    _mm_store_pd(w, c);
    _mm_store_pd(r, z);
    for (int i = 0; i < 2; ++i)
    {
        if ((exact_lanes & (1 << i)) == 0)
        {
            r[i] = libm::fma(x[i], y[i], w[i]);
        }
    }
    return _mm_load_pd(r);
}

CRD_FORCEINLINE __m128d fma_pd(__m128d a, __m128d b, __m128d c) noexcept
{
    const __m128d uh = _mm_mul_pd(a, b);
    const __m128d ah = split_high_pd(a);
    const __m128d bh = split_high_pd(b);
    const __m128d al = _mm_sub_pd(a, ah);
    const __m128d bl = _mm_sub_pd(b, bh);
    __m128d ul = _mm_sub_pd(_mm_mul_pd(ah, bh), uh);
    ul = _mm_add_pd(ul, _mm_mul_pd(ah, bl));
    ul = _mm_add_pd(ul, _mm_mul_pd(al, bh));
    ul = _mm_add_pd(ul, _mm_mul_pd(al, bl));
    __m128d th;
    __m128d tl;
    two_sum_pd(c, uh, th, tl);
    __m128d z = _mm_add_pd(th, add_round_odd_pd(tl, ul));

    const __m128d zero = _mm_setzero_pd();
    const __m128d zero_product = _mm_or_pd(_mm_cmpeq_pd(a, zero), _mm_cmpeq_pd(b, zero));
    z = _mm_or_pd(_mm_and_pd(zero_product, _mm_add_pd(c, uh)), _mm_andnot_pd(zero_product, z));

    const __m128d magnitude = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    const __m128d abs_a = _mm_and_pd(a, magnitude);
    const __m128d abs_b = _mm_and_pd(b, magnitude);
    const __m128d abs_u = _mm_and_pd(uh, magnitude);
    const __m128d split_max = _mm_set1_pd(0x1p995);
    const __m128d normal_min = _mm_set1_pd(0x1p-1022);
    const __m128d sum_max = _mm_set1_pd(0x1p1020);
    __m128d exact = _mm_and_pd(_mm_cmplt_pd(abs_a, split_max), _mm_cmplt_pd(abs_b, split_max));
    exact = _mm_and_pd(exact, _mm_and_pd(_mm_cmpge_pd(abs_a, normal_min), _mm_cmpge_pd(abs_b, normal_min)));
    exact = _mm_and_pd(exact, _mm_and_pd(_mm_cmpge_pd(abs_u, _mm_set1_pd(0x1p-900)), _mm_cmplt_pd(abs_u, sum_max)));
    exact = _mm_and_pd(exact, _mm_cmplt_pd(_mm_and_pd(c, magnitude), sum_max));
    const int exact_lanes = _mm_movemask_pd(_mm_or_pd(exact, zero_product));
    if (CRD_UNLIKELY(exact_lanes != 3))
    {
        z = fma_pd_lanes(a, b, c, z, exact_lanes);
    }
    return z;
}

CRD_FORCEINLINE __m128d fma_widened_pd(__m128 a, __m128 b, __m128 c) noexcept
{
    return add_round_odd_pd(_mm_mul_pd(_mm_cvtps_pd(a), _mm_cvtps_pd(b)), _mm_cvtps_pd(c));
}

CRD_FORCEINLINE __m128 fma_ps(__m128 a, __m128 b, __m128 c) noexcept
{
    const __m128 low = _mm_cvtpd_ps(fma_widened_pd(a, b, c));
    const __m128 high = _mm_cvtpd_ps(fma_widened_pd(_mm_movehl_ps(a, a), _mm_movehl_ps(b, b), _mm_movehl_ps(c, c)));
    return _mm_movelh_ps(low, high);
}

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS::detail

#endif
