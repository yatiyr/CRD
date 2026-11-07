// crd-fuzz-hesap-quantize: hesap-net's quantisation (HESAP-NET; crd/hesap/net/quantize.hpp) under the bounded fuzz
// harness (REPO.DEV.9; docs/design/test-instruments.md). The floats of a format, the values and the words a peer writes
// are all the fuzzer's.
//
// Input. Op bytes, each followed by its arguments; a short input reads zeros.
//
// Oracles, none of them the kernels' own verdict:
//   * a format: accepted exactly when its restated conditions hold, nothing written on a refusal, and the top code's
//     value finite;
//   * a bounded code: in range for any value, the scalar's at every level the CPU runs, and within half a step of the
//     value once decoded for a value in bounds;
//   * a bounded decode: refused exactly above max_code, the value min there, at every level;
//   * a quaternion word: within its bits for any input, and the portable level's at every level;
//   * a decoded word: finite and unit length for any bits on the wire, refused exactly as restated from the word;
//   * a word of a unit quaternion: never refused, and its stored components within reach of the input's.
#include <crd/fuzz/harness.hpp>
#include <crd/hesap/net/quantize.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>

namespace
{
using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::net;

constexpr usize kMaxBatch = 37U;
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr f32 kEdges[] = {0.0F,
                          -0.0F,
                          1.0F,
                          -1.0F,
                          0.5F,
                          1024.0F,
                          -1024.0F,
                          1e-40F,
                          kInf,
                          -kInf,
                          std::numeric_limits<f32>::quiet_NaN(),
                          std::numeric_limits<f32>::max(),
                          16777215.0F,
                          8388608.0F,
                          0.0009765625F};

struct Reader
{
    const u8* data;
    usize size;

    [[nodiscard]] u8 next() noexcept
    {
        if (size == 0U)
        {
            return 0U;
        }
        --size;
        return *data++;
    }

    [[nodiscard]] u32 next_u32() noexcept
    {
        u32 value = 0U;
        for (u32 i = 0U; i < 4U; ++i)
        {
            value |= u32{next()} << (8U * i);
        }
        return value;
    }

    [[nodiscard]] u64 next_u64() noexcept
    {
        const u64 low = next_u32();
        return low | (u64{next_u32()} << 32U);
    }

