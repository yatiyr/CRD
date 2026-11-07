// crd-net -- the route and frame headers (NET.3; docs/design/network-foundation.md#frames).

#include <crd/containers/checksum.hpp>
#include <crd/net/frame.hpp>
#include <crd/net/wire.hpp>

#include <cstring>

namespace crd::net
{
namespace
{
constexpr u32 kGroup = 7U;
constexpr u64 kMaxU32 = 0xFFFFFFFFU;
constexpr u32 kFormBits = 2U;
constexpr u64 kFormMask = 3U;

[[nodiscard]] FrameRead read_error(wire::WireError error) noexcept
{
    if (error == wire::WireError::Ok)
    {
        return FrameRead::Ok;
    }
    return (error == wire::WireError::Truncated) ? FrameRead::Short : FrameRead::Malformed;
}

[[nodiscard]] FrameRead take_u32(cont::BitReader& r, u32& value) noexcept
{
    u64 raw = 0U;
    const FrameRead read = read_error(wire::wire_take_varuint(r, kGroup, kMaxU32, raw));
    value = static_cast<u32>(raw);
    return read;
}

[[nodiscard]] FrameRead take_u64(cont::BitReader& r, u64& value) noexcept
{
    return read_error(wire::wire_take_varuint(r, kGroup, ~u64{0}, value));
}

[[nodiscard]] usize bytes_taken(const cont::BitReader& r) noexcept
{
    return static_cast<usize>(r.bit_position() / 8U);
}

[[nodiscard]] usize number_bytes(u64 value) noexcept
{
    return static_cast<usize>(wire::wire_varuint_bits(value, kGroup) / 8U);
}

// The count of a padding that takes exactly `gap` bytes: `head` bytes of its own, its number and what that counts.
// False when `gap` holds no such number, or one's width jumps past it.
[[nodiscard]] bool padding_count_for(u64 gap, usize head, u64& count) noexcept
{
    for (usize width = 1U; width <= 3U && gap >= head + width; ++width)
    {
        const u64 candidate = gap - head - width;
        if (number_bytes(candidate) == width)
        {
            count = candidate;
            return true;
        }
    }
    return false;
}
} // namespace

u32 frame_hash(u32 message, u64 object, cont::ConstSpan<u8> body) noexcept
{
    return static_cast<u32>(cont::xxh3_64(body, (u64{message} << 32U) ^ object));
}

FrameRead read_route(cont::ConstSpan<u8> bytes, Route& route, usize& consumed) noexcept
{
    cont::BitReader r(bytes);
    u64 value = 0U;
    const FrameRead read = read_error(wire::wire_take_varuint(r, kGroup, (kMaxU32 << kFormBits) | kFormMask, value));
    if (read != FrameRead::Ok)
    {
        return read;
    }
    route.channel = static_cast<u32>(value >> kFormBits);
    route.form = static_cast<RouteForm>(value & kFormMask);
    consumed = bytes_taken(r);
    return FrameRead::Ok;
}

usize write_route(const Route& route, cont::Span<u8> out) noexcept
{
    cont::BitWriter w(out);
    wire::wire_put_varuint(w, (u64{route.channel} << kFormBits) | static_cast<u64>(route.form), kGroup);
    const usize size = w.finish();
    return w.overflowed() ? 0U : size;
}

FrameRead read_frame_number(cont::ConstSpan<u8> bytes, u32& number, usize& consumed) noexcept
{
    cont::BitReader r(bytes);
    const FrameRead read = take_u32(r, number);
    consumed = bytes_taken(r);
    return read;
}

FrameRead read_frame_tail(cont::ConstSpan<u8> bytes, HeaderForm form, FrameShape shape, FrameHeader& header,
                          usize& consumed) noexcept
{
    cont::BitReader r(bytes);
    FrameRead read = FrameRead::Ok;
    if (shape.object)
    {
        read = take_u64(r, header.object);
    }
    if (shape.hash && read == FrameRead::Ok)
    {
        u64 hash = 0U;
        read = r.take(kFrameHashBytes * 8U, hash) ? FrameRead::Ok : FrameRead::Short;
        header.hash = static_cast<u32>(hash);
    }
    if (form == HeaderForm::Fragment)
    {
        read = (read == FrameRead::Ok) ? take_u32(r, header.chunk) : read;
        read = (read == FrameRead::Ok) ? take_u64(r, header.sequence) : read;
        read = (read == FrameRead::Ok) ? take_u32(r, header.index) : read;
    }
    consumed = bytes_taken(r);
    return read;
}

usize write_frame_header(HeaderForm form, FrameShape shape, const FrameHeader& header, cont::Span<u8> out) noexcept
{
    cont::BitWriter w(out);
    wire::wire_put_varuint(w, header.message, kGroup);
    if (form != HeaderForm::ToEnd)
    {
        u64 length = header.length;
        if (form == HeaderForm::Sized)
        {
            length += (shape.object ? number_bytes(header.object) : 0U) + (shape.hash ? kFrameHashBytes : 0U);
        }
        if (length > kMaxU32)
        {
            return 0U;
        }
        wire::wire_put_varuint(w, length, kGroup);
    }
    if (shape.object)
    {
        wire::wire_put_varuint(w, header.object, kGroup);
    }
    if (shape.hash)
    {
        w.put(header.hash, kFrameHashBytes * 8U);
    }
    if (form == HeaderForm::Fragment)
    {
        wire::wire_put_varuint(w, header.chunk, kGroup);
        wire::wire_put_varuint(w, header.sequence, kGroup);
        wire::wire_put_varuint(w, header.index, kGroup);
    }
    const usize size = w.finish();
    return w.overflowed() ? 0U : size;
}

usize write_fragment(u32 channel, FrameShape shape, const FrameHeader& header, cont::ConstSpan<u8> body,
                     cont::Span<u8> out) noexcept
{
    if (header.length != body.size())
    {
        return 0U;
    }
    const u64 offset = u64{header.index} * header.chunk;
    const usize left = (offset < body.size()) ? (body.size() - static_cast<usize>(offset)) : 0U;
    const usize piece = (left < header.chunk) ? left : header.chunk;
    if (!fragment_shape_holds(header.length, header.chunk, header.index, piece))
    {
        return 0U;
    }
    const usize route = write_route(Route{channel, RouteForm::Fragment}, out);
    if (route == 0U)
    {
        return 0U;
    }
    const usize head = write_frame_header(HeaderForm::Fragment, shape, header, out.subspan(route));
    if (head == 0U || out.size() - route - head < piece)
    {
        return 0U;
    }
    std::memcpy(out.data() + route + head, body.data() + offset, piece);
    return route + head + piece;
}

usize pad_datagram(u32 channel, u32 block, usize used, cont::Span<u8> out) noexcept
{
    u8 route[kMaxRouteBytes];
    const usize head = write_route(Route{channel, RouteForm::Padded}, route);
    usize target = padded_size(used, block, out.size());
    u64 count = 0U;
    // The padding lands on a multiple, or on the room at the last; its own header may not fit the first gap.
    while (target > used && !padding_count_for(target - used, head, count))
    {
        target = (target < out.size()) ? padded_size(target + 1U, block, out.size()) : used;
    }
    if (target <= used)
    {
        return used;
    }
    std::memmove(out.data() + (target - used), out.data(), used);
    std::memcpy(out.data(), route, head);
    cont::BitWriter w(out.subspan(head));
    wire::wire_put_varuint(w, count, kGroup);
    std::memset(out.data() + head + w.finish(), 0, static_cast<usize>(count));
    return target;
}

usize write_stream_padding(u64 pending, u32 block, cont::Span<u8> out) noexcept
{
    if (block == 0U || pending % block == 0U)
    {
        return 0U;
    }
    u64 gap = block - (pending % block);
    u64 count = 0U;
    // At most three rounds: a gap too small for the frame's own header, or one a number's width jumps past.
    while (!padding_count_for(gap, 1U, count))
    {
        gap += block;
    }
    if (gap > out.size())
    {
        return 0U;
    }
    out[0] = static_cast<u8>(kPaddingMessage);
    cont::BitWriter w(out.subspan(1U));
    wire::wire_put_varuint(w, count, kGroup);
    std::memset(out.data() + 1U + w.finish(), 0, static_cast<usize>(count));
    return static_cast<usize>(gap);
}

} // namespace crd::net
