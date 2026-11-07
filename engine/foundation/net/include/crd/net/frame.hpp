#pragma once

// crd-net -- how generated messages travel (NET.3; contract: docs/design/network-foundation.md#frames): the route a
// carrier names once, and the header each frame puts before its body. Every number is the wire formats' 7-bit varuint,
// shortest form only, so one frame has one encoding. Nothing here allocates or keeps state; admission decides what a
// header it reads may cost.
//
//   stream      route (form Whole), then [message][length][object?][body] ...
//   datagram    route (form Whole):    [message][object?][body to the end]
//               route (form Listed):   [message][length][object?][body], at least two of them, to the end
//               route (form Fragment): [message][total][object?][chunk][sequence][index][chunk bytes to the end]
//
// The object is there only when the message has an operation bit (a transport message authorizes nothing). The length
// comes before it so that a reader can step over a message a newer writer added, whose description it does not have.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::net
{
namespace cont = crd::containers;

enum class RouteForm : u8
{
    Whole,    // a stream's preamble, or a datagram holding one message
    Listed,   // a datagram holding several messages
    Fragment, // a datagram holding one piece of a message
};

// The channel, in the session's flat table of its schemas' channels, and how the carrier holds its frames.
struct Route
{
    u32 channel = 0U;
    RouteForm form = RouteForm::Whole;
};

// A message's header. `length` is its body's size, or for a fragment the whole message's size.
struct FrameHeader
{
    u32 message = 0U;
    u32 length = 0U;
    u64 object = 0U;
    u32 chunk = 0U;    // a fragment's size, the last one's being what is left
    u64 sequence = 0U; // which message of its channel the fragment belongs to
    u32 index = 0U;    // which piece of it
};

enum class HeaderForm : u8
{
    Sized,    // message, length, object
    ToEnd,    // message, object: the body runs to the end of the datagram
    Fragment, // message, total, object, chunk, sequence, index
};

enum class FrameRead : u8
{
    Ok,
    Short,     // the bytes end inside it
    Malformed, // an overlong or out-of-range number, or a route that names both forms
};

inline constexpr usize kMaxRouteBytes = 5U;
// The most a stream frame's header takes: a 32-bit message and length, a 64-bit object.
inline constexpr usize kMaxFrameHeaderBytes = 20U;
// The most a fragment's header takes, with its route.
inline constexpr usize kMaxFragmentHeaderBytes = kMaxRouteBytes + 5U + 5U + 10U + 5U + 10U + 5U;
// A message takes at most this many fragments, which keeps a reassembly's state a fixed size.
inline constexpr u32 kMaxFragments = 256U;
// And a fragment is no smaller than this, except the last, so a message cannot ask for many tiny pieces.
inline constexpr u32 kMinFragmentChunk = 64U;

// The route's value is the channel times four, plus one for Listed or two for Fragment.
[[nodiscard]] FrameRead read_route(cont::ConstSpan<u8> bytes, Route& route, usize& consumed) noexcept;

// Writes the route and returns its size; zero when `out` is too small.
[[nodiscard]] usize write_route(const Route& route, cont::Span<u8> out) noexcept;

// One number of a header: the message, which says what the rest looks like, or a Sized or Fragment header's length,
// what a reader needs to step over a message it has no description of.
[[nodiscard]] FrameRead read_frame_number(cont::ConstSpan<u8> bytes, u32& number, usize& consumed) noexcept;

// The rest of a header, after the message and any length. Fills object, chunk, sequence and index as `form` has them.
[[nodiscard]] FrameRead read_frame_tail(cont::ConstSpan<u8> bytes, HeaderForm form, bool has_object,
                                        FrameHeader& header, usize& consumed) noexcept;

// Writes a header and returns its size; zero when `out` is too small.
[[nodiscard]] usize write_frame_header(HeaderForm form, bool has_object, const FrameHeader& header,
                                       cont::Span<u8> out) noexcept;

// Writes one datagram holding one fragment of the message `body`: its route, its header (`header.length` must be the
// body's size) and the piece `header.index` names. Returns the datagram's size; zero when `out` is too small or the
// shape is one fragment_shape_holds() refuses.
[[nodiscard]] usize write_fragment(u32 channel, bool has_object, const FrameHeader& header, cont::ConstSpan<u8> body,
                                   cont::Span<u8> out) noexcept;

// How many fragments a message of `total` bytes takes at `chunk` bytes each; `chunk` is not zero.
[[nodiscard]] constexpr u32 fragment_count(u32 total, u32 chunk) noexcept
{
    return (total / chunk) + (((total % chunk) != 0U) ? 1U : 0U);
}

// Whether a fragment's shape is one an encoder writes: more than one piece, a chunk at or above the smallest, no more
// pieces than kMaxFragments, its index inside them and its size the chunk, or what is left for the last.
[[nodiscard]] constexpr bool fragment_shape_holds(u32 total, u32 chunk, u32 index, usize size) noexcept
{
    if (chunk < kMinFragmentChunk || total <= chunk)
    {
        return false;
    }
    const u32 pieces = fragment_count(total, chunk);
    if (pieces > kMaxFragments || index >= pieces)
    {
        return false;
    }
    const u32 last_size = total - ((pieces - 1U) * chunk);
    return size == ((index + 1U == pieces) ? last_size : chunk);
}

} // namespace crd::net
