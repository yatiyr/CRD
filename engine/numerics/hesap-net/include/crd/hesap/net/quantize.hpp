// Quantisation of replicated state (HESAP-NET, capability numerics.quantization, owner crd-hesap-net): bit-packed wire
// values and their dispatched batches. Nothing allocates; the bit stream that carries the codes is the caller's.
//
//   BoundedFloat   A float between two bounds as a code of the fewest bits that hold its resolution, as
//                  mas-bandwidth/serialize's compressed_float: the product rounds before 0.5 is added and the decoded
//                  product before min is, so every level, and that library, gives the same bits. At most 24 bits:
//                  float32 resolves no more steps, and past them encoding would lose codes.
//   SmallestThree  A unit quaternion as the index of its largest component and the other three at `bits` each, as
//                  networked-physics' compressed_quaternion: q and -q are one rotation, so the sign is dropped by
//                  negating to make the largest positive, and the other three lie within 1 / sqrt(2). The bound is that
//                  source's 1 / 1.414214f and ties go to the first of x, y, z, w; both are wire format, so kept. A word
//                  is largest << 3 * bits | a << 2 * bits | b << bits | c.
//
// A peer writes the codes, so decoding refuses what no encoder writes and still returns something harmless: a refused
// bounded code decodes to min, and every smallest-three word to a unit quaternion. An encoder takes any input: NaN and
// out-of-bounds values give codes in range, and a quaternion is the caller's to keep near unit.

#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/quantize_dispatch.hpp>
#include <crd/hesap/net/quantize_kernels.hpp>

#include <bit>
#include <cmath>

