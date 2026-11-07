// HESAP-NET's FlexFEC and RED held to libwebrtc's own files (crd-webrtc-fec-peer): its RFC 8627 header reader and
// writer on every header a peer can send, its FEC codec on its own packet masks, sequence gaps and losses, each side
// recovering from the other's repair packets, and NetEq's RED splitter on hostile payloads and on red_encode()'s.
// libwebrtc reads only flexible masks and writes at most 48 mask bits, so retransmissions, rows, columns and wider
// writes stay with test_flexfec.cpp; hesap refuses a mask that protects nothing, which libwebrtc reads and later drops,
// and it writes the shortest mask that holds the highest set bit, where libwebrtc's 48-bit table form keeps 46 bits for
// a low row.

#include "fec_packets.hpp"
#include "fec_peer.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/flexfec.hpp>
#include <crd/hesap/net/red.hpp>

#include <catch2/catch_test_macros.hpp>

namespace hn = crd::hesap::net;
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using fec_peer::Fec;
using hn::FlexFecBlock;
using hn::FlexFecHeader;
using hn::FlexFecStatus;
using hn::FlexFecVariant;
using Bytes = crd::containers::Array<u8>;
using Span = std::span<const u8>;

namespace
{

constexpr u32 kMediaSsrc = 0x11223344U;
constexpr u32 kFecSsrc = 0x55667788U;
constexpr usize kWideRepair = 1500U + FlexFecHeader::kMaxBytes;
constexpr u32 kMaxMedia = webrtc::kUlpfecMaxMediaPackets;
constexpr usize kMaskBytes[3] = {2U, 6U, 14U};
constexpr u32 kMaskBits[3] = {15U, 46U, 110U};
constexpr u32 kStreamCounts[] = {0U, 1U, 1U, 1U, 1U, 2U, 3U, 4U, 15U};
constexpr usize kMaxRedBlocks = 64U;

// The index in kMaskBytes of a packed mask's size.
[[nodiscard]] u32 mask_form(usize bytes)
{
    u32 form = 0U;
    while (kMaskBytes[form] != bytes)
    {
        ++form;
    }
    return form;
}

[[nodiscard]] bool same(Span a, Span b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

[[nodiscard]] bool mask_bit(const FlexFecBlock& block, u32 j)
{
    return j < FlexFecHeader::kMaxMaskBits && ((block.mask[j >> 6U] >> (j & 63U)) & 1U) != 0U;
}

// A flexible-mask header: stream blocks of random widths, a mask with nothing set now and then, the R and F bits clear
// but for a few, then sometimes cut short anywhere.
[[nodiscard]] usize hostile_header(SeededWords& rng, u32 streams, std::span<u8> bytes)
{
    for (u8& b : bytes)
    {
        b = static_cast<u8>(rng.next_u32());
    }
    if (rng.next_u32() % 8U != 0U)
    {
        bytes[0] &= 0x3FU;
    }
    usize at = 8U;
    for (u32 s = 0U; s < streams; ++s)
    {
        at += 2U;
        const u32 width = rng.next_u32() % 3U;
        const usize mask_bytes = kMaskBytes[width];
        if (rng.next_u32() % 8U == 0U)
        {
            std::memset(&bytes[at], 0, mask_bytes);
        }
        bytes[at] = static_cast<u8>(width == 0U ? (bytes[at] & 0x7FU) : (bytes[at] | 0x80U));
        if (width != 0U)
        {
            bytes[at + 2U] = static_cast<u8>(width == 1U ? (bytes[at + 2U] & 0x7FU) : (bytes[at + 2U] | 0x80U));
        }
        at += mask_bytes;
    }
    const usize whole = at + (rng.next_u32() % 16U);
    return rng.next_u32() % 4U == 0U ? rng.next_u32() % (whole + 1U) : whole;
}

// libwebrtc's packed mask against hesap's bits, every bit of the width on the wire and the zeros after the last.
[[nodiscard]] bool same_mask(Span packed, const FlexFecBlock& block)
{
    const u32 bits = kMaskBits[mask_form(packed.size())];
    for (u32 j = 0U; j < packed.size() * 8U; ++j)
    {
        const bool theirs = ((packed[j >> 3U] >> (7U - (j & 7U))) & 1U) != 0U;
        if (theirs != (j < bits && mask_bit(block, j)))
        {
            return false;
        }
    }
    for (u32 j = bits; j < FlexFecHeader::kMaxMaskBits; ++j)
    {
        if (mask_bit(block, j))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool empty_mask(Span packed)
{
    return std::ranges::all_of(packed, [](u8 b) { return b == 0U; });
}

// Two repair packets alike but for the mask width each writer chose.
[[nodiscard]] bool same_repair(Span a, Span b)
{
    FlexFecHeader ha;
    FlexFecHeader hb;
    if (hn::flexfec_parse(a, 1U, ha) != FlexFecStatus::Ok || hn::flexfec_parse(b, 1U, hb) != FlexFecStatus::Ok)
    {
        return false;
    }
    return std::memcmp(a.data(), b.data(), 8U) == 0 && ha.blocks[0].sn_base == hb.blocks[0].sn_base &&
           ha.blocks[0].mask[0] == hb.blocks[0].mask[0] && ha.blocks[0].mask[1] == hb.blocks[0].mask[1] &&
           same(a.subspan(ha.header_bytes), b.subspan(hb.header_bytes));
}

// A window of media packets, some sequence numbers skipped.
struct Media
{
    u16 base = 0U;
    u32 span = 0U;
    crd::containers::Array<Bytes> packets; // packets[i] has sequence number base + i; empty when skipped
};

[[nodiscard]] Media media_window(SeededWords& rng, u32 count, u32 span, usize max_payload)
{
    Media media;
    media.base = rng.next_u32() % 4U == 0U ? static_cast<u16>(0xFFF0U + (rng.next_u32() % 16U))
                                           : static_cast<u16>(rng.next_u32());
    media.span = span;
    media.packets.resize(span);
    u32 left = count;
    for (u32 i = 0U; i < span; ++i)
    {
        // The first and last are kept, so the window is exactly `span` wide.
        const u32 room = span - i;
        if (i == 0U || i + 1U == span || left >= room || (left > 2U && (rng.next_u32() % room) < left))
        {
            media.packets[i] =
                rtp_packet(rng, kMediaSsrc, static_cast<u16>(media.base + i), rng.next_u32() % (max_payload + 1U));
            --left;
        }
    }
    return media;
}

// hesap's repair packet for the mask of `block`, over this window.
[[nodiscard]] Bytes hesap_repair(const Media& media, const FlexFecBlock& block)
{
    crd::containers::Array<Span> sources;
    for (u32 i = 0U; i < media.span; ++i)
    {
        if (mask_bit(block, static_cast<u16>(media.base + i - block.sn_base)) && !media.packets[i].empty())
        {
            sources.push_back(Span(media.packets[i].data(), media.packets[i].size()));
        }
    }
    const u32 ssrcs[1] = {kMediaSsrc};
    Bytes out;
    out.resize(kWideRepair);
    usize written = 0U;
    const FlexFecStatus status =
        hn::flexfec_encode(std::span<const Span>(sources.data(), sources.size()), ssrcs, FlexFecVariant::Mask,
                           std::span<const FlexFecBlock>(&block, 1U), std::span<u8>(out.data(), out.size()), written);
    REQUIRE(status == FlexFecStatus::Ok);
    out.resize(written);
    return out;
}

// Each lost packet of the window recovered by hesap from `repairs`; the slots' buffers live in `store`.
struct HesapWindow
{
    crd::containers::Array<Bytes> store;
    crd::containers::Array<hn::FlexFecSlot> slots;
    u32 recovered = 0U;
};

[[nodiscard]] HesapWindow hesap_decode(const Media& media, std::span<const bool> lost, std::span<const Bytes> repairs)
{
    HesapWindow window;
    window.store.resize(media.span);
    window.slots.resize(media.span);
    for (u32 i = 0U; i < media.span; ++i)
    {
        Bytes& buffer = window.store[i];
        buffer.resize(1500U);
        window.slots[i].data = buffer.data();
        window.slots[i].capacity = buffer.size();
        if (!media.packets[i].empty() && !lost[i])
        {
            std::memcpy(buffer.data(), media.packets[i].data(), media.packets[i].size());
            window.slots[i].length = media.packets[i].size();
            window.slots[i].present = true;
        }
    }
    const u32 ssrcs[1] = {kMediaSsrc};
    const hn::FlexFecStream stream{kMediaSsrc, media.base, std::span<hn::FlexFecSlot>(window.slots.data(), media.span)};
    crd::containers::Array<hn::FlexFecRepair> views;
    for (const Bytes& repair : repairs)
    {
        views.push_back({Span(repair.data(), repair.size()), ssrcs});
    }
    window.recovered = hn::flexfec_recover_window(std::span<const hn::FlexFecStream>(&stream, 1U),
                                                  std::span<const hn::FlexFecRepair>(views.data(), views.size()));
    return window;
}

// libwebrtc's decoder fed the window's received packets and `repairs`, in an order from the stream.
void peer_decode(SeededWords& rng, const Media& media, std::span<const bool> lost, std::span<const Bytes> repairs,
                 Fec::RecoveredPacketList& recovered)
{
    const std::unique_ptr<Fec> fec = fec_peer::codec(kFecSsrc, kMediaSsrc);
    crd::containers::Array<u32> order;
    for (u32 i = 0U; i < media.span + repairs.size(); ++i)
    {
        if (i >= media.span || (!media.packets[i].empty() && !lost[i]))
        {
            order.push_back(i);
        }
    }
    for (usize i = order.size(); i > 1U; --i)
    {
        std::swap(order[i - 1U], order[rng.next_u32() % i]);
    }
    for (const u32 which : order)
    {
        if (which < media.span)
        {
            fec_peer::decode(*fec, recovered, kMediaSsrc, static_cast<u16>(media.base + which), false,
                             Span(media.packets[which].data(), media.packets[which].size()));
        }
        else
        {
            const Bytes& repair = repairs[which - media.span];
            fec_peer::decode(*fec, recovered, kFecSsrc, static_cast<u16>(which), true,
                             Span(repair.data(), repair.size()));
        }
    }
}

// Both decoders reached the same packets of the window, each byte for byte the packet that was lost.
void require_same_recovery(const Media& media, std::span<const bool> lost, const HesapWindow& ours,
                           const Fec::RecoveredPacketList& theirs)
{
    for (u32 i = 0U; i < media.span; ++i)
    {
        const Span original(media.packets[i].data(), media.packets[i].size());
        if (media.packets[i].empty() || !lost[i])
        {
            continue;
        }
        const u16 sequence = static_cast<u16>(media.base + i);
        const auto found = std::ranges::find_if(
            theirs, [&](const std::unique_ptr<Fec::RecoveredPacket>& p) { return p->seq_num == sequence; });
        const bool peer_has = found != theirs.end();
        REQUIRE(ours.slots[i].present == peer_has);
        if (peer_has)
        {
            const bool peer_same = same(Span((*found)->pkt->data.cdata(), (*found)->pkt->data.size()), original);
            const bool ours_same = same(Span(ours.slots[i].data, ours.slots[i].length), original);
            REQUIRE((peer_same && ours_same));
        }
    }
}

// A RED payload: well formed, off by a few bytes, or the stream's bytes.
[[nodiscard]] usize hostile_red(SeededWords& rng, std::span<u8> bytes)
{
    const u32 kind = rng.next_u32() % 4U;
    if (kind == 0U)
    {
        const usize size = rng.next_u32() % 64U;
        for (usize i = 0U; i < size; ++i)
        {
            bytes[i] = static_cast<u8>(rng.next_u32());
        }
        return size;
    }
    const u32 redundant = rng.next_u32() % 8U == 0U ? 30U + (rng.next_u32() % 12U) : rng.next_u32() % 5U;
    usize at = 0U;
    usize lengths = 0U;
    for (u32 b = 0U; b < redundant; ++b)
    {
        const u32 pt = rng.next_u32() & 0x7FU;
        const u32 offset = rng.next_u32() & 0x3FFFU;
        const u32 length = rng.next_u32() % 4U == 0U ? 0U : rng.next_u32() % 40U;
        bytes[at] = static_cast<u8>(0x80U | pt);
        bytes[at + 1U] = static_cast<u8>(offset >> 6U);
        bytes[at + 2U] = static_cast<u8>((offset << 2U) | (length >> 8U));
        bytes[at + 3U] = static_cast<u8>(length);
        at += 4U;
        lengths += length;
    }
    bytes[at++] = static_cast<u8>(rng.next_u32() & 0x7FU);
    const usize primary = rng.next_u32() % 3U == 0U ? 0U : rng.next_u32() % 64U;
    usize size = at + lengths + primary;
    if (kind == 3U)
    {
        size = rng.next_u32() % (size + 1U); // cut anywhere
    }
    for (usize i = at; i < size; ++i)
    {
        bytes[i] = static_cast<u8>(rng.next_u32());
    }
    return size;
}

} // namespace

TEST_CASE("FlexFEC: hesap and libwebrtc's RFC 8627 reader read every flexible-mask header alike", "[flexfec][webrtc]")
{
    SeededWords rng(8627U, 1U);
    const webrtc::FlexfecHeaderReader reader;
    u32 read_alike = 0U;
    u32 refused_alike = 0U;
    u32 empty_masks = 0U;
    u32 widths[3] = {};
    for (u32 round = 0U; round < 300'000U; ++round)
    {
        u8 bytes[FlexFecHeader::kMaxBytes + 32U];
        const u32 streams = kStreamCounts[rng.next_u32() % std::size(kStreamCounts)];
        const usize size = hostile_header(rng, streams, bytes);
        const Span view(bytes, size);

        Fec::ReceivedFecPacket packet;
        packet.pkt = fec_peer::shared_packet(view);
        packet.protected_streams.resize(streams);
        const bool peer_ok = reader.webrtc::FlexfecHeaderReader::ReadFecHeader(&packet);

        FlexFecHeader header;
        const FlexFecStatus status = hn::flexfec_parse(view, streams, header);
        if (size != 0U && (bytes[0] & 0xC0U) != 0U)
        {
            REQUIRE_FALSE(peer_ok); // libwebrtc reads neither retransmissions nor rows and columns
            continue;
        }
        if (!peer_ok)
        {
            REQUIRE(status != FlexFecStatus::Ok);
            ++refused_alike;
            continue;
        }
        bool any_empty = false;
        const Span parsed(packet.pkt->data.cdata(), packet.pkt->data.size());
        for (u32 s = 0U; s < streams; ++s)
        {
            const auto& stream = packet.protected_streams[s];
            any_empty = any_empty || empty_mask(parsed.subspan(stream.packet_mask_offset, stream.packet_mask_size));
        }
        if (any_empty)
        {
            REQUIRE(status == FlexFecStatus::Reserved);
            ++empty_masks;
            continue;
        }
        REQUIRE(status == FlexFecStatus::Ok);
        // libwebrtc reads the recovery fields where they lie, at recovery.
        bool alike = header.header_bytes == packet.fec_header_size &&
                     packet.protection_length == size - packet.fec_header_size &&
                     header.recovery == (((u32{parsed[0]} << 8U) | parsed[1]) & 0x3FFFU) &&
                     header.length_recovery == ((u32{parsed[2]} << 8U) | parsed[3]) &&
                     header.ts_recovery == ((u32{parsed[4]} << 24U) | (u32{parsed[5]} << 16U) |
                                            (u32{parsed[6]} << 8U) | parsed[7]);
        for (u32 s = 0U; s < streams; ++s)
        {
            const auto& stream = packet.protected_streams[s];
            alike = alike && header.blocks[s].sn_base == stream.seq_num_base &&
                    same_mask(parsed.subspan(stream.packet_mask_offset, stream.packet_mask_size), header.blocks[s]);
            ++widths[mask_form(stream.packet_mask_size)];
        }
        REQUIRE(alike);
        ++read_alike;
    }
    CHECK(read_alike > 50'000U);
    CHECK(refused_alike > 10'000U);
    CHECK(empty_masks > 5'000U);
    CHECK((widths[0] > 10'000U && widths[1] > 10'000U && widths[2] > 10'000U));
}

TEST_CASE("FlexFEC: repair packets equal libwebrtc's generator and writer on any mask of 48 bits", "[flexfec][webrtc]")
{
    SeededWords rng(8627U, 2U);
    const std::unique_ptr<Fec> fec = fec_peer::codec(kFecSsrc, kMediaSsrc);
    u32 byte_equal = 0U;
    u32 wider_form = 0U;
    for (u32 round = 0U; round < 6000U; ++round)
    {
        // The table's two forms: 16 bits, or 48 even when a low row needs fewer.
        const usize form = rng.next_u32() % 2U == 0U ? 2U : 6U;
        const u32 bits = static_cast<u32>(form) * 8U;
        u64 mask = (u64{rng.next_u32()} << 32U) | rng.next_u32();
        mask &= rng.next_u32() % 4U == 0U ? 0x7FFFU : ~u64{0U};
        mask = (mask & ((u64{1U} << bits) - 1U)) | 1U;

        u8 masks[6] = {};
        FlexFecBlock block;
        block.sn_base = static_cast<u16>(rng.next_u32());
        block.mask[0] = mask;
        Media media;
        media.base = block.sn_base;
        media.span = bits;
        media.packets.resize(bits);
        Fec::PacketList list;
        for (u32 j = 0U; j < bits; ++j)
        {
            if (((mask >> j) & 1U) != 0U)
            {
                masks[j >> 3U] = static_cast<u8>(masks[j >> 3U] | (0x80U >> (j & 7U)));
                media.packets[j] =
                    rtp_packet(rng, kMediaSsrc, static_cast<u16>(block.sn_base + j), rng.next_u32() % 300U);
                list.push_back(fec_peer::owned_packet(Span(media.packets[j].data(), media.packets[j].size())));
            }
        }

        Fec::Packet& generated = fec->generated_fec_packets_[0];
        generated.data.EnsureCapacity(IP_PACKET_SIZE);
        std::memset(generated.data.MutableData(), 0, IP_PACKET_SIZE);
        generated.data.SetSize(0);
        std::memcpy(fec->packet_masks_.data(), masks, form);
        fec->packet_mask_size_ = form;
        fec->GenerateFecPayloads(list, 1U);
        fec->FinalizeFecHeaders(1U, kMediaSsrc, block.sn_base);

        const Bytes ours = hesap_repair(media, block);
        const Span theirs(generated.data.cdata(), generated.data.size());
        if (form == 6U && (mask >> 15U) == 0U)
        {
            REQUIRE(ours.size() + 4U == theirs.size());
            REQUIRE(same_repair(Span(ours.data(), ours.size()), theirs));
            ++wider_form;
        }
        else
        {
            REQUIRE(same(Span(ours.data(), ours.size()), theirs));
            ++byte_equal;
        }
    }
    CHECK(byte_equal > 3000U);
    CHECK(wider_form > 300U);
}

TEST_CASE("FlexFEC: libwebrtc's writer writes hesap's header for up to 15 streams", "[flexfec][webrtc]")
{
    SeededWords rng(8627U, 3U);
    const webrtc::FlexfecHeaderWriter writer;
    for (u32 round = 0U; round < 3000U; ++round)
    {
        const u32 streams = 1U + (rng.next_u32() % FlexFecHeader::kMaxStreams);
        u32 ssrcs[FlexFecHeader::kMaxStreams];
        FlexFecBlock blocks[FlexFecHeader::kMaxStreams];
        u8 masks[FlexFecHeader::kMaxStreams][6] = {};
        webrtc::FecHeaderWriter::ProtectedStream views[FlexFecHeader::kMaxStreams];
        crd::containers::Array<Bytes> packets;
        for (u32 s = 0U; s < streams; ++s)
        {
            ssrcs[s] = 0x1000U + s;
            blocks[s].sn_base = static_cast<u16>(rng.next_u32());
            const u32 bits = 1U + (rng.next_u32() % 48U);
            const u64 mask = ((u64{rng.next_u32()} << 32U) | rng.next_u32()) & ((u64{1U} << bits) - 1U);
            blocks[s].mask[0] = mask | 1U;
            const usize form = (blocks[s].mask[0] >> 16U) == 0U ? 2U : 6U;
            for (u32 j = 0U; j < 48U; ++j)
            {
                if (((blocks[s].mask[0] >> j) & 1U) != 0U)
                {
                    masks[s][j >> 3U] = static_cast<u8>(masks[s][j >> 3U] | (0x80U >> (j & 7U)));
                    packets.push_back(
                        rtp_packet(rng, ssrcs[s], static_cast<u16>(blocks[s].sn_base + j), rng.next_u32() % 40U));
                }
            }
            views[s] = {ssrcs[s], blocks[s].sn_base, Span(masks[s], form)};
        }
        crd::containers::Array<Span> sources;
        for (const Bytes& packet : packets)
        {
            sources.push_back(Span(packet.data(), packet.size()));
        }
        Bytes out;
        out.resize(kWideRepair);
        usize written = 0U;
        REQUIRE(hn::flexfec_encode(std::span<const Span>(sources.data(), sources.size()),
                                   std::span<const u32>(ssrcs, streams), FlexFecVariant::Mask,
                                   std::span<const FlexFecBlock>(blocks, streams),
                                   std::span<u8>(out.data(), out.size()), written) == FlexFecStatus::Ok);
        FlexFecHeader header;
        REQUIRE(hn::flexfec_parse(Span(out.data(), written), streams, header) == FlexFecStatus::Ok);

        // libwebrtc rewrites everything FinalizeFecHeader owns; the XORed fields and payload it leaves are hesap's.
        Fec::Packet packet;
        packet.data.SetData(out.data(), written);
        for (usize i = 8U; i < header.header_bytes; ++i)
        {
            packet.data.MutableData()[i] = static_cast<u8>(rng.next_u32());
        }
        packet.data.MutableData()[0] |= 0xC0U;
        writer.FinalizeFecHeader(std::span(views, streams), packet);
        REQUIRE(same(Span(packet.data.cdata(), packet.data.size()), Span(out.data(), written)));
    }
}

TEST_CASE("FlexFEC: libwebrtc's own packet masks, losses and gaps recover alike from either side's repair packets",
          "[flexfec][webrtc]")
{
    SeededWords rng(8627U, 4U);
    const std::unique_ptr<Fec> fec = fec_peer::codec(kFecSsrc, kMediaSsrc);
    u32 recovered_total = 0U;
    u32 byte_equal = 0U;
    u32 repairs_total = 0U;
    for (u32 round = 0U; round < 4000U; ++round)
    {
        const u32 count = 1U + (rng.next_u32() % kMaxMedia);
        const u32 span =
            rng.next_u32() % 4U == 0U && count > 1U ? count + (rng.next_u32() % (kMaxMedia + 1U - count)) : count;
        const Media media = media_window(rng, count, span, 1200U);
        Fec::PacketList list;
        for (const Bytes& packet : media.packets)
        {
            if (!packet.empty())
            {
                list.push_back(fec_peer::owned_packet(Span(packet.data(), packet.size())));
            }
        }
        const u8 factor = static_cast<u8>(1U + (rng.next_u32() % 255U));
        const int important = static_cast<int>(rng.next_u32() % (list.size() + 1U));
        const bool unequal = rng.next_u32() % 2U == 0U;
        const auto mask_type = rng.next_u32() % 2U == 0U ? webrtc::kFecMaskRandom : webrtc::kFecMaskBursty;
        std::list<Fec::Packet*> generated; // crd-lint-allow-std-container: its interface
        REQUIRE(fec->EncodeFec(list, factor, important, unequal, mask_type, &generated) == 0);
        REQUIRE_FALSE(generated.empty());

        crd::containers::Array<Bytes> theirs;
        crd::containers::Array<Bytes> ours;
        for (const Fec::Packet* repair : generated)
        {
            Bytes copy;
            copy.resize(repair->data.size());
            std::memcpy(copy.data(), repair->data.cdata(), copy.size());
            FlexFecHeader header;
            REQUIRE(hn::flexfec_parse(Span(copy.data(), copy.size()), 1U, header) == FlexFecStatus::Ok);
            Bytes mine = hesap_repair(media, header.blocks[0]);
            if (same(Span(mine.data(), mine.size()), Span(copy.data(), copy.size())))
            {
                ++byte_equal;
            }
            else
            {
                REQUIRE(same_repair(Span(mine.data(), mine.size()), Span(copy.data(), copy.size())));
            }
            theirs.push_back(std::move(copy));
            ours.push_back(std::move(mine));
            ++repairs_total;
        }

        bool lost[kMaxMedia] = {};
        for (u32 i = 0U; i < span; ++i)
        {
            lost[i] = rng.next_u32() % 5U == 0U;
        }
        const std::span<const bool> lost_view(lost, span);
        const std::span<const Bytes> their_view(theirs.data(), theirs.size());
        const std::span<const Bytes> our_view(ours.data(), ours.size());

        // Each decoder on the other's packets.
        const HesapWindow from_theirs = hesap_decode(media, lost_view, their_view);
        Fec::RecoveredPacketList from_ours;
        peer_decode(rng, media, lost_view, our_view, from_ours);
        require_same_recovery(media, lost_view, from_theirs, from_ours);
        // Each on its own.
        const HesapWindow own_ours = hesap_decode(media, lost_view, our_view);
        Fec::RecoveredPacketList own_theirs;
        peer_decode(rng, media, lost_view, their_view, own_theirs);
        require_same_recovery(media, lost_view, own_ours, own_theirs);
        recovered_total += from_theirs.recovered;
    }
    CHECK(recovered_total > 2000U);
    CHECK(byte_equal > repairs_total / 2U);
}

TEST_CASE("FlexFEC: libwebrtc recovers from hesap's masks of up to 110 bits, which its writer cannot send",
          "[flexfec][webrtc]")
{
    SeededWords rng(8627U, 5U);
    u32 recovered_total = 0U;
    for (u32 round = 0U; round < 2000U; ++round)
    {
        const u32 span = 47U + (rng.next_u32() % (FlexFecHeader::kMaxMaskBits - 46U));
        const Media media = media_window(rng, (span / 2U) + 1U, span, 200U);
        crd::containers::Array<Bytes> repairs;
        for (u32 r = 0U; r < 6U; ++r)
        {
            FlexFecBlock block;
            block.sn_base = media.base;
            for (u32 i = 0U; i < span; ++i)
            {
                if (!media.packets[i].empty() && rng.next_u32() % 3U == 0U)
                {
                    block.mask[i >> 6U] |= u64{1U} << (i & 63U);
                }
            }
            if ((block.mask[0] | block.mask[1]) != 0U)
            {
                repairs.push_back(hesap_repair(media, block));
            }
        }
        bool lost[FlexFecHeader::kMaxMaskBits] = {};
        for (u32 i = 0U; i < span; ++i)
        {
            lost[i] = rng.next_u32() % 10U == 0U;
        }
        const std::span<const bool> lost_view(lost, span);
        const std::span<const Bytes> repair_view(repairs.data(), repairs.size());
        const HesapWindow ours = hesap_decode(media, lost_view, repair_view);
        Fec::RecoveredPacketList theirs;
        peer_decode(rng, media, lost_view, repair_view, theirs);
        require_same_recovery(media, lost_view, ours, theirs);
        recovered_total += ours.recovered;
    }
    CHECK(recovered_total > 1000U);
}

TEST_CASE("RED: hesap and NetEq's splitter split every payload alike", "[red][webrtc]")
{
    SeededWords rng(2198U, 1U);
    webrtc::RedPayloadSplitter splitter;
    u32 split_alike = 0U;
    u32 refused_alike = 0U;
    u32 past_peer_limit = 0U;
    for (u32 round = 0U; round < 300'000U; ++round)
    {
        u8 bytes[2048];
        const usize size = hostile_red(rng, bytes);
        const Span view(bytes, size);
        const u32 timestamp = rng.next_u32();

        hn::RedBlock blocks[kMaxRedBlocks];
        u32 count = 0U;
        const hn::RedStatus status = hn::red_parse(view, timestamp, blocks, count);

        webrtc::PacketList list;
        webrtc::Packet whole;
        whole.timestamp = timestamp;
        whole.sequence_number = 77U;
        whole.payload.SetData(bytes, size);
        list.push_back(std::move(whole));
        const bool peer_ok = splitter.SplitRed(&list);

        if (status != hn::RedStatus::Ok)
        {
            REQUIRE(status == hn::RedStatus::Truncated);
            REQUIRE_FALSE(peer_ok);
            ++refused_alike;
            continue;
        }
        // NetEq drops empty blocks and refuses more than 32 others.
        u32 nonempty = 0U;
        for (u32 b = 0U; b < count; ++b)
        {
            nonempty += blocks[b].data.empty() ? 0U : 1U;
        }
        if (nonempty > 32U)
        {
            REQUIRE_FALSE(peer_ok);
            REQUIRE(list.empty());
            ++past_peer_limit;
            continue;
        }
        REQUIRE(peer_ok);
        REQUIRE(list.size() == nonempty);
        // NetEq's list holds the primary first and the oldest last.
        auto it = list.begin();
        bool alike = true;
        u32 level = 0U;
        for (u32 b = count; b-- > 0U;)
        {
            if (blocks[b].data.empty())
            {
                continue;
            }
            alike = alike && it->timestamp == blocks[b].timestamp && it->payload_type == blocks[b].payload_type &&
                    it->sequence_number == 77U && it->priority.red_level == static_cast<int>(level) &&
                    same(Span(it->payload.data(), it->payload.size()), blocks[b].data);
            ++it;
            ++level;
        }
        REQUIRE(alike);
        ++split_alike;
    }
    CHECK(split_alike > 50'000U);
    CHECK(refused_alike > 50'000U);
    CHECK(past_peer_limit > 100U);
}

TEST_CASE("RED: NetEq's splitter reads what red_encode writes", "[red][webrtc]")
{
    SeededWords rng(2198U, 2U);
    webrtc::RedPayloadSplitter splitter;
    for (u32 round = 0U; round < 50'000U; ++round)
    {
        const u32 redundant = rng.next_u32() % 6U;
        hn::RedBlock blocks[6];
        u8 data[6][64];
        u32 offset = 0U;
        for (u32 b = 0U; b < redundant; ++b)
        {
            offset += 1U + (rng.next_u32() % 2000U);
            for (u8& byte : data[b])
            {
                byte = static_cast<u8>(rng.next_u32());
            }
            blocks[b].payload_type = static_cast<u8>(rng.next_u32() & 0x7FU);
            blocks[b].offset = static_cast<u16>(offset);
            blocks[b].data = Span(data[b], 1U + (rng.next_u32() % 64U));
        }
        u8 primary[160];
        const usize primary_bytes = 1U + (rng.next_u32() % std::size(primary));
        for (usize i = 0U; i < primary_bytes; ++i)
        {
            primary[i] = static_cast<u8>(rng.next_u32());
        }
        const u8 primary_type = static_cast<u8>(rng.next_u32() & 0x7FU);
        u8 out[1024];
        usize written = 0U;
        REQUIRE(hn::red_encode(std::span<const hn::RedBlock>(blocks, redundant), primary_type,
                               Span(primary, primary_bytes), out, written) == hn::RedStatus::Ok);

        const u32 timestamp = rng.next_u32();
        webrtc::PacketList list;
        webrtc::Packet whole;
        whole.timestamp = timestamp;
        whole.payload.SetData(out, written);
        list.push_back(std::move(whole));
        REQUIRE(splitter.SplitRed(&list));
        REQUIRE(list.size() == redundant + 1U);
        REQUIRE(same(Span(list.front().payload.data(), list.front().payload.size()), Span(primary, primary_bytes)));
        REQUIRE(list.front().payload_type == primary_type);
        REQUIRE(list.front().timestamp == timestamp);
        list.pop_front();
        bool alike = true;
        for (u32 b = redundant; b-- > 0U;)
        {
            alike = alike && same(Span(list.front().payload.data(), list.front().payload.size()), blocks[b].data) &&
                    list.front().payload_type == blocks[b].payload_type &&
                    list.front().timestamp == timestamp - blocks[b].offset;
            list.pop_front();
        }
        REQUIRE(alike);
    }
}
