// SIMD-DISPATCH (ADR-0134 §5, §7): the crd::math::simd types follow the CPU level of the unit that compiles them, keep
// one data layout on every level, and give the same bits on every level. kernels/simd_level_probe.cpp is compiled once
// per level the build compiles; each level the host executes runs every probe operation over the same inputs as the
// portable level, in this one process.

#include "simd_level_probe.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/backend.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace probe = crd::test::simd_probe;
using crd::CpuLevel;

namespace
{

struct Level
{
    CpuLevel level;
    const probe::Table* table;
    const char* backend; // the backend the level's types must compile for
};

// CRD_KERNELS_SIMD_PROBE: the levels crd_add_cpu_kernels built (CMakeLists.txt).
constexpr Level kLevels[] = {
    {CpuLevel::Portable, &probe::kSimdProbePortable, "SCALAR"},
#if CRD_KERNEL_BUILT(CRD_KERNELS_SIMD_PROBE, SSE2)
    {CpuLevel::Sse2, &probe::kSimdProbeSse2, "SSE2"},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SIMD_PROBE, SSE42)
    {CpuLevel::Sse42, &probe::kSimdProbeSse42, "SSE4.2"},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SIMD_PROBE, AVX2)
    {CpuLevel::Avx2, &probe::kSimdProbeAvx2, "AVX2"},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SIMD_PROBE, AVX512)
    {CpuLevel::Avx512, &probe::kSimdProbeAvx512, "AVX2"}, // no type uses 512-bit registers yet
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SIMD_PROBE, NEON)
    {CpuLevel::Neon, &probe::kSimdProbeNeon, "NEON"},
#endif
};

constexpr crd::usize kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);

// The classes every operation must agree on: signed zeros, the round-to-integral boundaries (halves, 2^23, 2^31),
// the i32 conversion range, subnormals, the extremes, infinities, quiet and signalling NaNs with payloads.
constexpr crd::u32 kF32Specials[] = {
    0x00000000U, 0x80000000U, 0x3F800000U, 0xBF800000U, 0x3F000000U, 0xBF000000U, 0x3FC00000U, 0x40200000U,
    0xC0200000U, 0x406CCCCDU, 0xC06CCCCDU, 0x3EFFFFFFU, 0xBEFFFFFFU, 0x4AFFFFFFU, 0x4B000000U, 0xCB000000U,
    0x4B000001U, 0xCAFFFFFFU, 0x4EFFFFFFU, 0x4F000000U, 0xCF000000U, 0xCF000001U, 0x4F32D05EU, 0xCF32D05EU,
    0x7149F2CAU, 0x7F7FFFFFU, 0xFF7FFFFFU, 0x00800000U, 0x80800000U, 0x00000001U, 0x807FFFFFU, 0x7F800000U,
    0xFF800000U, 0x7FC00000U, 0xFFC00000U, 0x7F800001U, 0x7FA5A5A5U, 0xFFBFFFFFU, 0x3EAAAAABU, 0x40490FDBU,
    0x42B16000U, 0xC2AEA000U, 0x3E800000U, 0xBE800000U, 0x40400000U, 0x40A00000U, 0xC0E00000U, 0x3DCCCCCDU,
};
constexpr crd::u64 kF64Specials[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x3FE0000000000000ULL,
    0x4004000000000000ULL, 0xC004000000000000ULL, 0x4330000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 0xFFEFFFFFFFFFFFFFULL,
    0x0010000000000000ULL, 0x0000000000000001ULL, 0x800FFFFFFFFFFFFFULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
    0x7FF8000000000000ULL, 0xFFF8000000000000ULL, 0x7FF0000000000001ULL, 0x7FF5A5A5A5A5A5A5ULL, 0x4086232BDD7ABCD2ULL,
    0x4086200000000000ULL, 0xC0874910D52D3051ULL, 0xC086240000000000ULL, 0x3FD5555555555555ULL, 0x400921FB54442D18ULL,
    0x3FB999999999999AULL, 0x4059000000000000ULL, 0xC08F400000000000ULL,
};
constexpr crd::i32 kI32Specials[] = {
    0, 1, -1, 2, 0x7FFFFFFF, static_cast<crd::i32>(0x80000000U), 0x00FFFFFF, 0x01000001, -0x01000001, 12345, -98765,
};
constexpr crd::usize kInputSets = 64;

