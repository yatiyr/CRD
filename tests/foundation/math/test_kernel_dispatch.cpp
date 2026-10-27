// SIMD-DISPATCH (ADR-0134 §5, §7, §8): which kernel level a process selects, which levels the build compiles, and the
// batch small-float conversions of every level the host executes against the scalar converts, bit for bit. The
// [kernels] cases also run once per compiled level with CRD_CPU_LEVEL forcing the selection (CMakeLists.txt).

#include <crd/core/cpu_features.hpp>
#include <crd/math/float_convert.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

using crd::CpuLevel;
using crd::math::FloatConvertKernels;
using crd::math::KernelLevel;

namespace
{

// The input patterns: every class of every format (zeros, subnormals, both rounding boundaries, overflow, the deep
// underflow that stochastic rounding stages through 64 bits, infinities, NaN payloads), then random bit patterns.
constexpr crd::u32 kSpecials[] = {
    0x00000000U, 0x80000000U, 0x00000001U, 0x807FFFFFU, 0x00800000U, 0x3F800000U, 0xBF800000U, 0x3F801000U, 0x3F802000U,
    0x3F803000U, 0x477FE000U, 0x477FF000U, 0x47800000U, 0xC77FF000U, 0x33800000U, 0x33000000U, 0x33000001U, 0x2F800000U,
    0x1F800000U, 0x0F800000U, 0x43E00000U, 0x43F00000U, 0x43E80000U, 0x47600000U, 0x47700000U, 0x3B000000U, 0x3A800000U,
    0x37800000U, 0x7F7FFFFFU, 0xFF7FFFFFU, 0x7F800000U, 0xFF800000U, 0x7FC00000U, 0xFFC00000U, 0x7F800001U, 0x7FA5A5A5U,
    0xFFBFFFFFU, 0x7FFFFFFFU, 0x3F807FFFU, 0x3F808000U, 0x3F818000U, 0x3F7FFFFFU,
};
constexpr crd::usize kRandomCount = 1U << 16U;
constexpr crd::usize kCount = sizeof(kSpecials) / sizeof(kSpecials[0]) + kRandomCount;

// SplitMix64: a fixed, seedable stream of 32-bit patterns.
crd::u32 next_pattern(crd::u64& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    crd::u64 z = state;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return static_cast<crd::u32>((z ^ (z >> 31U)) >> 16U);
}

struct Inputs
{
    crd::f32 values[kCount];
    crd::u32 rand[kCount];
};

const Inputs& inputs()
{
    static const Inputs kInputs = []()
    {
        Inputs in{};
        crd::u64 state = 0xC0FFEE5EEDULL;
        for (crd::usize i = 0; i < kCount; ++i)
        {
            const crd::u32 bits = i < sizeof(kSpecials) / sizeof(kSpecials[0]) ? kSpecials[i] : next_pattern(state);
            in.values[i] = std::bit_cast<crd::f32>(bits);
            in.rand[i] = next_pattern(state);
        }
        return in;
    }();
    return kInputs;
}

// Mismatches of one routine against its scalar reference, over the whole input and over every length 0..19 from an
// odd offset (each vector tail and an unaligned start).
template <typename Bits, typename Run, typename Reference>
crd::usize mismatches(const crd::f32* src, Run run, Reference reference)
{
    static Bits out[kCount];
    crd::usize bad = 0;
    run(src, out, kCount);
    for (crd::usize i = 0; i < kCount; ++i)
    {
        bad += out[i] == reference(i) ? 0U : 1U;
    }
    for (crd::usize n = 0; n < 20U; ++n)
    {
        run(src + 3, out, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            bad += out[i] == reference(i + 3U) ? 0U : 1U;
        }
    }
    return bad;
}

} // namespace

