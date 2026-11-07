// HESAP-NET's quantisation: bounded floats against closed forms, smallest-three quaternions against their definition
// and their word layout, refusals of what no encoder writes, and every kernel level the host runs bit for bit against
// the scalars (crd-hesap-net-kernels-at-<level> runs the [kernels] cases under each forced level).
// crd-hesap-quantize-peer-tests holds the bits to mas-bandwidth/serialize and networked-physics.

#include "kernel_levels.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/net/quantize.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>

namespace hn = crd::hesap::net;
using crd::CpuLevel;
using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using hn::BoundedFloat;
using hn::QuantizeKernels;
using hn::SmallestThree;
template <typename T> using Array = crd::containers::Array<T>;

namespace
{

constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr f32 kNan = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kMax = std::numeric_limits<f32>::max();

[[nodiscard]] u32 bits_of(f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}

[[nodiscard]] BoundedFloat format_of(f32 min, f32 max, f32 resolution)
{
    BoundedFloat format{};
    REQUIRE(hn::bounded_float(min, max, resolution, format));
    return format;
}

[[nodiscard]] SmallestThree format_of(u32 bits)
{
    SmallestThree format{};
    REQUIRE(hn::smallest_three(bits, format));
    return format;
}

// A float of any exponent or sign, biased to the values the formats meet.
[[nodiscard]] f32 random_value(SeededWords& rng) noexcept
{
    const u32 word = rng.next_u32();
    switch (word % 5U)
    {
        case 0U:
            return std::bit_cast<f32>(rng.next_u32());
        case 1U:
            return static_cast<f32>(rng.next_u32() % 4096U) / 64.0F - 32.0F;
        case 2U:
            return (word & 32U) != 0U ? kInf : -kInf;
        default:
            return (static_cast<f32>(rng.next_u32() >> 8U) / 16777216.0F) * 200.0F - 100.0F;
    }
}

// A unit quaternion: four gaussians' direction, by ratios of uniforms kept simple since uniformity does not matter.
void random_unit(SeededWords& rng, f32* q) noexcept
{
    double sum = 0.0;
    double v[4];
    for (double& c : v)
    {
        c = (static_cast<double>(rng.next_u32()) / 2147483648.0) - 1.0;
        sum += c * c;
    }
    const double scale = 1.0 / std::sqrt(sum > 1e-12 ? sum : 1.0);
    for (int i = 0; i < 4; ++i)
    {
        q[i] = static_cast<f32>(v[i] * scale);
    }
}

// The quaternions at the format's edges: the axes, ties of every multiplicity, and the sign of each.
constexpr f32 kS = 0.70710678F;
constexpr f32 kT = 0.57735027F;
constexpr f32 kEdges[][4] = {
    {1.0F, 0.0F, 0.0F, 0.0F},   {0.0F, 1.0F, 0.0F, 0.0F},   {0.0F, 0.0F, 1.0F, 0.0F},     {0.0F, 0.0F, 0.0F, 1.0F},
    {-1.0F, 0.0F, 0.0F, 0.0F},  {0.0F, -1.0F, 0.0F, 0.0F},  {0.0F, 0.0F, -1.0F, 0.0F},    {0.0F, 0.0F, 0.0F, -1.0F},
    {kS, kS, 0.0F, 0.0F},       {kS, -kS, 0.0F, 0.0F},      {-kS, kS, 0.0F, 0.0F},        {0.0F, kS, -kS, 0.0F},
    {0.0F, 0.0F, kS, kS},       {0.0F, 0.0F, -kS, -kS},     {kS, 0.0F, 0.0F, -kS},        {kT, kT, kT, 0.0F},
    {0.0F, kT, kT, kT},         {kT, 0.0F, -kT, kT},        {-kT, -kT, -kT, 0.0F},        {0.5F, 0.5F, 0.5F, 0.5F},
    {-0.5F, 0.5F, -0.5F, 0.5F}, {0.5F, -0.5F, 0.5F, -0.5F}, {-0.5F, -0.5F, -0.5F, -0.5F},
};

[[nodiscard]] u64 encode_one(const SmallestThree& format, const f32* q) noexcept
{
    u64 word = 0U;
    hn::kQuantizeKernelsPortable.smallest_three_encode(format, q, 1U, &word);
    return word;
}

[[nodiscard]] usize decode_one(const SmallestThree& format, u64 word, f32* q) noexcept
{
    return hn::kQuantizeKernelsPortable.smallest_three_decode(format, &word, 1U, q);
}

// The kernel's refusal restated from the word: a stray bit above it, or a remainder an encoder cannot reach.
[[nodiscard]] bool restated_refusal(const SmallestThree& format, u64 word) noexcept
{
    constexpr f32 bound = 1.0F / 1.414214F;
    constexpr f32 range = bound - (-1.0F / 1.414214F);
    if ((word >> hn::smallest_three_word_bits(format)) != 0U)
    {
        return true;
    }
    const u64 mask = (u64{1} << format.bits) - 1U;
    const auto component = [&](u32 shift) noexcept
    {
        const auto code = static_cast<f32>((word >> shift) & mask);
        const f32 scaled = (code * format.inverse_scale) * range;
        return scaled + (-bound);
    };
    const f32 a = component(2U * format.bits);
    const f32 b = component(format.bits);
    const f32 c = component(0U);
    const f32 a2 = a * a;
    const f32 b2 = b * b;
    const f32 c2 = c * c;
    return (((1.0F - a2) - b2) - c2) < format.smallest_remainder;
}

std::string_view routine_of(const char* routine)
{
    return routine != nullptr ? routine : "";
}

void bounded_encode_off(const BoundedFloat& format, const f32* values, usize count, u32* codes) noexcept
{
    hn::kQuantizeKernelsPortable.bounded_encode(format, values, count, codes);
    if (count != 0U)
    {
        codes[count - 1U] ^= 1U;
    }
}

usize bounded_decode_off(const BoundedFloat& format, const u32* codes, usize count, f32* values) noexcept
{
    const usize refused = hn::kQuantizeKernelsPortable.bounded_decode(format, codes, count, values);
    return refused == 0U ? 0U : refused - 1U;
}

void smallest_three_encode_off(const SmallestThree& format, const f32* xyzw, usize count, u64* words) noexcept
{
    hn::kQuantizeKernelsPortable.smallest_three_encode(format, xyzw, count, words);
    if (count != 0U)
    {
        words[0] ^= 1U;
    }
}

usize smallest_three_decode_off(const SmallestThree& format, const u64* words, usize count, f32* xyzw) noexcept
{
    const usize refused = hn::kQuantizeKernelsPortable.smallest_three_decode(format, words, count, xyzw);
    if (count != 0U)
    {
        xyzw[(4U * count) - 1U] = std::bit_cast<f32>(bits_of(xyzw[(4U * count) - 1U]) ^ 1U);
    }
    return refused;
}

} // namespace

