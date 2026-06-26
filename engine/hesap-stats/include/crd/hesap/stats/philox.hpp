#pragma once

// philox.hpp — Phase 3.1.6 v12-PULL (built early for v7-i, per the v7 plan's "name the dep or build a minimal
// RNG"): the **Philox4x32-10 counter-based RNG** (Salmon-Moraes-Dror-Shaw, SC'11 "Parallel Random Numbers:
// As Easy as 1, 2, 3" — the Random123 generator JAX/XLA and cuRAND standardize on).
//
// WHY COUNTER-BASED (the determinism-moat fit): the generator is a PURE FUNCTION (counter, key) → 4×u32 —
// there is no sequential hidden state, so the value at any position exists independently of who computes it,
// in what order, on how many workers. Reproducible minibatch sampling (v7-i), replay (eylem), and the v12
// stats cluster all get bit-exact randomness by CONSTRUCTION: same (seed, stream, position) ⇒ same value,
// {1..16} workers alike. Crush-class statistical quality (passes BigCrush per the paper), 2^130 distinct
// streams (64-bit seed × 64-bit stream id), O(1) random access.
//
// VERIFICATION: `philox4x32` is tested against the PUBLISHED Random123 known-answer vectors (zero, all-ones,
// and the π-digits counter/key lines) in tests/hesap-stats/test_philox.cpp — the implementation is correct
// iff all three match (they probe every constant + the round/bump structure).
//
// Raw lower layer (u32/u64/f32/f64 — ADR-0078 §5). Header-only; integer-only (no <cmath>).

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace crd::hesap::stats
{

// One 128-bit Philox block: 4 lanes of u32.
struct PhiloxBlock
{
    crd::u32 v[4];
};

// The Philox4x32-10 block function: PURE — output depends only on (counter, key). constexpr so known-answer
// checks can run at compile time too. Counter lanes are (c0, c1, c2, c3); key lanes (k0, k1).
[[nodiscard]] constexpr PhiloxBlock philox4x32(const crd::u32 (&counter)[4], const crd::u32 (&key)[2]) noexcept
{
    constexpr crd::u32 m0 = 0xD2511F53U; // PHILOX_M4x32_0
    constexpr crd::u32 m1 = 0xCD9E8D57U; // PHILOX_M4x32_1
    constexpr crd::u32 w0 = 0x9E3779B9U; // PHILOX_W32_0 (golden ratio)
    constexpr crd::u32 w1 = 0xBB67AE85U; // PHILOX_W32_1 (sqrt(3)−1)

    crd::u32 c0 = counter[0];
    crd::u32 c1 = counter[1];
    crd::u32 c2 = counter[2];
    crd::u32 c3 = counter[3];
    crd::u32 k0 = key[0];
    crd::u32 k1 = key[1];

    for (int round = 0; round < 10; ++round)
    {
        if (round > 0) // the key schedule bumps BETWEEN rounds (9 bumps for 10 rounds)
        {
            k0 += w0;
            k1 += w1;
        }
        const crd::u64 p0 = static_cast<crd::u64>(m0) * c0;
        const crd::u64 p1 = static_cast<crd::u64>(m1) * c2;
        const crd::u32 hi0 = static_cast<crd::u32>(p0 >> 32);
        const crd::u32 lo0 = static_cast<crd::u32>(p0);
        const crd::u32 hi1 = static_cast<crd::u32>(p1 >> 32);
        const crd::u32 lo1 = static_cast<crd::u32>(p1);
        const crd::u32 n0 = hi1 ^ c1 ^ k0;
        const crd::u32 n1 = lo1;
        const crd::u32 n2 = hi0 ^ c3 ^ k1;
        const crd::u32 n3 = lo0;
        c0 = n0;
        c1 = n1;
        c2 = n2;
        c3 = n3;
    }
    return PhiloxBlock{{c0, c1, c2, c3}};
}

#if defined(__AVX2__)
namespace detail
{
// 8-wide u32 high product: lane i = (a[i]*b[i]) >> 32.
inline __m256i philox_mulhi_epu32(__m256i a, __m256i b) noexcept
{
    const __m256i evn = _mm256_mul_epu32(a, b);                                                     // lanes 0,2,4,6
    const __m256i odd = _mm256_mul_epu32(_mm256_srli_epi64(a, 32), _mm256_srli_epi64(b, 32));       // lanes 1,3,5,7
    return _mm256_blend_epi32(_mm256_srli_epi64(evn, 32), _mm256_slli_epi64(_mm256_srli_epi64(odd, 32), 32), 0xAA);
}

// 8 Philox4x32-10 blocks at once (per-lane counters c0/c1; stream c2/c3; key k0/k1) → 16 u64 in next_u64 stream
// order. Bit-identical to the scalar block function (the round + key schedule are transcribed exactly).
inline void philox4x32_avx2_8(const crd::u32 (&c0s)[8], const crd::u32 (&c1s)[8], crd::u32 stream_lo,
                              crd::u32 stream_hi, crd::u32 key0, crd::u32 key1, crd::u64* out) noexcept
{
    const __m256i m0 = _mm256_set1_epi32(static_cast<int>(0xD2511F53U));
    const __m256i m1 = _mm256_set1_epi32(static_cast<int>(0xCD9E8D57U));
    const __m256i w0 = _mm256_set1_epi32(static_cast<int>(0x9E3779B9U));
    const __m256i w1 = _mm256_set1_epi32(static_cast<int>(0xBB67AE85U));
    __m256i c0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c0s));
    __m256i c1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c1s));
    __m256i c2 = _mm256_set1_epi32(static_cast<int>(stream_lo));
    __m256i c3 = _mm256_set1_epi32(static_cast<int>(stream_hi));
    __m256i k0 = _mm256_set1_epi32(static_cast<int>(key0));
    __m256i k1 = _mm256_set1_epi32(static_cast<int>(key1));
    for (int round = 0; round < 10; ++round)
    {
        if (round > 0)
        {
            k0 = _mm256_add_epi32(k0, w0);
            k1 = _mm256_add_epi32(k1, w1);
        }
        const __m256i hi0 = philox_mulhi_epu32(m0, c0);
        const __m256i lo0 = _mm256_mullo_epi32(m0, c0);
        const __m256i hi1 = philox_mulhi_epu32(m1, c2);
        const __m256i lo1 = _mm256_mullo_epi32(m1, c2);
        const __m256i n0 = _mm256_xor_si256(_mm256_xor_si256(hi1, c1), k0);
        const __m256i n2 = _mm256_xor_si256(_mm256_xor_si256(hi0, c3), k1);
        c0 = n0;
        c1 = lo1;
        c2 = n2;
        c3 = lo0;
    }
    // transpose 8×4 u32 (SoA → AoS blocks) — the AoS u32 stream reinterpreted as u64 = the next_u64 stream.
    const __m256i a = _mm256_unpacklo_epi32(c0, c1);
    const __m256i b = _mm256_unpackhi_epi32(c0, c1);
    const __m256i c = _mm256_unpacklo_epi32(c2, c3);
    const __m256i d = _mm256_unpackhi_epi32(c2, c3);
    const __m256i e = _mm256_unpacklo_epi64(a, c);
    const __m256i f = _mm256_unpackhi_epi64(a, c);
    const __m256i g = _mm256_unpacklo_epi64(b, d);
    const __m256i h = _mm256_unpackhi_epi64(b, d);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 0), _mm256_permute2x128_si256(e, f, 0x20));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 4), _mm256_permute2x128_si256(g, h, 0x20));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 8), _mm256_permute2x128_si256(e, f, 0x31));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 12), _mm256_permute2x128_si256(g, h, 0x31));
}
} // namespace detail
#endif

