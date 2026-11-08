#pragma once

// crd-net -- QUIC headers as admission stage S1 reads them, and the two packets a server sends before it holds any
// state for a peer: Version Negotiation and Retry (4.2a; contract: docs/design/network-foundation.md#ingress).
// Sources: RFC 8999 (the version-independent invariants), RFC 9000 sections 16 and 17, RFC 9369 section 3.2 (version
// 2's packet types) and RFC 9001 section 5.4.2 (the header protection sample).
//
//   reading   only what routing and admission need: the form, the version, both connection IDs, a long header's type,
//             an Initial's token and the extent of the datagram's first packet. Nothing is unprotected here; the
//             views point into the datagram. A long header of a version this module does not read is checked against
//             the invariants alone (connection IDs up to 255 bytes), so a server can still answer it.
//   writing   into the caller's span, nothing allocated; a span too small is refused, never truncated.

#include <crd/containers/span.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>

namespace crd::net
{
namespace cont = crd::containers;

inline constexpr u32 kQuicVersionNegotiation = 0U; // the version field of a Version Negotiation packet
inline constexpr usize kQuicMinInitialDatagram = 1200U; // RFC 9000 section 14.1: smaller client Initials are dropped
inline constexpr usize kQuicMaxInvariantConnectionIdBytes = 255U; // RFC 8999 section 5.1
// A protected packet holds at least four bytes from its packet number on and then the 16-byte header protection
// sample (RFC 9001 section 5.4.2); a shorter one cannot be unprotected.
inline constexpr usize kQuicMinProtectedBytes = 4U + crypto::kQuicSampleBytes;
inline constexpr u64 kQuicMaxVarint = (u64{1} << 62U) - 1U;
// A stateless reset's least size (RFC 9000 section 10.3): 5 bytes of a short header and the token.
inline constexpr usize kQuicMinResetBytes = 5U + crypto::kQuicResetTokenBytes;

// ---- Variable-length integers (RFC 9000 section 16) ----

// 1, 2, 4 or 8: the shortest encoding of `value`; 0 above kQuicMaxVarint.
[[nodiscard]] usize quic_varint_bytes(u64 value) noexcept;
// Writes the shortest encoding of `value`, which is at most kQuicMaxVarint (a larger one stops the process: the layout
// that sized `out` was wrong).
void put_quic_varint(cont::WireWriter& out, u64 value) noexcept;
// Takes one integer in any of its encodings (the transport accepts the longer ones, section 16); false, consuming
// nothing, for a short input.
[[nodiscard]] bool take_quic_varint(cont::WireReader& in, u64& value) noexcept;

// ---- Versions (RFC 9000 section 15) ----

// The reserved versions 0x?a?a?a?a, which a server lists in Version Negotiation so that clients never ossify on its
// list and which no endpoint ever selects.
[[nodiscard]] constexpr bool quic_version_reserved(u32 version) noexcept
{
    return (version & 0x0F0F0F0FU) == 0x0A0A0A0AU;
}

// A reserved version drawn from `entropy`'s high nibbles.
[[nodiscard]] constexpr u32 quic_grease_version(u32 entropy) noexcept
{
    return (entropy & 0xF0F0F0F0U) | 0x0A0A0A0AU;
}

// ---- Reading ----

enum class QuicPacketType : u8
{
    Initial,
    ZeroRtt,
    Handshake,
    Retry,
    VersionNegotiation,
    OneRtt,         // a short header
    OtherVersion,   // a long header of a version this module does not read: its invariants only
};

enum class QuicHeaderError : u8
{
    None,
    Empty,
    Truncated,           // the datagram ends inside the header
    ConnectionIdTooLong, // more than 20 bytes in a version 1 or 2 header (RFC 9000 section 17.2), dropped
    LengthPastDatagram,  // a long header's Length runs past the datagram
    TooShortToProtect,   // fewer than kQuicMinProtectedBytes from the packet number on
};

// The first packet of a datagram. Views point into the datagram.
struct QuicHeader
{
    QuicPacketType type = QuicPacketType::OneRtt;
    u8 first = 0U;    // the first byte, still under header protection
    u32 version = 0U; // a long header's
    cont::ConstSpan<u8> dcid;
    cont::ConstSpan<u8> scid;  // a long header's
    cont::ConstSpan<u8> token; // an Initial's
    usize number_offset = 0U;  // where the protected packet number begins (Initial, 0-RTT, Handshake, 1-RTT)
    usize packet_bytes = 0U;   // the packet's extent: through its Length for Initial, 0-RTT and Handshake, else the rest
};

// The first byte's long packet type bits (4 and 5, shifted down) of `type` (Initial, ZeroRtt, Handshake or Retry) in
// `version`: version 2 rotates version 1's codes by one (RFC 9369 section 3.2).
[[nodiscard]] u8 quic_long_type_bits(crypto::QuicVersion version, QuicPacketType type) noexcept;

// Reads the first packet's header. A short header's Destination Connection ID is `short_dcid_bytes` long: the length
// of the connection IDs this endpoint issues, which only it knows.
[[nodiscard]] QuicHeaderError parse_quic_header(cont::ConstSpan<u8> datagram, usize short_dcid_bytes,
                                                QuicHeader& out) noexcept;

// ---- Writing ----

// The largest Version Negotiation packet write_version_negotiation() builds: both connection IDs at the invariants'
// maximum and kQuicMaxNegotiationVersions versions with the reserved one.
inline constexpr usize kQuicMaxNegotiationVersions = 8U;
inline constexpr usize kQuicMaxVersionNegotiationBytes =
    1U + 4U + 2U * (1U + kQuicMaxInvariantConnectionIdBytes) + 4U * (kQuicMaxNegotiationVersions + 1U);

// Version Negotiation answering a long header whose connection IDs were `dcid` and `scid` (RFC 9000 section 17.2.1;
// they are swapped in the answer). It lists `versions` (1 to kQuicMaxNegotiationVersions, none reserved) and one
// reserved version, quic_grease_version(entropy's bits 8 to 39), at position (entropy >> 40) modulo the count; its
// unused first-byte bits are 0x40 (the fixed bit, for protocol multiplexing, RFC 7983) and entropy's low six bits.
// Returns the bytes written; zero, writing nothing, when `out` is too small or an argument is out of range.
[[nodiscard]] usize write_version_negotiation(cont::ConstSpan<u8> dcid, cont::ConstSpan<u8> scid,
                                              cont::ConstSpan<u32> versions, u64 entropy,
                                              cont::Span<u8> out) noexcept;

// The bytes of a Retry with these connection IDs and token: what write_retry() writes.
[[nodiscard]] constexpr usize quic_retry_bytes(usize dcid_bytes, usize scid_bytes, usize token_bytes) noexcept
{
    return 1U + 4U + 1U + dcid_bytes + 1U + scid_bytes + token_bytes + crypto::kQuicTagBytes;
}

// A Retry (RFC 9000 section 17.2.5) to the client whose Initial had Source Connection ID `client_scid` (the Retry's
// destination) and Destination Connection ID `original_dcid` (which its integrity tag binds), from `retry_scid` and
// carrying `token`; `entropy`'s low four bits fill the unused bits. None with `written` set; BadLength when `out` is
// too small, a connection ID is longer than 20 bytes, `retry_scid` is empty or the packet would pass
// kQuicMaxRetryBytes; Overlap when `retry_scid` equals `original_dcid` (section 17.2.5.1); Unsupported for a version
// crypto::quic_version_supported() refuses.
[[nodiscard]] crypto::CryptoError write_retry(crypto::QuicVersion version, cont::ConstSpan<u8> client_scid,
                                              cont::ConstSpan<u8> retry_scid, cont::ConstSpan<u8> original_dcid,
                                              cont::ConstSpan<u8> token, u8 entropy, cont::Span<u8> out,
                                              usize& written) noexcept;

} // namespace crd::net
