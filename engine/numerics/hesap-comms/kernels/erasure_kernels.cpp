// The erasure-code kernels of crd/hesap/comms/erasure_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS) and reads only
// the expanded coefficients, so no function of this unit is shared with general code or with another level.
//
// The avx2, avx512 and neon bodies multiply by byte shuffles of the coefficient's two nibble tables (vpshufb, tbl),
// one vector column of up to four outputs at a time, so each source vector is loaded and split into nibbles once per
// group of four outputs. SSE2 has no byte shuffle: its body multiplies bit by bit, c·x as the XOR over the bits b of
// x of c·2^b, each selected by the sign mask of x shifted left by 7 - b; it accumulates one output and one source at a
// time over a tile of up to 1 KiB, so the eight broadcast products c·2^b are set once per tile. The portable body and
// every scalar remainder look the nibbles up. GF(2^8) addition is a vector XOR at each level. All bodies give the same
// bytes.

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/comms/erasure_kernels.hpp>

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2
#include <emmintrin.h>
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2 || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
#include <immintrin.h>
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON
#include <arm_neon.h>
#endif

namespace crd::hesap::comms::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::u8;
using crd::usize;

// Bytes [first, bytes) of every output by nibble lookups; row g of the coefficients starts at coefficients + g * count.
void dot_bytes(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
               usize output_count, usize first, usize bytes, bool accumulate) noexcept
{
    for (usize g = 0U; g < output_count; ++g)
    {
        const GfCoefficient* row = coefficients + (g * count);
        u8* out = outputs[g];
        for (usize i = first; i < bytes; ++i)
        {
            u8 acc = accumulate ? out[i] : u8{0U};
            for (usize j = 0U; j < count; ++j)
            {
                const u8 x = sources[j][i];
                acc = static_cast<u8>(acc ^ row[j].lo[x & 15U] ^ row[j].hi[x >> 4U]);
            }
            out[i] = acc;
        }
    }
}

// Bytes [first, bytes) of output ^= source.
void add_bytes(u8* output, const u8* source, usize first, usize bytes) noexcept
{
    for (usize i = first; i < bytes; ++i)
    {
        output[i] = static_cast<u8>(output[i] ^ source[i]);
    }
}

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_PORTABLE

void add(u8* output, const u8* source, usize bytes) noexcept
{
    add_bytes(output, source, 0U, bytes);
}

void dot(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
         usize output_count, usize bytes, bool accumulate) noexcept
{
    dot_bytes(coefficients, sources, count, outputs, output_count, 0U, bytes, accumulate);
}

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2

constexpr usize kTile = 1024U;

void add(u8* output, const u8* source, usize bytes) noexcept
{
    const usize vector_bytes = bytes & ~usize{15U};
    for (usize i = 0U; i < vector_bytes; i += 16U)
    {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(output + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + i), _mm_xor_si128(x, y));
    }
    add_bytes(output, source, vector_bytes, bytes);
}

void dot(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
         usize output_count, usize bytes, bool accumulate) noexcept
{
    const usize vector_bytes = bytes & ~usize{15U};
    const __m128i zero = _mm_setzero_si128();
    for (usize g = 0U; g < output_count; ++g)
    {
        const GfCoefficient* row = coefficients + (g * count);
        u8* out = outputs[g];
        for (usize tile = 0U; tile < vector_bytes; tile += kTile)
        {
            const usize end = tile + kTile < vector_bytes ? tile + kTile : vector_bytes;
            if (!accumulate)
            {
                for (usize i = tile; i < end; i += 16U)
                {
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), zero);
                }
            }
            for (usize j = 0U; j < count; ++j)
            {
                const GfCoefficient& c = row[j];
                // c·2^b for b = 7 .. 0: the high nibble table's 8, 4, 2, 1, then the low one's.
                const __m128i p[8] = {
                    _mm_set1_epi8(static_cast<char>(c.hi[8])), _mm_set1_epi8(static_cast<char>(c.hi[4])),
                    _mm_set1_epi8(static_cast<char>(c.hi[2])), _mm_set1_epi8(static_cast<char>(c.hi[1])),
                    _mm_set1_epi8(static_cast<char>(c.lo[8])), _mm_set1_epi8(static_cast<char>(c.lo[4])),
                    _mm_set1_epi8(static_cast<char>(c.lo[2])), _mm_set1_epi8(static_cast<char>(c.lo[1]))};
                const u8* source = sources[j];
                for (usize i = tile; i < end; i += 16U)
                {
                    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + i));
                    __m128i acc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(out + i));
                    for (usize b = 0U; b < 8U; ++b)
                    {
                        acc = _mm_xor_si128(acc, _mm_and_si128(_mm_cmpgt_epi8(zero, x), p[b]));
                        x = _mm_add_epi8(x, x);
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), acc);
                }
            }
        }
    }
    dot_bytes(coefficients, sources, count, outputs, output_count, vector_bytes, bytes, accumulate);
}

#else

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2

