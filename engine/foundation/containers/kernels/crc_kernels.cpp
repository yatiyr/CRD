// The crc kernel family (crd/containers/checksum_kernels.hpp), compiled by crd_add_cpu_kernels (../CMakeLists.txt) at
// portable and sse4.2 and as the PCLMULQDQ, VPCLMULQDQ, CRC32 and PMULL variants; crd/core/kernel_level.hpp names the
// unit's level.
//
// Every body computes the same reflected CRC register, so every level gives the same bits. The carry-less multiply
// folds follow Gopal et al., "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction" (Intel, 2009),
// for reflected polynomials: a 128-bit lane holding message bits at distance D ahead of the next data folds as
// lo64 * (x^(D+32) mod P) xor hi64 * (x^(D-32) mod P), each constant bit-reflected and shifted left once; 128 bits
// reduce to 64 with x^64 mod P and to 32 by Barrett reduction with floor(x^64 / P). The constants are computed below
// from the polynomial and checked against the published CRC-32 ones. x64's PCLMULQDQ and ARM64's PMULL run the same
// 128-bit fold through a few vector primitives each.

#include <crd/containers/checksum_kernels.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>

#include <bit>
#include <cstring>

// NOLINTBEGIN(cppcoreguidelines-macro-usage): select the unit's bodies
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE42 || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2 ||                         \
    CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
#include <immintrin.h>
#define CRD_CRC_HARDWARE_C 1  // the SSE4.2 CRC32 instruction computes CRC-32C
#define CRD_CRC_HARDWARE_32 0 // and no x64 instruction computes CRC-32
#elif CRD_KERNEL_HAS_CRC32
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <arm_acle.h>
#endif
#define CRD_CRC_HARDWARE_C 1 // ARM64's CRC32 instructions compute both polynomials
#define CRD_CRC_HARDWARE_32 1
#else
#define CRD_CRC_HARDWARE_C 0
#define CRD_CRC_HARDWARE_32 0
#endif
#if CRD_KERNEL_HAS_PMULL
#include <arm_neon.h>
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

