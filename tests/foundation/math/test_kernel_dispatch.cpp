// SIMD-DISPATCH (ADR-0134 §5, §7, §8): which kernel level a process selects, which levels the build compiles, the
// batch small-float conversions of every level the host executes against the scalar converts, bit for bit, and the
// scalar transcendental table of every such level against portable's. The [kernels] cases also run once per compiled
// level with CRD_CPU_LEVEL forcing the selection (CMakeLists.txt).

#include "kernel_levels.hpp"
#include "nan_aside.hpp"
#include "transcendental_refs.inc" // crd::math::txref::ref_<fn>_x

#include <crd/core/cpu_features.hpp>
#include <crd/math/float_convert.hpp>
#include <crd/math/kernel_dispatch.hpp>
#include <crd/math/scalar_dispatch.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using crd::CpuLevel;
using crd::KernelLevel;
using crd::math::FloatConvertKernels;
using crd::math::ScalarMathKernels;

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

// The guard words after a short call's output: a routine writes nothing past its n elements.
constexpr crd::usize kGuard = 17U;

// Mismatches of one routine against its scalar reference, over the whole input and over every length 0..33 from an
// odd offset (each vector tail of up to two 16-lane steps and an unaligned start), with guard words past the output.
template <typename Bits, typename Run, typename Reference>
crd::usize mismatches(const crd::f32* src, Run run, Reference reference)
{
    static Bits out[kCount];
    constexpr Bits guard = static_cast<Bits>(0xA5A5U);
    crd::usize bad = 0;
    run(src, out, kCount);
    for (crd::usize i = 0; i < kCount; ++i)
    {
        bad += out[i] == reference(i) ? 0U : 1U;
    }
    for (crd::usize n = 0; n < 34U; ++n)
    {
        for (crd::usize i = 0; i < n + kGuard; ++i)
        {
            out[i] = guard;
        }
        run(src + 3, out, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            bad += out[i] == reference(i + 3U) ? 0U : 1U;
        }
        for (crd::usize i = n; i < n + kGuard; ++i)
        {
            bad += out[i] == guard ? 0U : 1U;
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
        return crd::select_kernel_level<int>(levels, selected, crd::CpuFeatureSet{}, cap).level;
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

TEST_CASE("kernel dispatch: the determinism mode is Default until code sets it, and reads back as set", "[math]")
{
    using crd::math::DeterminismMode;
    CHECK(crd::math::kernel_determinism() == DeterminismMode::Default);
    for (const DeterminismMode mode :
         {DeterminismMode::CrossPlatform, DeterminismMode::BackwardCompat, DeterminismMode::Default})
    {
        crd::math::set_kernel_determinism(mode);
        CHECK(crd::math::kernel_determinism() == mode);
    }
}

TEST_CASE("kernel dispatch: a feature variant runs only where its level runs and the selection holds its features",
          "[math][kernels]")
{
    using crd::CpuFeature;
    using crd::CpuFeatureSet;
    const int tables[6] = {};
    const CpuFeatureSet vnni = CpuFeatureSet{}.with(CpuFeature::AvxVnni);
    const CpuFeatureSet vnni512 = CpuFeatureSet{}.with(CpuFeature::Avx512Vnni);
    const KernelLevel<int> levels[] = {
        {CpuLevel::Portable, &tables[0], {}}, {CpuLevel::Avx2, &tables[1], {}},
        {CpuLevel::Avx2, &tables[2], vnni},   {CpuLevel::Avx2, &tables[3], vnni.with(CpuFeature::Gfni)},
        {CpuLevel::Avx512, &tables[4], {}},   {CpuLevel::Avx512, &tables[5], vnni512}};
    const auto pick = [&](CpuLevel selected, CpuFeatureSet features, CpuLevel cap)
    {
        return crd::select_kernel_level<int>(levels, selected, features, cap).table - tables;
    };
    const CpuFeatureSet none;
    const CpuFeatureSet both = vnni.with(CpuFeature::Gfni);
    CHECK(pick(CpuLevel::Avx2, none, CpuLevel::Avx512) == 1);
    CHECK(pick(CpuLevel::Avx2, vnni, CpuLevel::Avx512) == 2);
    CHECK(pick(CpuLevel::Avx2, vnni.with(CpuFeature::Sha), CpuLevel::Avx512) == 2); // other features change nothing
    CHECK(pick(CpuLevel::Avx2, CpuFeatureSet{}.with(CpuFeature::Gfni), CpuLevel::Avx512) == 1); // needs both
    CHECK(pick(CpuLevel::Avx2, both, CpuLevel::Avx512) == 3);
    CHECK(pick(CpuLevel::Avx512, both, CpuLevel::Avx512) == 4); // a higher level outranks a lower one's variant
    CHECK(pick(CpuLevel::Avx512, both.with(CpuFeature::Avx512Vnni), CpuLevel::Avx512) == 5);
    CHECK(pick(CpuLevel::Avx512, vnni512, CpuLevel::Avx512) == 5);
    CHECK(pick(CpuLevel::Avx2, vnni512, CpuLevel::Avx512) == 1); // the avx512 variant needs its level
    CHECK(pick(CpuLevel::Avx512, vnni512, CpuLevel::Avx2) == 1); // and the build's cap
    CHECK(pick(CpuLevel::Avx512, both, CpuLevel::Avx2) == 3);    // the build's cap
    CHECK(pick(CpuLevel::Sse42, both, CpuLevel::Avx512) == 0);   // the features never lift the level
    CHECK(pick(CpuLevel::Neon, both, CpuLevel::Avx512) == 0);
}

TEST_CASE("float conversion kernels: the archive holds portable and each body level the build compiles",
          "[math][kernels]")
{
    const auto levels = crd::math::float_convert_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool sse2 = false;
    bool sse42 = false;
    bool avx2 = false;
    bool avx512 = false;
    for (crd::usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        sse2 = sse2 || levels[i].level == CpuLevel::Sse2;
        sse42 = sse42 || levels[i].level == CpuLevel::Sse42;
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == CpuLevel::Avx512;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(sse42 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse42)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx512)));
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
        if (!crd::test::kernel_level_runs_here(level))
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
        // Short calls from an odd offset: every tail, nothing written past n.
        crd::usize short_bad = 0;
        for (crd::usize n = 0; n < 34U; ++n)
        {
            const crd::u32 guard = 0x7FA5A5A5U;
            for (crd::usize i = 0; i < n + kGuard; ++i)
            {
                wide[i] = std::bit_cast<crd::f32>(guard);
            }
            k.f16_to_f32(halves + 0x7BF5U, wide, n); // the top normals, the infinity and the first NaNs
            for (crd::usize i = 0; i < n; ++i)
            {
                const crd::u32 want = std::bit_cast<crd::u32>(crd::math::f16_bits_to_f32(halves[0x7BF5U + i]));
                short_bad += std::bit_cast<crd::u32>(wide[i]) == want ? 0U : 1U;
            }
            for (crd::usize i = n; i < n + kGuard; ++i)
            {
                short_bad += std::bit_cast<crd::u32>(wide[i]) == guard ? 0U : 1U;
            }
        }
        CHECK(short_bad == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

TEST_CASE("float conversion kernels: the process runs the level its CPU selection resolves", "[math][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = crd::math::float_convert_levels();
    const KernelLevel<FloatConvertKernels>& chosen = crd::math::float_convert_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::select_kernel_level<FloatConvertKernels>(levels, s.selected, s.kernel_features,
                                                                    crd::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::kernel_level_cap(), chosen.level));
}

// The scalar transcendental table (crd/math/scalar_kernels.hpp): every level the host runs returns portable's bits,
// NaN payloads aside, for every routine over the mpmath grids, the IEEE specials and random doubles. Its accuracy is
// gated through the public functions (test_transcendental.cpp), which general code of an x64 build reaches through the
// selected table.
namespace
{

// Signed zeros, the subnormal and normal edges, +-1, the largest finites, infinities, NaNs (quiet, signalling, with a
// payload), the log seams at sqrt(2) and sqrt(1/2), exp's overflow and underflow edges and pi.
constexpr crd::u64 kDoubleSpecials[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x0000000000000001ULL, 0x800FFFFFFFFFFFFFULL, 0x0010000000000000ULL,
    0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x7FEFFFFFFFFFFFFFULL, 0xFFEFFFFFFFFFFFFFULL, 0x7FF0000000000000ULL,
    0xFFF0000000000000ULL, 0x7FF8000000000000ULL, 0xFFF8000000000001ULL, 0x7FF0000000000001ULL, 0x3FE0000000000000ULL,
    0x4086232BDD7ABCD2ULL, 0xC0874910D52D3051ULL, 0x3FF6A09E667F3BCDULL, 0x3FE6A09E667F3BCCULL, 0x400921FB54442D18ULL,
};

// Every grid of transcendental_refs.inc, the specials and random bit patterns over the whole double range.
const double* scalar_inputs(crd::usize& n)
{
    namespace r = crd::math::txref;
    static double xs[40000];
    static crd::usize count = 0;
    if (count == 0U)
    {
        const auto add = [&](const double* g, int m)
        {
            for (int i = 0; i < m; ++i)
            {
                xs[count++] = g[i];
            }
        };
        add(r::ref_exp_x, r::ref_exp_n);
        add(r::ref_exp2_x, r::ref_exp2_n);
        add(r::ref_exp10_x, r::ref_exp10_n);
        add(r::ref_expm1_x, r::ref_expm1_n);
        add(r::ref_log_x, r::ref_log_n);
        add(r::ref_log1p_x, r::ref_log1p_n);
        add(r::ref_sin_x, r::ref_sin_n);
        add(r::ref_sin_large_x, r::ref_sin_large_n);
        add(r::ref_sinh_x, r::ref_sinh_n);
        add(r::ref_asinh_x, r::ref_asinh_n);
        add(r::ref_acosh_x, r::ref_acosh_n);
        add(r::ref_atanh_x, r::ref_atanh_n);
        add(r::ref_atan_x, r::ref_atan_n);
        add(r::ref_asin_x, r::ref_asin_n);
        add(r::ref_pow_x, r::ref_pow_n);
        add(r::ref_pow_p, r::ref_pow_n);
        for (const crd::u64 bits : kDoubleSpecials)
        {
            xs[count++] = std::bit_cast<double>(bits);
        }
        crd::u64 state = 0x5CA1A2D15EA5EULL;
        while (count < sizeof(xs) / sizeof(xs[0]))
        {
            const crd::u64 hi = next_pattern(state);
            xs[count++] = std::bit_cast<double>((hi << 32U) | next_pattern(state));
        }
    }
    n = count;
    return xs;
}

// Disagreements of one level's routine with portable's over the inputs (one argument) or input pairs (two).
template <typename Level, typename Portable> crd::usize scalar_mismatches(Level level, Portable portable)
{
    crd::usize n = 0;
    const double* xs = scalar_inputs(n);
    crd::usize bad = 0;
    for (crd::usize i = 0; i < n; ++i)
    {
        if constexpr (std::is_invocable_v<Level, double, double>)
        {
            const double y = xs[(i * 7919U + 13U) % n];
            bad += crd::test::same_bits_nan_aside(level(xs[i], y), portable(xs[i], y)) ? 0U : 1U;
        }
        else
        {
            bad += crd::test::same_bits_nan_aside(level(xs[i]), portable(xs[i])) ? 0U : 1U;
        }
    }
    return bad;
}

} // namespace

TEST_CASE("scalar transcendental kernels: the archive holds portable and avx2 where the build compiles it",
          "[math][kernels]")
{
    const auto levels = crd::math::scalar_math_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    const bool avx2 = x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2);
    REQUIRE(levels.size() == (avx2 ? 2U : 1U));
    CHECK(levels[0].table == &crd::math::kScalarMathKernelsPortable);
    CHECK(levels.back().level == (avx2 ? CpuLevel::Avx2 : CpuLevel::Portable));
    CHECK(levels.back().table != nullptr);
}

TEST_CASE("scalar transcendental kernels: every level the host runs gives portable's bits", "[math][kernels]")
{
    const ScalarMathKernels& p = *crd::math::scalar_math_levels()[0].table;
    crd::usize ran = 0;
    for (const KernelLevel<ScalarMathKernels>& level : crd::math::scalar_math_levels())
    {
        if (!crd::test::kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        const ScalarMathKernels& k = *level.table;
        INFO("level " << crd::cpu_level_name(level.level));
        CHECK(scalar_mismatches(k.exp, p.exp) == 0U);
        CHECK(scalar_mismatches(k.exp2, p.exp2) == 0U);
        CHECK(scalar_mismatches(k.exp10, p.exp10) == 0U);
        CHECK(scalar_mismatches(k.expm1, p.expm1) == 0U);
        CHECK(scalar_mismatches(k.log, p.log) == 0U);
        CHECK(scalar_mismatches(k.log2, p.log2) == 0U);
        CHECK(scalar_mismatches(k.log10, p.log10) == 0U);
        CHECK(scalar_mismatches(k.log1p, p.log1p) == 0U);
        CHECK(scalar_mismatches(k.sin, p.sin) == 0U);
        CHECK(scalar_mismatches(k.cos, p.cos) == 0U);
        CHECK(scalar_mismatches(k.tan, p.tan) == 0U);
        CHECK(scalar_mismatches(k.atan, p.atan) == 0U);
        CHECK(scalar_mismatches(k.atan2, p.atan2) == 0U);
        CHECK(scalar_mismatches(k.asin, p.asin) == 0U);
        CHECK(scalar_mismatches(k.acos, p.acos) == 0U);
        CHECK(scalar_mismatches(k.pow, p.pow) == 0U);
        CHECK(scalar_mismatches(k.sinh, p.sinh) == 0U);
        CHECK(scalar_mismatches(k.cosh, p.cosh) == 0U);
        CHECK(scalar_mismatches(k.tanh, p.tanh) == 0U);
        CHECK(scalar_mismatches(k.asinh, p.asinh) == 0U);
        CHECK(scalar_mismatches(k.acosh, p.acosh) == 0U);
        CHECK(scalar_mismatches(k.atanh, p.atanh) == 0U);
        const auto sin_of = [](const ScalarMathKernels& t)
        {
            return [&t](double x)
            {
                double s = 0.0;
                double c = 0.0;
                t.sincos(x, s, c);
                return s;
            };
        };
        const auto cos_of = [](const ScalarMathKernels& t)
        {
            return [&t](double x)
            {
                double s = 0.0;
                double c = 0.0;
                t.sincos(x, s, c);
                return c;
            };
        };
        CHECK(scalar_mismatches(sin_of(k), sin_of(p)) == 0U);
        CHECK(scalar_mismatches(cos_of(k), cos_of(p)) == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

TEST_CASE("scalar transcendental kernels: the process runs the level its CPU selection resolves", "[math][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = crd::math::scalar_math_levels();
    const KernelLevel<ScalarMathKernels>& chosen = crd::math::scalar_math_level();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen ==
          &crd::select_kernel_level<ScalarMathKernels>(levels, s.selected, s.kernel_features, crd::kernel_level_cap()));
    CHECK(&crd::math::scalar_math() == chosen.table);
}
