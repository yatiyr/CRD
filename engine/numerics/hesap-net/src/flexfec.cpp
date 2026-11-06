// RFC 8627's FlexFEC parity (crd/hesap/net/flexfec.hpp). The payload XOR runs on the dispatched GF(2^8) addition
// (crd/hesap/net/erasure_dispatch.hpp); the 8 bytes of recovery fields are XORed here.

#include <crd/containers/wire_bytes.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/net/erasure_dispatch.hpp>
#include <crd/hesap/net/flexfec.hpp>

#include <bit>
#include <cstring>

namespace crd::hesap::net
{
namespace
{

using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::wire_load;
using crd::containers::wire_store;
using crd::containers::WireWriter;

constexpr usize kRtpHeader = 12U;
constexpr usize kMaxRtpPacket = kRtpHeader + 0xFFFFU;
constexpr u32 kMaxSet = 255U; // the largest protected set of one block: L or D columns or rows, or 110 mask bits
constexpr u32 kNone = 0xFFFFFFFFU;

bool mask_bit(const FlexFecBlock& b, u32 j) noexcept
{
    return ((b.mask[j >> 6U] >> (j & 63U)) & 1U) != 0U;
}

// The number of mask bits on the wire: the shortest of 15, 46 and 110 that holds the highest set bit.
u32 mask_width(const FlexFecBlock& b) noexcept
{
    const u32 top = b.mask[1] != 0U ? 64U + 63U - static_cast<u32>(std::countl_zero(b.mask[1]))
                                    : 63U - static_cast<u32>(std::countl_zero(b.mask[0]));
    if (top < 15U)
    {
        return 15U;
    }
    return top < 46U ? 46U : 110U;
}

bool mask_valid(const FlexFecBlock& b) noexcept
{
    return (b.mask[0] | b.mask[1]) != 0U && (b.mask[1] >> (FlexFecHeader::kMaxMaskBits - 64U)) == 0U;
}

u32 set_size(FlexFecVariant variant, const FlexFecBlock& b) noexcept
{
    if (variant == FlexFecVariant::Mask)
    {
        return static_cast<u32>(std::popcount(b.mask[0]) + std::popcount(b.mask[1]));
    }
    return b.rows <= 1U ? b.columns : b.rows;
}

// The place of sequence number `sn` in the block's set (below kMaxSet), or kNone.
u32 set_index(FlexFecVariant variant, const FlexFecBlock& b, u16 sn) noexcept
{
    const u32 offset = static_cast<u16>(sn - b.sn_base);
    if (variant == FlexFecVariant::Mask)
    {
        return offset < FlexFecHeader::kMaxMaskBits && mask_bit(b, offset) ? offset : kNone;
    }
    if (b.columns == 0U)
    {
        return kNone;
    }
    if (b.rows <= 1U)
    {
        return offset < b.columns ? offset : kNone;
    }
    return offset % b.columns == 0U && offset / b.columns < b.rows ? offset / b.columns : kNone;
}

// One bit per place of each block's set.
struct SeenSet
{
    u64 words[FlexFecHeader::kMaxStreams][4] = {};