TEST_CASE("kernel dispatch: the selected level is the highest one both the selection and the build cap run",
          "[math][kernels]")
{
    const int tables[4] = {};
    const KernelLevel<int> levels[] = {{CpuLevel::Portable, &tables[0], {}},
                                       {CpuLevel::Sse2, &tables[1], {}},
                                       {CpuLevel::Avx2, &tables[2], {}},
                                       {CpuLevel::Avx512, &tables[3], {}}};
    const auto pick = [&](CpuLevel selected, CpuLevel cap)
    {
        return crd::math::select_kernel_level<int>(levels, selected, crd::CpuFeatureSet{}, cap).level;
    };
    CHECK(pick(CpuLevel::Avx512, CpuLevel::Avx512) == CpuLevel::Avx512);
    CHECK(pick(CpuLevel::Avx2, CpuLevel::Avx512) == CpuLevel::Avx2);
    CHECK(pick(CpuLevel::Sse42, CpuLevel::Avx512) == CpuLevel::Sse2); // between two bodies: the lower one
    CHECK(pick(CpuLevel::Sse2, CpuLevel::Avx512) == CpuLevel::Sse2);
    CHECK(pick(CpuLevel::Portable, CpuLevel::Avx512) == CpuLevel::Portable);
    CHECK(pick(CpuLevel::Avx512, CpuLevel::Avx2) == CpuLevel::Avx2); // the build's cap
    CHECK(pick(CpuLevel::Avx512, CpuLevel::Sse2) == CpuLevel::Sse2);
    CHECK(pick(CpuLevel::Avx512, CpuLevel::Portable) == CpuLevel::Portable);
    CHECK(pick(CpuLevel::Neon, CpuLevel::Avx512) == CpuLevel::Portable); // another architecture runs none of them
}

TEST_CASE("kernel dispatch: a feature variant runs only where its level runs and the selection holds its features",
          "[math][kernels]")
{
    using crd::CpuFeature;
    using crd::CpuFeatureSet;
    const int tables[5] = {};
    const CpuFeatureSet vnni = CpuFeatureSet{}.with(CpuFeature::AvxVnni);
    const KernelLevel<int> levels[] = {{CpuLevel::Portable, &tables[0], {}},
                                       {CpuLevel::Avx2, &tables[1], {}},
                                       {CpuLevel::Avx2, &tables[2], vnni},
                                       {CpuLevel::Avx2, &tables[3], vnni.with(CpuFeature::Gfni)},
                                       {CpuLevel::Avx512, &tables[4], {}}};
    const auto pick = [&](CpuLevel selected, CpuFeatureSet features, CpuLevel cap)
    {
        return crd::math::select_kernel_level<int>(levels, selected, features, cap).table - tables;
    };
    const CpuFeatureSet none;
    const CpuFeatureSet both = vnni.with(CpuFeature::Gfni);
    CHECK(pick(CpuLevel::Avx2, none, CpuLevel::Avx512) == 1);
    CHECK(pick(CpuLevel::Avx2, vnni, CpuLevel::Avx512) == 2);
    CHECK(pick(CpuLevel::Avx2, vnni.with(CpuFeature::Sha), CpuLevel::Avx512) == 2); // other features change nothing
    CHECK(pick(CpuLevel::Avx2, CpuFeatureSet{}.with(CpuFeature::Gfni), CpuLevel::Avx512) == 1); // needs both
    CHECK(pick(CpuLevel::Avx2, both, CpuLevel::Avx512) == 3);
    CHECK(pick(CpuLevel::Avx512, both, CpuLevel::Avx512) == 4); // a higher level outranks a lower one's variant
    CHECK(pick(CpuLevel::Avx512, both, CpuLevel::Avx2) == 3);   // the build's cap
    CHECK(pick(CpuLevel::Sse42, both, CpuLevel::Avx512) == 0);  // the features never lift the level
    CHECK(pick(CpuLevel::Neon, both, CpuLevel::Avx512) == 0);
}

