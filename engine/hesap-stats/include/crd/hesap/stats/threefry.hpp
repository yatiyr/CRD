#pragma once

// crd-hesap-stats v12-e — Threefry-4×64-20 (Salmon-Moraes-Dror-Shaw, Random123): a counter-based RNG built from the
// Threefish-256 mix (no S-boxes; pure ARX). Like Philox it is PURE — output depends only on (counter, key) — so
// parallel streams are bit-identical and trivially seekable (the determinism moat). MATLAB's `rng(...,'threefry')`.
// Gated vs the published Random123 known-answer vectors.

#include <crd/containers/span.hpp>
#include <crd/hesap/stats/bitgen.hpp> // rotl64
#include <crd/core/types.hpp>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace crd::hesap::stats
{
// One 256-bit Threefry block (4 lanes of u64).
struct ThreefryBlock
{
    crd::u64 v[4];
};

// Threefry4x64-20 block function: PURE (output depends only on counter+key). constexpr ⇒ KAT-checkable at compile time.
[[nodiscard]] constexpr ThreefryBlock threefry4x64(const crd::u64 (&ctr)[4], const crd::u64 (&key)[4]) noexcept
{
    constexpr crd::u64 parity = 0x1BD11BDAA9FC1A22ULL; // skein key-schedule constant
    constexpr int rot[8][2] = {{14, 16}, {52, 57}, {23, 40}, {5, 37}, {25, 33}, {46, 12}, {58, 22}, {32, 32}};
    crd::u64 ks[5] = {key[0], key[1], key[2], key[3], parity ^ key[0] ^ key[1] ^ key[2] ^ key[3]};
    crd::u64 x0 = ctr[0] + ks[0];
    crd::u64 x1 = ctr[1] + ks[1];
    crd::u64 x2 = ctr[2] + ks[2];
    crd::u64 x3 = ctr[3] + ks[3];
    for (int r = 0; r < 20; ++r)
    {
        const int m = r & 7;
        x0 += x1;
        x1 = (x1 << rot[m][0]) | (x1 >> (64 - rot[m][0]));
        x1 ^= x0;
        x2 += x3;
        x3 = (x3 << rot[m][1]) | (x3 >> (64 - rot[m][1]));
        x3 ^= x2;
        const crd::u64 t = x1; // N=4 word permutation (0,3,2,1): swap x1 and x3
        x1 = x3;
        x3 = t;
        if ((r & 3) == 3) // key injection every 4 rounds
        {
            const int inj = (r + 1) / 4;
            x0 += ks[inj % 5];
            x1 += ks[(inj + 1) % 5];
            x2 += ks[(inj + 2) % 5];
            x3 += ks[(inj + 3) % 5] + static_cast<crd::u64>(inj);
        }
    }
    return ThreefryBlock{{x0, x1, x2, x3}};
}

namespace detail
{
inline constexpr int kThreefryRot[8][2] = {{14, 16}, {52, 57}, {23, 40}, {5, 37},
                                           {25, 33}, {46, 12}, {58, 22}, {32, 32}};

#if defined(__AVX2__)
inline __m256i tf_rotl(__m256i x, int r) noexcept
{
    return _mm256_or_si256(_mm256_slli_epi64(x, r), _mm256_srli_epi64(x, 64 - r));
}

// 4 Threefry blocks at once (counters base0+0..3; key schedule ks[5]) → 16 u64 in stream order. Bit-identical to the
// scalar block function. (Within-chunk wrap of counter[0] would need per-lane carry — only at counter ≈ 2^64.)
inline void threefry4x64_avx2_4(crd::u64 base0, crd::u64 c1, crd::u64 c2, crd::u64 c3, const crd::u64 (&ks)[5],
                                crd::u64* out) noexcept
{
    __m256i x0 = _mm256_add_epi64(_mm256_set_epi64x(static_cast<long long>(base0 + 3), static_cast<long long>(base0 + 2),
                                                    static_cast<long long>(base0 + 1), static_cast<long long>(base0)),
                                  _mm256_set1_epi64x(static_cast<long long>(ks[0])));
    __m256i x1 = _mm256_set1_epi64x(static_cast<long long>(c1 + ks[1]));
    __m256i x2 = _mm256_set1_epi64x(static_cast<long long>(c2 + ks[2]));
    __m256i x3 = _mm256_set1_epi64x(static_cast<long long>(c3 + ks[3]));
    for (int r = 0; r < 20; ++r)
    {
        const int m = r & 7;
        x0 = _mm256_add_epi64(x0, x1);
        x1 = _mm256_xor_si256(tf_rotl(x1, kThreefryRot[m][0]), x0);
        x2 = _mm256_add_epi64(x2, x3);
        x3 = _mm256_xor_si256(tf_rotl(x3, kThreefryRot[m][1]), x2);
        const __m256i t = x1; // permute: swap x1,x3
        x1 = x3;
        x3 = t;
        if ((r & 3) == 3)
        {
            const int inj = (r + 1) / 4;
            x0 = _mm256_add_epi64(x0, _mm256_set1_epi64x(static_cast<long long>(ks[inj % 5])));
            x1 = _mm256_add_epi64(x1, _mm256_set1_epi64x(static_cast<long long>(ks[(inj + 1) % 5])));
            x2 = _mm256_add_epi64(x2, _mm256_set1_epi64x(static_cast<long long>(ks[(inj + 2) % 5])));
            x3 = _mm256_add_epi64(
                x3, _mm256_set1_epi64x(static_cast<long long>(ks[(inj + 3) % 5] + static_cast<crd::u64>(inj))));
        }
    }
    // transpose 4×4 u64 (SoA lanes → AoS blocks) and store 16 u64 in stream order.
    const __m256i t0 = _mm256_unpacklo_epi64(x0, x1);
    const __m256i t1 = _mm256_unpackhi_epi64(x0, x1);
    const __m256i t2 = _mm256_unpacklo_epi64(x2, x3);
    const __m256i t3 = _mm256_unpackhi_epi64(x2, x3);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 0), _mm256_permute2x128_si256(t0, t2, 0x20));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 4), _mm256_permute2x128_si256(t1, t3, 0x20));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 8), _mm256_permute2x128_si256(t0, t2, 0x31));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 12), _mm256_permute2x128_si256(t1, t3, 0x31));
}
#endif
} // namespace detail

