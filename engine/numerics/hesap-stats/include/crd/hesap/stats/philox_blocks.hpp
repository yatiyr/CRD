#pragma once

// The Philox4x32-10 block loop: blocks first .. first + n - 1 of (seed, stream), bit-identical to philox4x32
// (counter_rng_core.hpp): 24 at a time on the AVX2 backend (three independent groups of eight), 16 at a time on the
// 128-bit backends (four groups of four), so the ten dependent rounds of one group overlap with the others'. The
// stats kernels run it per level (../../../../kernels/stats_kernels.cpp), and so do the hesap-tensor kernels for the
// stochastic rounding and sketches, which keep Philox include-only (ADR-0096 §1). It lives in the unit's level
// namespace and takes the unit's backend (crd/math/simd/backend.hpp); only kernel units include it.

#include <crd/core/types.hpp>
#include <crd/hesap/stats/counter_rng_core.hpp>
#include <crd/math/simd/backend.hpp>

#include <cstring>

namespace crd::hesap::stats::inline CRD_MATH_LEVEL_NS
{

#if CRD_SIMD_HAS_AVX2
// Lane i of hi and lo: the high and low words of a[i] * m, from two 32x32->64 multiplies (vpmuludq reads the low word
// of each 64-bit lane, so the broadcast m serves the odd lanes too).
inline void philox_mulhilo8(__m256i a, __m256i m, __m256i& hi, __m256i& lo) noexcept
{
    const __m256i evn = _mm256_mul_epu32(a, m);
    const __m256i odd = _mm256_mul_epu32(_mm256_srli_epi64(a, 32), m);
    hi = _mm256_blend_epi32(_mm256_srli_epi64(evn, 32), odd, 0xAA);
    lo = _mm256_blend_epi32(evn, _mm256_slli_epi64(odd, 32), 0xAA);
}

// Philox blocks first .. first + 8G - 1 (mod 2^64) of (seed, stream) in G groups of eight lanes: counter (position
// low, position high, stream low, stream high), key (seed low, seed high). Stores the blocks' 32G u32 words in block
// order to out (128G bytes, any alignment), which a little-endian u64 array reads as PhiloxRng::next_u64's stream.
template <int G> inline void philox4x32_x8(crd::u64 seed, crd::u64 stream, crd::u64 first, void* out) noexcept
{
    __m256i c0[G];
    __m256i c1[G];
    for (int g = 0; g < G; ++g)
    {
        const __m256i lane = _mm256_set_epi64x(3, 2, 1, 0);
        const __m256i base = _mm256_set1_epi64x(static_cast<long long>(first + 8U * static_cast<crd::u64>(g)));
        const __m256i p_lo = _mm256_add_epi64(base, lane); // positions 0..3 of the group
        const __m256i p_hi = _mm256_add_epi64(base, _mm256_add_epi64(lane, _mm256_set1_epi64x(4))); // 4..7
        // Deinterleave the eight 64-bit positions into their low and high words, lanes in position order.
        const __m256i order = _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7);
        const __m256i a = _mm256_permutevar8x32_epi32(p_lo, order); // lo0..lo3 | hi0..hi3
        const __m256i b = _mm256_permutevar8x32_epi32(p_hi, order); // lo4..lo7 | hi4..hi7
        c0[g] = _mm256_permute2x128_si256(a, b, 0x20);
        c1[g] = _mm256_permute2x128_si256(a, b, 0x31);
    }
    const __m256i m0 = _mm256_set1_epi32(static_cast<int>(0xD2511F53U));
    const __m256i m1 = _mm256_set1_epi32(static_cast<int>(0xCD9E8D57U));
    const __m256i w0 = _mm256_set1_epi32(static_cast<int>(0x9E3779B9U));
    const __m256i w1 = _mm256_set1_epi32(static_cast<int>(0xBB67AE85U));
    __m256i c2[G];
    __m256i c3[G];
    for (int g = 0; g < G; ++g)
    {
        c2[g] = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(stream)));
        c3[g] = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(stream >> 32)));
    }
    __m256i k0 = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(seed)));
    __m256i k1 = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(seed >> 32)));
    for (int round = 0; round < 10; ++round)
    {
        if (round > 0) // the key schedule bumps between rounds
        {
            k0 = _mm256_add_epi32(k0, w0);
            k1 = _mm256_add_epi32(k1, w1);
        }
        for (int g = 0; g < G; ++g)
        {
            __m256i hi0;
            __m256i lo0;
            __m256i hi1;
            __m256i lo1;
            philox_mulhilo8(c0[g], m0, hi0, lo0);
            philox_mulhilo8(c2[g], m1, hi1, lo1);
            c0[g] = _mm256_xor_si256(_mm256_xor_si256(hi1, c1[g]), k0);
            c2[g] = _mm256_xor_si256(_mm256_xor_si256(hi0, c3[g]), k1);
            c1[g] = lo1;
            c3[g] = lo0;
        }
    }
    auto* words = static_cast<__m256i*>(out);
    for (int g = 0; g < G; ++g)
    {
        // Transpose 8x4 u32 (lanes to blocks).
        const __m256i t0 = _mm256_unpacklo_epi32(c0[g], c1[g]);
        const __m256i t1 = _mm256_unpackhi_epi32(c0[g], c1[g]);
        const __m256i t2 = _mm256_unpacklo_epi32(c2[g], c3[g]);
        const __m256i t3 = _mm256_unpackhi_epi32(c2[g], c3[g]);
        const __m256i e = _mm256_unpacklo_epi64(t0, t2);
        const __m256i f = _mm256_unpackhi_epi64(t0, t2);
        const __m256i h0 = _mm256_unpacklo_epi64(t1, t3);
        const __m256i h1 = _mm256_unpackhi_epi64(t1, t3);
        _mm256_storeu_si256(words + 4 * g + 0, _mm256_permute2x128_si256(e, f, 0x20));
        _mm256_storeu_si256(words + 4 * g + 1, _mm256_permute2x128_si256(h0, h1, 0x20));
        _mm256_storeu_si256(words + 4 * g + 2, _mm256_permute2x128_si256(e, f, 0x31));
        _mm256_storeu_si256(words + 4 * g + 3, _mm256_permute2x128_si256(h0, h1, 0x31));
    }
}
#endif

