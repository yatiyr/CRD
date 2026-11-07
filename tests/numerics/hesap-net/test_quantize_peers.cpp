// HESAP-NET's quantisation against its two reference implementations, bit for bit, at every kernel level the host runs:
// mas-bandwidth/serialize 2.1.3's compressed_float through its own bit streams, and networked-physics'
// compressed_quaternion on unit quaternions, ties and the signs of both. CRD_BUILD_QUANTIZE_VS_PEERS.

#include "kernel_levels.hpp"
#include "quantize_peers.hpp"
#include "seeded_words.hpp"

#include <crd/hesap/net/quantize.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace hn = crd::hesap::net;
using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::usize;
using hn::BoundedFloat;
using hn::QuantizeKernels;
using hn::SmallestThree;

namespace
{

constexpr usize kBatch = 67U;

[[nodiscard]] u32 bits_of(f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}

// A declaration: bounds and a resolution of many magnitudes, each of the shapes a schema writes.
struct Declaration
{
    f32 min;
    f32 max;
    f32 resolution;
};

[[nodiscard]] Declaration random_declaration(SeededWords& rng) noexcept
{
    const f32 magnitude = std::ldexp(1.0F, static_cast<int>(rng.next_u32() % 41U) - 20);
    const f32 unit_a = static_cast<f32>(rng.next_u32() >> 8U) / 16777216.0F;
    const f32 unit_b = static_cast<f32>(rng.next_u32() >> 8U) / 16777216.0F;
    const f32 min = (rng.next_u32() & 1U) != 0U ? -magnitude * unit_a : 0.0F;
    const f32 max = min + (magnitude * (unit_b + 0.001F));
    // A whole number of steps, or a resolution that leaves a part step the format must round up.
    const int bits = 1 + static_cast<int>(rng.next_u32() % 24U);
    const f32 part = 1.0F + (static_cast<f32>(rng.next_u32() >> 8U) / 33554432.0F);
    return Declaration{min, max, ((max - min) / std::ldexp(1.0F, bits)) * ((rng.next_u32() & 1U) != 0U ? part : 1.0F)};
}

[[nodiscard]] f32 value_in(SeededWords& rng, const BoundedFloat& format) noexcept
{
    const f32 unit = static_cast<f32>(rng.next_u32() >> 8U) / 16777216.0F;
    switch (rng.next_u32() % 8U)
    {
        case 0U:
            return format.min;
        case 1U:
            return format.min + format.delta;
        case 2U:
            return format.min - (unit * format.delta);
        case 3U:
            return format.min + format.delta + (unit * format.delta);
        default:
            return format.min + (unit * format.delta);
    }
}

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

// A quaternion with some components equal or zero, the cases that reach ties and each sign's branch.
void structured_unit(SeededWords& rng, f32* q) noexcept
{
    constexpr f32 values[] = {0.0F, 0.25F, 0.5F, 0.70710678F, 0.57735027F, 1.0F, -0.0F};
    double sum = 0.0;
    double v[4];
    for (double& c : v)
    {
        const f32 value = values[rng.next_u32() % std::size(values)];
        c = (rng.next_u32() & 1U) != 0U ? -static_cast<double>(value) : static_cast<double>(value);
        sum += c * c;
    }
    if (sum == 0.0)
    {
        v[3] = 1.0;
        sum = 1.0;
    }
    const double scale = 1.0 / std::sqrt(sum);
    for (int i = 0; i < 4; ++i)
    {
        q[i] = static_cast<f32>(v[i] * scale);
    }
}

} // namespace