// Stateful CONVENIENCE wrapper over the pure block function. Layout of the 128-bit counter:
//   (c0, c1) = the 64-bit block position (low, high) · (c2, c3) = the 64-bit stream id (low, high)
// and key = the 64-bit seed. Same (seed, stream, position) ⇒ same value, on any thread/worker — the
// counter-based moat. `jump_to_block` gives O(1) random access into the stream.
class PhiloxRng
{
public:
    explicit PhiloxRng(crd::u64 seed, crd::u64 stream = 0) noexcept : m_seed(seed), m_stream(stream) {}

    // ---- sequential draws (buffered 4-lane blocks) ----

    [[nodiscard]] crd::u32 next_u32() noexcept
    {
        if (m_lane == 4U)
        {
            refill();
        }
        return m_block.v[m_lane++];
    }

    [[nodiscard]] crd::u64 next_u64() noexcept
    {
        const crd::u64 lo = next_u32();
        const crd::u64 hi = next_u32();
        return (hi << 32) | lo;
    }

    // Uniform [0, 1) with the full 53-bit / 24-bit mantissa resolution.
    [[nodiscard]] crd::f64 next_f64() noexcept
    {
        return static_cast<crd::f64>(next_u64() >> 11) * (1.0 / 9007199254740992.0); // / 2^53
    }
    [[nodiscard]] crd::f32 next_f32() noexcept
    {
        return static_cast<crd::f32>(next_u32() >> 8) * (1.0F / 16777216.0F); // / 2^24
    }

