// The hash kernel family (crd/containers/checksum_kernels.hpp): XXH3's long-input loop, compiled by crd_add_cpu_kernels
// (../CMakeLists.txt) at portable, sse2, avx2, avx512 and neon; crd/core/kernel_level.hpp names the unit's level.
//
// The xxHash 0.8 specification (Collet, doc/xxhash_spec.md): 8 u64 accumulators; each 64-byte stripe adds every input
// word to its neighbour's accumulator and the product of the low and high halves of input xor secret to its own; every
// 16 stripes the accumulators are scrambled. Integer lanes only, so every level gives the same bits.

#include <crd/containers/checksum_kernels.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>

#include <bit>
#include <cstring>

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2 || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2 ||                          \
    CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
#include <immintrin.h>
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON
#include <arm_neon.h>
#endif

namespace crd::containers::inline CRD_MATH_LEVEL_NS
{
namespace
{

static_assert(std::endian::native == std::endian::little, "XXH3 reads little-endian words");

constexpr crd::u32 kPrime32One = 0x9E3779B1U;
constexpr crd::usize kStripe = 64U;
constexpr crd::usize kSecretSize = 192U;
constexpr crd::usize kStripesPerBlock = (kSecretSize - kStripe) / 8U;
constexpr crd::usize kBlock = kStripe * kStripesPerBlock;
constexpr crd::usize kLastStripeSecret = kSecretSize - kStripe - 7U; // XXH_SECRET_LASTACC_START

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2

struct Accumulators
{
    __m128i v[4]; // NOLINT(modernize-avoid-c-arrays): register lanes

    void load(const crd::u64* acc) noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            v[i] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + 2 * i));
        }
    }
    void store(crd::u64* acc) const noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(acc + 2 * i), v[i]);
        }
    }
    void accumulate(const crd::u8* in, const crd::u8* secret) noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            const __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16 * i));
            const __m128i key = _mm_xor_si128(data, _mm_loadu_si128(reinterpret_cast<const __m128i*>(secret + 16 * i)));
            const __m128i product = _mm_mul_epu32(key, _mm_shuffle_epi32(key, _MM_SHUFFLE(0, 3, 0, 1)));
            v[i] = _mm_add_epi64(_mm_add_epi64(v[i], _mm_shuffle_epi32(data, _MM_SHUFFLE(1, 0, 3, 2))), product);
        }
    }
    void scramble(const crd::u8* secret) noexcept
    {
        const __m128i prime = _mm_set1_epi32(static_cast<int>(kPrime32One));
        for (int i = 0; i < 4; ++i)
        {
            __m128i x = _mm_xor_si128(v[i], _mm_srli_epi64(v[i], 47));
            x = _mm_xor_si128(x, _mm_loadu_si128(reinterpret_cast<const __m128i*>(secret + 16 * i)));
            const __m128i low = _mm_mul_epu32(x, prime);
            const __m128i high = _mm_mul_epu32(_mm_shuffle_epi32(x, _MM_SHUFFLE(0, 3, 0, 1)), prime);
            v[i] = _mm_add_epi64(low, _mm_slli_epi64(high, 32));
        }
    }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2

struct Accumulators
{
    __m256i v[2]; // NOLINT(modernize-avoid-c-arrays): register lanes

    void load(const crd::u64* acc) noexcept
    {
        for (int i = 0; i < 2; ++i)
        {
            v[i] = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + 4 * i));
        }
    }
    void store(crd::u64* acc) const noexcept
    {
        for (int i = 0; i < 2; ++i)
        {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc + 4 * i), v[i]);
        }
    }
    void accumulate(const crd::u8* in, const crd::u8* secret) noexcept
    {
        for (int i = 0; i < 2; ++i)
        {
            const __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + 32 * i));
            const __m256i key =
                _mm256_xor_si256(data, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(secret + 32 * i)));
            const __m256i product = _mm256_mul_epu32(key, _mm256_shuffle_epi32(key, _MM_SHUFFLE(0, 3, 0, 1)));
            v[i] =
                _mm256_add_epi64(_mm256_add_epi64(v[i], _mm256_shuffle_epi32(data, _MM_SHUFFLE(1, 0, 3, 2))), product);
        }
    }
    void scramble(const crd::u8* secret) noexcept
    {
        const __m256i prime = _mm256_set1_epi32(static_cast<int>(kPrime32One));
        for (int i = 0; i < 2; ++i)
        {
            __m256i x = _mm256_xor_si256(v[i], _mm256_srli_epi64(v[i], 47));
            x = _mm256_xor_si256(x, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(secret + 32 * i)));
            const __m256i low = _mm256_mul_epu32(x, prime);
            const __m256i high = _mm256_mul_epu32(_mm256_shuffle_epi32(x, _MM_SHUFFLE(0, 3, 0, 1)), prime);
            v[i] = _mm256_add_epi64(low, _mm256_slli_epi64(high, 32));
        }
    }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512

struct Accumulators
{
    __m512i v;

