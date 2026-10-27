// SIMD-DISPATCH (ADR-0134 §5, §7): the crd::math::simd types follow the CPU level of the unit that compiles them, keep
// one data layout on every level, and give the same bits on every level. kernels/simd_level_probe.cpp is compiled once
// per level the build compiles; each level the host executes runs every probe operation over the same inputs as the
// portable level, in this one process.

#include "simd_level_probe.hpp"

#include <crd/core/cpu_features.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/backend.hpp>

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>

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

namespace
{

// ---- the fused multiply-add's hard cases ----
//
// The sets above draw a, b and c independently, so a wrong fused multiply-add (one that rounds twice, or loses the
// product's low half) rarely shows there. These lanes tie the operands: c cancelling the rounded product, products
// that end on or beside a rounding midpoint of c, one addend far below the other, subnormal and overflowing products
// and results, the bounds of the f64 emulated region (crd/math/simd/fma_exact.hpp) on both sides, infinities, NaNs
// and signed zeros. Every level, portable included, must give the C library's fma, bit for bit apart from NaN
// payloads.

crd::f32 make_f32(crd::u64 r, int exponent)
{
    const crd::u32 sign = static_cast<crd::u32>(r >> 63U) << 31U;
    const crd::u32 mantissa = static_cast<crd::u32>(r >> 20U) & 0x7FFFFFU;
    const int biased = std::clamp(exponent + 127, 0, 254);
    return std::bit_cast<crd::f32>(sign | (static_cast<crd::u32>(biased) << 23U) | mantissa);
}

crd::f64 make_f64(crd::u64 r, int exponent)
{
    const crd::u64 sign = (r >> 63U) << 63U;
    const crd::u64 mantissa = (r >> 7U) & 0xFFFFFFFFFFFFFULL;
    const int biased = std::clamp(exponent + 1023, 0, 2046);
    return std::bit_cast<crd::f64>(sign | (static_cast<crd::u64>(biased) << 52U) | mantissa);
}

// x moved by `steps` units in the last place along its bit pattern; values next to zero or past the largest finite one
// stay put.
template <typename F, typename U> F nudge(F x, int steps)
{
    const U bits = std::bit_cast<U>(x);
    const U magnitude = bits & static_cast<U>(~U{0} >> 1U);
    if (magnitude < 4U || magnitude > static_cast<U>(std::bit_cast<U>(std::numeric_limits<F>::max()) - 4U))
    {
        return x;
    }
    return std::bit_cast<F>(static_cast<U>(bits + static_cast<U>(steps)));
}

int pick(crd::u64 r, int low, int high)
{
    return low + static_cast<int>(r % static_cast<crd::u64>(high - low + 1));
}

// One lane of related operands of the format with `Precision` significand bits.
template <typename F, typename U, int Precision> void hard_lane(crd::u64& state, F& a, F& b, F& c)
{
    constexpr int emax = Precision == 24 ? 127 : 1023;
    constexpr int emin = Precision == 24 ? -126 : -1022;
    const auto make = [](crd::u64 r, int e)
    {
        if constexpr (Precision == 24)
        {
            return make_f32(r, e);
        }
        else
        {
            return make_f64(r, e);
        }
    };
    const crd::u64 kind = next(state) % 11U;
    const crd::u64 r0 = next(state);
    const crd::u64 r1 = next(state);
    const crd::u64 r2 = next(state);
    const int ea = pick(r0 >> 8U, -Precision, Precision);
    const int eb = pick(r1 >> 8U, -Precision, Precision);
    switch (kind)
    {
        case 0: // c cancels the rounded product to within two units
        {
            a = make(r0, ea);
            b = make(r1, eb);
            c = nudge<F, U>(static_cast<F>(-(a * b)), pick(r2, -2, 2));
            break;
        }
        case 1: // (1 + 2^-j)(1 +- 2^-j), so the product ends on or beside a midpoint of c
        {
            const int j = pick(r2 >> 8U, 1, Precision - 1);
            const F step = static_cast<F>(std::ldexp(1.0, -j));
            const F one{1};
            a = std::ldexp(one + step, ea) * ((r0 & 1U) != 0U ? -one : one);
            b = std::ldexp((r1 & 1U) != 0U ? one + step : one - step, eb);
            c = make(r2, ea + eb + pick(r2 >> 32U, Precision - 2, Precision + 2));
            break;
        }
        case 2: // c within a few binades of the product
        {
            a = make(r0, ea);
            b = make(r1, eb);
            c = make(r2, ea + eb + pick(r2 >> 32U, -3, 3));
            break;
        }
        case 3: // one addend far below the other
        {
            a = make(r0, ea);
            b = make(r1, eb);
            c = make(r2, ea + eb + ((r2 & 1U) != 0U ? 1 : -1) * pick(r2 >> 32U, Precision + 1, 3 * Precision));
            break;
        }
        case 4: // subnormal products and results
        {
            a = make(r0, (emin / 2) + pick(r0 >> 40U, -Precision, 2));
            b = make(r1, (emin / 2) + pick(r1 >> 40U, -Precision, 2));
            c = (r2 & 1U) != 0U ? nudge<F, U>(static_cast<F>(-(a * b)), pick(r2, -2, 2))
                                : make(r2, emin + pick(r2, -Precision, 4));
            break;
        }
        case 5: // products and sums at the top of the range, overflowing ones included
        {
            a = make(r0, (emax / 2) + pick(r0 >> 40U, -2, 2));
            b = make(r1, (emax / 2) + pick(r1 >> 40U, -2, 2));
            c = make(r2, emax - pick(r2 >> 40U, 0, 4));
            break;
        }
        case 6: // the f64 region's bounds: |a| at 2^995, |ab| at 2^-900 and 2^1020, |c| at 2^1020
        {
            const int edges[] = {995, -900, 1020, 1020};
            const int edge = pick(r2 >> 40U, 0, 3);
            const int e = edges[edge];
            a = make(r0, edge == 0 ? e + pick(r0 >> 40U, -1, 0) : (e / 2) + pick(r0 >> 40U, -1, 1));
            b = make(r1, edge == 0 ? pick(r1 >> 40U, -3, 3) : e - (e / 2) + pick(r1 >> 40U, -1, 1));
            c = edge == 3 ? make(r2, 1020 + pick(r2, -1, 0)) : make(r2, e + pick(r2, -60, 2));
            break;
        }
        case 7: // signed zeros: zero products, zero addends and exact cancellation
        {
            const F zeros[] = {F{0}, -F{0}};
            a = (r0 & 1U) != 0U ? zeros[(r0 >> 1U) & 1U] : make(r0, ea);
            b = (r0 & 4U) != 0U ? zeros[(r0 >> 3U) & 1U] : make(r1, eb);
            c = (r0 & 16U) != 0U ? zeros[(r0 >> 5U) & 1U] : static_cast<F>(-(a * b));
            break;
        }
        case 8: // infinities and NaNs in each slot
        {
            const F special[] = {std::numeric_limits<F>::infinity(), -std::numeric_limits<F>::infinity(),
                                 std::numeric_limits<F>::quiet_NaN(), F{0}, std::numeric_limits<F>::max()};
            a = (r0 & 3U) == 0U ? special[(r0 >> 2U) % 5U] : make(r0, ea);
            b = (r1 & 3U) == 0U ? special[(r1 >> 2U) % 5U] : make(r1, eb);
            c = (r2 & 1U) == 0U ? special[(r2 >> 2U) % 5U] : make(r2, ea + eb);
            break;
        }
        default: // random bit patterns
        {
            a = std::bit_cast<F>(static_cast<U>(r0));
            b = std::bit_cast<F>(static_cast<U>(r1));
            c = std::bit_cast<F>(static_cast<U>(r2));
            break;
        }
    }
}

void fill_fma(probe::Input& in, crd::u64& state)
{
    for (crd::usize i = 0; i < probe::kLanes; ++i)
    {
        hard_lane<crd::f32, crd::u32, 24>(state, in.a[i], in.b[i], in.c[i]);
        hard_lane<crd::f64, crd::u64, 53>(state, in.da[i], in.db[i], in.dc[i]);
    }
}

enum class Fused : crd::u8
{
    Fma,
    Fnmadd,
    Fmaddsub, // subtracts c on the even lanes
};

template <typename F> F fused_reference(Fused op, F a, F b, F c, crd::usize lane)
{
    switch (op)
    {
        case Fused::Fnmadd:
            return std::fma(-a, b, c);
        case Fused::Fmaddsub:
            return std::fma(a, b, (lane & 1U) != 0U ? c : -c);
        case Fused::Fma:
            break;
    }
    return std::fma(a, b, c);
}

struct FusedOp
{
    const char* name;
    Fused op;
    bool f64;
};

constexpr FusedOp kFusedOps[] = {
    {"vec4f fma", Fused::Fma, false},          {"vec8f fma", Fused::Fma, false},
    {"vec8f fnmadd", Fused::Fnmadd, false},    {"vec8f fmaddsub", Fused::Fmaddsub, false},
    {"vec4d fma", Fused::Fma, true},           {"vec4d fnmadd", Fused::Fnmadd, true},
    {"vec4d fmaddsub", Fused::Fmaddsub, true},
};
constexpr crd::usize kFusedCount = sizeof(kFusedOps) / sizeof(kFusedOps[0]);

const probe::Op* find_op(const probe::Table& table, const char* name)
{
    for (crd::usize op = 0; op < table.op_count; ++op)
    {
        if (std::strcmp(table.ops[op].name, name) == 0)
        {
            return &table.ops[op];
        }
    }
    return nullptr;
}

struct Tally
{
    crd::usize mismatches = 0;
    crd::usize words = 0;
    crd::u64 first[4] = {}; // a, b, c, the C library's result
    crd::u64 first_got = 0;
};

} // namespace

