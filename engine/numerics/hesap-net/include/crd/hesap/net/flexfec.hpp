// FlexFEC parity (RFC 8627; HESAP-NET, capability numerics.erasure-codes, owner crd-hesap-net).
//
// A repair packet protects a set of RTP source packets of one or more streams. Its FEC header and repair payload are
// the XOR of the sources' bit strings (§6.2): the first 16 bits of the RTP header, the packet length minus 12 as a
// big-endian u16, the timestamp, then every octet after the fixed 12-byte header, the shorter strings padded with
// zeros. The header names the protected packets per stream (one block per CSRC of the repair packet's RTP header):
// a flexible mask of 15, 46 or 110 bits after a sequence number base (R=0, F=0), or L columns and D rows (R=0,
// F=1): D <= 1 protects the row SN base .. SN base + L - 1, D > 1 the column SN base, SN base + L, ..
// SN base + (D - 1)·L. R=1 is a retransmission, the source packet itself. A lost packet is the XOR of its set's
// repair packet and every other packet of the set (§6.3); recover_window() repeats that until nothing changes, which
// reaches §6.3.4's iterative row-and-column result for any mix of masks, rows and columns, since recovering one packet
// never stops another.
//
// The values here are bytes after the repair packet's own RTP header (and its CSRC list): the consumer (crd-net)
// writes and strips that header, and passes its CSRC list as the protected SSRCs. §6.3.2 lists the 80-bit header
// string as "the first 64 bits" of the RTP header and the length, which would XOR the sequence number into the
// length recovery field; §6.2 and §4.2.2's layout skip the sequence number, and both directions here follow §6.2.
// Sequence numbers wrap modulo 2^16. The packets are plain byte spans; nothing allocates, and a refusal writes
// nothing.

#pragma once

#include <crd/core/types.hpp>

#include <span>

namespace crd::hesap::net
{

// A refused request reports its first fault in this order. Parsing: the first byte (Truncated) and its R and F bits
// (Reserved); a retransmission's 12 bytes (Truncated); the stream count (WrongCount); then each block, its bytes
// (Truncated) before its fields (Reserved). Encoding and recovery: the variant, the blocks or the parsed header
// (WrongCount, Reserved); the SSRCs (WrongCount, then Duplicate); the lost packet (NotProtected); each packet in order
// (NotRtp, NotProtected, Duplicate); the count of the set (WrongCount); the repair payload (Truncated); the output's
// room (NoSpace).
enum class FlexFecStatus : crd::u8
{
    Ok,
    Truncated,    // the bytes end inside the FEC header, or before the length the recovery fields give
    Reserved,     // R=1 with F=1, F=1 with L=0, or a mask that protects nothing or sets a bit past 109
    WrongCount,   // the stream count is not 1..15 or not the SSRCs', or the packets are not the set the header names
    NotRtp,       // a source packet is shorter than 12 bytes, not RTP version 2, or longer than 12 + 65535 bytes
    NotProtected, // a packet's SSRC and sequence number are not in the repair packet's set
    Duplicate,    // a packet of the set is given twice, or the lost one is among the received ones
    NoSpace,      // the output cannot hold the result
};

enum class FlexFecVariant : crd::u8
{
    Mask,           // R=0, F=0
    Fixed,          // R=0, F=1
    Retransmission, // R=1, F=0
};

// One protected stream of a repair packet. mask[w] bit b is mask bit j = 64·w + b (j = 0 is the first bit on the
// wire), protecting SN base + j; the writer sends the shortest of 15, 46 and 110 bits that holds the highest set bit.
struct FlexFecBlock
{
    crd::u16 sn_base = 0U;
    crd::u8 columns = 0U; // L (F=1)
    crd::u8 rows = 0U;    // D (F=1)
    crd::u64 mask[2] = {};
};

struct FlexFecHeader
{
    static constexpr crd::u32 kMaxStreams = 15U; // the CSRC count's range
    static constexpr crd::u32 kMaxMaskBits = 110U;
    static constexpr crd::usize kMaxBytes = 8U + (kMaxStreams * 16U);

    FlexFecVariant variant = FlexFecVariant::Mask;
    crd::u16 recovery = 0U; // P, X, CC, M and PT recovery: the low 14 bits of the first 16
    crd::u16 length_recovery = 0U;
    crd::u32 ts_recovery = 0U;
    crd::u16 sequence = 0U; // R=1: the retransmitted packet's sequence number
    crd::u32 ssrc = 0U;     // R=1: its SSRC
    crd::u32 stream_count = 0U;
    FlexFecBlock blocks[kMaxStreams] = {};
    crd::usize header_bytes = 0U; // the repair payload starts here
};

// Reads a FEC header from `bytes` (after the repair packet's RTP header) protecting `stream_count` streams, the
// repair packet's CSRC count; a retransmission ignores it.
[[nodiscard]] FlexFecStatus flexfec_parse(std::span<const crd::u8> bytes, crd::u32 stream_count,
                                          FlexFecHeader& header) noexcept;

// The sequence numbers block `block` of a parsed R=0 header protects, in order (at most 255), into `out`, which must
// hold them; returns their count.
crd::u32 flexfec_protected(const FlexFecHeader& header, crd::u32 block, std::span<crd::u16> out) noexcept;

// Writes the FEC header and repair payload protecting `sources` (whole RTP packets, in any order): exactly the
// packets `blocks` names, blocks[i] for the stream ssrcs[i]. `variant` is Mask or Fixed.
[[nodiscard]] FlexFecStatus flexfec_encode(std::span<const std::span<const crd::u8>> sources,
                                           std::span<const crd::u32> ssrcs, FlexFecVariant variant,
                                           std::span<const FlexFecBlock> blocks, std::span<crd::u8> out,
                                           crd::usize& written) noexcept;

// Recovers the packet (lost_ssrc, lost_sequence) of a repair packet's set from the repair packet (`bytes`, parsed into
// `header` with the SSRCs `ssrcs` of its CSRC list) and every other packet of the set (`received`, in any order):
// 12 + Y bytes of RTP packet into `out`. A retransmission needs no other packet.
[[nodiscard]] FlexFecStatus flexfec_recover(std::span<const crd::u8> bytes, const FlexFecHeader& header,
                                            std::span<const crd::u32> ssrcs,
                                            std::span<const std::span<const crd::u8>> received, crd::u32 lost_ssrc,
                                            crd::u16 lost_sequence, std::span<crd::u8> out,
                                            crd::usize& written) noexcept;

// The receive window of one stream: packets[i] holds sequence number first + i. A missing packet's buffer is
// `capacity` bytes; recover_window() fills it and sets `length` and `present`.
struct FlexFecSlot
{
    crd::u8* data = nullptr;
    crd::usize capacity = 0U;
    crd::usize length = 0U;
    bool present = false;
};

struct FlexFecStream
{
    crd::u32 ssrc = 0U;
    crd::u16 first = 0U;
    std::span<FlexFecSlot> packets;
};

// One received repair packet: its bytes after the RTP header and its CSRC list.
struct FlexFecRepair
{
    std::span<const crd::u8> bytes;
    std::span<const crd::u32> ssrcs;
};

// Recovers every missing packet the repairs reach, repeating until no repair recovers one more (§6.3.4); a repair
// that is malformed, names a packet outside the windows or whose result does not fit its slot is ignored. Returns the
// number of packets recovered.
crd::u32 flexfec_recover_window(std::span<const FlexFecStream> streams,
                                std::span<const FlexFecRepair> repairs) noexcept;

} // namespace crd::hesap::net