crd::u64 next(crd::u64& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    crd::u64 z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

// Set 0 starts with the specials; every set mixes them with random bit patterns and random ordinary values.
void fill(probe::Input& in, crd::usize set, crd::u64& state)
{
    for (crd::usize i = 0; i < probe::kLanes; ++i)
    {
        const crd::u64 r = next(state);
        const crd::usize pick = (set * probe::kLanes) + i;
        crd::u32 f[3];
        for (crd::u32& bits : f)
        {
            const crd::u64 k = next(state);
            if ((k & 3U) == 0U || pick < sizeof(kF32Specials) / sizeof(kF32Specials[0]))
            {
                bits = kF32Specials[(pick + (k >> 8U)) % (sizeof(kF32Specials) / sizeof(kF32Specials[0]))];
            }
            else if ((k & 3U) == 1U)
            {
                bits = static_cast<crd::u32>(k >> 32U);
            }
            else
            {
                const crd::f32 numerator = static_cast<crd::f32>(static_cast<crd::i32>(k >> 40U) - 0x800000);
                bits = std::bit_cast<crd::u32>(numerator / static_cast<crd::f32>((k >> 20U) & 0xFFFU));
            }
        }
        in.a[i] = std::bit_cast<crd::f32>(f[0]);
        in.b[i] = std::bit_cast<crd::f32>(f[1]);
        in.c[i] = std::bit_cast<crd::f32>(f[2]);
        crd::u64 d[3];
        for (crd::u64& bits : d)
        {
            const crd::u64 k = next(state);
            constexpr crd::usize count = sizeof(kF64Specials) / sizeof(kF64Specials[0]);
            if ((k & 3U) == 0U)
            {
                bits = kF64Specials[(k >> 8U) % count];
            }
            else if ((k & 3U) == 1U)
            {
                bits = next(state);
            }
            else
            {
                const crd::f64 numerator = static_cast<crd::f64>(static_cast<crd::i64>(k >> 11U) - (1LL << 52));
                bits = std::bit_cast<crd::u64>(numerator / static_cast<crd::f64>((k >> 2U) & 0xFFFFFU));
            }
        }
        in.da[i] = std::bit_cast<crd::f64>(d[0]);
        in.db[i] = std::bit_cast<crd::f64>(d[1]);
        in.dc[i] = std::bit_cast<crd::f64>(d[2]);
        constexpr crd::usize ints = sizeof(kI32Specials) / sizeof(kI32Specials[0]);
        in.ia[i] = (r & 7U) == 0U ? kI32Specials[(r >> 8U) % ints] : static_cast<crd::i32>(r >> 32U);
        in.ib[i] = (r & 7U) == 1U ? kI32Specials[(r >> 16U) % ints] : static_cast<crd::i32>(r);
    }
}

bool same(probe::Compare compare, crd::u64 x, crd::u64 y)
{
    if (x == y)
    {
        return true;
    }
    if (compare == probe::Compare::F32Value)
    {
        return (x & 0x7FFFFFFFU) > 0x7F800000U && (y & 0x7FFFFFFFU) > 0x7F800000U;
    }
    if (compare == probe::Compare::F64Value)
    {
        constexpr crd::u64 inf = 0x7FF0000000000000ULL;
        return (x & ~(1ULL << 63U)) > inf && (y & ~(1ULL << 63U)) > inf;
    }
    return false;
}

} // namespace

TEST_CASE("simd types: each level's unit compiles its own backend and the one data layout", "[math][simd-levels]")
{
    REQUIRE(kLevels[0].level == CpuLevel::Portable);
    const probe::Table& portable = *kLevels[0].table;
    for (const Level& level : kLevels)
    {
        INFO("level " << crd::cpu_level_name(level.level));
        CHECK(std::strcmp(level.table->backend, level.backend) == 0);
        CHECK(level.table->lane_width == crd::math::simd::kNativeLaneWidth);
        REQUIRE(level.table->op_count == portable.op_count);
        for (crd::usize op = 0; op < portable.op_count; ++op)
        {
            CHECK(std::strcmp(level.table->ops[op].name, portable.ops[op].name) == 0);
        }
    }
    // LEVELS lists every level, so the probe is built up to the configuration's top level.
    CHECK(static_cast<int>(kLevels[kLevelCount - 1U].level) == CRD_TEST_KERNEL_LEVELS_TOP);
}

TEST_CASE("simd types: every level the host executes gives the portable level's bits", "[math][simd-levels]")
{
    const probe::Table& portable = *kLevels[0].table;
    static probe::Input inputs[kInputSets];
    crd::u64 state = 0x51D0'1E7E'15EDULL;
    for (crd::usize set = 0; set < kInputSets; ++set)
    {
        fill(inputs[set], set, state);
    }
    crd::usize ran = 0;
    for (const Level& level : kLevels)
    {
        if (!crd::cpu_level_includes(crd::cpu_selection().detected, level.level))
        {
            continue;
        }
        ++ran;
        for (crd::usize op = 0; op < portable.op_count; ++op)
        {
            const probe::Op& reference = portable.ops[op];
            crd::usize mismatches = 0;
            crd::usize words = 0;
            crd::usize first_set = 0;
            crd::usize first_word = 0;
            crd::u64 first_expected = 0;
            crd::u64 first_got = 0;
            for (crd::usize set = 0; set < kInputSets; ++set)
            {
                crd::u64 expected[probe::kMaxWords];
                crd::u64 got[probe::kMaxWords];
                const crd::usize n = reference.run(inputs[set], expected);
                REQUIRE(n <= probe::kMaxWords);
                REQUIRE(level.table->ops[op].run(inputs[set], got) == n);
                words += n;
                for (crd::usize i = 0; i < n; ++i)
                {
                    if (!same(reference.compare, expected[i], got[i]))
                    {
                        if (mismatches == 0U)
                        {
                            first_set = set;
                            first_word = i;
                            first_expected = expected[i];
                            first_got = got[i];
                        }
                        ++mismatches;
                    }
                }
            }
            INFO("level " << crd::cpu_level_name(level.level) << ", " << reference.name << ", " << words << " words");
            INFO("first difference: set " << first_set << " word " << first_word << ", portable 0x" << std::hex
                                          << first_expected << ", level 0x" << first_got << std::dec);
            CHECK(mismatches == 0U);
        }
    }
    // Every CPU of the architecture executes the level above portable (SSE2 on x64, NEON on ARM64).
    CHECK(ran >= (kLevelCount > 1U ? 2U : 1U));
}
