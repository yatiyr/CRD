// Four interleaved channels and their planes: p[0 .. 4 * lanes) as x y z w per element, against one lane register per
// channel, for quaternions and any other four-channel record. In registers on every backend, so a kernel's block of
// records does not write a plane in scalars and read it back in a wider load (a store-forwarding stall on x86). Moves only,
// so each channel's bits are its input's on every level.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
{

CRD_FORCEINLINE void load_interleaved4(const f32* p, Vec4f& x, Vec4f& y, Vec4f& z, Vec4f& w) noexcept
{
#if CRD_SIMD_HAS_SSE2
    const __m128 r0 = _mm_loadu_ps(p);
    const __m128 r1 = _mm_loadu_ps(p + 4);
    const __m128 r2 = _mm_loadu_ps(p + 8);
    const __m128 r3 = _mm_loadu_ps(p + 12);
    const __m128 low01 = _mm_unpacklo_ps(r0, r1);  // x0 x1 y0 y1
    const __m128 high01 = _mm_unpackhi_ps(r0, r1); // z0 z1 w0 w1
    const __m128 low23 = _mm_unpacklo_ps(r2, r3);  // x2 x3 y2 y3
    const __m128 high23 = _mm_unpackhi_ps(r2, r3); // z2 z3 w2 w3
    x.v = _mm_movelh_ps(low01, low23);
    y.v = _mm_movehl_ps(low23, low01);
    z.v = _mm_movelh_ps(high01, high23);
    w.v = _mm_movehl_ps(high23, high01);
#elif CRD_SIMD_HAS_NEON
    const float32x4x4_t planes = vld4q_f32(p);
    x.v = planes.val[0];
    y.v = planes.val[1];
    z.v = planes.val[2];
    w.v = planes.val[3];
#else
    x = Vec4f(p[0], p[4], p[8], p[12]);
    y = Vec4f(p[1], p[5], p[9], p[13]);
    z = Vec4f(p[2], p[6], p[10], p[14]);
    w = Vec4f(p[3], p[7], p[11], p[15]);
#endif
}

CRD_FORCEINLINE void store_interleaved4(f32* p, Vec4f x, Vec4f y, Vec4f z, Vec4f w) noexcept
{
#if CRD_SIMD_HAS_SSE2
    const __m128 low_xy = _mm_unpacklo_ps(x.v, y.v);  // x0 y0 x1 y1
    const __m128 high_xy = _mm_unpackhi_ps(x.v, y.v); // x2 y2 x3 y3
    const __m128 low_zw = _mm_unpacklo_ps(z.v, w.v);  // z0 w0 z1 w1
    const __m128 high_zw = _mm_unpackhi_ps(z.v, w.v); // z2 w2 z3 w3
    _mm_storeu_ps(p, _mm_movelh_ps(low_xy, low_zw));
    _mm_storeu_ps(p + 4, _mm_movehl_ps(low_zw, low_xy));
    _mm_storeu_ps(p + 8, _mm_movelh_ps(high_xy, high_zw));
    _mm_storeu_ps(p + 12, _mm_movehl_ps(high_zw, high_xy));
#elif CRD_SIMD_HAS_NEON
    float32x4x4_t planes;
    planes.val[0] = x.v;
    planes.val[1] = y.v;
    planes.val[2] = z.v;
    planes.val[3] = w.v;
    vst4q_f32(p, planes);
#else
    f32 planes[4][4];
    x.store(planes[0]);
    y.store(planes[1]);
    z.store(planes[2]);
    w.store(planes[3]);
    for (usize lane = 0U; lane < 4U; ++lane)
    {
        for (usize channel = 0U; channel < 4U; ++channel)
        {
            p[(4U * lane) + channel] = planes[channel][lane];
        }
    }
#endif
}

CRD_FORCEINLINE void load_interleaved4(const f32* p, Vec8f& x, Vec8f& y, Vec8f& z, Vec8f& w) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256 r0 = _mm256_loadu_ps(p);      // Elements 0, 1.
    const __m256 r1 = _mm256_loadu_ps(p + 8);  // 2, 3.
    const __m256 r2 = _mm256_loadu_ps(p + 16); // 4, 5.
    const __m256 r3 = _mm256_loadu_ps(p + 24); // 6, 7.
    // Elements 0 and 4, 1 and 5, 2 and 6, 3 and 7 in one register each, so the per-128 transpose keeps the lanes in order.
    const __m256 e04 = _mm256_permute2f128_ps(r0, r2, 0x20);
    const __m256 e15 = _mm256_permute2f128_ps(r0, r2, 0x31);
    const __m256 e26 = _mm256_permute2f128_ps(r1, r3, 0x20);
    const __m256 e37 = _mm256_permute2f128_ps(r1, r3, 0x31);
    const __m256 low01 = _mm256_unpacklo_ps(e04, e15);
    const __m256 high01 = _mm256_unpackhi_ps(e04, e15);
    const __m256 low23 = _mm256_unpacklo_ps(e26, e37);
    const __m256 high23 = _mm256_unpackhi_ps(e26, e37);
    x.v = _mm256_shuffle_ps(low01, low23, 0x44);
    y.v = _mm256_shuffle_ps(low01, low23, 0xEE);
    z.v = _mm256_shuffle_ps(high01, high23, 0x44);
    w.v = _mm256_shuffle_ps(high01, high23, 0xEE);
#else
    Vec4f lo[4];
    Vec4f hi[4];
    load_interleaved4(p, lo[0], lo[1], lo[2], lo[3]);
    load_interleaved4(p + 16, hi[0], hi[1], hi[2], hi[3]);
    x = Vec8f(lo[0], hi[0]);
    y = Vec8f(lo[1], hi[1]);
    z = Vec8f(lo[2], hi[2]);
    w = Vec8f(lo[3], hi[3]);
#endif
}