    // Four bits name a value at an edge; the rest read a float's bits.
    [[nodiscard]] f32 next_float() noexcept
    {
        const u8 pick = next();
        if (pick < std::size(kEdges))
        {
            return kEdges[pick];
        }
        return std::bit_cast<f32>(next_u32());
    }
};

template <typename Check> void for_each_level(const Check& check)
{
    const crd::CpuSelection& selection = crd::cpu_selection();
    const crd::CpuLevel cap = crd::kernel_level_cap();
    for (const KernelLevel<QuantizeKernels>& level : quantize_kernel_levels())
    {
        if (crd::kernel_level_eligible(level, selection.selected, selection.kernel_features, cap))
        {
            check(*level.table);
        }
    }
}

void format_and_values(Reader& in)
{
    const f32 min = in.next_float();
    const f32 max = in.next_float();
    const u8 kind = in.next();
    BoundedFloat format{1.0F, 2.0F, 3U, 4U};
    bool accepted = false;
    if ((kind & 1U) != 0U)
    {
        const u32 bits = in.next();
        accepted = bounded_float_with_bits(min, max, bits, format);
        CRD_FUZZ_REQUIRE(!accepted || (format.bits == bits && format.max_code == (u32{1} << bits) - 1U));
    }
    else
    {
        const f32 resolution = in.next_float();
        accepted = bounded_float(min, max, resolution, format);
        if (accepted)
        {
            CRD_FUZZ_REQUIRE(resolution > 0.0F);
            const f32 steps = (max - min) / resolution;
            CRD_FUZZ_REQUIRE(static_cast<f32>(format.max_code) >= steps);
            CRD_FUZZ_REQUIRE(format.max_code == 1U || static_cast<f32>(format.max_code - 1U) < steps);
        }
    }
    if (!accepted)
    {
        CRD_FUZZ_REQUIRE(format.min == 1.0F && format.delta == 2.0F && format.max_code == 3U && format.bits == 4U);
        return;
    }
    const bool finite = std::isfinite(min) && std::isfinite(max) && std::isfinite(max - min);
    CRD_FUZZ_REQUIRE(min < max && finite && format.max_code >= 1U && format.max_code <= kBoundedMaxCode);
    CRD_FUZZ_REQUIRE(static_cast<u32>(std::bit_width(format.max_code)) == format.bits);
    f32 top = 0.0F;
    CRD_FUZZ_REQUIRE(bounded_decode(format, format.max_code, top) && std::isfinite(top));

    const usize count = in.next() % (kMaxBatch + 1U);
    f32 values[kMaxBatch];
    u32 wire[kMaxBatch];
    for (usize i = 0U; i < count; ++i)
    {
        values[i] = in.next_float();
        wire[i] = in.next_u32();
    }
    const f32 step = format.delta / static_cast<f32>(format.max_code);
    const f32 extent = std::fmax(std::fabs(min), std::fabs(max));
    const f32 slack = (0.5F * step) + (extent * 4.0F * std::numeric_limits<f32>::epsilon());
    for (usize i = 0U; i < count; ++i)
    {
        const u32 code = bounded_encode(format, values[i]);
        CRD_FUZZ_REQUIRE(code <= format.max_code);
        if (values[i] >= min && values[i] <= max)
        {
            f32 back = 0.0F;
            CRD_FUZZ_REQUIRE(bounded_decode(format, code, back));
            CRD_FUZZ_REQUIRE(std::fabs(back - values[i]) <= slack);
        }
    }
    for_each_level(
        [&](const QuantizeKernels& table)
        {
            u32 codes[kMaxBatch];
            f32 decoded[kMaxBatch];
            table.bounded_encode(format, values, count, codes);
            for (usize i = 0U; i < count; ++i)
            {
                CRD_FUZZ_REQUIRE(codes[i] == bounded_encode(format, values[i]));
            }
            const usize refused = table.bounded_decode(format, wire, count, decoded);
            usize want = 0U;
            for (usize i = 0U; i < count; ++i)
            {
                f32 expected = format.min;
                want += wire[i] > format.max_code ? 1U : 0U;
                CRD_FUZZ_REQUIRE(bounded_decode(format, wire[i], expected) == (wire[i] <= format.max_code));
                CRD_FUZZ_REQUIRE(std::bit_cast<u32>(decoded[i]) == std::bit_cast<u32>(expected));
            }
            CRD_FUZZ_REQUIRE(refused == want);
        });
}

// The refusal restated from the word.
[[nodiscard]] bool restated_refusal(const SmallestThree& format, u64 word) noexcept
{
    constexpr f32 bound = 1.0F / 1.414214F;
    constexpr f32 range = bound - (-1.0F / 1.414214F);
    if ((word >> smallest_three_word_bits(format)) != 0U)
    {
        return true;
    }
    const u64 mask = (u64{1} << format.bits) - 1U;
    f32 remainder = 1.0F;
    for (const u32 shift : {2U * format.bits, format.bits, 0U})
    {
        const f32 scaled = (static_cast<f32>((word >> shift) & mask) * format.inverse_scale) * range;
        const f32 component = scaled + (-bound);
        remainder = remainder - (component * component);
    }
    return remainder < format.smallest_remainder;
}

void check_decoded(const SmallestThree& format, const u64* words, usize count, const QuantizeKernels& table)
{
    f32 xyzw[4U * kMaxBatch];
    const usize refused = table.smallest_three_decode(format, words, count, xyzw);
    usize want = 0U;
    for (usize i = 0U; i < count; ++i)
    {
        want += restated_refusal(format, words[i]) ? 1U : 0U;
        double length = 0.0;
        for (usize c = 0U; c < 4U; ++c)
        {
            CRD_FUZZ_REQUIRE(std::isfinite(xyzw[(4U * i) + c]));
            length += static_cast<double>(xyzw[(4U * i) + c]) * static_cast<double>(xyzw[(4U * i) + c]);
        }
        CRD_FUZZ_REQUIRE(std::fabs(std::sqrt(length) - 1.0) <= 3e-7);
    }
    CRD_FUZZ_REQUIRE(refused == want);
}

void quaternions(Reader& in)
{
    SmallestThree format{};
    const u32 bits = in.next();
    if (!smallest_three(bits, format))
    {
        CRD_FUZZ_REQUIRE(bits < kSmallestThreeMinBits || bits > kSmallestThreeMaxBits);
        return;
    }
    const usize count = in.next() % (kMaxBatch + 1U);
    f32 xyzw[4U * kMaxBatch];
    for (usize i = 0U; i < 4U * count; ++i)
    {
        xyzw[i] = in.next_float();
    }
    u64 expected[kMaxBatch];
    kQuantizeKernelsPortable.smallest_three_encode(format, xyzw, count, expected);
    for (usize i = 0U; i < count; ++i)
    {
        CRD_FUZZ_REQUIRE((expected[i] >> smallest_three_word_bits(format)) == 0U);
    }
    for_each_level(
        [&](const QuantizeKernels& table)
        {
            u64 words[kMaxBatch];
            table.smallest_three_encode(format, xyzw, count, words);
            CRD_FUZZ_REQUIRE(std::memcmp(words, expected, count * sizeof(u64)) == 0);
            check_decoded(format, words, count, table);
        });

    // The same inputs made unit, where a finite direction exists: an encoder's word is never refused.
    for (usize i = 0U; i < count; ++i)
    {
        double sum = 0.0;
        double v[4];
        for (usize c = 0U; c < 4U; ++c)
        {
            const f32 raw = xyzw[(4U * i) + c];
            v[c] = (std::isfinite(raw) && std::fabs(raw) < 1e18F) ? static_cast<double>(raw) : 0.0;
            sum += v[c] * v[c];
        }
        if (!(sum > 1e-30))
        {
            continue;
        }
        f32 unit[4];
        for (usize c = 0U; c < 4U; ++c)
        {
            unit[c] = static_cast<f32>(v[c] / std::sqrt(sum));
        }
        u64 word = 0U;
        smallest_three_encode(format, unit, 1U, &word);
        f32 back[4];
        CRD_FUZZ_REQUIRE(smallest_three_decode(format, &word, 1U, back) == 0U);
    }
}

void raw_words(Reader& in)
{
    SmallestThree format{};
    if (!smallest_three(kSmallestThreeMinBits + (in.next() % (kSmallestThreeMaxBits - 1U)), format))
    {
        CRD_FUZZ_REQUIRE(false);
    }
    const usize count = in.next() % (kMaxBatch + 1U);
    u64 words[kMaxBatch];
    for (usize i = 0U; i < count; ++i)
    {
        words[i] = in.next_u64();
        // Half of them within the word, which reach every component.
        if ((words[i] & 1U) != 0U)
        {
            words[i] >>= 64U - smallest_three_word_bits(format);
        }
    }
    u8 expected[4U * kMaxBatch * sizeof(f32)];
    f32 portable[4U * kMaxBatch];
    (void)kQuantizeKernelsPortable.smallest_three_decode(format, words, count, portable);
    std::memcpy(expected, portable, count * 4U * sizeof(f32));
    for_each_level(
        [&](const QuantizeKernels& table)
        {
            f32 xyzw[4U * kMaxBatch];
            (void)table.smallest_three_decode(format, words, count, xyzw);
            CRD_FUZZ_REQUIRE(std::memcmp(xyzw, expected, count * 4U * sizeof(f32)) == 0);
            check_decoded(format, words, count, table);
        });
}

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    sink.add(name, bytes.begin(), bytes.size());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader in{data, size};
    for (u32 ops = 0U; ops < 8U && in.size != 0U; ++ops)
    {
        switch (in.next() % 3U)
        {
            case 0U:
                format_and_values(in);
                break;
            case 1U:
                quaternions(in);
                break;
            default:
                raw_words(in);
                break;
        }
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // One op of each kind at small values and at the extremes.
    add(sink, "bounded-resolution", {0U, 3U, 5U, 0U, 15U, 0U, 0U, 128U, 61U, 7U, 2U, 4U, 0U, 0U, 0U, 0U});
    add(sink, "bounded-bits", {0U, 3U, 2U, 1U, 24U, 16U, 2U, 2U, 255U, 255U, 255U, 255U, 3U, 0U, 0U, 0U, 1U, 4U});
    add(sink, "bounded-refused", {0U, 2U, 2U, 0U, 0U});
    add(sink, "bounded-nan", {0U, 10U, 2U, 0U, 2U, 37U, 10U, 8U, 9U, 11U});
    add(sink, "bounded-top", {0U, 0U, 13U, 1U, 24U, 37U, 13U, 255U, 255U, 255U, 0U});
    add(sink, "quaternion-axes", {1U, 9U, 8U, 2U, 0U, 0U, 0U, 0U, 2U, 0U, 0U, 0U, 0U, 2U, 0U, 3U, 0U, 3U, 0U, 3U});
    add(sink, "quaternion-tie", {1U, 16U, 2U, 4U, 4U, 4U, 4U, 5U, 4U, 5U, 4U});
    add(sink, "quaternion-low", {1U, 2U, 3U, 4U, 4U, 4U, 0U, 0U, 4U, 4U, 4U, 5U, 5U, 5U, 0U});
    add(sink, "quaternion-extremes", {1U, 8U, 2U, 10U, 8U, 9U, 11U, 7U, 0U, 7U, 12U});
    add(sink, "quaternion-bad-bits", {1U, 17U});
    add(sink, "words-small",
        {2U, 0U, 5U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 255U, 255U, 255U, 255U, 255U, 255U, 255U, 255U});
    add(sink, "words-wide", {2U, 14U, 37U, 255U, 255U, 255U, 255U, 255U, 255U, 255U, 255U});
    add(sink, "words-sequence", {2U, 7U, 2U, 3U, 0U, 0U, 0U, 0U, 0U, 0U, 16U, 1U, 9U, 1U, 3U, 3U, 3U, 3U, 0U, 3U, 3U});
}