TEST_CASE("bounded float: a format takes the fewest bits its resolution needs", "[quantize]")
{
    BoundedFloat format = format_of(0.0F, 1.0F, 1.0F / 16.0F);
    CHECK(format.max_code == 16U);
    CHECK(format.bits == 5U);
    CHECK(format.delta == 1.0F);
    format = format_of(-8.0F, 8.0F, 1.0F);
    CHECK(format.max_code == 16U);
    CHECK(format.bits == 5U);
    CHECK(format.min == -8.0F);
    // A range narrower than one step still has two codes, its bounds.
    format = format_of(0.0F, 0.25F, 1.0F);
    CHECK(format.max_code == 1U);
    CHECK(format.bits == 1U);
    // The largest format there is.
    format = format_of(0.0F, 16777215.0F, 1.0F);
    CHECK(format.max_code == hn::kBoundedMaxCode);
    CHECK(format.bits == 24U);
    // 15.5 steps round up.
    format = format_of(0.0F, 15.5F, 1.0F);
    CHECK(format.max_code == 16U);
    REQUIRE(hn::bounded_float_with_bits(-1.0F, 1.0F, 9U, format));
    CHECK(format.max_code == 511U);
    CHECK(format.bits == 9U);
    REQUIRE(hn::bounded_float_with_bits(-1.0F, 1.0F, 24U, format));
    CHECK(format.max_code == hn::kBoundedMaxCode);
    REQUIRE(hn::bounded_float_with_bits(-1.0F, 1.0F, 1U, format));
    CHECK(format.max_code == 1U);
    static_assert(hn::kBoundedMaxBits == 24U);
}

