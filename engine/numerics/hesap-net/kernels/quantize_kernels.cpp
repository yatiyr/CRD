// The quantisation kernels of crd/hesap/net/quantize_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), so no function
// of this unit is shared with general code or with another level.
//
// One body serves every level, written over the level's float and i32 lanes (crd/math/simd): sixteen on AVX-512, eight
// on AVX2, four on SSE2 and NEON, and four scalars on the portable level. A short tail runs the same body on a padded
// block, so no scalar path can round differently. The build pins contraction off and no body uses a fused operation, so
// each product rounds before its sum, the wire format's two roundings; truncation is floor because every argument has
// been clamped to at least 0.5.

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/net/quantize_kernels.hpp>
#include <crd/math/simd/convert.hpp>
#include <crd/math/simd/interleave4.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec4i.hpp>

#if CRD_SIMD_HAS_AVX2
#include <crd/math/simd/vec8f.hpp>
#include <crd/math/simd/vec8i.hpp>
#endif
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec16i.hpp>
#endif

#include <cstring>

namespace crd::hesap::net::inline CRD_MATH_LEVEL_NS
{
namespace
{

using crd::f32;
using crd::i32;
using crd::u32;
using crd::u64;
using crd::usize;

#if CRD_SIMD_HAS_AVX512
using F = crd::math::simd::Vec16f;
using I = crd::math::simd::Vec16i;
constexpr usize kLanes = 16U;
#elif CRD_SIMD_HAS_AVX2
using F = crd::math::simd::Vec8f;
using I = crd::math::simd::Vec8i;
constexpr usize kLanes = 8U;
#else
using F = crd::math::simd::Vec4f;
using I = crd::math::simd::Vec4i;
constexpr usize kLanes = 4U;
#endif

// The stored components' bound and width, as compressed_quaternion spells them (crd/hesap/net/quantize.hpp).
constexpr f32 kMinimum = -1.0F / 1.414214F;
constexpr f32 kMaximum = 1.0F / 1.414214F;
constexpr f32 kRange = kMaximum - kMinimum;

CRD_FORCEINLINE I load_i32(const u32* lanes) noexcept
{
    f32 raw[kLanes];
    std::memcpy(raw, lanes, sizeof(raw));
    return crd::math::simd::bitcast_to_int(F::load(raw));
}

CRD_FORCEINLINE void store_i32(I lanes, u32* out) noexcept
{
    f32 raw[kLanes];
    crd::math::simd::bitcast_to_float(lanes).store(raw);
    std::memcpy(out, raw, sizeof(raw));
}

// [0, top] of a normalised value, NaN at 0, rounded by adding a half after the product has rounded and cut to the top.
CRD_FORCEINLINE I code_of(F normal, F top) noexcept
{
    const F zero = F::zero();
    F clamped = select(cmp_ge(normal, zero), normal, zero);
    clamped = select(cmp_le(clamped, F::one()), clamped, F::one());
    const F scaled = clamped * top;
    const F rounded = scaled + F(0.5F);
    return crd::math::simd::convert_truncate(select(cmp_gt(rounded, top), top, rounded));
}

CRD_FORCEINLINE void bounded_encode_block(const BoundedFloat& format, const f32* values, u32* codes) noexcept
{
    const F normal = (F::load(values) - F(format.min)) / F(format.delta);
    store_i32(code_of(normal, F(static_cast<f32>(format.max_code))), codes);
}

void bounded_encode(const BoundedFloat& format, const f32* values, usize count, u32* codes) noexcept
{
    usize i = 0U;
    for (; i + kLanes <= count; i += kLanes)
    {
        bounded_encode_block(format, values + i, codes + i);
    }
    if (i < count)
    {
        f32 last[kLanes] = {};
        u32 last_codes[kLanes];
        std::memcpy(last, values + i, (count - i) * sizeof(f32));
        bounded_encode_block(format, last, last_codes);
        std::memcpy(codes + i, last_codes, (count - i) * sizeof(u32));
    }
}

// The refused lanes of the block, each -1. A code past 2^31 is a negative lane, so both comparisons are needed;
// max_code is below 2^24.
CRD_FORCEINLINE I bounded_decode_block(const BoundedFloat& format, const u32* codes, f32* values) noexcept
{
    const I code = load_i32(codes);
    const I bad = cmp_gt(code, I(static_cast<i32>(format.max_code))) | cmp_gt(I::zero(), code);
    const F normal = crd::math::simd::convert_to_float(code) / F(static_cast<f32>(format.max_code));
    const F scaled = normal * F(format.delta);
    const F value = scaled + F(format.min);
    select(crd::math::simd::bitcast_to_float(bad), F(format.min), value).store(values);
    return bad;
}

[[nodiscard]] CRD_FORCEINLINE usize negated_sum(I lanes) noexcept
{
    u32 values[kLanes];
    store_i32(lanes, values);
    usize total = 0U;
    for (const u32 value : values)
    {
        total += static_cast<usize>(0U - value);
    }
    return total;
}

usize bounded_decode(const BoundedFloat& format, const u32* codes, usize count, f32* values) noexcept
{
    // Each lane counts down its refusals; folded every 2^30 values, so no lane can wrap whatever the batch's length.
    I refused = I::zero();
    usize total = 0U;
    usize i = 0U;
    for (; i + kLanes <= count; i += kLanes)
    {
        refused = refused + bounded_decode_block(format, codes + i, values + i);
        if (((i + kLanes) & ((usize{1} << 30U) - 1U)) == 0U)
        {
            total += negated_sum(refused);
            refused = I::zero();
        }
    }
    total += negated_sum(refused);
    if (i < count)
    {
        // The pad's zero codes are never refused.
        u32 last[kLanes] = {};
        f32 last_values[kLanes];
        std::memcpy(last, codes + i, (count - i) * sizeof(u32));
        total += negated_sum(bounded_decode_block(format, last, last_values));
        std::memcpy(values + i, last_values, (count - i) * sizeof(f32));
    }
    return total;
}

CRD_FORCEINLINE void smallest_three_encode_block(const SmallestThree& format, const f32* xyzw, u64* words) noexcept
{
    F x;
    F y;
    F z;
    F w;
    crd::math::simd::load_interleaved4(xyzw, x, y, z, w);

    // The first of the largest, by a strict comparison in x, y, z, w order; a NaN never wins.
    const F is_y = cmp_gt(abs(y), abs(x));
    F largest = select(is_y, abs(y), abs(x));
    F chosen = select(is_y, y, x);
    F index = select(is_y, F::one(), F::zero());
    const F is_z = cmp_gt(abs(z), largest);
    largest = select(is_z, abs(z), largest);
    chosen = select(is_z, z, chosen);
    index = select(is_z, F(2.0F), index);
    const F is_w = cmp_gt(abs(w), largest);
    chosen = select(is_w, w, chosen);
    index = select(is_w, F(3.0F), index);

    // The other three in order: x y z w without the largest, negated when it is not positive.
    const F a = select(cmp_eq(index, F::zero()), y, x);
    const F b = select(cmp_lt(index, F(2.0F)), z, y);
    const F c = select(cmp_eq(index, F(3.0F)), z, w);
    const F keep = cmp_ge(chosen, F::zero());

    const F minimum(kMinimum);
    const F range(kRange);
    const F top(format.scale);
    u32 fields[4][kLanes];
    store_i32(code_of((select(keep, a, -a) - minimum) / range, top), fields[0]);
    store_i32(code_of((select(keep, b, -b) - minimum) / range, top), fields[1]);
    store_i32(code_of((select(keep, c, -c) - minimum) / range, top), fields[2]);
    store_i32(crd::math::simd::convert_truncate(index), fields[3]);
    const u32 bits = format.bits;
    for (usize lane = 0U; lane < kLanes; ++lane)
    {
        words[lane] = (u64{fields[3][lane]} << (3U * bits)) | (u64{fields[0][lane]} << (2U * bits)) |
                      (u64{fields[1][lane]} << bits) | u64{fields[2][lane]};
    }
}

void smallest_three_encode(const SmallestThree& format, const f32* xyzw, usize count, u64* words) noexcept
{
    usize i = 0U;
    for (; i + kLanes <= count; i += kLanes)
    {
        smallest_three_encode_block(format, xyzw + (4U * i), words + i);
    }
    if (i < count)
    {
        f32 last[4U * kLanes] = {};
        u64 last_words[kLanes];
        std::memcpy(last, xyzw + (4U * i), (count - i) * 4U * sizeof(f32));
        smallest_three_encode_block(format, last, last_words);
        std::memcpy(words + i, last_words, (count - i) * sizeof(u64));
    }
}

// Decodes a block of words and writes its first `used` lanes (the rest pad a tail); the number of those refused.
CRD_FORCEINLINE usize smallest_three_decode_block(const SmallestThree& format, const u64* words, f32* xyzw,
                                                  usize used) noexcept
{
    const u32 bits = format.bits;
    const u64 mask = (u64{1} << bits) - 1U;
    const u32 word_bits = 2U + (3U * bits);
    u32 fields[4][kLanes];
    u32 stray[kLanes];
    for (usize lane = 0U; lane < kLanes; ++lane)
    {
        const u64 word = words[lane];
        fields[0][lane] = static_cast<u32>((word >> (2U * bits)) & mask);
        fields[1][lane] = static_cast<u32>((word >> bits) & mask);
        fields[2][lane] = static_cast<u32>(word & mask);
        fields[3][lane] = static_cast<u32>((word >> (3U * bits)) & 3U);
        stray[lane] = ((word >> word_bits) != 0U) ? 1U : 0U;
    }

    const F inverse(format.inverse_scale);
    const F range(kRange);
    const F minimum(kMinimum);
    F a = ((crd::math::simd::convert_to_float(load_i32(fields[0])) * inverse) * range) + minimum;
    F b = ((crd::math::simd::convert_to_float(load_i32(fields[1])) * inverse) * range) + minimum;
    F c = ((crd::math::simd::convert_to_float(load_i32(fields[2])) * inverse) * range) + minimum;

    const F a2 = a * a;
    const F b2 = b * b;
    const F c2 = c * c;
    const F remainder = ((F::one() - a2) - b2) - c2;
    const F zero = F::zero();
    const F over = cmp_lt(remainder, zero);

    // Past length one, which only a hostile word or a width of two or three bits reaches, the largest is zero and the
    // other three are cut back to unit length; dividing by one leaves the other lanes exact.
    u32 over_lanes[kLanes];
    store_i32(crd::math::simd::bitcast_to_int(over), over_lanes);
    u32 any_over = 0U;
    for (const u32 lane : over_lanes)
    {
        any_over |= lane;
    }
    if (any_over != 0U)
    {
        const F length = sqrt(select(over, (a2 + b2) + c2, F::one()));
        a = a / length;
        b = b / length;
        c = c / length;
    }
    const F big = sqrt(select(over, zero, remainder));

    const F index = crd::math::simd::convert_to_float(load_i32(fields[3]));
    const F first = cmp_eq(index, zero);
    const F last = cmp_eq(index, F(3.0F));
    f32 out[4][kLanes];
    select(first, big, a).store(out[0]);
    select(first, a, select(cmp_eq(index, F::one()), big, b)).store(out[1]);
    select(last, c, select(cmp_eq(index, F(2.0F)), big, b)).store(out[2]);
    select(last, big, c).store(out[3]);

    u32 refused[kLanes];
    store_i32(crd::math::simd::bitcast_to_int(cmp_lt(remainder, F(format.smallest_remainder))), refused);
    usize total = 0U;
    for (usize lane = 0U; lane < used; ++lane)
    {
        xyzw[(4U * lane) + 0U] = out[0][lane];
        xyzw[(4U * lane) + 1U] = out[1][lane];
        xyzw[(4U * lane) + 2U] = out[2][lane];
        xyzw[(4U * lane) + 3U] = out[3][lane];
        total += ((refused[lane] | stray[lane]) != 0U) ? 1U : 0U;
    }
    return total;
}

usize smallest_three_decode(const SmallestThree& format, const u64* words, usize count, f32* xyzw) noexcept
{
    usize total = 0U;
    usize i = 0U;
    for (; i + kLanes <= count; i += kLanes)
    {
        total += smallest_three_decode_block(format, words + i, xyzw + (4U * i), kLanes);
    }
    if (i < count)
    {
        u64 last[kLanes] = {};
        std::memcpy(last, words + i, (count - i) * sizeof(u64));
        total += smallest_three_decode_block(format, last, xyzw + (4U * i), count - i);
    }
    return total;
}

} // namespace
} // namespace crd::hesap::net::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::net
{

const QuantizeKernels CRD_KERNEL_TABLE(kQuantizeKernels) = {
    &CRD_MATH_LEVEL_NS::bounded_encode, &CRD_MATH_LEVEL_NS::bounded_decode, &CRD_MATH_LEVEL_NS::smallest_three_encode,
    &CRD_MATH_LEVEL_NS::smallest_three_decode};

} // namespace crd::hesap::net
