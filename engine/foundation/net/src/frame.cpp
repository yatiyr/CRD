// crd-net -- the route and frame headers (NET.3; docs/design/network-foundation.md#frames).

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
} // namespace

FrameRead read_route(cont::ConstSpan<u8> bytes, Route& route, usize& consumed) noexcept
{
    cont::BitReader r(bytes);
    u64 value = 0U;
    const FrameRead read = read_error(wire::wire_take_varuint(r, kGroup, (kMaxU32 << kFormBits) | kFormMask, value));
    if (read != FrameRead::Ok)
    {
        return read;
    }
    const u64 form = value & kFormMask;
    if (form == kFormMask)
    {
        return FrameRead::Malformed;
    }
    route.channel = static_cast<u32>(value >> kFormBits);
    route.form = static_cast<RouteForm>(form);
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

FrameRead read_frame_tail(cont::ConstSpan<u8> bytes, HeaderForm form, bool has_object, FrameHeader& header,
                          usize& consumed) noexcept
{
    cont::BitReader r(bytes);
    FrameRead read = FrameRead::Ok;
    if (has_object)
    {
        read = take_u64(r, header.object);
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

usize write_frame_header(HeaderForm form, bool has_object, const FrameHeader& header, cont::Span<u8> out) noexcept
{
    cont::BitWriter w(out);
    wire::wire_put_varuint(w, header.message, kGroup);
    if (form != HeaderForm::ToEnd)
    {
        wire::wire_put_varuint(w, header.length, kGroup);
    }
    if (has_object)
    {
        wire::wire_put_varuint(w, header.object, kGroup);
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

usize write_fragment(u32 channel, bool has_object, const FrameHeader& header, cont::ConstSpan<u8> body,
                     cont::Span<u8> out) noexcept
{
    if (header.length != body.size() || header.chunk == 0U)
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
    const usize head = write_frame_header(HeaderForm::Fragment, has_object, header, out.subspan(route));
    if (head == 0U || out.size() - route - head < piece)
    {
        return 0U;
    }
    std::memcpy(out.data() + route + head, body.data() + offset, piece);
    return route + head + piece;
}

} // namespace crd::net