namespace crd::containers::inline CRD_MATH_LEVEL_NS
{
namespace
{

static_assert(std::endian::native == std::endian::little, "the byte loops read little-endian words");

crd::u64 load_u64(const crd::u8* p) noexcept
{
    crd::u64 v = 0U;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

#if !CRD_CRC_HARDWARE_32
// Slicing by 8 (Kounavis and Berry, 2008): eight bytes per step, each through the table of its distance to the end.
crd::u32 crc_slices(const detail::CrcSlices& s, crd::u32 c, const crd::u8* p, crd::usize n) noexcept
{
    while (n >= 8U)
    {
        const crd::u64 v = load_u64(p) ^ c;
        c = s.table[7][v & 0xFFU] ^ s.table[6][(v >> 8U) & 0xFFU] ^ s.table[5][(v >> 16U) & 0xFFU] ^
            s.table[4][(v >> 24U) & 0xFFU] ^ s.table[3][(v >> 32U) & 0xFFU] ^ s.table[2][(v >> 40U) & 0xFFU] ^
            s.table[1][(v >> 48U) & 0xFFU] ^ s.table[0][v >> 56U];
        p += 8;
        n -= 8U;
    }
    for (; n > 0U; --n)
    {
        c = s.table[0][(c ^ *p) & 0xFFU] ^ (c >> 8U);
        ++p;
    }
    return c;
}
#endif

// The CRC register over n bytes a word at a time: the unit's CRC instruction where it has one, else slicing by 8.
crd::u32 crc32_bytes(crd::u32 state, const crd::u8* data, crd::usize n) noexcept
{
#if CRD_CRC_HARDWARE_32
    for (; n >= 8U; n -= 8U)
    {
        state = __crc32d(state, load_u64(data));
        data += 8;
    }
    for (; n > 0U; --n)
    {
        state = __crc32b(state, *data);
        ++data;
    }
    return state;
#else
    return crc_slices(detail::kCrc32Slices, state, data, n);
#endif
}

crd::u32 crc32c_bytes(crd::u32 state, const crd::u8* data, crd::usize n) noexcept
{
#if CRD_CRC_HARDWARE_32
    for (; n >= 8U; n -= 8U)
    {
        state = __crc32cd(state, load_u64(data));
        data += 8;
    }
    for (; n > 0U; --n)
    {
        state = __crc32cb(state, *data);
        ++data;
    }
    return state;
#elif CRD_CRC_HARDWARE_C
    crd::u64 c = state;
    for (; n >= 8U; n -= 8U)
    {
        c = _mm_crc32_u64(c, load_u64(data));
        data += 8;
    }
    auto c32 = static_cast<crd::u32>(c);
    for (; n > 0U; --n)
    {
        c32 = _mm_crc32_u8(c32, *data);
        ++data;
    }
    return c32;
#else
    return crc_slices(detail::kCrc32cSlices, state, data, n);
#endif
}

#if CRD_KERNEL_HAS_PCLMULQDQ || CRD_KERNEL_HAS_PMULL

constexpr crd::u32 kPoly32 = 0x04C11DB7U;  // CRC-32, unreflected, without x^32
constexpr crd::u32 kPoly32c = 0x1EDC6F41U; // CRC-32C

constexpr crd::u64 reflect(crd::u64 v, crd::u32 bits) noexcept
{
    crd::u64 r = 0U;
    for (crd::u32 i = 0U; i < bits; ++i)
    {
        r = (r << 1U) | ((v >> i) & 1U);
    }
    return r;
}

// x^n mod P(x), P = x^32 + poly.
constexpr crd::u32 xpow_mod(crd::u32 n, crd::u32 poly) noexcept
{
    crd::u64 r = 1U;
    for (crd::u32 i = 0U; i < n; ++i)
    {
        r <<= 1U;
        if ((r >> 32U) != 0U)
        {
            r ^= (crd::u64{1} << 32U) | poly;
        }
    }
    return static_cast<crd::u32>(r);
}

// The fold multiplier of x^n: reflected and shifted left once (33 bits).
constexpr crd::u64 fold_k(crd::u32 n, crd::u32 poly) noexcept
{
    return reflect(xpow_mod(n, poly), 32U) << 1U;
}

// floor(x^64 / P(x)) reflected over its 33 bits: the Barrett constant.
constexpr crd::u64 barrett_mu(crd::u32 poly) noexcept
{
    crd::u64 window = crd::u64{1} << 32U; // the dividend's coefficients x^64 .. x^32
    crd::u64 q = 0U;
    for (crd::u32 i = 33U; i-- > 0U;)
    {
        if ((window >> 32U) != 0U)
        {
            q |= crd::u64{1} << i;
            window ^= (crd::u64{1} << 32U) | poly;
        }
        window <<= 1U;
    }
    return reflect(q, 33U);
}

// A fold pair: lo64 multiplies by x^(D+32), hi64 by x^(D-32), for a distance of D bits.
struct FoldPair
{
    crd::u64 lo = 0U;
    crd::u64 hi = 0U;
};

constexpr FoldPair fold_pair(crd::u32 distance, crd::u32 poly) noexcept
{
    return {fold_k(distance + 32U, poly), fold_k(distance - 32U, poly)};
}

struct FoldConstants
{
    FoldPair d128;  // one 128-bit lane ahead
    FoldPair d256;  // one 256-bit register ahead
    FoldPair d512;  // four lanes, or one 512-bit register, ahead
    FoldPair d1024; // four 256-bit registers ahead
    FoldPair d2048; // four 512-bit registers ahead
    crd::u64 x64 = 0U;
    crd::u64 poly = 0U; // P reflected over 33 bits
    crd::u64 mu = 0U;
};

constexpr FoldConstants fold_constants(crd::u32 poly) noexcept
{
    return {fold_pair(128U, poly),
            fold_pair(256U, poly),
            fold_pair(512U, poly),
            fold_pair(1024U, poly),
            fold_pair(2048U, poly),
            fold_k(64U, poly),
            reflect((crd::u64{1} << 32U) | poly, 33U),
            barrett_mu(poly)};
}

constexpr FoldConstants kFold32 = fold_constants(kPoly32);
constexpr FoldConstants kFold32c = fold_constants(kPoly32c);

// The published CRC-32 constants (the Intel paper; Linux crc32-pclmul_asm.S): the derivation above reproduces them.
static_assert(kFold32.d512.lo == 0x154442BD4U && kFold32.d512.hi == 0x1C6E41596U);
static_assert(kFold32.d128.lo == 0x1751997D0U && kFold32.d128.hi == 0x0CCAA009EU);
static_assert(kFold32.x64 == 0x163CD6124U && kFold32.poly == 0x1DB710641U && kFold32.mu == 0x1F7011641U);

// The fold's 128-bit primitives: two 64-bit lanes, lo the low one.
#if CRD_KERNEL_HAS_PCLMULQDQ
using V128 = __m128i;

V128 make128(crd::u64 lo, crd::u64 hi) noexcept
{
    return _mm_set_epi64x(static_cast<long long>(hi), static_cast<long long>(lo));
}

V128 load128(const crd::u8* p) noexcept
{
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
}

V128 xor128(V128 a, V128 b) noexcept
{
    return _mm_xor_si128(a, b);
}

V128 and128(V128 a, V128 b) noexcept
{
    return _mm_and_si128(a, b);
}

V128 clmul_lo(V128 a, V128 b) noexcept // a.lo * b.lo
{
    return _mm_clmulepi64_si128(a, b, 0x00);
}

V128 clmul_hi(V128 a, V128 b) noexcept // a.hi * b.hi
{
    return _mm_clmulepi64_si128(a, b, 0x11);
}

template <int Bytes> V128 shift_right_bytes(V128 x) noexcept
{
    return _mm_srli_si128(x, Bytes);
}

crd::u32 bits_32_to_63(V128 x) noexcept
{
    return static_cast<crd::u32>(_mm_extract_epi32(x, 1));
}
#else
using V128 = uint64x2_t;

V128 make128(crd::u64 lo, crd::u64 hi) noexcept
{
    return vcombine_u64(vcreate_u64(lo), vcreate_u64(hi));
}

V128 load128(const crd::u8* p) noexcept
{
    return vreinterpretq_u64_u8(vld1q_u8(p));
}

V128 xor128(V128 a, V128 b) noexcept
{
    return veorq_u64(a, b);
}

V128 and128(V128 a, V128 b) noexcept
{
    return vandq_u64(a, b);
}

V128 clmul_lo(V128 a, V128 b) noexcept // a.lo * b.lo
{
#if defined(_MSC_VER) && !defined(__clang__)
    return vmull_p64(vget_low_u64(a), vget_low_u64(b)); // cl's PMULL takes and gives vector registers
#else
    return vreinterpretq_u64_p128(
        vmull_p64(static_cast<poly64_t>(vgetq_lane_u64(a, 0)), static_cast<poly64_t>(vgetq_lane_u64(b, 0))));
#endif
}

V128 clmul_hi(V128 a, V128 b) noexcept // a.hi * b.hi
{
    return vreinterpretq_u64_p128(vmull_high_p64(vreinterpretq_p64_u64(a), vreinterpretq_p64_u64(b)));
}

template <int Bytes> V128 shift_right_bytes(V128 x) noexcept
{
    return vreinterpretq_u64_u8(vextq_u8(vreinterpretq_u8_u64(x), vdupq_n_u8(0U), Bytes));
}

crd::u32 bits_32_to_63(V128 x) noexcept
{
    return vgetq_lane_u32(vreinterpretq_u32_u64(x), 1);
}
#endif

V128 pair128(const FoldPair& k) noexcept
{
    return make128(k.lo, k.hi);
}

// x folded `k`'s distance onto data.
V128 fold128(V128 x, V128 data, V128 k) noexcept
{
    return xor128(xor128(clmul_lo(x, k), clmul_hi(x, k)), data);
}

// The CRC register of a 128-bit remainder.
crd::u32 reduce128(V128 x, const FoldConstants& f) noexcept
{
    const V128 low32 = make128(0xFFFFFFFFU, 0U);
    // 128 bits to 64: the low qword one lane ahead (x^(128-32)), the high qword in place.
    x = xor128(shift_right_bytes<8>(x), clmul_lo(x, make128(f.d128.hi, 0U)));
    // 64 bits to 32 above a 32-bit remainder: the low dword times x^64.
    x = xor128(shift_right_bytes<4>(x), clmul_lo(and128(x, low32), make128(f.x64, 0U)));
    // Barrett: the quotient's low 32 bits times mu, times P, cancel all but the remainder.
    const V128 q = clmul_lo(and128(x, low32), make128(f.mu, 0U));
    return bits_32_to_63(xor128(x, clmul_lo(and128(q, low32), make128(f.poly, 0U))));
}

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2
__m256i pair256(const FoldPair& k) noexcept
{
    return _mm256_set_epi64x(static_cast<long long>(k.hi), static_cast<long long>(k.lo), static_cast<long long>(k.hi),
                             static_cast<long long>(k.lo));
}

__m256i load256(const crd::u8* p) noexcept
{
    return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
}

__m256i fold256(__m256i x, __m256i data, __m256i k) noexcept
{
    return _mm256_xor_si256(
        _mm256_xor_si256(_mm256_clmulepi64_epi128(x, k, 0x00), _mm256_clmulepi64_epi128(x, k, 0x11)), data);
}

// n >= 128: four 256-bit registers fold 128 bytes a step, then one; leaves n < 32 and the 128-bit remainder.
__m128i fold_wide(crd::u32 state, const crd::u8*& p, crd::usize& n, const FoldConstants& f) noexcept
{
    __m256i y0 = _mm256_xor_si256(load256(p), _mm256_set_epi64x(0, 0, 0, static_cast<long long>(state)));
    __m256i y1 = load256(p + 32);
    __m256i y2 = load256(p + 64);
    __m256i y3 = load256(p + 96);
    p += 128;
    n -= 128U;
    const __m256i k4 = pair256(f.d1024);
    for (; n >= 128U; n -= 128U)
    {
        y0 = fold256(y0, load256(p), k4);
        y1 = fold256(y1, load256(p + 32), k4);
        y2 = fold256(y2, load256(p + 64), k4);
        y3 = fold256(y3, load256(p + 96), k4);
        p += 128;
    }
    const __m256i k1 = pair256(f.d256);
    y0 = fold256(fold256(fold256(y0, y1, k1), y2, k1), y3, k1);
    for (; n >= 32U; n -= 32U)
    {
        y0 = fold256(y0, load256(p), k1);
        p += 32;
    }
    return fold128(_mm256_castsi256_si128(y0), _mm256_extracti128_si256(y0, 1), pair128(f.d128));
}
constexpr crd::usize kWideMin = 128U;
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
__m512i pair512(const FoldPair& k) noexcept
{
    return _mm512_broadcast_i32x4(pair128(k));
}

__m512i load512(const crd::u8* p) noexcept
{
    return _mm512_loadu_si512(p);
}

__m512i fold512(__m512i x, __m512i data, __m512i k) noexcept
{
    return _mm512_ternarylogic_epi64(_mm512_clmulepi64_epi128(x, k, 0x00), _mm512_clmulepi64_epi128(x, k, 0x11), data,
                                     0x96);
}

// n >= 256: four 512-bit registers fold 256 bytes a step, then one; leaves n < 64 and the 128-bit remainder.
__m128i fold_wide(crd::u32 state, const crd::u8*& p, crd::usize& n, const FoldConstants& f) noexcept
{
    __m512i z0 = _mm512_xor_si512(load512(p), _mm512_set_epi64(0, 0, 0, 0, 0, 0, 0, static_cast<long long>(state)));
    __m512i z1 = load512(p + 64);
    __m512i z2 = load512(p + 128);
    __m512i z3 = load512(p + 192);
    p += 256;
    n -= 256U;
    const __m512i k4 = pair512(f.d2048);
    for (; n >= 256U; n -= 256U)
    {
        z0 = fold512(z0, load512(p), k4);
        z1 = fold512(z1, load512(p + 64), k4);
        z2 = fold512(z2, load512(p + 128), k4);
        z3 = fold512(z3, load512(p + 192), k4);
        p += 256;
    }
    const __m512i k1 = pair512(f.d512);
    z0 = fold512(fold512(fold512(z0, z1, k1), z2, k1), z3, k1);
    for (; n >= 64U; n -= 64U)
    {
        z0 = fold512(z0, load512(p), k1);
        p += 64;
    }
    const __m128i k = pair128(f.d128);
    __m128i x = fold128(_mm512_castsi512_si128(z0), _mm512_extracti32x4_epi32(z0, 1), k);
    x = fold128(x, _mm512_extracti32x4_epi32(z0, 2), k);
    return fold128(x, _mm512_extracti32x4_epi32(z0, 3), k);
}
constexpr crd::usize kWideMin = 256U;
#endif

// n >= 64: four 128-bit lanes fold 64 bytes a step; leaves n < 64 and the 128-bit remainder.
V128 fold_lanes(crd::u32 state, const crd::u8*& p, crd::usize& n, const FoldConstants& f) noexcept
{
    V128 x0 = xor128(load128(p), make128(state, 0U));
    V128 x1 = load128(p + 16);
    V128 x2 = load128(p + 32);
    V128 x3 = load128(p + 48);
    p += 64;
    n -= 64U;
    const V128 k4 = pair128(f.d512);
    for (; n >= 64U; n -= 64U)
    {
        x0 = fold128(x0, load128(p), k4);
        x1 = fold128(x1, load128(p + 16), k4);
        x2 = fold128(x2, load128(p + 32), k4);
        x3 = fold128(x3, load128(p + 48), k4);
        p += 64;
    }
    const V128 k1 = pair128(f.d128);
    return fold128(fold128(fold128(x0, x1, k1), x2, k1), x3, k1);
}

// The folded CRC register of n bytes; `Small` takes the bytes no 128-bit lane holds.
template <crd::u32 (*Small)(crd::u32, const crd::u8*, crd::usize) noexcept>
crd::u32 crc_fold(crd::u32 state, const crd::u8* p, crd::usize n, const FoldConstants& f) noexcept
{
    if (n < 64U)
    {
        return Small(state, p, n);
    }
#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2 || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
    V128 x = n >= kWideMin ? fold_wide(state, p, n, f) : fold_lanes(state, p, n, f);
#else
    V128 x = fold_lanes(state, p, n, f);
#endif
    const V128 k1 = pair128(f.d128);
    for (; n >= 16U; n -= 16U)
    {
        x = fold128(x, load128(p), k1);
        p += 16;
    }
    return Small(reduce128(x, f), p, n);
}

crd::u32 crc32_fold(crd::u32 state, const crd::u8* data, crd::usize n) noexcept
{
    return crc_fold<&crc32_bytes>(state, data, n, kFold32);
}

crd::u32 crc32c_fold(crd::u32 state, const crd::u8* data, crd::usize n) noexcept
{
    return crc_fold<&crc32c_bytes>(state, data, n, kFold32c);
}

#endif // CRD_KERNEL_HAS_PCLMULQDQ || CRD_KERNEL_HAS_PMULL

} // namespace
} // namespace crd::containers::inline CRD_MATH_LEVEL_NS

namespace crd::containers
{

#if CRD_KERNEL_HAS_PCLMULQDQ || CRD_KERNEL_HAS_PMULL
const CrcKernels CRD_KERNEL_TABLE(kCrcKernels) = {&CRD_MATH_LEVEL_NS::crc32_fold, &CRD_MATH_LEVEL_NS::crc32c_fold};
#else
const CrcKernels CRD_KERNEL_TABLE(kCrcKernels) = {&CRD_MATH_LEVEL_NS::crc32_bytes, &CRD_MATH_LEVEL_NS::crc32c_bytes};
#endif

} // namespace crd::containers