#if CRD_SIMD_HAS_SSE2
// Lane i of hi and lo: the high and low words of a[i] * m, from two 32x32->64 multiplies (pmuludq reads the low word of
// each 64-bit lane).
inline void philox_mulhilo4(__m128i a, __m128i m, __m128i& hi, __m128i& lo) noexcept
{
    const __m128i evn = _mm_shuffle_epi32(_mm_mul_epu32(a, m), _MM_SHUFFLE(3, 1, 2, 0)); // lo0 lo2 hi0 hi2
    const __m128i odd = _mm_shuffle_epi32(_mm_mul_epu32(_mm_srli_epi64(a, 32), m), _MM_SHUFFLE(3, 1, 2, 0)); // 1 3
    lo = _mm_unpacklo_epi32(evn, odd);
    hi = _mm_unpackhi_epi32(evn, odd);
}

// Philox blocks first .. first + 4G - 1 (mod 2^64) of (seed, stream) in G groups of four lanes, as philox4x32_x8
// stores its groups: 64G bytes at out.
template <int G> inline void philox4x32_x4(crd::u64 seed, crd::u64 stream, crd::u64 first, void* out) noexcept
{
    __m128i c0[G];
    __m128i c1[G];
    __m128i c2[G];
    __m128i c3[G];
    for (int g = 0; g < G; ++g)
    {
        const __m128i base = _mm_set1_epi64x(static_cast<long long>(first + 4U * static_cast<crd::u64>(g)));
        // The four 64-bit positions' low and high words, lanes in position order.
        const __m128i a = _mm_shuffle_epi32(_mm_add_epi64(base, _mm_set_epi64x(1, 0)), _MM_SHUFFLE(3, 1, 2, 0));
        const __m128i b = _mm_shuffle_epi32(_mm_add_epi64(base, _mm_set_epi64x(3, 2)), _MM_SHUFFLE(3, 1, 2, 0));
        c0[g] = _mm_unpacklo_epi64(a, b); // lo0 lo1 lo2 lo3
        c1[g] = _mm_unpackhi_epi64(a, b); // hi0 hi1 hi2 hi3
        c2[g] = _mm_set1_epi32(static_cast<int>(static_cast<crd::u32>(stream)));
        c3[g] = _mm_set1_epi32(static_cast<int>(static_cast<crd::u32>(stream >> 32)));
    }
    const __m128i m0 = _mm_set1_epi32(static_cast<int>(0xD2511F53U));
    const __m128i m1 = _mm_set1_epi32(static_cast<int>(0xCD9E8D57U));
    const __m128i w0 = _mm_set1_epi32(static_cast<int>(0x9E3779B9U));
    const __m128i w1 = _mm_set1_epi32(static_cast<int>(0xBB67AE85U));
    __m128i k0 = _mm_set1_epi32(static_cast<int>(static_cast<crd::u32>(seed)));
    __m128i k1 = _mm_set1_epi32(static_cast<int>(static_cast<crd::u32>(seed >> 32)));
    for (int round = 0; round < 10; ++round)
    {
        if (round > 0) // the key schedule bumps between rounds
        {
            k0 = _mm_add_epi32(k0, w0);
            k1 = _mm_add_epi32(k1, w1);
        }
        for (int g = 0; g < G; ++g)
        {
            __m128i hi0;
            __m128i lo0;
            __m128i hi1;
            __m128i lo1;
            philox_mulhilo4(c0[g], m0, hi0, lo0);
            philox_mulhilo4(c2[g], m1, hi1, lo1);
            c0[g] = _mm_xor_si128(_mm_xor_si128(hi1, c1[g]), k0);
            c2[g] = _mm_xor_si128(_mm_xor_si128(hi0, c3[g]), k1);
            c1[g] = lo1;
            c3[g] = lo0;
        }
    }
    auto* words = static_cast<__m128i*>(out);
    for (int g = 0; g < G; ++g)
    {
        // Transpose 4x4 u32 (lanes to blocks).
        const __m128i t0 = _mm_unpacklo_epi32(c0[g], c1[g]);
        const __m128i t1 = _mm_unpackhi_epi32(c0[g], c1[g]);
        const __m128i t2 = _mm_unpacklo_epi32(c2[g], c3[g]);
        const __m128i t3 = _mm_unpackhi_epi32(c2[g], c3[g]);
        _mm_storeu_si128(words + 4 * g + 0, _mm_unpacklo_epi64(t0, t2));
        _mm_storeu_si128(words + 4 * g + 1, _mm_unpackhi_epi64(t0, t2));
        _mm_storeu_si128(words + 4 * g + 2, _mm_unpacklo_epi64(t1, t3));
        _mm_storeu_si128(words + 4 * g + 3, _mm_unpackhi_epi64(t1, t3));
    }
}
#endif