TEST_CASE("simd types: every level's fused multiply-add rounds once on hard cases", "[math][simd-levels]")
{
    constexpr crd::usize set_count = 16384; // 2^20 triples of each type
    static probe::Input in;
    static Tally tally[kLevelCount][kFusedCount];
    const probe::Op* ops[kLevelCount][kFusedCount] = {};
    bool runs[kLevelCount] = {};
    for (crd::usize l = 0; l < kLevelCount; ++l)
    {
        runs[l] = crd::cpu_level_includes(crd::cpu_selection().detected, kLevels[l].level);
        for (crd::usize f = 0; f < kFusedCount; ++f)
        {
            ops[l][f] = find_op(*kLevels[l].table, kFusedOps[f].name);
            REQUIRE(ops[l][f] != nullptr);
        }
    }
    crd::u64 state = 0xF0A5'ED00'0D0DULL;
    for (crd::usize set = 0; set < set_count; ++set)
    {
        fill_fma(in, state);
        for (crd::usize f = 0; f < kFusedCount; ++f)
        {
            crd::u64 expected[probe::kLanes];
            crd::u64 operands[probe::kLanes][3];
            for (crd::usize i = 0; i < probe::kLanes; ++i)
            {
                if (kFusedOps[f].f64)
                {
                    expected[i] =
                        std::bit_cast<crd::u64>(fused_reference(kFusedOps[f].op, in.da[i], in.db[i], in.dc[i], i));
                    operands[i][0] = std::bit_cast<crd::u64>(in.da[i]);
                    operands[i][1] = std::bit_cast<crd::u64>(in.db[i]);
                    operands[i][2] = std::bit_cast<crd::u64>(in.dc[i]);
                }
                else
                {
                    expected[i] =
                        std::bit_cast<crd::u32>(fused_reference(kFusedOps[f].op, in.a[i], in.b[i], in.c[i], i));
                    operands[i][0] = std::bit_cast<crd::u32>(in.a[i]);
                    operands[i][1] = std::bit_cast<crd::u32>(in.b[i]);
                    operands[i][2] = std::bit_cast<crd::u32>(in.c[i]);
                }
            }
            for (crd::usize l = 0; l < kLevelCount; ++l)
            {
                if (!runs[l])
                {
                    continue;
                }
                crd::u64 got[probe::kMaxWords];
                REQUIRE(ops[l][f]->run(in, got) == probe::kLanes);
                Tally& t = tally[l][f];
                t.words += probe::kLanes;
                for (crd::usize i = 0; i < probe::kLanes; ++i)
                {
                    if (!same(ops[l][f]->compare, expected[i], got[i]))
                    {
                        if (t.mismatches == 0U)
                        {
                            t.first[0] = operands[i][0];
                            t.first[1] = operands[i][1];
                            t.first[2] = operands[i][2];
                            t.first[3] = expected[i];
                            t.first_got = got[i];
                        }
                        ++t.mismatches;
                    }
                }
            }
        }
    }
    for (crd::usize l = 0; l < kLevelCount; ++l)
    {
        for (crd::usize f = 0; runs[l] && f < kFusedCount; ++f)
        {
            const Tally& t = tally[l][f];
            INFO("level " << crd::cpu_level_name(kLevels[l].level) << ", " << kFusedOps[f].name << ", " << t.words
                          << " words");
            INFO("first difference: a 0x" << std::hex << t.first[0] << " b 0x" << t.first[1] << " c 0x" << t.first[2]
                                          << ", C library 0x" << t.first[3] << ", level 0x" << t.first_got << std::dec);
            CHECK(t.mismatches == 0U);
        }
    }
}