TEST_CASE("bounded float: a format no value could round trip through is refused and writes nothing", "[quantize]")
{
    const BoundedFloat sentinel{7.0F, 7.0F, 7U, 7U};
    BoundedFloat out = sentinel;
    const auto untouched = [&]
    {
        return out.min == 7.0F && out.delta == 7.0F && out.max_code == 7U && out.bits == 7U;
    };
    const f32 bad_bounds[][2] = {{1.0F, 1.0F},
                                 {2.0F, 1.0F},
                                 {kNan, 1.0F},
                                 {0.0F, kNan},
                                 {0.0F, kInf},
                                 {-kInf, 0.0F},
                                 {-kInf, kInf},
                                 {-kMax, kMax},
                                 {kNan, kNan},
                                 // The width is finite but the top code would round to infinity.
                                 {std::ldexp(3.0F, 103), kMax}};
    for (const auto& bounds : bad_bounds)
    {
        INFO("min " << bounds[0] << " max " << bounds[1]);
        CHECK_FALSE(hn::bounded_float(bounds[0], bounds[1], 1.0F, out));
        CHECK_FALSE(hn::bounded_float_with_bits(bounds[0], bounds[1], 8U, out));
        CHECK(untouched());
    }
    for (const f32 resolution : {0.0F, -0.0F, -1.0F, kNan})
    {
        CHECK_FALSE(hn::bounded_float(0.0F, 1.0F, resolution, out));
        CHECK(untouched());
    }
    // More steps than a code holds: 2^24 steps, a tiny resolution, and an infinite quotient.
    CHECK_FALSE(hn::bounded_float(0.0F, 16777216.0F, 1.0F, out));
    CHECK_FALSE(hn::bounded_float(0.0F, 1.0F, 1e-30F, out));
    CHECK_FALSE(hn::bounded_float(0.0F, kMax, 1e-30F, out));
    CHECK(untouched());
    for (const u32 bits : {0U, 25U, 32U, 64U, 0xFFFFFFFFU})
    {
        CHECK_FALSE(hn::bounded_float_with_bits(0.0F, 1.0F, bits, out));
        CHECK(untouched());
    }
    CHECK_FALSE(hn::bounded_float_with_max_code(0.0F, 1.0F, 0U, out));
    CHECK_FALSE(hn::bounded_float_with_max_code(0.0F, 1.0F, hn::kBoundedMaxCode + 1U, out));
    CHECK(untouched());
}

TEST_CASE("bounded float: bounds, clamps and non-finite values", "[quantize]")
{
    const BoundedFloat format = format_of(-10.0F, 10.0F, 0.5F);
    CHECK(format.max_code == 40U);
    CHECK(hn::bounded_encode(format, -10.0F) == 0U);
    CHECK(hn::bounded_encode(format, 10.0F) == 40U);
    CHECK(hn::bounded_encode(format, 0.0F) == 20U);
    CHECK(hn::bounded_encode(format, -0.0F) == 20U);
    CHECK(hn::bounded_encode(format, -11.0F) == 0U);
    CHECK(hn::bounded_encode(format, 11.0F) == 40U);
    CHECK(hn::bounded_encode(format, -kInf) == 0U);
    CHECK(hn::bounded_encode(format, kInf) == 40U);
    CHECK(hn::bounded_encode(format, kNan) == 0U);
    CHECK(hn::bounded_encode(format, -kNan) == 0U);
    // Halfway rounds up.
    CHECK(hn::bounded_encode(format, 0.25F) == 21U);
    CHECK(hn::bounded_encode(format, -0.25F) == 20U);
    f32 value = 99.0F;
    REQUIRE(hn::bounded_decode(format, 40U, value));
    CHECK(value == 10.0F);
    REQUIRE(hn::bounded_decode(format, 0U, value));
    CHECK(value == -10.0F);
    REQUIRE(hn::bounded_decode(format, 21U, value));
    CHECK(value == 0.5F);
    value = 99.0F;
    CHECK_FALSE(hn::bounded_decode(format, 41U, value));
    CHECK_FALSE(hn::bounded_decode(format, 0xFFFFFFFFU, value));
    CHECK(value == 99.0F);

    // The scalars are constexpr: the self-check's answers are evaluated at compile time.
    constexpr BoundedFloat fixed{-1.0F, 2.0F, 255U, 8U};
    static_assert(hn::bounded_encode(fixed, 1.0F) == 255U);
    static_assert(hn::bounded_encode(fixed, -2.0F) == 0U);
}