    // Unbiased uniform integer in [0, bound) — Lemire-style rejection on the 64-bit draw. bound ≥ 1.
    [[nodiscard]] crd::u64 next_below(crd::u64 bound) noexcept
    {
        CRD_ASSERT_MSG(bound >= 1U, "PhiloxRng::next_below: bound must be >= 1");
        const crd::u64 threshold = (~bound + 1U) % bound; // (2^64 − bound) mod bound
        for (;;)
        {
            const crd::u64 x = next_u64();
            if (x >= threshold)
            {
                return x % bound;
            }
        }
    }

    // ---- O(1) random access ----

    // Jump so the NEXT draw is lane 0 of the given 64-bit block index (4 u32 draws per block).
    void jump_to_block(crd::u64 block_index) noexcept
    {
        m_pos = block_index;
        m_lane = 4U; // force refill on the next draw
    }
    [[nodiscard]] crd::u64 seed() const noexcept { return m_seed; }
    [[nodiscard]] crd::u64 stream() const noexcept { return m_stream; }

    // Bulk fill (AVX2 8-block); bit-identical to repeated next_u64(). The crush path vs NumPy/MATLAB.
    void fill(crd::containers::Span<crd::u64> out) noexcept
    {
        crd::usize i = 0;
        while (m_lane != 4U && i < out.size()) // drain a partial buffered block first
        {
            out[i++] = next_u64();
        }
#if defined(__AVX2__)
        const crd::u32 stream_lo = static_cast<crd::u32>(m_stream);
        const crd::u32 stream_hi = static_cast<crd::u32>(m_stream >> 32);
        const crd::u32 key0 = static_cast<crd::u32>(m_seed);
        const crd::u32 key1 = static_cast<crd::u32>(m_seed >> 32);
        while (i + 16U <= out.size())
        {
            crd::u32 c0s[8];
            crd::u32 c1s[8];
            for (int j = 0; j < 8; ++j) // per-lane counters (handles the 32-bit position wrap exactly)
            {
                const crd::u64 p = m_pos + static_cast<crd::u64>(j);
                c0s[j] = static_cast<crd::u32>(p);
                c1s[j] = static_cast<crd::u32>(p >> 32);
            }
            detail::philox4x32_avx2_8(c0s, c1s, stream_lo, stream_hi, key0, key1, &out[i]);
            m_pos += 8U;
            i += 16U;
        }
#endif
        while (i < out.size()) // tail (and the whole span when AVX2 is absent)
        {
            out[i++] = next_u64();
        }
    }

private:
    void refill() noexcept
    {
        const crd::u32 counter[4] = {static_cast<crd::u32>(m_pos), static_cast<crd::u32>(m_pos >> 32),
                                     static_cast<crd::u32>(m_stream), static_cast<crd::u32>(m_stream >> 32)};
        const crd::u32 key[2] = {static_cast<crd::u32>(m_seed), static_cast<crd::u32>(m_seed >> 32)};
        m_block = philox4x32(counter, key);
        ++m_pos;
        m_lane = 0U;
    }

    crd::u64 m_seed;
    crd::u64 m_stream;
    crd::u64 m_pos = 0;
    PhiloxBlock m_block{{0U, 0U, 0U, 0U}};
    crd::u32 m_lane = 4U; // empty ⇒ refill on first draw
};

// Deterministic Fisher-Yates shuffle (the reproducible-minibatch primitive): same rng state ⇒ same
// permutation, bit-exact on any worker count (the shuffle itself is serial by definition).
template <typename T> inline void shuffle(crd::containers::Span<T> items, PhiloxRng& rng) noexcept
{
    const crd::usize n = items.size();
    for (crd::usize i = n; i > 1; --i)
    {
        const crd::usize j = static_cast<crd::usize>(rng.next_below(static_cast<crd::u64>(i)));
        const T tmp = items[i - 1];
        items[i - 1] = items[j];
        items[j] = tmp;
    }
}

} // namespace crd::hesap::stats
