// The erasure-code kernels of crd/hesap/net/erasure_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS) and reads only
// the expanded coefficients, so no function of this unit is shared with general code or with another level.
//
// The avx2, avx512 and neon bodies multiply by byte shuffles of the coefficient's two nibble tables (vpshufb, tbl),
// one vector column of up to four outputs at a time, so each source vector is loaded and split into nibbles once per
// group of four outputs. The GFNI variants of avx2 and avx512 (cmake/CrdSimd.cmake) apply each coefficient's bit matrix
// with one instruction instead. SSE2 has no byte shuffle: its body multiplies bit by bit, c·x as the XOR over the bits
// b of x of c·2^b, each selected by the sign mask of x shifted left by 7 - b; it accumulates one output and one source
// at a time over a tile of up to 1 KiB, so the eight broadcast products c·2^b are set once per tile. The portable body
// and SSE2's remainder look the nibbles up. GF(2^8) addition is a vector XOR at each level. All bodies give the same
// bytes.

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/erasure_kernels.hpp>

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2
#include <emmintrin.h>
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2 || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
#include <immintrin.h>
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON
#include <arm_neon.h>
#endif

namespace crd::hesap::net::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::u8;
using crd::usize;

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_PORTABLE || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2

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

#endif

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_PORTABLE || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2

// Bytes [first, bytes) of the output as the sum of the sources.
void sum_bytes(u8* output, const u8* const* sources, usize count, usize first, usize bytes, bool accumulate) noexcept
{
    for (usize i = first; i < bytes; ++i)
    {
        u8 acc = accumulate ? output[i] : u8{0U};
        for (usize j = 0U; j < count; ++j)
        {
            acc = static_cast<u8>(acc ^ sources[j][i]);
        }
        output[i] = acc;
    }
}

#endif

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_PORTABLE

void sum(u8* output, const u8* const* sources, usize count, usize bytes, bool accumulate) noexcept
{
    sum_bytes(output, sources, count, 0U, bytes, accumulate);
}

void dot(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
         usize output_count, usize bytes, bool accumulate) noexcept
{
    dot_bytes(coefficients, sources, count, outputs, output_count, 0U, bytes, accumulate);
}

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2

constexpr usize kTile = 1024U;

void sum(u8* output, const u8* const* sources, usize count, usize bytes, bool accumulate) noexcept
{
    const usize vector_bytes = bytes & ~usize{15U};
    for (usize i = 0U; i < vector_bytes; i += 16U)
    {
        __m128i acc = accumulate ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(output + i)) : _mm_setzero_si128();
        for (usize j = 0U; j < count; ++j)
        {
            acc = _mm_xor_si128(acc, _mm_loadu_si128(reinterpret_cast<const __m128i*>(sources[j] + i)));
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(output + i), acc);
    }
    sum_bytes(output, sources, count, vector_bytes, bytes, accumulate);
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
#if CRD_KERNEL_HAS_GFNI
    static V matrix(crd::u64 bits) noexcept { return _mm256_set1_epi64x(static_cast<long long>(bits)); }
    static V affine(V x, V bits) noexcept { return _mm256_gf2p8affine_epi64_epi8(x, bits, 0); }
#endif
    static V load_short(const u8* p, usize n) noexcept
    {
        alignas(kBytes) u8 padded[kBytes] = {};
        for (usize i = 0U; i < n; ++i)
        {
            padded[i] = p[i];
        }
        return load(padded);
    }
    static void store_short(u8* p, V v, usize n) noexcept
    {
        alignas(kBytes) u8 padded[kBytes];
        store(padded, v);
        for (usize i = 0U; i < n; ++i)
        {
            p[i] = padded[i];
        }
    }
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
#if CRD_KERNEL_HAS_GFNI
    static V matrix(crd::u64 bits) noexcept { return _mm512_set1_epi64(static_cast<long long>(bits)); }
    static V affine(V x, V bits) noexcept { return _mm512_gf2p8affine_epi64_epi8(x, bits, 0); }
