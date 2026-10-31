// The checksum family (crd/containers/checksum.hpp): the CRC slicing tables, the dispatch of both kernel families and
// XXH3's short inputs and final merge, which are scalar on every level.

#include <crd/containers/checksum.hpp>
#include <crd/containers/checksum_dispatch.hpp>
#include <crd/core/kernel_level.hpp>
#include <crd/core/wide_multiply.hpp>

#include <bit>
#include <cstring>

namespace crd::containers
{
namespace detail
{
namespace
{

constexpr CrcSlices make_slices(crd::u32 reflected_poly) noexcept
{
    CrcSlices s{};
    for (crd::u32 b = 0U; b < 256U; ++b)
    {
        crd::u32 c = b;
        for (int k = 0; k < 8; ++k)
        {
            c = (c >> 1U) ^ ((c & 1U) != 0U ? reflected_poly : 0U);
        }
        s.table[0][b] = c;
    }
    for (crd::u32 b = 0U; b < 256U; ++b)
    {
        for (int k = 1; k < 8; ++k)
        {
            const crd::u32 prev = s.table[k - 1][b];
            s.table[k][b] = (prev >> 8U) ^ s.table[0][prev & 0xFFU];
        }
    }
    return s;
}

} // namespace

constexpr CrcSlices kCrc32Slices = make_slices(0xEDB88320U);
constexpr CrcSlices kCrc32cSlices = make_slices(0x82F63B78U);
static_assert(kCrc32Slices.table[0][1] == 0x77073096U && kCrc32cSlices.table[0][1] == 0xF26B8303U);

} // namespace detail

namespace
{

using crd::KernelLevel;
using F = crd::CpuFeature;

// CRD_KERNELS_CRC and CRD_KERNELS_HASH: the levels crd_add_cpu_kernels built (../CMakeLists.txt), and
// CRD_KERNELS_CRC_<LEVEL>_<FEATURE> the crc variants. Each needs its entry below.
constexpr int kCrcLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE42);
static_assert((CRD_KERNELS_CRC & ~kCrcLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_CRC, PORTABLE), "the portable level is always built");
constexpr int kHashLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                    (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512) |
                                    (1 << CRD_KERNEL_LEVEL_NEON);
static_assert((CRD_KERNELS_HASH & ~kHashLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_HASH, PORTABLE), "the portable level is always built");

constexpr KernelLevel<CrcKernels> kCrcLevels[] = {
    {crd::CpuLevel::Portable, &kCrcKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_CRC, SSE42)
    {crd::CpuLevel::Sse42, &kCrcKernelsSse42, {}},
#endif
#if CRD_KERNELS_CRC_SSE42_PCLMULQDQ
    {crd::CpuLevel::Sse42, &kCrcKernelsSse42Pclmulqdq, crd::CpuFeatureSet{}.with(F::Pclmulqdq)},
#endif
#if CRD_KERNELS_CRC_AVX2_VPCLMULQDQ
    {crd::CpuLevel::Avx2, &kCrcKernelsAvx2Vpclmulqdq, crd::CpuFeatureSet{}.with(F::Pclmulqdq).with(F::Vpclmulqdq)},
#endif
#if CRD_KERNELS_CRC_AVX512_VPCLMULQDQ
    {crd::CpuLevel::Avx512, &kCrcKernelsAvx512Vpclmulqdq, crd::CpuFeatureSet{}.with(F::Pclmulqdq).with(F::Vpclmulqdq)},
#endif
#if CRD_KERNELS_CRC_NEON_CRC32
    {crd::CpuLevel::Neon, &kCrcKernelsNeonCrc32, crd::CpuFeatureSet{}.with(F::Crc32)},
#endif
#if CRD_KERNELS_CRC_NEON_PMULL
    {crd::CpuLevel::Neon, &kCrcKernelsNeonPmull, crd::CpuFeatureSet{}.with(F::Crc32).with(F::Pmull)},
#endif
};

constexpr KernelLevel<HashKernels> kHashLevels[] = {
    {crd::CpuLevel::Portable, &kHashKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_HASH, SSE2)
    {crd::CpuLevel::Sse2, &kHashKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_HASH, AVX2)
    {crd::CpuLevel::Avx2, &kHashKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_HASH, AVX512)
    {crd::CpuLevel::Avx512, &kHashKernelsAvx512, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_HASH, NEON)
    {crd::CpuLevel::Neon, &kHashKernelsNeon, {}},
#endif
};

// ---- XXH3-64 (the xxHash 0.8 specification) -------------------------------------------------------------------------

static_assert(std::endian::native == std::endian::little, "XXH3 reads little-endian words");

// The specification's XXH_PRIME32_n, XXH_PRIME64_n and XXH_PRIME_MXn.
constexpr crd::u64 kPrime32One = 0x9E3779B1U;
constexpr crd::u64 kPrime32Two = 0x85EBCA77U;
constexpr crd::u64 kPrime32Three = 0xC2B2AE3DU;
constexpr crd::u64 kPrime64One = 0x9E3779B185EBCA87U;
constexpr crd::u64 kPrime64Two = 0xC2B2AE3D27D4EB4FU;
constexpr crd::u64 kPrime64Three = 0x165667B19E3779F9U;
constexpr crd::u64 kPrime64Four = 0x85EBCA77C2B2AE63U;
constexpr crd::u64 kPrime64Five = 0x27D4EB2F165667C5U;
constexpr crd::u64 kPrimeMx1 = 0x165667919E3779F9U;
constexpr crd::u64 kPrimeMx2 = 0x9FB21C651E98DF25U;
constexpr crd::usize kSecretSize = 192U;

// The specification's default secret (kSecret), 16 bytes a row.
// clang-format off
// NOLINTNEXTLINE(modernize-avoid-c-arrays): the published bytes
alignas(64) constexpr crd::u8 kSecret[kSecretSize] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};
// clang-format on