namespace crd::hesap::net
{

inline constexpr crd::u32 kBoundedMaxBits = 24U;
inline constexpr crd::u32 kBoundedMaxCode = (crd::u32{1} << kBoundedMaxBits) - 1U;

inline constexpr crd::u32 kSmallestThreeMinBits = 2U;
inline constexpr crd::u32 kSmallestThreeMaxBits = 16U;

// A format of max_code steps over [min, max]: false, writing nothing, unless min < max, the width and the top of the
// range are finite and max_code is 1 to kBoundedMaxCode.
[[nodiscard]] constexpr bool bounded_float_with_max_code(crd::f32 min, crd::f32 max, crd::u32 max_code,
                                                         BoundedFloat& out) noexcept
{
    constexpr crd::f32 largest = 3.4028234663852886e38F;
    if (!(min < max) || !(min >= -largest) || !(max <= largest) || max_code < 1U || max_code > kBoundedMaxCode)
    {
        return false;
    }
    const crd::f32 delta = max - min;
    const crd::f32 top = delta + min;
    if (!(delta <= largest) || !(top <= largest) || !(top >= -largest))
    {
        return false;
    }
    out = BoundedFloat{min, delta, max_code, static_cast<crd::u32>(std::bit_width(max_code))};
    return true;
}

// A format holding steps of at most `resolution`; a range narrower than one step takes one bit. False, writing nothing,
// for a resolution not above zero, one that needs more than kBoundedMaxBits, and the bounds above.
[[nodiscard]] constexpr bool bounded_float(crd::f32 min, crd::f32 max, crd::f32 resolution, BoundedFloat& out) noexcept
{
    if (!(min < max) || !(resolution > 0.0F))
    {
        return false;
    }
    const crd::f32 steps = (max - min) / resolution;
    if (!(steps <= static_cast<crd::f32>(kBoundedMaxCode)))
    {
        return false;
    }
    const auto whole = static_cast<crd::u32>(steps);
    const crd::u32 ceiling = whole + ((static_cast<crd::f32>(whole) < steps) ? 1U : 0U);
    return bounded_float_with_max_code(min, max, (ceiling < 1U) ? 1U : ceiling, out);
}

// A format of exactly `bits` bits: false, writing nothing, outside 1 to kBoundedMaxBits and for the bounds above.
[[nodiscard]] constexpr bool bounded_float_with_bits(crd::f32 min, crd::f32 max, crd::u32 bits,
                                                     BoundedFloat& out) noexcept
{
    if (bits < 1U || bits > kBoundedMaxBits)
    {
        return false;
    }
    return bounded_float_with_max_code(min, max, (crd::u32{1} << bits) - 1U, out);
}

[[nodiscard]] constexpr crd::u32 bounded_encode(const BoundedFloat& format, crd::f32 value) noexcept
{
    crd::f32 normal = (value - format.min) / format.delta;
    normal = (normal >= 0.0F) ? normal : 0.0F;
    normal = (normal <= 1.0F) ? normal : 1.0F;
    const auto top = static_cast<crd::f32>(format.max_code);
    const crd::f32 scaled = normal * top;
    const crd::f32 rounded = scaled + 0.5F;
    // At least 0.5, so truncation is floor; past 2^23 the sum can round above the top.
    return static_cast<crd::u32>((rounded > top) ? top : rounded);
}

// Whether an encoder writes `code`: the verdict of bounded_decode without the float it rebuilds.
[[nodiscard]] constexpr bool bounded_valid(const BoundedFloat& format, crd::u32 code) noexcept
{
    return code <= format.max_code;
}

// False, writing nothing, for a code above max_code. The decoded value lies in [min, max] to within the rounding of the
// top code's min + delta.
[[nodiscard]] constexpr bool bounded_decode(const BoundedFloat& format, crd::u32 code, crd::f32& value) noexcept
{
    if (!bounded_valid(format, code))
    {
        return false;
    }
    const crd::f32 normal = static_cast<crd::f32>(code) / static_cast<crd::f32>(format.max_code);
    const crd::f32 scaled = normal * format.delta;
    value = scaled + format.min;
    return true;
}

// The batches, through the level the process runs: the scalars above bit for bit, and the decode returns the number of
// codes refused (QuantizeKernels states the contract).
inline void bounded_encode(const BoundedFloat& format, const crd::f32* values, crd::usize count,
                           crd::u32* codes) noexcept
{
    quantize_kernels().table->bounded_encode(format, values, count, codes);
}

[[nodiscard]] inline crd::usize bounded_decode(const BoundedFloat& format, const crd::u32* codes, crd::usize count,
                                               crd::f32* values) noexcept
{
    return quantize_kernels().table->bounded_decode(format, codes, count, values);
}

// A format of `bits` per stored component: false, writing nothing, outside the two limits above.
//
// A unit quaternion's largest square is at least 1/4, so its other three squares sum to at most 3/4 and, at their
// equal worst, each component is at most 1/2. Quantising moves each by at most half a step, so a word an encoder wrote
// decodes to a^2 + b^2 + c^2 <= 3 * (1/2 + half step)^2; 2^-20 covers the float rounding. Below a remainder of
// 1 minus that, no encoder wrote the word. At two and three bits the limit passes one, and such words are legitimate.
[[nodiscard]] constexpr bool smallest_three(crd::u32 bits, SmallestThree& out) noexcept
{
    if (bits < kSmallestThreeMinBits || bits > kSmallestThreeMaxBits)
    {
        return false;
    }
    constexpr crd::f32 bound = 1.0F / 1.414214F;
    const auto scale = static_cast<crd::f32>((crd::u32{1} << bits) - 1U);
    const crd::f32 reach = 0.5F + (bound / scale);
    out = SmallestThree{bits, scale, 1.0F / scale, 1.0F - (3.0F * reach * reach) - 9.5367431640625e-07F};
    return true;
}

// The bits one word takes on the wire.
[[nodiscard]] constexpr crd::u32 smallest_three_word_bits(const SmallestThree& format) noexcept
{
    return 2U + (3U * format.bits);
}

namespace detail
{

// The kernels' rounding of one normalised value to [0, top]: max and min as SSE's, whose first operand wins only by a
// strict comparison, so NaN goes to 0.
[[nodiscard]] inline crd::u32 smallest_three_code(crd::f32 normal, crd::f32 top) noexcept
{
    const crd::f32 low = (normal > 0.0F) ? normal : 0.0F;
    const crd::f32 clamped = (low < 1.0F) ? low : 1.0F;
    const crd::f32 rounded = (clamped * top) + 0.5F;
    return static_cast<crd::u32>((rounded < top) ? rounded : top);
}

} // namespace detail

// One quaternion's word and one word's quaternion, the batches' arithmetic on a single lane, so every level and these
// give the same bits (test_quantize.cpp holds them to it). A message codes its fields one at a time, and a dispatched
// batch of one pays for a whole block; decode returns false where the batch counts a refusal, writing all the same.
[[nodiscard]] inline crd::u64 smallest_three_encode_one(const SmallestThree& format, const crd::f32* xyzw) noexcept
{
    const crd::f32 x = xyzw[0];
    const crd::f32 y = xyzw[1];
    const crd::f32 z = xyzw[2];
    const crd::f32 w = xyzw[3];
    // The first of the largest, by a strict comparison in x, y, z, w order; a NaN never wins.
    crd::f32 largest = std::fabs(x);
    crd::f32 chosen = x;
    crd::u32 index = 0U;
    if (std::fabs(y) > largest)
    {
        largest = std::fabs(y);
        chosen = y;
        index = 1U;
    }
    if (std::fabs(z) > largest)
    {
        largest = std::fabs(z);
        chosen = z;
        index = 2U;
    }
    if (std::fabs(w) > largest)
    {
        chosen = w;
        index = 3U;
    }
    const crd::f32 a = (index == 0U) ? y : x;
    const crd::f32 b = (index < 2U) ? z : y;
    const crd::f32 c = (index == 3U) ? z : w;
    const bool keep = chosen >= 0.0F;
    const crd::f32 top = format.scale;
    const auto code = [&](crd::f32 value)
    {
        return crd::u64{
            detail::smallest_three_code(((keep ? value : -value) - kSmallestThreeMinimum) / kSmallestThreeRange, top)};
    };
    const crd::u32 bits = format.bits;
    return (crd::u64{index} << (3U * bits)) | (code(a) << (2U * bits)) | (code(b) << bits) | code(c);
}

// A word's three stored components and the square left for the largest, which decide whether an encoder wrote it. The
// decode and the check below both read them here, so their verdicts agree on every word.
struct SmallestThreeStored
{
    crd::f32 a;
    crd::f32 b;
    crd::f32 c;
    crd::f32 a2;
    crd::f32 b2;
    crd::f32 c2;
    crd::f32 remainder;
};

[[nodiscard]] CRD_FORCEINLINE SmallestThreeStored smallest_three_stored(const SmallestThree& format,
                                                                        crd::u64 word) noexcept
{
    const crd::u32 bits = format.bits;
    const crd::u64 mask = (crd::u64{1} << bits) - 1U;
    const auto component = [&](crd::u32 shift)
    {
        const auto field = static_cast<crd::f32>(static_cast<crd::u32>((word >> shift) & mask));
        return ((field * format.inverse_scale) * kSmallestThreeRange) + kSmallestThreeMinimum;
    };
    SmallestThreeStored stored{};
    stored.a = component(2U * bits);
    stored.b = component(bits);
    stored.c = component(0U);
    stored.a2 = stored.a * stored.a;
    stored.b2 = stored.b * stored.b;
    stored.c2 = stored.c * stored.c;
    stored.remainder = ((1.0F - stored.a2) - stored.b2) - stored.c2;
    return stored;
}

[[nodiscard]] CRD_FORCEINLINE bool smallest_three_wrote(const SmallestThree& format, crd::u64 word,
                                                        crd::f32 remainder) noexcept
{
    return !(remainder < format.smallest_remainder) && (word >> smallest_three_word_bits(format)) == 0U;
}

// Whether an encoder writes `word`: the verdict of smallest_three_decode_one without the square root, the cut back to
// unit length or the placement of the largest.
[[nodiscard]] inline bool smallest_three_valid(const SmallestThree& format, crd::u64 word) noexcept
{
    return smallest_three_wrote(format, word, smallest_three_stored(format, word).remainder);
}

[[nodiscard]] inline bool smallest_three_decode_one(const SmallestThree& format, crd::u64 word, crd::f32* xyzw) noexcept
{
    const crd::u32 bits = format.bits;
    const SmallestThreeStored stored = smallest_three_stored(format, word);
    crd::f32 a = stored.a;
    crd::f32 b = stored.b;
    crd::f32 c = stored.c;
    const crd::f32 a2 = stored.a2;
    const crd::f32 b2 = stored.b2;
    const crd::f32 c2 = stored.c2;
    const crd::f32 remainder = stored.remainder;
    // Past length one, which only a hostile word or a width of two or three bits reaches, the largest is zero and the
    // other three are cut back to unit length.
    const bool over = remainder < 0.0F;
    if (over)
    {
        const crd::f32 length = std::sqrt((a2 + b2) + c2);
        a = a / length;
        b = b / length;
        c = c / length;
    }
    const crd::f32 big = std::sqrt(over ? 0.0F : remainder);
    // The largest goes back at its index; the other three fill the rest in order.
    const auto index = static_cast<crd::u32>((word >> (3U * bits)) & 3U);
    const crd::f32 others[3] = {a, b, c};
    crd::u32 next = 0U;
    for (crd::u32 slot = 0U; slot < 4U; ++slot)
    {
        xyzw[slot] = (slot == index) ? big : others[next];
        next += (slot == index) ? 0U : 1U;
    }
    return smallest_three_wrote(format, word, remainder);
}

inline void smallest_three_encode(const SmallestThree& format, const crd::f32* xyzw, crd::usize count,
                                  crd::u64* words) noexcept
{
    quantize_kernels().table->smallest_three_encode(format, xyzw, count, words);
}

[[nodiscard]] inline crd::usize smallest_three_decode(const SmallestThree& format, const crd::u64* words,
                                                      crd::usize count, crd::f32* xyzw) noexcept
{
    return quantize_kernels().table->smallest_three_decode(format, words, count, xyzw);
}

} // namespace crd::hesap::net