#endif
    static V load_short(const u8* p, usize n) noexcept
    {
        return _mm512_maskz_loadu_epi8((__mmask64{1U} << n) - 1U, p);
    }
    static void store_short(u8* p, V v, usize n) noexcept
    {
        _mm512_mask_storeu_epi8(p, (__mmask64{1U} << n) - 1U, v);
    }
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
    static V load_short(const u8* p, usize n) noexcept
    {
        u8 padded[kBytes] = {};
        for (usize i = 0U; i < n; ++i)
        {
            padded[i] = p[i];
        }
        return load(padded);
    }
    static void store_short(u8* p, V v, usize n) noexcept
    {
        u8 padded[kBytes];
        store(padded, v);
        for (usize i = 0U; i < n; ++i)
        {
            p[i] = padded[i];
        }
    }
};

#else
#error "erasure_kernels.cpp has no body for this CPU level"
#endif

using V = Lanes::V;

// M vector columns of the output from byte i.
template <usize M>
inline void sum_columns(u8* output, const u8* const* sources, usize count, usize i, bool accumulate) noexcept
{
    V acc[M];
    for (usize m = 0U; m < M; ++m)
    {
        acc[m] = accumulate ? Lanes::load(output + i + (m * Lanes::kBytes)) : Lanes::zero();
    }
    for (usize j = 0U; j < count; ++j)
    {
        const u8* const source = sources[j] + i;
        for (usize m = 0U; m < M; ++m)
        {
            acc[m] = Lanes::bxor(acc[m], Lanes::load(source + (m * Lanes::kBytes)));
        }
    }
    for (usize m = 0U; m < M; ++m)
    {
        Lanes::store(output + i + (m * Lanes::kBytes), acc[m]);
    }
}

void sum(u8* output, const u8* const* sources, usize count, usize bytes, bool accumulate) noexcept
{
    constexpr usize kSumColumns = 4U;
    constexpr usize wide = kSumColumns * Lanes::kBytes;
    const usize tail = bytes % Lanes::kBytes;
    const usize vector_bytes = bytes - tail;
    usize i = 0U;
    if (count == 1U && accumulate)
    {
        // One addition, FlexFEC's: a tight loop leaves nothing for the columns to gain.
        const u8* const source = sources[0];
        for (; i < vector_bytes; i += Lanes::kBytes)
        {
            Lanes::store(output + i, Lanes::bxor(Lanes::load(output + i), Lanes::load(source + i)));
        }
        for (; i < bytes; ++i)
        {
            output[i] = static_cast<u8>(output[i] ^ source[i]);
        }
        return;
    }
    for (; i + wide <= vector_bytes; i += wide)
    {
        sum_columns<kSumColumns>(output, sources, count, i, accumulate);
    }
    for (; i < vector_bytes; i += Lanes::kBytes)
    {
        sum_columns<1U>(output, sources, count, i, accumulate);
    }
    if (tail == 0U)
    {
        return;
    }
    if (!accumulate && vector_bytes != 0U)
    {
        sum_columns<1U>(output, sources, count, bytes - Lanes::kBytes, false);
        return;
    }
    V acc = accumulate ? Lanes::load_short(output + vector_bytes, tail) : Lanes::zero();
    for (usize j = 0U; j < count; ++j)
    {
        acc = Lanes::bxor(acc, Lanes::load_short(sources[j] + vector_bytes, tail));
    }
    Lanes::store_short(output + vector_bytes, acc, tail);
}

#if CRD_KERNEL_HAS_GFNI

struct Multiplier
{
    V matrix;
};

struct SourceVector
{
    V x;
};

inline Multiplier multiplier(const GfCoefficient& c) noexcept
{
    return {Lanes::matrix(c.affine)};
}

inline SourceVector source_vector(V x) noexcept
{
    return {x};
}

inline V product(const Multiplier& c, const SourceVector& s) noexcept
{
    return Lanes::affine(s.x, c.matrix);
}

// Vector columns per pass over the sources, so each matrix is broadcast once for several of them.
template <usize G>
inline constexpr usize kColumns = 2U;
template <>
inline constexpr usize kColumns<1U> = 4U;
template <>
inline constexpr usize kColumns<2U> = 3U;

#else

