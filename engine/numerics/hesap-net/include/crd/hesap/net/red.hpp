// Redundant audio data (RFC 2198; HESAP-NET, capability numerics.erasure-codes, owner crd-hesap-net).
//
// A RED payload (the bytes after an RTP header whose payload type is the session's RED type) carries earlier media
// as redundant blocks and the packet's own media, the primary, last. Each redundant block has a 4-byte header (§3):
// F = 1, its payload type (7 bits), a timestamp offset (14 bits) that is subtracted from the RTP header's timestamp,
// and its length in bytes (10 bits). The primary's header is one byte, F = 0 and its payload type, and its length is
// what the payload leaves. The data blocks follow the last header in the headers' order, with no padding. The marker
// bit and the CSRC list belong to the primary (§4): a block recovered from redundancy has neither, and which payload
// types a block may carry is the session's (§5's fmtp list), checked by the consumer (crd-net).
//
// The RFC bounds a packet's headers only by its length; red_parse() takes as many as the caller's block span holds.
// Its sender rules are kept by red_encode(): an offset is unsigned, so redundancy is always older than the primary,
// and §3's "different time intervals" is enforced (no offset 0, no offset twice). The parser accepts what the wire
// format can express. Timestamps wrap modulo 2^32. Nothing allocates, and a refusal writes nothing.

#pragma once

#include <crd/core/types.hpp>

#include <span>

namespace crd::hesap::net
{

// A refused request reports its first fault in this order. Parsing: each header in turn, its bytes (Truncated) and
// then room for it (NoSpace); then the redundant lengths against what the headers leave (Truncated). Encoding: each
// redundant block in turn, its fields (OutOfRange) and its interval (Repeated); then the output's room (NoSpace).
enum class RedStatus : crd::u8
{
    Ok,
    Truncated,  // the bytes end inside a header, or hold less than the redundant blocks' lengths
    NoSpace,    // more blocks than the block span holds, or an output too small for the payload
    OutOfRange, // a payload type above 127, an offset above 16383 or a redundant block longer than 1023 bytes
    Repeated,   // a redundant block at offset 0 (the primary's interval) or at another block's offset
};

struct RedBlock
{
    static constexpr crd::u32 kMaxPayloadType = 0x7FU;
    static constexpr crd::u32 kMaxOffset = 0x3FFFU;
    static constexpr crd::u32 kMaxLength = 0x3FFU;

    crd::u8 payload_type = 0U;
    crd::u16 offset = 0U;    // the primary's timestamp minus this block's; 0 for the primary
    crd::u32 timestamp = 0U; // red_parse(): the block's media timestamp; red_encode() ignores it
    std::span<const crd::u8> data;
};

// Reads the RED payload `bytes` of an RTP packet with timestamp `timestamp` into `blocks`, the redundant blocks in
// their order and the primary last; `count` is the number of blocks, set only on Ok. The data spans view `bytes`.
[[nodiscard]] RedStatus red_parse(std::span<const crd::u8> bytes, crd::u32 timestamp, std::span<RedBlock> blocks,
                                  crd::u32& count) noexcept;

// The bytes red_encode() writes for these blocks: 4 per redundant header, 1 for the primary's, and every block's data.
[[nodiscard]] crd::usize red_bytes(std::span<const RedBlock> redundant, crd::usize primary_bytes) noexcept;

// Writes the RED payload carrying `redundant` (in that order) and then the primary (`primary_type`, `primary`).
[[nodiscard]] RedStatus red_encode(std::span<const RedBlock> redundant, crd::u8 primary_type,
                                   std::span<const crd::u8> primary, std::span<crd::u8> out,
                                   crd::usize& written) noexcept;

} // namespace crd::hesap::net