crd::u64 read64(const crd::u8* p) noexcept
{
    crd::u64 v = 0U;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

crd::u32 read32(const crd::u8* p) noexcept
{
    crd::u32 v = 0U;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

void write64(crd::u8* p, crd::u64 v) noexcept
{
    std::memcpy(p, &v, sizeof(v));
}

constexpr crd::u32 swap32(crd::u32 v) noexcept
{
    return (v >> 24U) | ((v >> 8U) & 0xFF00U) | ((v << 8U) & 0xFF0000U) | (v << 24U);
}

constexpr crd::u64 swap64(crd::u64 v) noexcept
{
    return (crd::u64{swap32(static_cast<crd::u32>(v))} << 32U) | swap32(static_cast<crd::u32>(v >> 32U));
}

crd::u64 mul128_fold64(crd::u64 a, crd::u64 b) noexcept
{
    const crd::WideProduct p = crd::multiply_wide(a, b);
    return p.lo ^ p.hi;
}

crd::u64 xxh64_avalanche(crd::u64 h) noexcept
{
    h ^= h >> 33U;
    h *= kPrime64Two;
    h ^= h >> 29U;
    h *= kPrime64Three;
    return h ^ (h >> 32U);
}

crd::u64 xxh3_avalanche(crd::u64 h) noexcept
{
    h ^= h >> 37U;
    h *= kPrimeMx1;
    return h ^ (h >> 32U);
}

crd::u64 rrmxmx(crd::u64 h, crd::u64 len) noexcept
{
    h ^= std::rotl(h, 49) ^ std::rotl(h, 24);
    h *= kPrimeMx2;
    h ^= (h >> 35U) + len;
    h *= kPrimeMx2;
    return h ^ (h >> 28U);
}

crd::u64 mix16(const crd::u8* in, const crd::u8* secret, crd::u64 seed) noexcept
{
    return mul128_fold64(read64(in) ^ (read64(secret) + seed), read64(in + 8) ^ (read64(secret + 8) - seed));
}

crd::u64 xxh3_0to16(const crd::u8* in, crd::u64 len, crd::u64 seed) noexcept
{
    if (len > 8U)
    {
        const crd::u64 lo = read64(in) ^ ((read64(kSecret + 24) ^ read64(kSecret + 32)) + seed);
        const crd::u64 hi = read64(in + len - 8U) ^ ((read64(kSecret + 40) ^ read64(kSecret + 48)) - seed);
        return xxh3_avalanche(len + swap64(lo) + hi + mul128_fold64(lo, hi));
    }
    if (len >= 4U)
    {
        seed ^= crd::u64{swap32(static_cast<crd::u32>(seed))} << 32U;
        const crd::u64 words = read32(in + len - 4U) + (crd::u64{read32(in)} << 32U);
        return rrmxmx(words ^ ((read64(kSecret + 8) ^ read64(kSecret + 16)) - seed), len);
    }
    if (len > 0U)
    {
        const crd::u32 combined = (crd::u32{in[0]} << 16U) | (crd::u32{in[len >> 1U]} << 24U) | crd::u32{in[len - 1U]} |
                                  (static_cast<crd::u32>(len) << 8U);
        return xxh64_avalanche(combined ^ ((crd::u64{read32(kSecret)} ^ read32(kSecret + 4)) + seed));
    }
    return xxh64_avalanche(seed ^ read64(kSecret + 56) ^ read64(kSecret + 64));
}

crd::u64 xxh3_17to128(const crd::u8* in, crd::u64 len, crd::u64 seed) noexcept
{
    crd::u64 acc = len * kPrime64One;
    if (len > 32U)
    {
        if (len > 64U)
        {
            if (len > 96U)
            {
                acc += mix16(in + 48, kSecret + 96, seed) + mix16(in + len - 64U, kSecret + 112, seed);
            }
            acc += mix16(in + 32, kSecret + 64, seed) + mix16(in + len - 48U, kSecret + 80, seed);
        }
        acc += mix16(in + 16, kSecret + 32, seed) + mix16(in + len - 32U, kSecret + 48, seed);
    }
    acc += mix16(in, kSecret, seed) + mix16(in + len - 16U, kSecret + 16, seed);
    return xxh3_avalanche(acc);
}

crd::u64 xxh3_129to240(const crd::u8* in, crd::u64 len, crd::u64 seed) noexcept
{
    constexpr crd::usize mid_start = 3U; // XXH3_MIDSIZE_STARTOFFSET
    constexpr crd::usize mid_last = 17U; // XXH3_MIDSIZE_LASTOFFSET
    constexpr crd::usize secret_min = 136U;
    crd::u64 acc = len * kPrime64One;
    for (crd::usize i = 0U; i < 8U; ++i)
    {
        acc += mix16(in + 16U * i, kSecret + 16U * i, seed);
    }
    acc = xxh3_avalanche(acc);
    const crd::usize rounds = static_cast<crd::usize>(len) / 16U;
    for (crd::usize i = 8U; i < rounds; ++i)
    {
        acc += mix16(in + 16U * i, kSecret + 16U * (i - 8U) + mid_start, seed);
    }
    acc += mix16(in + len - 16U, kSecret + secret_min - mid_last, seed);
    return xxh3_avalanche(acc);
}

crd::u64 xxh3_long(const crd::u8* in, crd::usize len, crd::u64 seed) noexcept
{
    alignas(64) crd::u8 derived[kSecretSize]; // NOLINT(modernize-avoid-c-arrays): a seed's secret
    const crd::u8* secret = kSecret;
    if (seed != 0U)
    {
        for (crd::usize i = 0U; i < kSecretSize; i += 16U)
        {
            write64(derived + i, read64(kSecret + i) + seed);
            write64(derived + i + 8U, read64(kSecret + i + 8U) - seed);
        }
        secret = derived;
    }
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): the accumulators the kernel runs on
    alignas(64) crd::u64 acc[8] = {kPrime32Three, kPrime64One, kPrime64Two,  kPrime64Three,
                                   kPrime64Four,  kPrime32Two, kPrime64Five, kPrime32One};
    hash_kernels().table->xxh3_accumulate(acc, in, len, secret);
    constexpr crd::usize merge_start = 11U; // XXH_SECRET_MERGEACCS_START
    crd::u64 result = static_cast<crd::u64>(len) * kPrime64One;
    for (crd::usize i = 0U; i < 4U; ++i)
    {
        const crd::u8* s = secret + merge_start + 16U * i;
        result += mul128_fold64(acc[2U * i] ^ read64(s), acc[(2U * i) + 1U] ^ read64(s + 8));
    }
    return xxh3_avalanche(result);
}

} // namespace