struct Multiplier
{
    V lo;
    V hi;
};

struct SourceVector
{
    V lo;
    V hi;
};

inline Multiplier multiplier(const GfCoefficient& c) noexcept
{
    return {Lanes::table(c.lo), Lanes::table(c.hi)};
}

inline SourceVector source_vector(V x) noexcept
{
    return {Lanes::low_nibbles(x), Lanes::high_nibbles(x)};
}

inline V product(const Multiplier& c, const SourceVector& s) noexcept
{
    return Lanes::bxor(Lanes::lookup(c.lo, s.lo), Lanes::lookup(c.hi, s.hi));
}

template <usize G>
inline constexpr usize kColumns = G <= 2U ? 2U : 1U;

#endif

// M vector columns of G outputs from byte i (row g at coefficients + g * count).
template <usize G, usize M>
inline void dot_columns(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
                        usize i, bool accumulate) noexcept
{
    V acc[M][G];
    for (usize m = 0U; m < M; ++m)
    {
        for (usize g = 0U; g < G; ++g)
        {
            acc[m][g] = accumulate ? Lanes::load(outputs[g] + i + (m * Lanes::kBytes)) : Lanes::zero();
        }
    }
    for (usize j = 0U; j < count; ++j)
    {
        Multiplier row[G];
        for (usize g = 0U; g < G; ++g)
        {
            row[g] = multiplier(coefficients[(g * count) + j]);
        }
        for (usize m = 0U; m < M; ++m)
        {
            const SourceVector s = source_vector(Lanes::load(sources[j] + i + (m * Lanes::kBytes)));
            for (usize g = 0U; g < G; ++g)
            {
                acc[m][g] = Lanes::bxor(acc[m][g], product(row[g], s));
            }
        }
    }
    for (usize m = 0U; m < M; ++m)
    {
        for (usize g = 0U; g < G; ++g)
        {
            Lanes::store(outputs[g] + i + (m * Lanes::kBytes), acc[m][g]);
        }
    }
}

// G outputs, a few vector columns at a time. A remainder shorter than a vector is the last whole vector again when
// that writes the same bytes (nothing accumulates), else one column through padded vectors.
template <usize G>
void dot_group(const GfCoefficient* coefficients, const u8* const* sources, usize count, u8* const* outputs,
               usize bytes, bool accumulate) noexcept
{
    constexpr usize wide = kColumns<G> * Lanes::kBytes;
    const usize tail = bytes % Lanes::kBytes;
    const usize vector_bytes = bytes - tail;
    usize i = 0U;
    for (; i + wide <= vector_bytes; i += wide)
    {
        dot_columns<G, kColumns<G>>(coefficients, sources, count, outputs, i, accumulate);
    }
    for (; i < vector_bytes; i += Lanes::kBytes)
    {
        dot_columns<G, 1U>(coefficients, sources, count, outputs, i, accumulate);
    }
    if (tail == 0U)
    {
        return;
    }
    if (!accumulate && vector_bytes != 0U)
    {
        dot_columns<G, 1U>(coefficients, sources, count, outputs, bytes - Lanes::kBytes, false);
        return;
    }
    V acc[G];
    for (usize g = 0U; g < G; ++g)
    {
        acc[g] = accumulate ? Lanes::load_short(outputs[g] + vector_bytes, tail) : Lanes::zero();
    }
    for (usize j = 0U; j < count; ++j)
    {
        const SourceVector s = source_vector(Lanes::load_short(sources[j] + vector_bytes, tail));
        for (usize g = 0U; g < G; ++g)
        {
            acc[g] = Lanes::bxor(acc[g], product(multiplier(coefficients[(g * count) + j]), s));
        }
    }
    for (usize g = 0U; g < G; ++g)
    {
        Lanes::store_short(outputs[g] + vector_bytes, acc[g], tail);
    }
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
} // namespace crd::hesap::net::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::net
{

const ErasureKernels CRD_KERNEL_TABLE(kErasureKernels) = {&CRD_MATH_LEVEL_NS::dot, &CRD_MATH_LEVEL_NS::sum};

} // namespace crd::hesap::net
