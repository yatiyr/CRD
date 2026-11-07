#pragma once

// crd-net -- how generated messages travel (NET.3; contract: docs/design/network-foundation.md#frames): the route a
// carrier names once, and the header each frame puts before its body. Every number is the wire formats' 7-bit varuint,
// shortest form only, so one frame has one encoding. Nothing here allocates or keeps state; admission decides what a
// header it reads may cost.
//
//   stream      route (form Whole), then [message][length][object?][hash?][body] ...
//   datagram    route (form Whole):    [message][object?][hash?][body to the end]
//               route (form Listed):   [message][length][object?][hash?][body], at least two of them, to the end
//               route (form Fragment): [message][total][object?][hash?][chunk][sequence][index][chunk bytes to the end]
//               route (form Padded):   [count][count zero bytes], then the datagram's own route and frames
//
// The object is there only when the message has an operation bit (a transport message authorizes nothing), the hash
// unless the message's channel or its delta body makes it redundant. The length counts everything after itself, so that
// a reader can step over a message a newer writer added, whose description it does not have. Message zero is no
// message: on a channel that declares padding it is a stream's padding, [0][length][length zero bytes].

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
    Padded,   // a datagram's padding, before its own route
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
    u32 hash = 0U;     // frame_hash() of the body: of the whole message, for a fragment
    u32 chunk = 0U;    // a fragment's size, the last one's being what is left
    u64 sequence = 0U; // which message of its channel the fragment belongs to
    u32 index = 0U;    // which piece of it
};

// What a message's description says its header holds past the message and the length.
struct FrameShape
{
    bool object = false;
    bool hash = false;
};

enum class HeaderForm : u8
{
    Sized,    // message, length, object, hash
    ToEnd,    // message, object, hash: the body runs to the end of the datagram
    Fragment, // message, total, object, hash, chunk, sequence, index
};

enum class FrameRead : u8
{
    Ok,
    Short,     // the bytes end inside it
    Malformed, // an overlong or out-of-range number
};

inline constexpr usize kMaxRouteBytes = 5U;
inline constexpr usize kFrameHashBytes = 4U;
// The most a frame holds between its length and its body: a 64-bit object and the hash.
inline constexpr usize kMaxFrameTailBytes = 10U + kFrameHashBytes;
// The most a stream frame's header takes: a 32-bit message and length, and its tail.
inline constexpr usize kMaxFrameHeaderBytes = 5U + 5U + kMaxFrameTailBytes;
// The most a fragment's header takes, with its route.
inline constexpr usize kMaxFragmentHeaderBytes = kMaxRouteBytes + 5U + 5U + kMaxFrameTailBytes + 5U + 10U + 5U;
// A message takes at most this many fragments, which keeps a reassembly's state a fixed size.
inline constexpr u32 kMaxFragments = 256U;
// And a fragment is no smaller than this, except the last, so a message cannot ask for many tiny pieces.
inline constexpr u32 kMinFragmentChunk = 64U;
// The message number of a stream's padding. A schema's messages start at one.
inline constexpr u32 kPaddingMessage = 0U;

// What a frame's hash holds: the low half of XXH3-64 over the body, seeded with the message and object it travels
// under. It is taken at serialisation and checked after decryption, so it catches what the AEAD tag seals as valid: a
// sender's own memory or pipeline fault (threat T12).
[[nodiscard]] u32 frame_hash(u32 message, u64 object, cont::ConstSpan<u8> body) noexcept;

// The route's value is the channel times four, plus one for Listed, two for Fragment or three for Padded.
[[nodiscard]] FrameRead read_route(cont::ConstSpan<u8> bytes, Route& route, usize& consumed) noexcept;

// Writes the route and returns its size; zero when `out` is too small.
[[nodiscard]] usize write_route(const Route& route, cont::Span<u8> out) noexcept;

// One number of a header: the message, which says what the rest looks like, or a Sized or Fragment header's length,
// what a reader needs to step over a message it has no description of.
[[nodiscard]] FrameRead read_frame_number(cont::ConstSpan<u8> bytes, u32& number, usize& consumed) noexcept;

// The rest of a header, after the message and any length. Fills object, hash, chunk, sequence and index as `form` and
// `shape` have them. A Sized header's body is its length less what this consumed.
[[nodiscard]] FrameRead read_frame_tail(cont::ConstSpan<u8> bytes, HeaderForm form, FrameShape shape,
                                        FrameHeader& header, usize& consumed) noexcept;

// Writes a header and returns its size; zero when `out` is too small. Its size does not depend on `header.hash`.
[[nodiscard]] usize write_frame_header(HeaderForm form, FrameShape shape, const FrameHeader& header,
                                       cont::Span<u8> out) noexcept;

// Writes one datagram holding one fragment of the message `body`: its route, its header (`header.length` must be the
// body's size, `header.hash` its frame_hash()) and the piece `header.index` names. Returns the datagram's size; zero
// when `out` is too small or the shape is one fragment_shape_holds() refuses.
[[nodiscard]] usize write_fragment(u32 channel, FrameShape shape, const FrameHeader& header, cont::ConstSpan<u8> body,
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

// ---- Padding (threat T15; docs/design/network-foundation.md#padding) ----

// The size a carrier holding `size` bytes takes under a channel's `block`: the next multiple of it, at most `room`.
// Zero pads nothing. A path that controls its packets (QUIC's PADDING frames, TLS record padding, the raw UDP path)
// pads a packet to this with the largest block among the channels whose frames it holds, and needs nothing below.
[[nodiscard]] constexpr usize padded_size(usize size, u32 block, usize room) noexcept
{
    if (block == 0U || size % block == 0U)
    {
        return size;
    }
    const usize next = size + block - (size % block);
    return (next < room) ? next : ((size < room) ? room : size);
}

// A path that does not control its packets (a browser's WebTransport, WebSocket and data channels) pads in band.
// Pads the datagram of `used` bytes at the start of `out` towards padded_size() for `out.size()`, moving it behind the
// padding, and returns its new size: `used` when it is already there or the padding's own header does not fit.
[[nodiscard]] usize pad_datagram(u32 channel, u32 block, usize used, cont::Span<u8> out) noexcept;

// The padding frame that brings a stream's `pending` unsent bytes to a multiple of `block`. Returns its size; zero when
// they already are one or `out` is too small, which leaves the stream unpadded and correct.
[[nodiscard]] usize write_stream_padding(u64 pending, u32 block, cont::Span<u8> out) noexcept;

// Whether a padding's declared `count` is one a writer can have written under `block`: none, when it declares none.
[[nodiscard]] constexpr bool padding_count_holds(u64 count, u32 block) noexcept
{
    return count < u64{2} * block;
}

} // namespace crd::net