TEST_CASE("bounded float: every value lands within half a step of its code", "[quantize]")
{
    SeededWords rng(2026, 21);
    const f32 shapes[][3] = {{-100.0F, 100.0F, 0.01F}, {0.0F, 1.0F, 0.0001F},      {-4096.0F, 4096.0F, 0.125F},
                             {-1.0F, 1.0F, 0.5F},      {1000.0F, 1001.0F, 0.001F}, {-1e6F, 1e6F, 1.0F},
                             {-1e-3F, 1e-3F, 1e-6F},   {0.0F, 16777215.0F, 1.0F},  {0.0F, 8388607.0F, 1.0F}};
    usize checked = 0U;
    for (const auto& shape : shapes)
    {
        const BoundedFloat format = format_of(shape[0], shape[1], shape[2]);
        const f32 step = format.delta / static_cast<f32>(format.max_code);
        // Half a step, and the rounding of the value's own subtraction, product and decode.
        const f32 extent = std::fmax(std::fabs(shape[0]), std::fabs(shape[1]));
        const f32 limit = (0.5F * step) + (extent * 4.0F * std::numeric_limits<f32>::epsilon());
        for (u32 i = 0U; i < 20000U; ++i)
        {
            const f32 unit = static_cast<f32>(rng.next_u32() >> 8U) / 16777216.0F;
            const f32 v = shape[0] + (unit * format.delta);
            const u32 code = hn::bounded_encode(format, v);
            REQUIRE(code <= format.max_code);
            f32 back = 0.0F;
            REQUIRE(hn::bounded_decode(format, code, back));
            const f32 cut = std::fmin(std::fmax(v, shape[0]), shape[1]);
            if (!(std::fabs(back - cut) <= limit))
            {
                FAIL("value " << v << " came back " << back << " past " << limit);
            }
            ++checked;
        }
    }
    CHECK(checked == 180000U);
}

TEST_CASE("bounded float: every code of a small format round trips and the order holds", "[quantize]")
{
    for (const u32 bits : {1U, 2U, 3U, 7U, 12U, 16U})
    {
        BoundedFloat format{};
        REQUIRE(hn::bounded_float_with_bits(-37.5F, 112.25F, bits, format));
        u32 previous = 0U;
        for (u32 code = 0U; code <= format.max_code; ++code)
        {
            f32 value = 0.0F;
            REQUIRE(hn::bounded_decode(format, code, value));
            const u32 again = hn::bounded_encode(format, value);
            if (again != code || again < previous)
            {
                FAIL("bits " << bits << " code " << code << " came back " << again);
            }
            previous = again;
        }
    }
}

TEST_CASE("bounded float: at 24 bits a code stays in range where the sum rounds past the top", "[quantize]")
{
    BoundedFloat format{};
    REQUIRE(hn::bounded_float_with_bits(0.0F, 1.0F, 24U, format));
    // 16777215 + 0.5 rounds to 16777216 in float32: one code past the field.
    CHECK(hn::bounded_encode(format, 1.0F) == hn::kBoundedMaxCode);
    CHECK(hn::bounded_encode(format, 2.0F) == hn::kBoundedMaxCode);
    f32 value = 0.0F;
    REQUIRE(hn::bounded_decode(format, hn::kBoundedMaxCode, value));
    CHECK(value == 1.0F);
    u32 code = 0U;
    const f32 one = 1.0F;
    hn::kQuantizeKernelsPortable.bounded_encode(format, &one, 1U, &code);
    CHECK(code == hn::kBoundedMaxCode);
}

TEST_CASE("quantize kernels: the archive holds portable and each body level the build compiles", "[quantize][kernels]")
{
    const auto levels = hn::quantize_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    const crd::CpuArch arch = crd::cpu_selection().arch;
    bool sse2 = false;
    bool avx2 = false;
    bool avx512 = false;
    bool neon = false;
    for (usize i = 0U; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        sse2 = sse2 || levels[i].level == CpuLevel::Sse2;
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == CpuLevel::Avx512;
        neon = neon || levels[i].level == CpuLevel::Neon;
    }
    const bool x64 = arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx512)));
    CHECK(neon == (arch == crd::CpuArch::Arm64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Neon)));
}