CRD_FORCEINLINE void store_interleaved4(f32* p, Vec8f x, Vec8f y, Vec8f z, Vec8f w) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256 low_xy = _mm256_unpacklo_ps(x.v, y.v);
    const __m256 high_xy = _mm256_unpackhi_ps(x.v, y.v);
    const __m256 low_zw = _mm256_unpacklo_ps(z.v, w.v);
    const __m256 high_zw = _mm256_unpackhi_ps(z.v, w.v);
    const __m256 e04 = _mm256_shuffle_ps(low_xy, low_zw, 0x44);
    const __m256 e15 = _mm256_shuffle_ps(low_xy, low_zw, 0xEE);
    const __m256 e26 = _mm256_shuffle_ps(high_xy, high_zw, 0x44);
    const __m256 e37 = _mm256_shuffle_ps(high_xy, high_zw, 0xEE);
    _mm256_storeu_ps(p, _mm256_permute2f128_ps(e04, e15, 0x20));
    _mm256_storeu_ps(p + 8, _mm256_permute2f128_ps(e26, e37, 0x20));
    _mm256_storeu_ps(p + 16, _mm256_permute2f128_ps(e04, e15, 0x31));
    _mm256_storeu_ps(p + 24, _mm256_permute2f128_ps(e26, e37, 0x31));
#else
    store_interleaved4(p, x.lo, y.lo, z.lo, w.lo);
    store_interleaved4(p + 16, x.hi, y.hi, z.hi, w.hi);
#endif
}

CRD_FORCEINLINE void load_interleaved4(const f32* p, Vec16f& x, Vec16f& y, Vec16f& z, Vec16f& w) noexcept
{
#if CRD_SIMD_HAS_AVX512
    const __m512 r0 = _mm512_loadu_ps(p);
    const __m512 r1 = _mm512_loadu_ps(p + 16);
    const __m512 r2 = _mm512_loadu_ps(p + 32);
    const __m512 r3 = _mm512_loadu_ps(p + 48);
    // From two registers: every first channel in order then every second, and every third then every fourth.
    const __m512i even = _mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 1, 5, 9, 13, 17, 21, 25, 29);
    const __m512i odd = _mm512_setr_epi32(2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15, 19, 23, 27, 31);
    const __m512 xy01 = _mm512_permutex2var_ps(r0, even, r1);
    const __m512 zw01 = _mm512_permutex2var_ps(r0, odd, r1);
    const __m512 xy23 = _mm512_permutex2var_ps(r2, even, r3);
    const __m512 zw23 = _mm512_permutex2var_ps(r2, odd, r3);
    x.v = _mm512_shuffle_f32x4(xy01, xy23, 0x44);
    y.v = _mm512_shuffle_f32x4(xy01, xy23, 0xEE);
    z.v = _mm512_shuffle_f32x4(zw01, zw23, 0x44);
    w.v = _mm512_shuffle_f32x4(zw01, zw23, 0xEE);
#else
    Vec8f lo[4];
    Vec8f hi[4];
    load_interleaved4(p, lo[0], lo[1], lo[2], lo[3]);
    load_interleaved4(p + 32, hi[0], hi[1], hi[2], hi[3]);
    x = Vec16f(lo[0], hi[0]);
    y = Vec16f(lo[1], hi[1]);
    z = Vec16f(lo[2], hi[2]);
    w = Vec16f(lo[3], hi[3]);
#endif
}

CRD_FORCEINLINE void store_interleaved4(f32* p, Vec16f x, Vec16f y, Vec16f z, Vec16f w) noexcept
{
#if CRD_SIMD_HAS_AVX512
    // The inverse of the load: pairs of channels, then the pairs of each half.
    const __m512i pairs_low = _mm512_setr_epi32(0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
    const __m512i pairs_high = _mm512_setr_epi32(8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31);
    const __m512i four_low = _mm512_setr_epi32(0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23);
    const __m512i four_high = _mm512_setr_epi32(8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31);
    const __m512 xy_low = _mm512_permutex2var_ps(x.v, pairs_low, y.v);
    const __m512 xy_high = _mm512_permutex2var_ps(x.v, pairs_high, y.v);
    const __m512 zw_low = _mm512_permutex2var_ps(z.v, pairs_low, w.v);
    const __m512 zw_high = _mm512_permutex2var_ps(z.v, pairs_high, w.v);
    _mm512_storeu_ps(p, _mm512_permutex2var_ps(xy_low, four_low, zw_low));
    _mm512_storeu_ps(p + 16, _mm512_permutex2var_ps(xy_low, four_high, zw_low));
    _mm512_storeu_ps(p + 32, _mm512_permutex2var_ps(xy_high, four_low, zw_high));
    _mm512_storeu_ps(p + 48, _mm512_permutex2var_ps(xy_high, four_high, zw_high));
#else
    store_interleaved4(p, x.lo, y.lo, z.lo, w.lo);
    store_interleaved4(p + 32, x.hi, y.hi, z.hi, w.hi);
#endif
}

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