// Philox blocks first .. first + blocks - 1 (mod 2^64) of (seed, stream): the blocks' u32 words in block order, 16
// bytes per block at out (any alignment), which a little-endian u64 array reads as PhiloxRng::next_u64's stream and
// a u32 array as next_u32's. The vector loops take whole groups, then single groups, and the scalar loop the rest.
inline void philox4x32_fill_blocks(crd::u64 seed, crd::u64 stream, crd::u64 first, void* out,
                                   crd::usize blocks) noexcept
{
    auto* bytes = static_cast<unsigned char*>(out);
    crd::usize b = 0;
#if CRD_SIMD_HAS_AVX2
    for (; b + 24U <= blocks; b += 24U)
    {
        philox4x32_x8<3>(seed, stream, first + b, bytes + 16U * b);
    }
    for (; b + 8U <= blocks; b += 8U)
    {
        philox4x32_x8<1>(seed, stream, first + b, bytes + 16U * b);
    }
#elif CRD_SIMD_HAS_SSE2
    for (; b + 16U <= blocks; b += 16U)
    {
        philox4x32_x4<4>(seed, stream, first + b, bytes + 16U * b);
    }
#endif
#if CRD_SIMD_HAS_SSE2
    for (; b + 4U <= blocks; b += 4U)
    {
        philox4x32_x4<1>(seed, stream, first + b, bytes + 16U * b);
    }
#endif
    const crd::u32 key[2] = {static_cast<crd::u32>(seed), static_cast<crd::u32>(seed >> 32)};
    for (; b < blocks; ++b)
    {
        const crd::u64 position = first + b;
        const crd::u32 counter[4] = {static_cast<crd::u32>(position), static_cast<crd::u32>(position >> 32),
                                     static_cast<crd::u32>(stream), static_cast<crd::u32>(stream >> 32)};
        const PhiloxBlock block = philox4x32(counter, key);
        std::memcpy(bytes + 16U * b, block.v, 16U);
    }
}

} // namespace crd::hesap::stats::inline CRD_MATH_LEVEL_NS