TEST_CASE("bounded float vs serialize: the same constants for every declaration", "[quantize][peer]")
{
    SeededWords rng(2026, 31);
    usize compared = 0U;
    for (u32 i = 0U; i < 200000U; ++i)
    {
        const Declaration d = random_declaration(rng);
        BoundedFloat format{};
        if (!hn::bounded_float(d.min, d.max, d.resolution, format))
        {
            continue;
        }
        u32 max_code = 0U;
        int bits = 0;
        f32 delta = 0.0F;
        quantize_peers::float_params(d.min, d.max, d.resolution, max_code, bits, delta);
        if (format.max_code != max_code || format.bits != static_cast<u32>(bits) ||
            bits_of(format.delta) != bits_of(delta))
        {
            FAIL("min " << d.min << " max " << d.max << " resolution " << d.resolution << ": " << format.max_code
                        << " against " << max_code);
        }
        ++compared;
    }
    CHECK(compared > 190000U);
}

TEST_CASE("bounded float vs serialize: every code and decoded value, at every level", "[quantize][peer]")
{
    SeededWords rng(2026, 32);
    usize values = 0U;
    usize levels_ran = 0U;
    for (const KernelLevel<QuantizeKernels>& level : hn::quantize_kernel_levels())
    {
        if (!crd::test::kernel_level_runs_here(level))
        {
            continue;
        }
        ++levels_ran;
        INFO("level " << crd::cpu_level_name(level.level));
        for (u32 i = 0U; i < 6000U; ++i)
        {
            const Declaration d = random_declaration(rng);
            BoundedFloat format{};
            if (!hn::bounded_float(d.min, d.max, d.resolution, format))
            {
                continue;
            }
            f32 inputs[kBatch];
            for (f32& input : inputs)
            {
                input = value_in(rng, format);
            }
            u32 codes[kBatch];
            f32 decoded[kBatch];
            level.table->bounded_encode(format, inputs, kBatch, codes);
            REQUIRE(level.table->bounded_decode(format, codes, kBatch, decoded) == 0U);
            for (usize j = 0U; j < kBatch; ++j)
            {
                const u32 want = quantize_peers::float_write(inputs[j], d.min, d.max, d.resolution);
                f32 peer_value = 0.0F;
                const bool read = quantize_peers::float_read(want, d.min, d.max, d.resolution, peer_value);
                if (codes[j] != want || !read || bits_of(decoded[j]) != bits_of(peer_value))
                {
                    FAIL("min " << d.min << " max " << d.max << " resolution " << d.resolution << " value " << inputs[j]
                                << ": code " << codes[j] << " against " << want << ", value " << decoded[j]
                                << " against " << peer_value);
                }
                ++values;
            }
        }
    }
    CHECK(levels_ran >= 1U);
    CHECK(values > 100000U);
}

TEST_CASE("bounded float vs serialize: the top of the 24-bit range and the codes it refuses", "[quantize][peer]")
{
    SeededWords rng(2026, 33);
    const f32 shapes[][2] = {{0.0F, 1.0F}, {-1.0F, 1.0F}, {1000.0F, 1001.0F}, {-100.0F, 100.0F}, {0.0F, 16777215.0F}};
    for (const auto& shape : shapes)
    {
        for (const u32 bits : {22U, 23U, 24U})
        {
            BoundedFloat format{};
            REQUIRE(hn::bounded_float_with_bits(shape[0], shape[1], bits, format));
            const auto wire_bits = static_cast<int>(format.bits);
            for (u32 i = 0U; i < 40000U; ++i)
            {
                // The last steps below the top, where the float sum rounds past it, then anywhere.
                const f32 unit = (i < 20000U) ? 1.0F - (static_cast<f32>(i) * 5.9604644775390625e-08F)
                                              : static_cast<f32>(rng.next_u32() >> 8U) / 16777216.0F;
                const f32 value = shape[0] + (unit * format.delta);
                const u32 want = quantize_peers::float_write_precomputed(value, format.max_code, wire_bits,
                                                                         format.delta, format.min);
                const u32 code = hn::bounded_encode(format, value);
                u32 batch = 0U;
                hn::bounded_encode(format, &value, 1U, &batch);
                REQUIRE(batch == code);
                f32 ours = 0.0F;
                f32 theirs = 0.0F;
                REQUIRE(hn::bounded_decode(format, code, ours));
                const bool read = quantize_peers::float_read_precomputed(want, format.max_code, wire_bits, format.delta,
                                                                         format.min, theirs);
                if (code != want || !read || bits_of(ours) != bits_of(theirs))
                {
                    FAIL("bits " << bits << " value " << value << ": code " << code << " against " << want);
                }
            }
        }
    }
    // A code above a max_code that does not fill its bits is refused by both, with nothing else on the wire.
    BoundedFloat format{};
    REQUIRE(hn::bounded_float(0.0F, 10.0F, 1.0F, format));
    REQUIRE(format.max_code == 10U);
    for (u32 code = 0U; code < 16U; ++code)
    {
        f32 ours = 0.0F;
        f32 theirs = 0.0F;
        CHECK(hn::bounded_decode(format, code, ours) == quantize_peers::float_read(code, 0.0F, 10.0F, 1.0F, theirs));
        if (code <= 10U)
        {
            CHECK(bits_of(ours) == bits_of(theirs));
        }
    }
}