    // Adds the place; false when it was already present.
    bool insert(u32 block, u32 index) noexcept
    {
        u64& word = words[block][index >> 6U];
        const u64 bit = u64{1U} << (index & 63U);
        const bool fresh = (word & bit) == 0U;
        word |= bit;
        return fresh;
    }
};

// The SSRCs of the blocks: as many as the streams, none twice.
FlexFecStatus check_ssrcs(std::span<const u32> ssrcs, u32 stream_count) noexcept
{
    if (ssrcs.size() != stream_count)
    {
        return FlexFecStatus::WrongCount;
    }
    for (usize i = 0U; i < ssrcs.size(); ++i)
    {
        for (usize m = 0U; m < i; ++m)
        {
            if (ssrcs[m] == ssrcs[i])
            {
                return FlexFecStatus::Duplicate;
            }
        }
    }
    return FlexFecStatus::Ok;
}

u32 block_of(std::span<const u32> ssrcs, u32 ssrc) noexcept
{
    for (usize i = 0U; i < ssrcs.size(); ++i)
    {
        if (ssrcs[i] == ssrc)
        {
            return static_cast<u32>(i);
        }
    }
    return kNone;
}

bool rtp_valid(std::span<const u8> packet) noexcept
{
    return packet.size() >= kRtpHeader && packet.size() <= kMaxRtpPacket && (packet[0] >> 6U) == 2U;
}

// Places `packet` (its SSRC and sequence number) in the set and marks it seen.
FlexFecStatus admit(const FlexFecHeader& header, std::span<const u32> ssrcs, u32 ssrc, u16 sn, SeenSet& seen) noexcept
{
    const u32 block = block_of(ssrcs, ssrc);
    const u32 index = block == kNone ? kNone : set_index(header.variant, header.blocks[block], sn);
    if (index == kNone)
    {
        return FlexFecStatus::NotProtected;
    }
    return seen.insert(block, index) ? FlexFecStatus::Ok : FlexFecStatus::Duplicate;
}

FlexFecStatus admit_packets(const FlexFecHeader& header, std::span<const u32> ssrcs,
                            std::span<const std::span<const u8>> packets, SeenSet& seen) noexcept
{
    for (const std::span<const u8> packet : packets)
    {
        if (!rtp_valid(packet))
        {
            return FlexFecStatus::NotRtp;
        }
        const FlexFecStatus s = admit(header, ssrcs, wire_load<u32>(packet, 8U), wire_load<u16>(packet, 2U), seen);
        if (s != FlexFecStatus::Ok)
        {
            return s;
        }
    }
    return FlexFecStatus::Ok;
}

u32 total_set(const FlexFecHeader& header) noexcept
{
    u32 total = 0U;
    for (u32 i = 0U; i < header.stream_count; ++i)
    {
        total += set_size(header.variant, header.blocks[i]);
    }
    return total;
}

// XORs the recovery fields (the first 16 bits, the length minus 12 and the timestamp) of `packet` into `fields`.
void add_fields(u8* fields, std::span<const u8> packet) noexcept
{
    const u32 length = static_cast<u32>(packet.size() - kRtpHeader);
    fields[0] = static_cast<u8>(fields[0] ^ packet[0]);
    fields[1] = static_cast<u8>(fields[1] ^ packet[1]);
    fields[2] = static_cast<u8>(fields[2] ^ (length >> 8U));
    fields[3] = static_cast<u8>(fields[3] ^ length);
    for (usize i = 4U; i < 8U; ++i)
    {
        fields[i] = static_cast<u8>(fields[i] ^ packet[i]);
    }
}

// The recovered packet (lost_ssrc, lost_sn) from the repair packet and the other packets of its set, which
// for_each(f) passes to f one at a time: 12 + Y bytes into `out`. The packets are known to be the rest of the set.
template <typename ForEach>
FlexFecStatus combine(std::span<const u8> bytes, const FlexFecHeader& header, ForEach for_each, u32 lost_ssrc,
                      u16 lost_sn, std::span<u8> out, usize& written) noexcept
{
    u8 fields[8];
    std::memcpy(fields, bytes.data(), sizeof(fields));
    for_each([&fields](std::span<const u8> p) noexcept { add_fields(fields, p); });
    const usize y = wire_load<u16>(fields, 2U);
    if (bytes.size() - header.header_bytes < y)
    {
        return FlexFecStatus::Truncated;
    }
    if (out.size() < kRtpHeader + y)
    {
        return FlexFecStatus::NoSpace;
    }
    u8* const packet = out.data();
    packet[0] = static_cast<u8>(0x80U | (fields[0] & 0x3FU));
    packet[1] = fields[1];
    wire_store<u16>(out, 2U, lost_sn);
    std::memcpy(packet + 4U, fields + 4U, 4U);
    wire_store<u32>(out, 8U, lost_ssrc);
    if (y != 0U)
    {
        std::memcpy(packet + kRtpHeader, bytes.data() + header.header_bytes, y);
    }
    const ErasureKernels& kernels = *erasure_kernels().table;
    for_each(
        [packet, y, &kernels](std::span<const u8> p) noexcept
        {
            const usize length = p.size() - kRtpHeader;
            kernels.add(packet + kRtpHeader, p.data() + kRtpHeader, length < y ? length : y);
        });
    written = kRtpHeader + y;
    return FlexFecStatus::Ok;
}

void write_block(WireWriter& w, FlexFecVariant variant, const FlexFecBlock& b) noexcept
{
    w.put_u16(b.sn_base);
    if (variant == FlexFecVariant::Fixed)
    {
        w.put_u8(b.columns);
        w.put_u8(b.rows);
        return;
    }
    const u32 width = mask_width(b);
    u32 first = width > 15U ? 0x8000U : 0U;
    for (u32 j = 0U; j < 15U; ++j)
    {
        first |= mask_bit(b, j) ? 1U << (14U - j) : 0U;
    }
    w.put_u16(static_cast<u16>(first));
    if (width == 15U)
    {
        return;
    }
    u32 second = width > 46U ? 0x80000000U : 0U;
    for (u32 j = 15U; j < 46U; ++j)
    {
        second |= mask_bit(b, j) ? 1U << (30U - (j - 15U)) : 0U;
    }
    w.put_u32(second);
    if (width == 46U)
    {
        return;
    }
    u64 third = 0U;
    for (u32 j = 46U; j < FlexFecHeader::kMaxMaskBits; ++j)
    {
        third |= mask_bit(b, j) ? u64{1U} << (63U - (j - 46U)) : 0U;
    }
    w.put_u64(third);
}

usize block_bytes(FlexFecVariant variant, const FlexFecBlock& b) noexcept
{
    if (variant == FlexFecVariant::Fixed)
    {
        return 4U;
    }
    const u32 width = mask_width(b);
    if (width == 15U)
    {
        return 4U;
    }
    return width == 46U ? 8U : 16U;
}

void set_mask_bit(FlexFecBlock& b, u32 j) noexcept
{
    b.mask[j >> 6U] |= u64{1U} << (j & 63U);
}

} // namespace

FlexFecStatus flexfec_parse(std::span<const u8> bytes, u32 stream_count, FlexFecHeader& header) noexcept
{
    if (bytes.empty())
    {
        return FlexFecStatus::Truncated;
    }
    const bool r = (bytes[0] & 0x80U) != 0U;
    const bool f = (bytes[0] & 0x40U) != 0U;
    if (r && f)
    {
        return FlexFecStatus::Reserved;
    }
    FlexFecHeader h;
    if (r)
    {
        if (bytes.size() < kRtpHeader)
        {
            return FlexFecStatus::Truncated;
        }
        h.variant = FlexFecVariant::Retransmission;
        h.recovery = static_cast<u16>(wire_load<u16>(bytes, 0U) & 0x3FFFU);
        h.sequence = wire_load<u16>(bytes, 2U);
        h.ts_recovery = wire_load<u32>(bytes, 4U);
        h.ssrc = wire_load<u32>(bytes, 8U);
        h.header_bytes = kRtpHeader;
        header = h;
        return FlexFecStatus::Ok;
    }
    if (bytes.size() < 8U)
    {
        return FlexFecStatus::Truncated;
    }
    if (stream_count < 1U || stream_count > FlexFecHeader::kMaxStreams)
    {
        return FlexFecStatus::WrongCount;
    }
    h.variant = f ? FlexFecVariant::Fixed : FlexFecVariant::Mask;
    h.recovery = static_cast<u16>(wire_load<u16>(bytes, 0U) & 0x3FFFU);
    h.length_recovery = wire_load<u16>(bytes, 2U);
    h.ts_recovery = wire_load<u32>(bytes, 4U);
    h.stream_count = stream_count;
    usize at = 8U;
    for (u32 i = 0U; i < stream_count; ++i)
    {
        FlexFecBlock& b = h.blocks[i];
        if (bytes.size() - at < 4U)
        {
            return FlexFecStatus::Truncated;
        }
        b.sn_base = wire_load<u16>(bytes, at);
        if (f)
        {
            b.columns = bytes[at + 2U];
            b.rows = bytes[at + 3U];
            at += 4U;
            if (b.columns == 0U)
            {
                return FlexFecStatus::Reserved;
            }
            continue;
        }
        const u32 first = wire_load<u16>(bytes, at + 2U);
        at += 4U;
        for (u32 j = 0U; j < 15U; ++j)
        {
            if (((first >> (14U - j)) & 1U) != 0U)
            {
                set_mask_bit(b, j);
            }
        }
        if ((first & 0x8000U) != 0U)
        {
            if (bytes.size() - at < 4U)
            {
                return FlexFecStatus::Truncated;
            }
            const u32 second = wire_load<u32>(bytes, at);
            at += 4U;
            for (u32 j = 15U; j < 46U; ++j)
            {
                if (((second >> (30U - (j - 15U))) & 1U) != 0U)
                {
                    set_mask_bit(b, j);
                }
            }
            if ((second & 0x80000000U) != 0U)
            {
                if (bytes.size() - at < 8U)
                {
                    return FlexFecStatus::Truncated;
                }
                const u64 third = wire_load<u64>(bytes, at);
                at += 8U;
                for (u32 j = 46U; j < FlexFecHeader::kMaxMaskBits; ++j)
                {
                    if (((third >> (63U - (j - 46U))) & 1U) != 0U)
                    {
                        set_mask_bit(b, j);
                    }
                }
            }
        }
        if (!mask_valid(b))
        {
            return FlexFecStatus::Reserved;
        }
    }
    h.header_bytes = at;
    header = h;
    return FlexFecStatus::Ok;
}

u32 flexfec_protected(const FlexFecHeader& header, u32 block, std::span<u16> out) noexcept
{
    if (header.variant == FlexFecVariant::Retransmission || block >= header.stream_count)
    {
        return 0U;
    }
    const FlexFecBlock& b = header.blocks[block];
    CRD_ASSERT(out.size() >= set_size(header.variant, b));
    u32 count = 0U;
    if (header.variant == FlexFecVariant::Mask)
    {
        for (u32 j = 0U; j < FlexFecHeader::kMaxMaskBits; ++j)
        {
            if (mask_bit(b, j))
            {
                out[count++] = static_cast<u16>(b.sn_base + j);
            }
        }
        return count;
    }
    const u32 n = b.rows <= 1U ? b.columns : b.rows;
    const u32 step = b.rows <= 1U ? 1U : b.columns;
    for (u32 i = 0U; i < n; ++i)
    {
        out[count++] = static_cast<u16>(b.sn_base + (i * step));
    }
    return count;
}

FlexFecStatus flexfec_encode(std::span<const std::span<const u8>> sources, std::span<const u32> ssrcs,
                             FlexFecVariant variant, std::span<const FlexFecBlock> blocks, std::span<u8> out,
                             usize& written) noexcept
{
    if (variant == FlexFecVariant::Retransmission)
    {
        if (sources.size() != 1U || !ssrcs.empty() || !blocks.empty())
        {
            return FlexFecStatus::WrongCount;
        }
        if (!rtp_valid(sources[0]))
        {
            return FlexFecStatus::NotRtp;
        }
        if (out.size() < sources[0].size())
        {
            return FlexFecStatus::NoSpace;
        }
        // The retransmission header is the source's RTP header: version 2's bits are R=1, F=0.
        std::memcpy(out.data(), sources[0].data(), sources[0].size());
        written = sources[0].size();
        return FlexFecStatus::Ok;
    }
    if (blocks.empty() || blocks.size() > FlexFecHeader::kMaxStreams)
    {
        return FlexFecStatus::WrongCount;
    }
    FlexFecHeader header;
    header.variant = variant;
    header.stream_count = static_cast<u32>(blocks.size());
    usize header_bytes = 8U;
    for (usize i = 0U; i < blocks.size(); ++i)
    {
        const FlexFecBlock& b = blocks[i];
        if (variant == FlexFecVariant::Fixed ? b.columns == 0U : !mask_valid(b))
        {
            return FlexFecStatus::Reserved;
        }
        header.blocks[i] = b;
        header_bytes += block_bytes(variant, b);
    }
    header.header_bytes = header_bytes;
    const FlexFecStatus ssrc_status = check_ssrcs(ssrcs, header.stream_count);
    if (ssrc_status != FlexFecStatus::Ok)
    {
        return ssrc_status;
    }
    SeenSet seen;
    const FlexFecStatus packet_status = admit_packets(header, ssrcs, sources, seen);
    if (packet_status != FlexFecStatus::Ok)
    {
        return packet_status;
    }
    if (sources.size() != total_set(header))
    {
        return FlexFecStatus::WrongCount;
    }
    usize y = 0U;
    for (const std::span<const u8> p : sources)
    {
        y = p.size() - kRtpHeader > y ? p.size() - kRtpHeader : y;
    }
    if (out.size() < header_bytes + y)
    {
        return FlexFecStatus::NoSpace;
    }
    u8* const fec = out.data();
    std::memset(fec, 0, header_bytes + y);
    for (const std::span<const u8> p : sources)
    {
        add_fields(fec, p);
    }
    fec[0] = static_cast<u8>((fec[0] & 0x3FU) | (variant == FlexFecVariant::Fixed ? 0x40U : 0U));
    WireWriter blocks_out(out.subspan(8U, header_bytes - 8U));
    for (u32 i = 0U; i < header.stream_count; ++i)
    {
        write_block(blocks_out, variant, header.blocks[i]);
    }
    CRD_ASSERT(blocks_out.written() == header_bytes - 8U);
    const ErasureKernels& kernels = *erasure_kernels().table;
    for (const std::span<const u8> p : sources)
    {
        kernels.add(fec + header_bytes, p.data() + kRtpHeader, p.size() - kRtpHeader);
    }
    written = header_bytes + y;
    return FlexFecStatus::Ok;
}

FlexFecStatus flexfec_recover(std::span<const u8> bytes, const FlexFecHeader& header, std::span<const u32> ssrcs,
                              std::span<const std::span<const u8>> received, u32 lost_ssrc, u16 lost_sequence,
                              std::span<u8> out, usize& written) noexcept
{
    if (header.variant == FlexFecVariant::Retransmission)
    {
        if (bytes.size() < kRtpHeader)
        {
            return FlexFecStatus::Truncated;
        }
        if (!received.empty())
        {
            return FlexFecStatus::WrongCount;
        }
        if (lost_ssrc != header.ssrc || lost_sequence != header.sequence)
        {
            return FlexFecStatus::NotProtected;
        }
        if (out.size() < bytes.size())
        {
            return FlexFecStatus::NoSpace;
        }
        std::memcpy(out.data(), bytes.data(), bytes.size());
        written = bytes.size();
        return FlexFecStatus::Ok;
    }
    if (header.header_bytes < 8U || bytes.size() < header.header_bytes)
    {
        return FlexFecStatus::Truncated;
    }
    if (header.stream_count < 1U || header.stream_count > FlexFecHeader::kMaxStreams)
    {
        return FlexFecStatus::WrongCount;
    }
    const FlexFecStatus ssrc_status = check_ssrcs(ssrcs, header.stream_count);
    if (ssrc_status != FlexFecStatus::Ok)
    {
        return ssrc_status;
    }
    SeenSet seen;
    const FlexFecStatus lost_status = admit(header, ssrcs, lost_ssrc, lost_sequence, seen);
    if (lost_status != FlexFecStatus::Ok)
    {
        return lost_status;
    }
    const FlexFecStatus packet_status = admit_packets(header, ssrcs, received, seen);
    if (packet_status != FlexFecStatus::Ok)
    {
        return packet_status;
    }
    if (received.size() + 1U != total_set(header))
    {
        return FlexFecStatus::WrongCount;
    }
    const auto each = [received](auto f) noexcept
    {
        for (const std::span<const u8> p : received)
        {
            f(p);
        }
    };
    return combine(bytes, header, each, lost_ssrc, lost_sequence, out, written);
}

u32 flexfec_recover_window(std::span<const FlexFecStream> streams, std::span<const FlexFecRepair> repairs) noexcept
{
    const auto slot_of = [streams](u32 ssrc, u16 sequence) noexcept -> FlexFecSlot*
    {
        for (const FlexFecStream& s : streams)
        {
            if (s.ssrc == ssrc)
            {
                const usize at = static_cast<u16>(sequence - s.first);
                return at < s.packets.size() ? &s.packets[at] : nullptr;
            }
        }
        return nullptr;
    };
    u32 recovered = 0U;
    bool progress = true;
    while (progress)
    {
        progress = false;
        for (const FlexFecRepair& repair : repairs)
        {
            FlexFecHeader header;
            const u32 stream_count =
                repair.ssrcs.size() <= FlexFecHeader::kMaxStreams ? static_cast<u32>(repair.ssrcs.size()) : 0U;
            if (flexfec_parse(repair.bytes, stream_count, header) != FlexFecStatus::Ok)
            {
                continue;
            }
            if (header.variant == FlexFecVariant::Retransmission)
            {
                FlexFecSlot* slot = slot_of(header.ssrc, header.sequence);
                usize written = 0U;
                if (slot != nullptr && !slot->present &&
                    flexfec_recover(repair.bytes, header, {}, {}, header.ssrc, header.sequence,
                                    {slot->data, slot->capacity}, written) == FlexFecStatus::Ok)
                {
                    slot->length = written;
                    slot->present = true;
                    ++recovered;
                    progress = true;
                }
                continue;
            }
            if (check_ssrcs(repair.ssrcs, header.stream_count) != FlexFecStatus::Ok)
            {
                continue;
            }
            // f(slot, ssrc, sequence) over the set, block by block; false when a member is outside the windows.
            const auto visit = [&header, &repair, &slot_of](auto f) noexcept
            {
                u16 sequences[kMaxSet];
                for (u32 i = 0U; i < header.stream_count; ++i)
                {
                    const u32 n = flexfec_protected(header, i, sequences);
                    for (u32 t = 0U; t < n; ++t)
                    {
                        FlexFecSlot* slot = slot_of(repair.ssrcs[i], sequences[t]);
                        if (slot == nullptr)
                        {
                            return false;
                        }
                        f(*slot, repair.ssrcs[i], sequences[t]);
                    }
                }
                return true;
            };
            u32 missing = 0U;
            bool valid = true;
            FlexFecSlot* lost = nullptr;
            u32 lost_ssrc = 0U;
            u16 lost_sequence = 0U;
            const bool inside = visit(
                [&](FlexFecSlot& slot, u32 ssrc, u16 sequence) noexcept
                {
                    if (!slot.present)
                    {
                        ++missing;
                        lost = &slot;
                        lost_ssrc = ssrc;
                        lost_sequence = sequence;
                    }
                    else
                    {
                        valid = valid && rtp_valid({slot.data, slot.length});
                    }
                });
            if (!inside || !valid || missing != 1U)
            {
                continue;
            }
            const auto others = [&visit, lost](auto f) noexcept
            {
                (void)visit(
                    [&f, lost](FlexFecSlot& slot, u32, u16) noexcept
                    {
                        if (&slot != lost)
                        {
                            f(std::span<const u8>(slot.data, slot.length));
                        }
                    });
            };
            usize written = 0U;
            if (combine(repair.bytes, header, others, lost_ssrc, lost_sequence, {lost->data, lost->capacity},
                        written) == FlexFecStatus::Ok)
            {
                lost->length = written;
                lost->present = true;
                ++recovered;
                progress = true;
            }
        }
    }
    return recovered;
}

} // namespace crd::hesap::net