    void load(const crd::u64* acc) noexcept { v = _mm512_loadu_si512(acc); }
    void store(crd::u64* acc) const noexcept { _mm512_storeu_si512(acc, v); }
    void accumulate(const crd::u8* in, const crd::u8* secret) noexcept
    {
        const __m512i data = _mm512_loadu_si512(in);
        const __m512i key = _mm512_xor_si512(data, _mm512_loadu_si512(secret));
        const __m512i product = _mm512_mul_epu32(key, _mm512_shuffle_epi32(key, _MM_PERM_ADAB));
        v = _mm512_add_epi64(_mm512_add_epi64(v, _mm512_shuffle_epi32(data, _MM_PERM_BADC)), product);
    }
    void scramble(const crd::u8* secret) noexcept
    {
        // x ^ (x >> 47) ^ secret in one three-input logic step.
        const __m512i x = _mm512_ternarylogic_epi64(v, _mm512_srli_epi64(v, 47), _mm512_loadu_si512(secret), 0x96);
        const __m512i prime = _mm512_set1_epi32(static_cast<int>(kPrime32One));
        const __m512i low = _mm512_mul_epu32(x, prime);
        const __m512i high = _mm512_mul_epu32(_mm512_shuffle_epi32(x, _MM_PERM_ADAB), prime);
        v = _mm512_add_epi64(low, _mm512_slli_epi64(high, 32));
    }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON

struct Accumulators
{
    uint64x2_t v[4]; // NOLINT(modernize-avoid-c-arrays): register lanes

    void load(const crd::u64* acc) noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            v[i] = vld1q_u64(acc + 2 * i);
        }
    }
    void store(crd::u64* acc) const noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            vst1q_u64(acc + 2 * i, v[i]);
        }
    }
    void accumulate(const crd::u8* in, const crd::u8* secret) noexcept
    {
        for (int i = 0; i < 4; ++i)
        {
            const uint64x2_t data = vreinterpretq_u64_u8(vld1q_u8(in + 16 * i));
            const uint64x2_t key = veorq_u64(data, vreinterpretq_u64_u8(vld1q_u8(secret + 16 * i)));
            const uint64x2_t product = vmull_u32(vmovn_u64(key), vshrn_n_u64(key, 32));
            v[i] = vaddq_u64(vaddq_u64(v[i], vextq_u64(data, data, 1)), product);
        }
    }
    void scramble(const crd::u8* secret) noexcept
    {
        const uint32x2_t prime = vdup_n_u32(kPrime32One);
        for (int i = 0; i < 4; ++i)
        {
            uint64x2_t x = veorq_u64(v[i], vshrq_n_u64(v[i], 47));
            x = veorq_u64(x, vreinterpretq_u64_u8(vld1q_u8(secret + 16 * i)));
            const uint64x2_t high = vshlq_n_u64(vmull_u32(vshrn_n_u64(x, 32), prime), 32);
            v[i] = vmlal_u32(high, vmovn_u64(x), prime);
        }
    }
};

#else

crd::u64 load_u64(const crd::u8* p) noexcept
{
    crd::u64 v = 0U;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

struct Accumulators
{
    crd::u64 v[8]; // NOLINT(modernize-avoid-c-arrays): the accumulators

    void load(const crd::u64* acc) noexcept { std::memcpy(v, acc, sizeof(v)); }
    void store(crd::u64* acc) const noexcept { std::memcpy(acc, v, sizeof(v)); }
    void accumulate(const crd::u8* in, const crd::u8* secret) noexcept
    {
        for (int i = 0; i < 8; ++i)
        {
            const crd::u64 data = load_u64(in + 8 * i);
            const crd::u64 key = data ^ load_u64(secret + 8 * i);
            v[i ^ 1] += data;
            v[i] += (key & 0xFFFFFFFFU) * (key >> 32U);
        }
    }
    void scramble(const crd::u8* secret) noexcept
    {
        for (int i = 0; i < 8; ++i)
        {
            crd::u64 x = v[i] ^ (v[i] >> 47U);
            x ^= load_u64(secret + 8 * i);
            v[i] = x * kPrime32One;
        }
    }
};

#endif

void xxh3_accumulate(crd::u64* acc, const crd::u8* data, crd::usize n, const crd::u8* secret) noexcept
{
    Accumulators a{};
    a.load(acc);
    const crd::usize blocks = (n - 1U) / kBlock;
    for (crd::usize b = 0U; b < blocks; ++b)
    {
        const crd::u8* block = data + b * kBlock;
        for (crd::usize s = 0U; s < kStripesPerBlock; ++s)
        {
            a.accumulate(block + s * kStripe, secret + s * 8U);
        }
        a.scramble(secret + kSecretSize - kStripe);
    }
    const crd::u8* tail = data + blocks * kBlock;
    const crd::usize stripes = ((n - 1U) - blocks * kBlock) / kStripe;
    for (crd::usize s = 0U; s < stripes; ++s)
    {
        a.accumulate(tail + s * kStripe, secret + s * 8U);
    }
    a.accumulate(data + n - kStripe, secret + kLastStripeSecret);
    a.store(acc);
}

} // namespace
} // namespace crd::containers::inline CRD_MATH_LEVEL_NS

namespace crd::containers
{

const HashKernels CRD_KERNEL_TABLE(kHashKernels) = {&CRD_MATH_LEVEL_NS::xxh3_accumulate};

} // namespace crd::containers