std::span<const KernelLevel<CrcKernels>> crc_kernel_levels() noexcept
{
    return kCrcLevels;
}

const KernelLevel<CrcKernels>& crc_kernels() noexcept
{
    static const KernelLevel<CrcKernels>& selected = crd::process_kernel_level<CrcKernels>(kCrcLevels);
    return selected;
}

std::span<const KernelLevel<HashKernels>> hash_kernel_levels() noexcept
{
    return kHashLevels;
}

const KernelLevel<HashKernels>& hash_kernels() noexcept
{
    static const KernelLevel<HashKernels>& selected = crd::process_kernel_level<HashKernels>(kHashLevels);
    return selected;
}

crd::u32 crc32(ConstSpan<crd::u8> data, crd::u32 crc) noexcept
{
    return ~crc_kernels().table->crc32(~crc, data.data(), data.size());
}

crd::u32 crc32c(ConstSpan<crd::u8> data, crd::u32 crc) noexcept
{
    return ~crc_kernels().table->crc32c(~crc, data.data(), data.size());
}

crd::u64 xxh3_64(ConstSpan<crd::u8> data, crd::u64 seed) noexcept
{
    const crd::u8* in = data.data();
    const crd::usize len = data.size();
    if (len <= 16U)
    {
        return xxh3_0to16(in, len, seed);
    }
    if (len <= 128U)
    {
        return xxh3_17to128(in, len, seed);
    }
    if (len <= 240U)
    {
        return xxh3_129to240(in, len, seed);
    }
    return xxh3_long(in, len, seed);
}

} // namespace crd::containers