struct Lanes
{
    using V = __m256i;
    static constexpr usize kBytes = 32U;
    static V zero() noexcept { return _mm256_setzero_si256(); }
    static V load(const u8* p) noexcept { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }
    static void store(u8* p, V v) noexcept { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v); }
    static V low_nibbles(V x) noexcept { return _mm256_and_si256(x, _mm256_set1_epi8(0x0F)); }
    static V high_nibbles(V x) noexcept { return _mm256_and_si256(_mm256_srli_epi64(x, 4), _mm256_set1_epi8(0x0F)); }
    static V table(const u8* t) noexcept
    {
        return _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(t)));
    }
    static V lookup(V t, V index) noexcept { return _mm256_shuffle_epi8(t, index); }
    static V bxor(V a, V b) noexcept { return _mm256_xor_si256(a, b); }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512

struct Lanes
{
    using V = __m512i;
    static constexpr usize kBytes = 64U;
    static V zero() noexcept { return _mm512_setzero_si512(); }
    static V load(const u8* p) noexcept { return _mm512_loadu_si512(p); }
    static void store(u8* p, V v) noexcept { _mm512_storeu_si512(p, v); }
    static V low_nibbles(V x) noexcept { return _mm512_and_si512(x, _mm512_set1_epi8(0x0F)); }
    static V high_nibbles(V x) noexcept { return _mm512_and_si512(_mm512_srli_epi64(x, 4), _mm512_set1_epi8(0x0F)); }
    static V table(const u8* t) noexcept
    {
        return _mm512_broadcast_i32x4(_mm_loadu_si128(reinterpret_cast<const __m128i*>(t)));
    }
    static V lookup(V t, V index) noexcept { return _mm512_shuffle_epi8(t, index); }
    static V bxor(V a, V b) noexcept { return _mm512_xor_si512(a, b); }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON

struct Lanes
{
    using V = uint8x16_t;
    static constexpr usize kBytes = 16U;
    static V zero() noexcept { return vdupq_n_u8(0U); }
    static V load(const u8* p) noexcept { return vld1q_u8(p); }
    static void store(u8* p, V v) noexcept { vst1q_u8(p, v); }
    static V low_nibbles(V x) noexcept { return vandq_u8(x, vdupq_n_u8(0x0FU)); }
    static V high_nibbles(V x) noexcept { return vshrq_n_u8(x, 4); }
    static V table(const u8* t) noexcept { return vld1q_u8(t); }
    static V lookup(V t, V index) noexcept { return vqtbl1q_u8(t, index); }
    static V bxor(V a, V b) noexcept { return veorq_u8(a, b); }
};

#else
#error "erasure_kernels.cpp has no body for this CPU level"
#endif

using V = Lanes::V;

void add(u8* output, const u8* source, usize bytes) noexcept
{
    const usize vector_bytes = bytes - (bytes % Lanes::kBytes);
    for (usize i = 0U; i < vector_bytes; i += Lanes::kBytes)
    {
        Lanes::store(output + i, Lanes::bxor(Lanes::load(output + i), Lanes::load(source + i)));
    }
    add_bytes(output, source, vector_bytes, bytes);
}

// G outputs (row g at coefficients + g * count), one vector column at a time.
template <usize G>
void dot_group(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
               usize bytes, bool accumulate) noexcept
{
    const usize vector_bytes = bytes - (bytes % Lanes::kBytes);
    for (usize i = 0U; i < vector_bytes; i += Lanes::kBytes)
    {
        V acc[G];
        for (usize g = 0U; g < G; ++g)
        {
            acc[g] = accumulate ? Lanes::load(outputs[g] + i) : Lanes::zero();
        }
        for (usize j = 0U; j < count; ++j)
        {
            const V x = Lanes::load(sources[j] + i);
            const V lo = Lanes::low_nibbles(x);
            const V hi = Lanes::high_nibbles(x);
            for (usize g = 0U; g < G; ++g)
            {
                const GfCoefficient& c = coefficients[(g * count) + j];
                acc[g] = Lanes::bxor(acc[g], Lanes::bxor(Lanes::lookup(Lanes::table(c.lo), lo),
                                                         Lanes::lookup(Lanes::table(c.hi), hi)));
            }
        }
        for (usize g = 0U; g < G; ++g)
        {
            Lanes::store(outputs[g] + i, acc[g]);
        }
    }
    dot_bytes(coefficients, sources, count, outputs, G, vector_bytes, bytes, accumulate);
}

void dot(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
         usize output_count, usize bytes, bool accumulate) noexcept
{
    usize g = 0U;
    for (; g + 4U <= output_count; g += 4U)
    {
        dot_group<4U>(coefficients + (g * count), sources, count, outputs + g, bytes, accumulate);
    }
    const GfCoefficient* rest = coefficients + (g * count);
    switch (output_count - g)
    {
    case 3U:
        dot_group<3U>(rest, sources, count, outputs + g, bytes, accumulate);
        break;
    case 2U:
        dot_group<2U>(rest, sources, count, outputs + g, bytes, accumulate);
        break;
    case 1U:
        dot_group<1U>(rest, sources, count, outputs + g, bytes, accumulate);
        break;
    default:
        break;
    }
}

#endif

} // namespace
} // namespace crd::hesap::comms::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::comms
{

const ErasureKernels CRD_KERNEL_TABLE(kErasureKernels) = {&CRD_MATH_LEVEL_NS::dot, &CRD_MATH_LEVEL_NS::add};

} // namespace crd::hesap::comms