TEST_CASE("smallest three vs networked-physics: every word and decoded value, at every bits and level",
          "[quantize][peer]")
{
    SeededWords rng(2026, 34);
    usize quaternions = 0U;
    usize cut_back = 0U;
    usize levels_ran = 0U;
    for (const KernelLevel<QuantizeKernels>& level : hn::quantize_kernel_levels())
    {
        if (!crd::test::kernel_level_runs_here(level))
        {
            continue;
        }
        ++levels_ran;
        INFO("level " << crd::cpu_level_name(level.level));
        for (u32 bits = hn::kSmallestThreeMinBits; bits <= hn::kSmallestThreeMaxBits; ++bits)
        {
            INFO("bits " << bits);
            SmallestThree format{};
            REQUIRE(hn::smallest_three(bits, format));
            for (u32 round = 0U; round < 400U; ++round)
            {
                f32 xyzw[4U * kBatch];
                for (usize j = 0U; j < kBatch; ++j)
                {
                    if ((j & 1U) != 0U)
                    {
                        random_unit(rng, xyzw + (4U * j));
                    }
                    else
                    {
                        structured_unit(rng, xyzw + (4U * j));
                    }
                }
                u64 words[kBatch];
                f32 decoded[4U * kBatch];
                level.table->smallest_three_encode(format, xyzw, kBatch, words);
                (void)level.table->smallest_three_decode(format, words, kBatch, decoded);
                for (usize j = 0U; j < kBatch; ++j)
                {
                    const quantize_peers::QuaternionWord want =
                        quantize_peers::quaternion_load(static_cast<int>(bits), xyzw + (4U * j));
                    const u64 word = (u64{want.largest} << (3U * bits)) | (u64{want.a} << (2U * bits)) |
                                     (u64{want.b} << bits) | u64{want.c};
                    if (words[j] != word)
                    {
                        FAIL("q " << xyzw[4U * j] << " " << xyzw[(4U * j) + 1U] << " " << xyzw[(4U * j) + 2U] << " "
                                  << xyzw[(4U * j) + 3U] << ": word " << words[j] << " against " << word);
                    }
                    f32 peer[4];
                    quantize_peers::quaternion_save(static_cast<int>(bits), want, peer);
                    // Past length one the peer's square root is NaN, where ours cuts back to unit length.
                    if (std::isnan(peer[want.largest]))
                    {
                        ++cut_back;
                        continue;
                    }
                    if (std::memcmp(peer, decoded + (4U * j), sizeof(peer)) != 0)
                    {
                        FAIL("word " << word << ": " << decoded[4U * j] << " " << decoded[(4U * j) + 1U] << " "
                                     << decoded[(4U * j) + 2U] << " " << decoded[(4U * j) + 3U] << " against "
                                     << peer[0] << " " << peer[1] << " " << peer[2] << " " << peer[3]);
                    }
                    ++quaternions;
                }
            }
        }
    }
    CHECK(levels_ran >= 1U);
    CHECK(quaternions > 300000U);
    // Only the lowest widths reach it, where the peer returns NaN.
    CHECK(cut_back != 0U);
}
