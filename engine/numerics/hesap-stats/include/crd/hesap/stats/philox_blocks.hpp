#pragma once

// The Philox4x32-10 block loop: blocks first .. first + n - 1 of (seed, stream), eight at a time on the AVX2 backend,
// bit-identical to philox4x32 (counter_rng_core.hpp). The stats kernels run it per level
// (../../../../kernels/stats_kernels.cpp), and so do the hesap-tensor kernels for the stochastic rounding and sketches,
// which keep Philox include-only (ADR-0096 §1). It lives in the unit's level namespace and takes the unit's backend
// (crd/math/simd/backend.hpp); only kernel units include it.

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

// Philox blocks first .. first + 7 (mod 2^64) of (seed, stream): counter (position low, position high, stream low,
// stream high), key (seed low, seed high). Stores the blocks' 32 u32 words in block order to out (128 bytes, any
// alignment), which a little-endian u64 array reads as PhiloxRng::next_u64's stream.
inline void philox4x32_x8(crd::u64 seed, crd::u64 stream, crd::u64 first, void* out) noexcept
{
    const __m256i lane = _mm256_set_epi64x(3, 2, 1, 0);
    const __m256i base = _mm256_set1_epi64x(static_cast<long long>(first));
    const __m256i p_lo = _mm256_add_epi64(base, lane); // positions first + 0..3
    const __m256i p_hi = _mm256_add_epi64(base, _mm256_add_epi64(lane, _mm256_set1_epi64x(4))); // first + 4..7
    // Deinterleave the eight 64-bit positions into their low and high words, lanes in position order.
    const __m256i order = _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7);
    const __m256i a = _mm256_permutevar8x32_epi32(p_lo, order); // lo0..lo3 | hi0..hi3
    const __m256i b = _mm256_permutevar8x32_epi32(p_hi, order); // lo4..lo7 | hi4..hi7
    __m256i c0 = _mm256_permute2x128_si256(a, b, 0x20);
    __m256i c1 = _mm256_permute2x128_si256(a, b, 0x31);
    const __m256i m0 = _mm256_set1_epi32(static_cast<int>(0xD2511F53U));
    const __m256i m1 = _mm256_set1_epi32(static_cast<int>(0xCD9E8D57U));
    const __m256i w0 = _mm256_set1_epi32(static_cast<int>(0x9E3779B9U));
    const __m256i w1 = _mm256_set1_epi32(static_cast<int>(0xBB67AE85U));
    __m256i c2 = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(stream)));
    __m256i c3 = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(stream >> 32)));
    __m256i k0 = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(seed)));
    __m256i k1 = _mm256_set1_epi32(static_cast<int>(static_cast<crd::u32>(seed >> 32)));
    for (int round = 0; round < 10; ++round)
    {
        if (round > 0) // the key schedule bumps between rounds
        {
            k0 = _mm256_add_epi32(k0, w0);
            k1 = _mm256_add_epi32(k1, w1);
        }
        __m256i hi0;
        __m256i lo0;
        __m256i hi1;
        __m256i lo1;
        philox_mulhilo8(c0, m0, hi0, lo0);
        philox_mulhilo8(c2, m1, hi1, lo1);
        c0 = _mm256_xor_si256(_mm256_xor_si256(hi1, c1), k0);
        c2 = _mm256_xor_si256(_mm256_xor_si256(hi0, c3), k1);
        c1 = lo1;
        c3 = lo0;
    }
    // Transpose 8x4 u32 (lanes to blocks).
    const __m256i t0 = _mm256_unpacklo_epi32(c0, c1);
    const __m256i t1 = _mm256_unpackhi_epi32(c0, c1);
    const __m256i t2 = _mm256_unpacklo_epi32(c2, c3);
    const __m256i t3 = _mm256_unpackhi_epi32(c2, c3);
    const __m256i e = _mm256_unpacklo_epi64(t0, t2);
    const __m256i f = _mm256_unpackhi_epi64(t0, t2);
    const __m256i g = _mm256_unpacklo_epi64(t1, t3);
    const __m256i h = _mm256_unpackhi_epi64(t1, t3);
    auto* words = static_cast<__m256i*>(out);
    _mm256_storeu_si256(words + 0, _mm256_permute2x128_si256(e, f, 0x20));
    _mm256_storeu_si256(words + 1, _mm256_permute2x128_si256(g, h, 0x20));
    _mm256_storeu_si256(words + 2, _mm256_permute2x128_si256(e, f, 0x31));
    _mm256_storeu_si256(words + 3, _mm256_permute2x128_si256(g, h, 0x31));
}
#endif

// Philox blocks first .. first + blocks - 1 (mod 2^64) of (seed, stream): the blocks' u32 words in block order, 16
// bytes per block at out (any alignment), which a little-endian u64 array reads as PhiloxRng::next_u64's stream and
// a u32 array as next_u32's.
inline void philox4x32_fill_blocks(crd::u64 seed, crd::u64 stream, crd::u64 first, void* out,
                                   crd::usize blocks) noexcept
{
    auto* bytes = static_cast<unsigned char*>(out);
    crd::usize b = 0;
#if CRD_SIMD_HAS_AVX2
    for (; b + 8U <= blocks; b += 8U)
    {
        philox4x32_x8(seed, stream, first + b, bytes + 16U * b);
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
