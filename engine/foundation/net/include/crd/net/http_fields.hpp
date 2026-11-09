#pragma once

// crd-net -- the field coding HPACK and QPACK share (4.2a; contract: docs/design/network-foundation.md#qpack). Sources:
// RFC 7541 sections 5.1 and 5.2 and appendix B, RFC 9204 sections 1.2 and 4.1.
//
//   integers  prefix integers up to 2^62 - 1 in at most ten continuation bytes; a longer or larger one is invalid.
//   strings   Huffman-coded only when that is strictly shorter. The decoder holds a code to section 5.2: at most seven
//             bits of padding, all ones, and no EOS.
//   memory    nothing allocates; hostile input costs time linear in its length.
//   threads   no state.

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>

namespace crd::net
{
namespace cont = crd::containers;

// One field line. A decoder's views live in the storage given to it (or in a static table).
struct HttpField
{
    cont::ConstSpan<u8> name;
    cont::ConstSpan<u8> value;
    bool never_index = false; // no intermediary or encoder may index it (RFC 9204 section 7.1.3)
};

inline constexpr usize kHttpFieldOverhead = 32U; // the bytes an entry or a field line counts for beyond its text
inline constexpr u64 kHttpMaxInteger = (u64{1} << 62U) - 1U;
inline constexpr usize kHttpMaxIntegerBytes = 11U; // a first byte and ten continuation bytes

enum class HttpDecode : u8
{
    Ok,
    NeedMore, // the input ended first
    Invalid,
};

// The bytes `value` takes with a prefix of `prefix_bits` (1 to 8).
[[nodiscard]] usize http_int_bytes(u64 value, u8 prefix_bits) noexcept;
// Writes `value` in the low `prefix_bits` of a byte holding `flags` in its high bits.
void put_http_int(cont::WireWriter& out, u8 flags, u8 prefix_bits, u64 value) noexcept;
// Reads the rest of an integer whose first byte, `first`, the caller has taken.
[[nodiscard]] HttpDecode take_http_int(cont::WireReader& in, u8 first, u8 prefix_bits, u64& value) noexcept;

// The bytes `text` takes as a Huffman code.
[[nodiscard]] usize http_huffman_bytes(cont::ConstSpan<u8> text) noexcept;
// Writes the code of `text`, padded with ones; `out` holds at least http_huffman_bytes(text).
void http_huffman_encode(cont::ConstSpan<u8> text, cont::Span<u8> out) noexcept;
// Decodes `coded`. `decoded` is the length of the text however small `out` is, and `out` holds its first bytes, so an
// empty `out` measures and validates. False for a code RFC 7541 section 5.2 refuses.
[[nodiscard]] bool http_huffman_decode(cont::ConstSpan<u8> coded, cont::Span<u8> out, usize& decoded) noexcept;

// The bytes a string takes: its length with a prefix of `prefix_bits` (the Huffman flag is the next bit up) and the
// shorter of its two forms.
[[nodiscard]] usize http_string_bytes(cont::ConstSpan<u8> text, u8 prefix_bits) noexcept;
void put_http_string(cont::WireWriter& out, u8 flags, u8 prefix_bits, cont::ConstSpan<u8> text) noexcept;
// Reads a string whose first byte, `first`, the caller has taken: `coded` views its bytes in the input and `huffman`
// tells their form. Invalid for one longer than `max_coded`, as soon as its length is read.
[[nodiscard]] HttpDecode take_http_string(cont::WireReader& in, u8 first, u8 prefix_bits, usize max_coded,
                                          cont::ConstSpan<u8>& coded, bool& huffman) noexcept;

} // namespace crd::net