TEST_CASE("quantize kernels: every level the host runs gives the scalar bounded codes and values",
          "[quantize][kernels]")
{
    SeededWords rng(2026, 22);
    const f32 shapes[][2] = {{-100.0F, 100.0F}, {0.0F, 1.0F}, {1000.0F, 1001.0F}, {-1e-3F, 1e-3F}, {-5e30F, 5e30F}};
    constexpr usize max_count = 70U;
    constexpr u32 guard = 0xA5A5A5A5U;
    usize ran = 0U;
    for (const KernelLevel<QuantizeKernels>& level : hn::quantize_kernel_levels())
    {
        if (!crd::test::kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        for (const auto& shape : shapes)
        {
            for (const u32 bits : {1U, 8U, 13U, 23U, 24U})
            {
                BoundedFloat format{};
                REQUIRE(hn::bounded_float_with_bits(shape[0], shape[1], bits, format));
                // Every length through several vectors of every level, from odd offsets.
                for (usize count = 0U; count <= max_count; ++count)
                {
                    f32 values[max_count + 1U];
                    u32 codes[max_count + 2U];
                    f32 decoded[max_count + 2U];
                    usize want_refused = 0U;
                    for (usize i = 0U; i < count; ++i)
                    {
                        const f32 sign = (rng.next_u32() & 1U) != 0U ? 1.0F : -1.0F;
                        values[i + 1U] = (i % 7U == 0U) ? std::copysign(kNan, sign) : random_value(rng);
                    }
                    codes[count + 1U] = guard;
                    level.table->bounded_encode(format, values + 1U, count, codes + 1U);
                    CHECK(codes[count + 1U] == guard);
                    bool same = true;
                    for (usize i = 0U; i < count; ++i)
                    {
                        same = same && codes[i + 1U] == hn::bounded_encode(format, values[i + 1U]);
                    }
                    CHECK(same);

                    // Codes past the top, past 2^24 and past 2^31 among the good ones.
                    for (usize i = 0U; i < count; ++i)
                    {
                        const u32 word = rng.next_u32();
                        switch (word % 6U)
                        {
                            case 0U:
                                codes[i + 1U] = format.max_code + 1U + (word >> 12U);
                                break;
                            case 1U:
                                codes[i + 1U] = 0x80000000U | word;
                                break;
                            case 2U:
                                codes[i + 1U] = 0xFFFFFFFFU;
                                break;
                            default:
                                codes[i + 1U] = word & format.max_code;
                                break;
                        }
                    }
                    decoded[count + 1U] = std::bit_cast<f32>(guard);
                    const usize refused = level.table->bounded_decode(format, codes + 1U, count, decoded + 1U);
                    CHECK(bits_of(decoded[count + 1U]) == guard);
                    same = true;
                    for (usize i = 0U; i < count; ++i)
                    {
                        f32 want = format.min;
                        if (!hn::bounded_decode(format, codes[i + 1U], want))
                        {
                            ++want_refused;
                        }
                        same = same && bits_of(decoded[i + 1U]) == bits_of(want);
                    }
                    CHECK(same);
                    CHECK(refused == want_refused);
                }
            }
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("quantize kernels: every level the host runs gives the portable quaternion words and values",
          "[quantize][kernels]")
{
    SeededWords rng(2026, 23);
    constexpr usize max_count = 70U;
    constexpr u64 guard = 0xA5A5A5A5A5A5A5A5ULL;
    usize ran = 0U;
    for (const KernelLevel<QuantizeKernels>& level : hn::quantize_kernel_levels())
    {
        if (!crd::test::kernel_level_runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        for (u32 bits = hn::kSmallestThreeMinBits; bits <= hn::kSmallestThreeMaxBits; ++bits)
        {
            const SmallestThree format = format_of(bits);
            for (usize count = 0U; count <= max_count; ++count)
            {
                f32 xyzw[4U * (max_count + 1U)];
                u64 words[max_count + 2U];
                u64 want_words[max_count + 1U];
                for (usize i = 0U; i < count; ++i)
                {
                    f32* q = xyzw + (4U * (i + 1U));
                    switch (rng.next_u32() % 4U)
                    {
                        case 0U:
                            std::memcpy(q, kEdges[rng.next_u32() % std::size(kEdges)], 4U * sizeof(f32));
                            break;
                        case 1U:
                            for (int c = 0; c < 4; ++c)
                            {
                                q[c] = random_value(rng);
                            }
                            break;
                        default:
                            random_unit(rng, q);
                            break;
                    }
                }
                words[count + 1U] = guard;
                level.table->smallest_three_encode(format, xyzw + 4U, count, words + 1U);
                CHECK(words[count + 1U] == guard);
                hn::kQuantizeKernelsPortable.smallest_three_encode(format, xyzw + 4U, count, want_words);
                CHECK(std::memcmp(words + 1U, want_words, count * sizeof(u64)) == 0);

                for (usize i = 0U; i < count; ++i)
                {
                    const u32 word = rng.next_u32();
                    if ((word & 3U) == 0U)
                    {
                        words[i + 1U] = (u64{rng.next_u32()} << 32U) | rng.next_u32();
                    }
                }
                f32 decoded[4U * (max_count + 2U)];
                f32 want[4U * (max_count + 1U)];
                const u32 last_guard = (4U * (static_cast<u32>(count) + 1U));
                decoded[last_guard] = std::bit_cast<f32>(0xA5A5A5A5U);
                const usize refused = level.table->smallest_three_decode(format, words + 1U, count, decoded + 4U);
                CHECK(bits_of(decoded[last_guard]) == 0xA5A5A5A5U);
                CHECK(refused == hn::kQuantizeKernelsPortable.smallest_three_decode(format, words + 1U, count, want));
                CHECK(std::memcmp(decoded + 4U, want, count * 4U * sizeof(f32)) == 0);
            }
        }
    }
    CHECK(ran >= 1U);
}

TEST_CASE("quantize kernels: the process runs the level its selection allows", "[quantize][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const KernelLevel<QuantizeKernels>& chosen = hn::quantize_kernels();
    CHECK(&chosen == &crd::select_kernel_level<QuantizeKernels>(hn::quantize_kernel_levels(), s.selected,
                                                                s.kernel_features, crd::kernel_level_cap()));
    const crd::KernelFamily* family = crd::test::kernel_family("hesap.quantize");
    REQUIRE(family != nullptr);
    CHECK(family->result.status == crd::KernelCheckStatus::Passed);
    CHECK(family->result.level == chosen.level);
}

TEST_CASE("quantize self-check: every table the host runs passes, and a wrong one fails", "[quantize][kernels]")
{
    const crd::test::SelfCheckRun run =
        crd::test::self_check_levels(hn::quantize_kernel_levels(), &hn::quantize_self_check);
    INFO("first failure " << crd::cpu_level_name(run.level) << " " << routine_of(run.routine));
    CHECK(run.failed == 0U);
    CHECK(run.ran >= 1U);
    QuantizeKernels wrong = hn::kQuantizeKernelsPortable;
    wrong.bounded_encode = &bounded_encode_off;
    CHECK(routine_of(hn::quantize_self_check(wrong)) == "bounded_encode");
    wrong = hn::kQuantizeKernelsPortable;
    wrong.bounded_decode = &bounded_decode_off;
    CHECK(routine_of(hn::quantize_self_check(wrong)) == "bounded_decode");
    wrong = hn::kQuantizeKernelsPortable;
    wrong.smallest_three_encode = &smallest_three_encode_off;
    CHECK(routine_of(hn::quantize_self_check(wrong)) == "smallest_three_encode");
    wrong = hn::kQuantizeKernelsPortable;
    wrong.smallest_three_decode = &smallest_three_decode_off;
    CHECK(routine_of(hn::quantize_self_check(wrong)) == "smallest_three_decode");
}

TEST_CASE("quantize batches: the public calls run the process's level", "[quantize][kernels]")
{
    const BoundedFloat bounded = format_of(-1.0F, 1.0F, 0.25F);
    const f32 values[5] = {-1.0F, -0.25F, 0.0F, 0.5F, 1.0F};
    u32 codes[5];
    hn::bounded_encode(bounded, values, 5U, codes);
    f32 decoded[5];
    CHECK(hn::bounded_decode(bounded, codes, 5U, decoded) == 0U);
    for (usize i = 0U; i < 5U; ++i)
    {
        CHECK(decoded[i] == values[i]);
    }
    const SmallestThree format = format_of(12U);
    u64 word = 0U;
    hn::smallest_three_encode(format, kEdges[3], 1U, &word);
    f32 q[4];
    CHECK(hn::smallest_three_decode(format, &word, 1U, q) == 0U);
    CHECK(q[3] > 0.9999F);
}

TEST_CASE("smallest three: a format has 2 to 16 bits and refuses the rest, writing nothing", "[quantize]")
{
    SmallestThree out{99U, 1.0F, 2.0F, 3.0F};
    for (const u32 bits : {0U, 1U, 17U, 32U, 0xFFFFFFFFU})
    {
        CHECK_FALSE(hn::smallest_three(bits, out));
        CHECK(out.bits == 99U);
    }
    for (u32 bits = hn::kSmallestThreeMinBits; bits <= hn::kSmallestThreeMaxBits; ++bits)
    {
        const SmallestThree format = format_of(bits);
        CHECK(format.bits == bits);
        CHECK(format.scale == static_cast<f32>((u32{1} << bits) - 1U));
        CHECK(hn::smallest_three_word_bits(format) == 2U + (3U * bits));
    }
}

TEST_CASE("smallest three: the word holds the largest's index and the other three in order", "[quantize]")
{
    const SmallestThree format = format_of(9U);
    constexpr u64 mask = (u64{1} << 9U) - 1U;
    // The midpoint of a 9-bit range is 255.5 rounded up; the bound is 1 / 1.414214f.
    constexpr u64 middle = 256U;
    const auto fields = [&](u64 word, u64& largest, u64& a, u64& b, u64& c)
    {
        largest = word >> 27U;
        a = (word >> 18U) & mask;
        b = (word >> 9U) & mask;
        c = word & mask;
    };
    u64 largest = 0U;
    u64 a = 0U;
    u64 b = 0U;
    u64 c = 0U;
    for (u64 axis = 0U; axis < 4U; ++axis)
    {
        INFO("axis " << axis);
        const u64 word = encode_one(format, kEdges[axis]);
        fields(word, largest, a, b, c);
        CHECK(largest == axis);
        CHECK(a == middle);
        CHECK(b == middle);
        CHECK(c == middle);
        // q and -q are one word.
        CHECK(word == encode_one(format, kEdges[axis + 4U]));
    }
    // Ties go to the first of x, y, z, w.
    const f32 half[4] = {0.5F, 0.5F, 0.5F, 0.5F};
    fields(encode_one(format, half), largest, a, b, c);
    CHECK(largest == 0U);
    const f32 yzw[4] = {0.0F, kT, kT, kT};
    fields(encode_one(format, yzw), largest, a, b, c);
    CHECK(largest == 1U);
    const f32 zw[4] = {0.0F, 0.0F, kS, kS};
    fields(encode_one(format, zw), largest, a, b, c);
    CHECK(largest == 2U);
    CHECK(a == middle);
    CHECK(b == middle);
    // The stored order is x, y, z, w without the largest, negated with it.
    const f32 q[4] = {0.1F, -0.2F, -0.9F, 0.3F};
    const f32 scale = 1.0F / std::sqrt(0.01F + 0.04F + 0.81F + 0.09F);
    const f32 unit[4] = {q[0] * scale, q[1] * scale, q[2] * scale, q[3] * scale};
    fields(encode_one(format, unit), largest, a, b, c);
    CHECK(largest == 2U);
    const auto code_of = [](f32 v)
    {
        const f32 normal = (v + (1.0F / 1.414214F)) / (2.0F / 1.414214F);
        return static_cast<u64>(std::floor((normal * 511.0F) + 0.5F));
    };
    CHECK(a == code_of(-unit[0]));
    CHECK(b == code_of(-unit[1]));
    CHECK(c == code_of(-unit[3]));
}

TEST_CASE("smallest three: a word comes back within half a step and at unit length", "[quantize]")
{
    SeededWords rng(2026, 24);
    constexpr double unit_limit = 3e-7;
    for (u32 bits = hn::kSmallestThreeMinBits; bits <= hn::kSmallestThreeMaxBits; ++bits)
    {
        INFO("bits " << bits);
        const SmallestThree format = format_of(bits);
        const double half_step = (2.0 / 1.414214) / static_cast<double>(format.scale) / 2.0;
        // At two and three bits a word can pass length one and be cut back, which moves the components further.
        const double component_limit = (bits <= 3U ? 1.0 : 2.5) * half_step + 2e-6;
        for (u32 i = 0U; i < 20000U + std::size(kEdges); ++i)
        {
            f32 q[4];
            if (i < std::size(kEdges))
            {
                std::memcpy(q, kEdges[i], sizeof(q));
            }
            else
            {
                random_unit(rng, q);
            }
            const u64 word = encode_one(format, q);
            REQUIRE((word >> hn::smallest_three_word_bits(format)) == 0U);
            f32 back[4];
            // An encoder's word is never refused.
            REQUIRE(decode_one(format, word, back) == 0U);
            double length = 0.0;
            double dot = 0.0;
            for (int c = 0; c < 4; ++c)
            {
                length += static_cast<double>(back[c]) * static_cast<double>(back[c]);
                dot += static_cast<double>(back[c]) * static_cast<double>(q[c]);
            }
            if (std::fabs(std::sqrt(length) - 1.0) > unit_limit)
            {
                FAIL("length " << std::sqrt(length) << " at word " << word);
            }
            const int largest = static_cast<int>(word >> (3U * bits));
            const double sign = dot < 0.0 ? -1.0 : 1.0;
            for (int c = 0; c < 4; ++c)
            {
                const double difference = std::fabs(static_cast<double>(back[c]) - (sign * static_cast<double>(q[c])));
                if (bits > 3U && c != largest && difference > component_limit)
                {
                    FAIL("component " << c << " moved " << difference << " at word " << word);
                }
            }
        }
    }
}

TEST_CASE("smallest three: every word decodes to a unit quaternion, refused exactly when no encoder writes it",
          "[quantize]")
{
    SeededWords rng(2026, 25);
    const auto check_word = [](const SmallestThree& format, u64 word)
    {
        f32 q[4];
        const usize refused = decode_one(format, word, q);
        double length = 0.0;
        for (const f32 c : q)
        {
            length += static_cast<double>(c) * static_cast<double>(c);
        }
        if (std::fabs(std::sqrt(length) - 1.0) > 3e-7)
        {
            FAIL("bits " << format.bits << " word " << word << " length " << std::sqrt(length));
        }
        if ((refused != 0U) != restated_refusal(format, word))
        {
            FAIL("bits " << format.bits << " word " << word << " refused " << refused);
        }
        return refused;
    };
    // Every word of the small formats, and one with a stray bit above it.
    usize refused_at_five = 0U;
    for (u32 bits = 2U; bits <= 5U; ++bits)
    {
        const SmallestThree format = format_of(bits);
        const u64 words = u64{1} << hn::smallest_three_word_bits(format);
        usize refused = 0U;
        for (u64 word = 0U; word < words; ++word)
        {
            refused += check_word(format, word);
        }
        CHECK(check_word(format, words) == 1U);
        // Two bits' limit passes every word it can hold.
        if (bits == 2U)
        {
            CHECK(refused == 0U);
        }
        if (bits == 5U)
        {
            refused_at_five = refused;
        }
    }
    CHECK(refused_at_five != 0U);
    for (u32 bits = 6U; bits <= hn::kSmallestThreeMaxBits; ++bits)
    {
        const SmallestThree format = format_of(bits);
        for (u32 i = 0U; i < 40000U; ++i)
        {
            const u64 word = (u64{rng.next_u32()} << 32U) | rng.next_u32();
            (void)check_word(format, word >> (64U - hn::smallest_three_word_bits(format)));
            (void)check_word(format, word);
        }
        // All-ones and all-zeros components: length sqrt(3/2).
        CHECK(check_word(format, (u64{1} << (2U + (3U * bits))) - 1U) == 1U);
        CHECK(check_word(format, u64{0}) == 1U);
    }
}

TEST_CASE("smallest three: the encoder takes any input and keeps its word in range", "[quantize]")
{
    SeededWords rng(2026, 26);
    const f32 extremes[] = {0.0F, -0.0F, kNan, kInf, -kInf, kMax, -kMax, 1e-40F, 2.0F};
    for (u32 bits = hn::kSmallestThreeMinBits; bits <= hn::kSmallestThreeMaxBits; ++bits)
    {
        const SmallestThree format = format_of(bits);
        const auto run = [&](const f32* q)
        {
            const u64 word = encode_one(format, q);
            if ((word >> hn::smallest_three_word_bits(format)) != 0U)
            {
                FAIL("bits " << bits << " word " << word);
            }
            f32 back[4];
            (void)decode_one(format, word, back);
            if (!(std::isfinite(back[0]) && std::isfinite(back[1]) && std::isfinite(back[2]) && std::isfinite(back[3])))
            {
                FAIL("bits " << bits << " word " << word << " decoded a non-finite value");
            }
        };
        for (const f32 x : extremes)
        {
            for (const f32 y : extremes)
            {
                for (const f32 z : extremes)
                {
                    for (const f32 w : extremes)
                    {
                        const f32 q[4] = {x, y, z, w};
                        run(q);
                    }
                }
            }
        }
        for (u32 i = 0U; i < 2000U; ++i)
        {
            const f32 q[4] = {std::bit_cast<f32>(rng.next_u32()), std::bit_cast<f32>(rng.next_u32()),
                              std::bit_cast<f32>(rng.next_u32()), std::bit_cast<f32>(rng.next_u32())};
            run(q);
        }
    }
}