// Counter-RNG wrapper: a seekable u64 stream (key = seed/stream, counter auto-incremented; 4 u64 per block).
class ThreefryRng
{
public:
    explicit ThreefryRng(crd::u64 seed, crd::u64 stream = 0) noexcept : m_key{seed, stream, 0U, 0U} {}

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        if (m_idx == 4U)
        {
            refill();
        }
        return m_block.v[m_idx++];
    }

    // Bulk fill (AVX2 4-block); bit-identical to repeated next_u64(). The crush path vs NumPy/MATLAB.
    void fill(crd::containers::Span<crd::u64> out) noexcept
    {
        crd::usize i = 0;
        while (m_idx != 4U && i < out.size()) // drain the partial buffer first
        {
            out[i++] = next_u64();
        }
#if defined(__AVX2__)
        const crd::u64 ks[5] = {m_key[0], m_key[1], m_key[2], m_key[3],
                                0x1BD11BDAA9FC1A22ULL ^ m_key[0] ^ m_key[1] ^ m_key[2] ^ m_key[3]};
        while (i + 16U <= out.size())
        {
            detail::threefry4x64_avx2_4(m_counter[0], m_counter[1], m_counter[2], m_counter[3], ks, &out[i]);
            const crd::u64 old = m_counter[0];
            m_counter[0] += 4U; // advance 4 blocks (carry to high words = scalar's cumulative counter)
            if (m_counter[0] < old && ++m_counter[1] == 0U)
            {
                ++m_counter[2];
            }
            i += 16U;
        }
#endif
        while (i < out.size()) // tail (and the whole span when AVX2 is absent)
        {
            out[i++] = next_u64();
        }
    }

    // Seek to the n-th u64 (O(1) — counter-based).
    void seek(crd::u64 n) noexcept
    {
        m_counter[0] = n >> 2;
        m_counter[1] = 0;
        m_counter[2] = 0;
        m_counter[3] = 0;
        m_block = threefry4x64(m_counter, m_key);
        m_idx = static_cast<crd::u32>(n & 3U);
    }

private:
    void refill() noexcept
    {
        m_block = threefry4x64(m_counter, m_key);
        if (++m_counter[0] == 0U)
        {
            if (++m_counter[1] == 0U)
            {
                ++m_counter[2];
            }
        }
        m_idx = 0;
    }

    crd::u64 m_key[4];
    crd::u64 m_counter[4] = {0U, 0U, 0U, 0U};
    ThreefryBlock m_block{{0U, 0U, 0U, 0U}};
    crd::u32 m_idx = 4U;
};

} // namespace crd::hesap::stats
