#pragma once

// crd-net -- the quantised fields of a generated wire format (NET.3): hesap-net's formats on one value, and all a schema
// with `bounded` or `quaternion` fields adds to crd/net/wire.hpp. The target that holds such a schema links
// crd-net-quantized.

#include <crd/hesap/net/quantize.hpp>
#include <crd/net/wire.hpp>

namespace crd::net::wire
{

// The format a schema declares; the generator has checked its bounds.
[[nodiscard]] constexpr hesap::net::BoundedFloat wire_bounded_format(f32 min, f32 max, u32 max_code) noexcept
{
    hesap::net::BoundedFloat format{};
    const bool               valid = hesap::net::bounded_float_with_max_code(min, max, max_code, format);
    return valid ? format : hesap::net::BoundedFloat{};
}

[[nodiscard]] constexpr hesap::net::SmallestThree wire_smallest_three_format(u32 bits) noexcept
{
    hesap::net::SmallestThree format{};
    const bool                valid = hesap::net::smallest_three(bits, format);
    return valid ? format : hesap::net::SmallestThree{};
}

[[nodiscard]] CRD_FORCEINLINE WireError wire_take_bounded(cont::BitReader& r, const hesap::net::BoundedFloat& format,
                                                          f32& value) noexcept
{
    u64 code = 0U;
    if (!r.take(format.bits, code))
    {
        return WireError::Truncated;
    }
    return wire_error(hesap::net::bounded_decode(format, static_cast<u32>(code), value), WireError::OutOfRange);
}

// A unit quaternion, as hesap-net's smallest-three word.
struct WireQuaternion
{
    f32 xyzw[4] = {0.0F, 0.0F, 0.0F, 1.0F};
};

[[nodiscard]] constexpr bool wire_same(const WireQuaternion& a, const WireQuaternion& b) noexcept
{
    return wire_same(a.xyzw[0], b.xyzw[0]) && wire_same(a.xyzw[1], b.xyzw[1]) && wire_same(a.xyzw[2], b.xyzw[2]) &&
           wire_same(a.xyzw[3], b.xyzw[3]);
}

[[nodiscard]] inline bool wire_quaternion_finite(const WireQuaternion& q) noexcept
{
    return wire_finite(q.xyzw[0]) && wire_finite(q.xyzw[1]) && wire_finite(q.xyzw[2]) && wire_finite(q.xyzw[3]);
}

CRD_FORCEINLINE void wire_put_quaternion(cont::BitWriter& w, const hesap::net::SmallestThree& format,
                                         const WireQuaternion& value) noexcept
{
    w.put(hesap::net::smallest_three_encode_one(format, value.xyzw), hesap::net::smallest_three_word_bits(format));
}

[[nodiscard]] CRD_FORCEINLINE WireError wire_take_quaternion(cont::BitReader& r,
                                                             const hesap::net::SmallestThree& format,
                                                             WireQuaternion& value) noexcept
{
    u64 word = 0U;
    if (!r.take(hesap::net::smallest_three_word_bits(format), word))
    {
        return WireError::Truncated;
    }
    return wire_error(hesap::net::smallest_three_decode_one(format, word, value.xyzw), WireError::BadQuaternion);
}

} // namespace crd::net::wire