TEST_CASE("float conversion kernels: the archive holds portable and each body level the build compiles",
          "[math][kernels]")
{
    const auto levels = crd::math::float_convert_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool avx2 = false;
    for (crd::usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
}

TEST_CASE("float conversion kernels: every level the host runs is bit-identical to the scalar converts",
          "[math][kernels]")
{
    const Inputs& in = inputs();
    const crd::f32* src = in.values;
    const crd::u32* rand = in.rand;
    crd::usize ran = 0;
    for (const KernelLevel<FloatConvertKernels>& level : crd::math::float_convert_levels())
    {
        if (!crd::cpu_level_includes(crd::cpu_selection().detected, level.level))
        {
            continue;
        }
        ++ran;
        const FloatConvertKernels& k = *level.table;
        INFO("level " << crd::cpu_level_name(level.level));
        const auto f16 = [&](crd::usize i)
        {
            return crd::math::f32_to_f16_bits(src[i]);
        };
        const auto bf16 = [&](crd::usize i)
        {
            return crd::math::f32_to_bf16_bits(src[i]);
        };
        const auto e4m3 = [&](crd::usize i)
        {
            return crd::math::f32_to_fp8_e4m3_bits(src[i]);
        };
        const auto e5m2 = [&](crd::usize i)
        {
            return crd::math::f32_to_fp8_e5m2_bits(src[i]);
        };
        const auto f16_sr = [&](crd::usize i)
        {
            return crd::math::f32_to_f16_bits_sr(src[i], rand[i]);
        };
        const auto bf16_sr = [&](crd::usize i)
        {
            return crd::math::f32_to_bf16_bits_sr(src[i], rand[i]);
        };
        const auto e4m3_sr = [&](crd::usize i)
        {
            return crd::math::f32_to_fp8_e4m3_bits_sr(src[i], rand[i]);
        };
        const auto e5m2_sr = [&](crd::usize i)
        {
            return crd::math::f32_to_fp8_e5m2_bits_sr(src[i], rand[i]);
        };
        CHECK(mismatches<crd::u16>(src, k.f32_to_f16, f16) == 0U);
        CHECK(mismatches<crd::u16>(src, k.f32_to_bf16, bf16) == 0U);
        CHECK(mismatches<crd::u8>(src, k.f32_to_fp8_e4m3, e4m3) == 0U);
        CHECK(mismatches<crd::u8>(src, k.f32_to_fp8_e5m2, e5m2) == 0U);
        // The SR routines see rand at the same offset as src, so element k always meets draw k.
        const auto sr = [&](auto routine)
        {
            return [&, routine](const crd::f32* s, auto* d, crd::usize n)
            {
                routine(s, d, rand + (s - src), n);
            };
        };
        CHECK(mismatches<crd::u16>(src, sr(k.f32_to_f16_sr), f16_sr) == 0U);
        CHECK(mismatches<crd::u16>(src, sr(k.f32_to_bf16_sr), bf16_sr) == 0U);
        CHECK(mismatches<crd::u8>(src, sr(k.f32_to_fp8_e4m3_sr), e4m3_sr) == 0U);
        CHECK(mismatches<crd::u8>(src, sr(k.f32_to_fp8_e5m2_sr), e5m2_sr) == 0U);

        // Widening: every one of the 65,536 f16 patterns, compared as f32 bits.
        static crd::u16 halves[1U << 16U];
        static crd::f32 wide[1U << 16U];
        for (crd::usize i = 0; i < (1U << 16U); ++i)
        {
            halves[i] = static_cast<crd::u16>(i);
        }
        k.f16_to_f32(halves, wide, 1U << 16U);
        crd::usize bad = 0;
        for (crd::usize i = 0; i < (1U << 16U); ++i)
        {
            const crd::u32 want = std::bit_cast<crd::u32>(crd::math::f16_bits_to_f32(halves[i]));
            bad += std::bit_cast<crd::u32>(wide[i]) == want ? 0U : 1U;
        }
        CHECK(bad == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

TEST_CASE("float conversion kernels: the process runs the level its CPU selection resolves", "[math][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = crd::math::float_convert_levels();
    const KernelLevel<FloatConvertKernels>& chosen = crd::math::float_convert_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<FloatConvertKernels>(levels, s.selected, s.kernel_features,
                                                                          crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}
