// HESAP-NET's FlexFEC parity (RFC 8627): the FEC header against Figures 12 and 13 byte by byte, repair packets
// against §6.2's bit strings built literally here, recovery of every lost packet of masks, rows, columns and several
// streams, sequence numbers across the wrap, retransmissions, §6.3.4's iterative example (Figures 16 to 18) and the
// patterns §1.1.4 says 2-D parity cannot repair (Figures 7 and 8), and every refusal in its order.

#include <crd/containers/array.hpp>
#include <crd/hesap/comms/flexfec.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <initializer_list>
#include <span>

namespace hc = crd::hesap::comms;
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using hc::FlexFecBlock;
using hc::FlexFecHeader;
using hc::FlexFecStatus;
using hc::FlexFecVariant;
template <typename T> using Array = crd::containers::Array<T>;
using Bytes = Array<u8>;
using Packet = std::span<const u8>;

namespace
{

constexpr u8 kSentinel = 0xA5U;

Bytes filled(usize n, u8 fill)
{
    Bytes v;
    v.resize(n, fill);
    return v;
}

bool same(std::span<const u8> a, std::span<const u8> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

bool untouched(const Bytes& out)
{
    for (const u8 b : out)
    {
        if (b != kSentinel)
        {
            return false;
        }
    }
    return true;
}

// An RTP version 2 packet of `payload` bytes after the fixed header, its P, X, CC, M and PT bits and its other
// bytes drawn from the stream.
Bytes rtp_packet(crd::hesap::stats::PhiloxRng& rng, u32 ssrc, u16 sequence, usize payload)
{
    Bytes p = filled(12U + payload, 0U);
    for (u8& b : p)
    {
        b = static_cast<u8>(rng.next_u32());
    }
    p[0] = static_cast<u8>(0x80U | (p[0] & 0x3FU));
    p[2] = static_cast<u8>(sequence >> 8U);
    p[3] = static_cast<u8>(sequence);
    p[8] = static_cast<u8>(ssrc >> 24U);
    p[9] = static_cast<u8>(ssrc >> 16U);
    p[10] = static_cast<u8>(ssrc >> 8U);
    p[11] = static_cast<u8>(ssrc);
    return p;
}

FlexFecBlock mask_block(u16 sn_base, std::initializer_list<u32> bits)
{
    FlexFecBlock b;
    b.sn_base = sn_base;
    for (const u32 j : bits)
    {
        b.mask[j / 64U] |= u64{1U} << (j % 64U);
    }
    return b;
}

FlexFecBlock fixed_block(u16 sn_base, u8 columns, u8 rows)
{
    FlexFecBlock b;
    b.sn_base = sn_base;
    b.columns = columns;
    b.rows = rows;
    return b;
}

// §6.2 literally: each packet's bit string (the first 16 bits, the length minus 12, the timestamp, the octets after
// the fixed header), the shorter ones padded with zeros, XORed; then the R and F bits written over the version.
Bytes rfc_bit_string(std::span<const Packet> sources, bool fixed)
{
    usize longest = 0U;
    for (const Packet p : sources)
    {
        longest = p.size() - 12U > longest ? p.size() - 12U : longest;
    }
    Bytes x = filled(8U + longest, 0U);
    for (const Packet p : sources)
    {
        Bytes s = filled(8U + longest, 0U);
        s[0] = p[0];
        s[1] = p[1];
        s[2] = static_cast<u8>((p.size() - 12U) >> 8U);
        s[3] = static_cast<u8>(p.size() - 12U);
        for (usize i = 0U; i < 4U; ++i)
        {
            s[4U + i] = p[4U + i];
        }
        for (usize i = 12U; i < p.size(); ++i)
        {
            s[8U + i - 12U] = p[i];
        }
        for (usize i = 0U; i < x.size(); ++i)
        {
            x[i] = static_cast<u8>(x[i] ^ s[i]);
        }
    }
    x[0] = static_cast<u8>((x[0] & 0x3FU) | (fixed ? 0x40U : 0U));
    return x;
}

struct Repair
{
    Bytes bytes;
    FlexFecHeader header;
};

Repair encode(std::span<const Packet> sources, std::span<const u32> ssrcs, FlexFecVariant variant,
              std::span<const FlexFecBlock> blocks)
{
    Repair r;
    r.bytes = filled(FlexFecHeader::kMaxBytes + 2000U, kSentinel);
    usize written = 0U;
    REQUIRE(hc::flexfec_encode(sources, ssrcs, variant, blocks, r.bytes, written) == FlexFecStatus::Ok);
    r.bytes.resize(written);
    REQUIRE(hc::flexfec_parse(r.bytes, static_cast<u32>(ssrcs.size()), r.header) == FlexFecStatus::Ok);
    return r;
}

// Every packet of `sources` dropped in turn and recovered from the repair packet and the others.
bool recovers_each(const Repair& repair, std::span<const u32> ssrcs, std::span<const Packet> sources)
{
    for (usize lost = 0U; lost < sources.size(); ++lost)
    {
        Array<Packet> received;
        for (usize i = 0U; i < sources.size(); ++i)
        {
            if (i != lost)
            {
                received.push_back(sources[i]);
            }
        }
        const Packet want = sources[lost];
        const u32 ssrc = (u32{want[8]} << 24U) | (u32{want[9]} << 16U) | (u32{want[10]} << 8U) | want[11];
        const u16 sequence = static_cast<u16>((want[2] << 8U) | want[3]);
        Bytes out = filled(want.size() + 5U, kSentinel);
        usize written = 0U;
        if (hc::flexfec_recover(repair.bytes, repair.header, ssrcs, received, ssrc, sequence, out, written) !=
                FlexFecStatus::Ok ||
            written != want.size() || !same({out.data(), written}, want) || out[written] != kSentinel)
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("flexfec: the flexible mask is Figure 12's, 15, 46 or 110 bits with k = 1 before another block", "[flexfec]")
{
    crd::hesap::stats::PhiloxRng rng(8627, 1);
    const u32 ssrc[1] = {0x11223344U};
    struct Case
    {
        FlexFecBlock block;
        Bytes wire; // bytes 8.. of the header
    };
    const Case cases[] = {
        {mask_block(0x1234U, {0U, 14U}), {0x12U, 0x34U, 0x40U, 0x01U}},
        {mask_block(0x1234U, {0U, 15U, 45U}), {0x12U, 0x34U, 0xC0U, 0x00U, 0x40U, 0x00U, 0x00U, 0x01U}},
        {mask_block(0xFFFFU, {14U, 46U, 109U}),
         {0xFFU, 0xFFU, 0x80U, 0x01U, 0x80U, 0x00U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
          0x01U}},
    };
    for (const Case& c : cases)
    {
        Array<Bytes> packets;
        Array<Packet> sources;
        u16 sequences[FlexFecHeader::kMaxMaskBits];
        FlexFecHeader probe;
        probe.variant = FlexFecVariant::Mask;
        probe.stream_count = 1U;
        probe.blocks[0] = c.block;
        const u32 n = hc::flexfec_protected(probe, 0U, sequences);
        for (u32 i = 0U; i < n; ++i)
        {
            packets.push_back(rtp_packet(rng, ssrc[0], sequences[i], 20U + (7U * i)));
        }
        for (const Bytes& p : packets)
        {
            sources.push_back(p);
        }
        const FlexFecBlock blocks[1] = {c.block};
        const Repair r = encode(sources, ssrc, FlexFecVariant::Mask, blocks);
        REQUIRE(r.bytes.size() >= 8U + c.wire.size());
        CHECK(same({r.bytes.data() + 8U, c.wire.size()}, c.wire));
        CHECK(r.header.header_bytes == 8U + c.wire.size());
        CHECK(r.header.blocks[0].sn_base == c.block.sn_base);
        CHECK(r.header.blocks[0].mask[0] == c.block.mask[0]);
        CHECK(r.header.blocks[0].mask[1] == c.block.mask[1]);
        // The header and payload are §6.2's bit string.
        const Bytes x = rfc_bit_string(sources, false);
        CHECK(same({r.bytes.data(), 8U}, {x.data(), 8U}));
        CHECK(same({r.bytes.data() + r.header.header_bytes, r.bytes.size() - r.header.header_bytes},
                   {x.data() + 8U, x.size() - 8U}));
        CHECK(recovers_each(r, ssrc, sources));
    }
}

TEST_CASE("flexfec: L and D are Figure 13's, rows for D <= 1 and columns for D > 1", "[flexfec]")
{
    crd::hesap::stats::PhiloxRng rng(8627, 2);
    const u32 ssrc[1] = {7U};
    struct Case
    {
        u8 l;
        u8 d;
        u32 count;
        u32 step;
    };
    for (const Case c : {Case{4U, 0U, 4U, 1U}, Case{4U, 1U, 4U, 1U}, Case{4U, 3U, 3U, 4U}, Case{1U, 0U, 1U, 1U},
                         Case{255U, 0U, 255U, 1U}, Case{255U, 255U, 255U, 255U}, Case{10U, 2U, 2U, 10U}})
    {
        INFO("L " << int{c.l} << " D " << int{c.d});
        const FlexFecBlock blocks[1] = {fixed_block(65000U, c.l, c.d)};
        FlexFecHeader probe;
        probe.variant = FlexFecVariant::Fixed;
        probe.stream_count = 1U;
        probe.blocks[0] = blocks[0];
        u16 sequences[255];
        REQUIRE(hc::flexfec_protected(probe, 0U, sequences) == c.count);
        Array<Bytes> packets;
        Array<Packet> sources;
        for (u32 i = 0U; i < c.count; ++i)
        {
            CHECK(sequences[i] == static_cast<u16>(65000U + (i * c.step)));
            packets.push_back(rtp_packet(rng, ssrc[0], sequences[i], (i * 13U) % 300U));
        }
        for (const Bytes& p : packets)
        {
            sources.push_back(p);
        }
        const Repair r = encode(sources, ssrc, FlexFecVariant::Fixed, blocks);
        const u8 wire[4] = {0xFDU, 0xE8U, c.l, c.d}; // 65000
        CHECK(same({r.bytes.data() + 8U, 4U}, wire));
        CHECK((r.bytes[0] & 0xC0U) == 0x40U);
        const Bytes x = rfc_bit_string(sources, true);
        CHECK(same({r.bytes.data(), 8U}, {x.data(), 8U}));
        CHECK(same({r.bytes.data() + 12U, r.bytes.size() - 12U}, {x.data() + 8U, x.size() - 8U}));
        CHECK(recovers_each(r, ssrc, sources));
    }
}

TEST_CASE("flexfec: one repair packet over several streams, across the sequence number wrap", "[flexfec]")
{
    crd::hesap::stats::PhiloxRng rng(8627, 3);
    const u32 ssrcs[3] = {0xA0000001U, 0xB0000002U, 0xC0000003U};
    const FlexFecBlock masks[3] = {mask_block(65530U, {0U, 3U, 5U, 6U, 9U}), mask_block(17U, {1U, 30U}),
                                   mask_block(0U, {100U, 109U})};
    const FlexFecBlock fixed[3] = {fixed_block(65534U, 5U, 0U), fixed_block(40000U, 9U, 4U), fixed_block(3U, 2U, 1U)};
    for (const FlexFecVariant variant : {FlexFecVariant::Mask, FlexFecVariant::Fixed})
    {
        const FlexFecBlock* blocks = variant == FlexFecVariant::Mask ? masks : fixed;
        Array<Bytes> packets;
        for (u32 s = 0U; s < 3U; ++s)
        {
            FlexFecHeader probe;
            probe.variant = variant;
            probe.stream_count = 1U;
            probe.blocks[0] = blocks[s];
            u16 sequences[255];
            const u32 n = hc::flexfec_protected(probe, 0U, sequences);
            for (u32 i = 0U; i < n; ++i)
            {
                packets.push_back(rtp_packet(rng, ssrcs[s], sequences[i], rng.next_u32() % 1400U));
            }
        }
        // Encoding takes the sources in any order.
        Array<Packet> sources;
        for (usize i = packets.size(); i-- > 0U;)
        {
            sources.push_back(packets[i]);
        }
        const Repair r = encode(sources, ssrcs, variant, {blocks, 3U});
        CHECK(r.header.stream_count == 3U);
        const Bytes x = rfc_bit_string(sources, variant == FlexFecVariant::Fixed);
        CHECK(same({r.bytes.data() + r.header.header_bytes, r.bytes.size() - r.header.header_bytes},
                   {x.data() + 8U, x.size() - 8U}));
        CHECK(recovers_each(r, ssrcs, sources));
    }
}

TEST_CASE("flexfec: packets of the fixed header only, and a retransmission, round-trip", "[flexfec]")
{
    crd::hesap::stats::PhiloxRng rng(8627, 4);
    const u32 ssrc[1] = {99U};
    const Bytes a = rtp_packet(rng, 99U, 10U, 0U);
    const Bytes b = rtp_packet(rng, 99U, 11U, 0U);
    const Packet sources[2] = {a, b};
    const FlexFecBlock blocks[1] = {fixed_block(10U, 2U, 0U)};
    const Repair r = encode(sources, ssrc, FlexFecVariant::Fixed, blocks);
    CHECK(r.bytes.size() == 12U);
    CHECK(r.header.length_recovery == 0U);
    CHECK(recovers_each(r, ssrc, sources));

    const Bytes c = rtp_packet(rng, 0x01020304U, 777U, 321U);
    const Packet one[1] = {c};
    Bytes rtx = filled(400U, kSentinel);
    usize written = 0U;
    REQUIRE(hc::flexfec_encode(one, {}, FlexFecVariant::Retransmission, {}, rtx, written) == FlexFecStatus::Ok);
    rtx.resize(written);
    CHECK(same(rtx, c));
    FlexFecHeader h;
    REQUIRE(hc::flexfec_parse(rtx, 0U, h) == FlexFecStatus::Ok);
    CHECK(h.variant == FlexFecVariant::Retransmission);
    CHECK(h.sequence == 777U);
    CHECK(h.ssrc == 0x01020304U);
    Bytes out = filled(c.size(), kSentinel);
    REQUIRE(hc::flexfec_recover(rtx, h, {}, {}, 0x01020304U, 777U, out, written) == FlexFecStatus::Ok);
    CHECK(same({out.data(), written}, c));
}

namespace
{

// A 3-row, 4-column source block (Figure 16's numbering: packet k is sequence number 1000 + k) with its row repair
// packets R_1..R_3 (L = 4, D = 1) and column repair packets C_1..C_4 (L = 4, D = 3).
struct Block2d
{
    static constexpr u32 kSsrc = 0x5EEDU;
    Array<Bytes> packets; // packets[k - 1]
    Array<Bytes> rows;
    Array<Bytes> columns;
};

Block2d block_2d(crd::hesap::stats::PhiloxRng& rng)
{
    Block2d b;
    for (u32 k = 1U; k <= 12U; ++k)
    {
        b.packets.push_back(rtp_packet(rng, Block2d::kSsrc, static_cast<u16>(1000U + k), 50U + ((k * 37U) % 200U)));
    }
    const u32 ssrc[1] = {Block2d::kSsrc};
    for (u32 row = 0U; row < 3U; ++row)
    {
        const Packet sources[4] = {b.packets[row * 4U], b.packets[(row * 4U) + 1U], b.packets[(row * 4U) + 2U],
                                   b.packets[(row * 4U) + 3U]};
        const FlexFecBlock blocks[1] = {fixed_block(static_cast<u16>(1001U + (row * 4U)), 4U, 1U)};
        b.rows.push_back(encode(sources, ssrc, FlexFecVariant::Fixed, blocks).bytes);
    }
    for (u32 col = 0U; col < 4U; ++col)
    {
        const Packet sources[3] = {b.packets[col], b.packets[col + 4U], b.packets[col + 8U]};
        const FlexFecBlock blocks[1] = {fixed_block(static_cast<u16>(1001U + col), 4U, 3U)};
        b.columns.push_back(encode(sources, ssrc, FlexFecVariant::Fixed, blocks).bytes);
    }
    return b;
}

// Runs the window decoder over the block with packets `lost` (1-based) and repair packets `dropped` (R_i as i,
// C_i as 10 + i) missing; returns the packets recovered, and checks each against the original.
u32 run_window(const Block2d& b, std::initializer_list<u32> lost, std::initializer_list<u32> dropped, bool& exact)
{
    Array<Bytes> buffers;
    for (u32 k = 0U; k < 14U; ++k)
    {
        buffers.push_back(filled(1600U, kSentinel));
    }
    hc::FlexFecSlot slots[14]; // sequence numbers 1000 .. 1013: 1000 and 1013 lie outside the block
    for (u32 i = 0U; i < 14U; ++i)
    {
        slots[i].data = buffers[i].data();
        slots[i].capacity = buffers[i].size();
        const bool in_block = i >= 1U && i <= 12U;
        bool missing = !in_block;
        for (const u32 k : lost)
        {
            missing = missing || k == i;
        }
        if (!missing)
        {
            std::memcpy(slots[i].data, b.packets[i - 1U].data(), b.packets[i - 1U].size());
            slots[i].length = b.packets[i - 1U].size();
            slots[i].present = true;
        }
    }
    const u32 ssrc[1] = {Block2d::kSsrc};
    Array<hc::FlexFecRepair> repairs;
    for (u32 i = 1U; i <= 7U; ++i)
    {
        const u32 id = i <= 3U ? i : 10U + (i - 3U);
        bool gone = false;
        for (const u32 d : dropped)
        {
            gone = gone || d == id;
        }
        if (!gone)
        {
            repairs.push_back({i <= 3U ? Packet(b.rows[i - 1U]) : Packet(b.columns[i - 4U]), ssrc});
        }
    }
    const hc::FlexFecStream streams[1] = {{Block2d::kSsrc, 1000U, slots}};
    const u32 recovered = hc::flexfec_recover_window(streams, repairs);
    exact = !slots[0].present && !slots[13].present;
    for (u32 k = 1U; k <= 12U; ++k)
    {
        if (slots[k].present)
        {
            exact = exact && same({slots[k].data, slots[k].length}, b.packets[k - 1U]);
        }
    }
    return recovered;
}

} // namespace

TEST_CASE("flexfec: the iterative decoder repairs Figure 16 in two rounds and fails Figures 7 and 8", "[flexfec]")
{
    crd::hesap::stats::PhiloxRng rng(8627, 5);
    const Block2d b = block_2d(rng);
    bool exact = false;
    // Figure 16: 1, 2, 10 and 11 lost; rows and columns recover 1 and 11, then 2 and 10.
    CHECK(run_window(b, {1U, 2U, 10U, 11U}, {}, exact) == 4U);
    CHECK(exact);
    // Figure 7: 2, 3, 10 and 11 lost: two in each damaged row and column.
    CHECK(run_window(b, {2U, 3U, 10U, 11U}, {}, exact) == 0U);
    CHECK(exact);
    // Figure 8: 3 and 11 lost with R_1 and R_3: column 3 has two losses and their rows have no repair packet.
    CHECK(run_window(b, {3U, 11U}, {1U, 3U}, exact) == 0U);
    CHECK(exact);
    // A whole row lost with its row repair: the four columns recover it.
    CHECK(run_window(b, {5U, 6U, 7U, 8U}, {2U}, exact) == 4U);
    CHECK(exact);
    // Nothing lost recovers nothing; without the column packets only single row losses come back.
    CHECK(run_window(b, {}, {}, exact) == 0U);
    CHECK(run_window(b, {1U, 6U, 7U, 12U}, {11U, 12U, 13U, 14U}, exact) == 2U);
    CHECK(exact);
}

TEST_CASE("flexfec: the window decoder ignores malformed repairs, members outside it and short slots", "[flexfec]")
{
    crd::hesap::stats::PhiloxRng rng(8627, 6);
    const Bytes p0 = rtp_packet(rng, 1U, 100U, 30U);
    const Bytes p1 = rtp_packet(rng, 1U, 101U, 500U);
    const Packet sources[2] = {p0, p1};
    const u32 ssrc[1] = {1U};
    const FlexFecBlock blocks[1] = {fixed_block(100U, 2U, 0U)};
    const Repair r = encode(sources, ssrc, FlexFecVariant::Fixed, blocks);
    Bytes buffer0 = filled(1000U, kSentinel);
    Bytes buffer1 = filled(1000U, kSentinel);
    hc::FlexFecSlot slots[2];
    slots[0] = {buffer0.data(), buffer0.size(), p0.size(), true};
    std::memcpy(buffer0.data(), p0.data(), p0.size());
    slots[1] = {buffer1.data(), 100U, 0U, false}; // too short for the 512-byte packet
    const hc::FlexFecStream streams[1] = {{1U, 100U, slots}};
    Bytes reserved = r.bytes;
    reserved[0] = static_cast<u8>(reserved[0] | 0x80U); // R=1, F=1
    const u32 two[2] = {1U, 1U};
    const hc::FlexFecRepair bad[3] = {{reserved, ssrc}, {r.bytes, two}, {r.bytes, ssrc}};
    CHECK(hc::flexfec_recover_window(streams, bad) == 0U);
    CHECK(!slots[1].present);
    slots[1].capacity = buffer1.size();
    const hc::FlexFecStream shifted[1] = {{1U, 101U, {slots + 1U, 1U}}}; // packet 100 outside the window
    CHECK(hc::flexfec_recover_window(shifted, bad) == 0U);
    CHECK(hc::flexfec_recover_window(streams, bad) == 1U);
    CHECK(same({slots[1].data, slots[1].length}, p1));
    // A retransmission fills its slot; a second copy of it does nothing.
    slots[1].present = false;
    const hc::FlexFecRepair rtx[2] = {{p1, {}}, {p1, {}}};
    CHECK(hc::flexfec_recover_window(streams, rtx) == 1U);
    CHECK(same({slots[1].data, slots[1].length}, p1));
}

TEST_CASE("flexfec: malformed headers are refused in their order", "[flexfec]")
{
    FlexFecHeader h;
    const u8 empty[1] = {0U};
    CHECK(hc::flexfec_parse({empty, 0U}, 1U, h) == FlexFecStatus::Truncated);
    const u8 rf[12] = {0xC0U};
    CHECK(hc::flexfec_parse({rf, 1U}, 1U, h) == FlexFecStatus::Reserved);
    const u8 rtx[11] = {0x80U};
    CHECK(hc::flexfec_parse(rtx, 1U, h) == FlexFecStatus::Truncated);
    const u8 shortest[7] = {0x00U};
    CHECK(hc::flexfec_parse(shortest, 1U, h) == FlexFecStatus::Truncated);
    // Mask: 0x4001 after SN base 5, with k = 1 chains a second block that is missing, then a third.
    const u8 mask15[12] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0x40U, 0x01U};
    CHECK(hc::flexfec_parse(mask15, 0U, h) == FlexFecStatus::WrongCount);
    CHECK(hc::flexfec_parse(mask15, 16U, h) == FlexFecStatus::WrongCount);
    CHECK(hc::flexfec_parse(mask15, 1U, h) == FlexFecStatus::Ok);
    CHECK(hc::flexfec_parse(mask15, 2U, h) == FlexFecStatus::Truncated);
    CHECK(hc::flexfec_parse({mask15, 11U}, 1U, h) == FlexFecStatus::Truncated);
    const u8 chained[24] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0xC0U, 0x00U,
                            0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
    CHECK(hc::flexfec_parse({chained, 15U}, 1U, h) == FlexFecStatus::Truncated);
    CHECK(hc::flexfec_parse({chained, 23U}, 1U, h) == FlexFecStatus::Truncated);
    CHECK(hc::flexfec_parse(chained, 1U, h) == FlexFecStatus::Ok);
    CHECK(h.header_bytes == 24U);
    CHECK(h.blocks[0].mask[0] == 1U);
    CHECK(h.blocks[0].mask[1] == (u64{1U} << 45U)); // bit 109
    const u8 zero_mask[12] = {0x00U};
    CHECK(hc::flexfec_parse(zero_mask, 1U, h) == FlexFecStatus::Reserved);
    // Fixed: L = 0 is reserved whatever D is.
    u8 fixed[12] = {0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0x00U, 0x00U};
    CHECK(hc::flexfec_parse(fixed, 1U, h) == FlexFecStatus::Reserved);
    fixed[11] = 3U;
    CHECK(hc::flexfec_parse(fixed, 1U, h) == FlexFecStatus::Reserved);
    fixed[10] = 1U;
    fixed[11] = 0U;
    CHECK(hc::flexfec_parse(fixed, 1U, h) == FlexFecStatus::Ok);
    CHECK(h.variant == FlexFecVariant::Fixed);
}

TEST_CASE("flexfec: malformed encodes and recoveries are refused in their order and write nothing", "[flexfec]")
{
    crd::hesap::stats::PhiloxRng rng(8627, 7);
    const Bytes a = rtp_packet(rng, 5U, 20U, 40U);
    const Bytes b = rtp_packet(rng, 5U, 21U, 90U);
    const Bytes c = rtp_packet(rng, 5U, 23U, 10U);
    const Bytes other = rtp_packet(rng, 6U, 20U, 10U);
    Bytes v1 = a;
    v1[0] = static_cast<u8>((v1[0] & 0x3FU) | 0x40U);
    const Bytes tiny = filled(11U, 0x80U);
    const u32 ssrc[1] = {5U};
    const u32 twice[2] = {5U, 5U};
    const FlexFecBlock row[1] = {fixed_block(20U, 2U, 0U)};
    const FlexFecBlock rows[2] = {fixed_block(20U, 2U, 0U), fixed_block(20U, 2U, 0U)};
    const FlexFecBlock zero_l[1] = {fixed_block(20U, 0U, 2U)};
    FlexFecBlock high_bit = mask_block(20U, {0U});
    high_bit.mask[1] = u64{1U} << 50U; // bit 114
    const FlexFecBlock high[1] = {high_bit};
    const Packet ab[2] = {a, b};
    struct Case
    {
        const char* what;
        std::span<const Packet> sources;
        std::span<const u32> ssrcs;
        FlexFecVariant variant;
        std::span<const FlexFecBlock> blocks;
        usize room;
        FlexFecStatus want;
    };
    const Packet aa[2] = {a, a};
    const Packet ac[2] = {a, c};
    const Packet a_other[2] = {a, other};
    const Packet a_v1[2] = {v1, b};
    const Packet a_tiny[2] = {tiny, b};
    const Packet only_a[1] = {a};
    const Packet abc[3] = {a, b, c};
    const Case cases[] = {
        {"no blocks", ab, ssrc, FlexFecVariant::Fixed, {}, 999U, FlexFecStatus::WrongCount},
        {"L = 0", ab, ssrc, FlexFecVariant::Fixed, zero_l, 999U, FlexFecStatus::Reserved},
        {"mask bit past 109", ab, ssrc, FlexFecVariant::Mask, high, 999U, FlexFecStatus::Reserved},
        {"SSRCs short", ab, {}, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::WrongCount},
        {"SSRC twice", ab, twice, FlexFecVariant::Fixed, rows, 999U, FlexFecStatus::Duplicate},
        {"under 12 bytes", a_tiny, ssrc, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::NotRtp},
        {"version 1", a_v1, ssrc, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::NotRtp},
        {"other SSRC", a_other, ssrc, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::NotProtected},
        {"outside the row", ac, ssrc, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::NotProtected},
        {"a packet twice", aa, ssrc, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::Duplicate},
        {"a member missing", only_a, ssrc, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::WrongCount},
        {"a packet extra", abc, ssrc, FlexFecVariant::Fixed, row, 999U, FlexFecStatus::NotProtected},
        {"no room", ab, ssrc, FlexFecVariant::Fixed, row, 12U + 89U, FlexFecStatus::NoSpace},
        {"retransmission of two", ab, {}, FlexFecVariant::Retransmission, {}, 999U, FlexFecStatus::WrongCount},
        {"retransmission no room", only_a, {}, FlexFecVariant::Retransmission, {}, 51U, FlexFecStatus::NoSpace},
    };
    for (const Case& k : cases)
    {
        INFO(k.what);
        Bytes out = filled(k.room, kSentinel);
        usize written = 7U;
        CHECK(hc::flexfec_encode(k.sources, k.ssrcs, k.variant, k.blocks, out, written) == k.want);
        CHECK(untouched(out));
        CHECK(written == 7U);
    }
    // Exactly enough room is enough.
    Bytes exact = filled(12U + 90U, kSentinel);
    usize written = 0U;
    CHECK(hc::flexfec_encode(ab, ssrc, FlexFecVariant::Fixed, row, exact, written) == FlexFecStatus::Ok);
    CHECK(written == exact.size());

    const Repair r = encode(ab, ssrc, FlexFecVariant::Fixed, row);
    const Packet with_b[1] = {b};
    const Packet with_a[1] = {a};
    const Packet with_bb[2] = {b, b};
    const Packet with_v1[1] = {v1};
    struct Recover
    {
        const char* what;
        usize bytes;
        std::span<const u32> ssrcs;
        std::span<const Packet> received;
        u32 lost_ssrc;
        u16 lost_sequence;
        usize room;
        FlexFecStatus want;
    };
    const Recover recovers[] = {
        {"header cut", 11U, ssrc, with_b, 5U, 20U, 999U, FlexFecStatus::Truncated},
        {"SSRCs twice", r.bytes.size(), twice, with_b, 5U, 20U, 999U, FlexFecStatus::WrongCount},
        {"lost outside", r.bytes.size(), ssrc, with_b, 5U, 22U, 999U, FlexFecStatus::NotProtected},
        {"lost other SSRC", r.bytes.size(), ssrc, with_b, 6U, 20U, 999U, FlexFecStatus::NotProtected},
        {"lost received", r.bytes.size(), ssrc, with_a, 5U, 20U, 999U, FlexFecStatus::Duplicate},
        {"received twice", r.bytes.size(), ssrc, with_bb, 5U, 20U, 999U, FlexFecStatus::Duplicate},
        {"received not RTP", r.bytes.size(), ssrc, with_v1, 5U, 21U, 999U, FlexFecStatus::NotRtp},
        {"nothing received", r.bytes.size(), ssrc, {}, 5U, 20U, 999U, FlexFecStatus::WrongCount},
        {"payload cut", r.bytes.size() - 1U, ssrc, with_a, 5U, 21U, 999U, FlexFecStatus::Truncated},
        {"no room", r.bytes.size(), ssrc, with_b, 5U, 20U, 12U + 39U, FlexFecStatus::NoSpace},
    };
    for (const Recover& k : recovers)
    {
        INFO(k.what);
        Bytes out = filled(k.room, kSentinel);
        usize got = 7U;
        CHECK(hc::flexfec_recover({r.bytes.data(), k.bytes}, r.header, k.ssrcs, k.received, k.lost_ssrc,
                                  k.lost_sequence, out, got) == k.want);
        CHECK(untouched(out));
        CHECK(got == 7U);
    }
    // A retransmission recovers only its own packet, from nothing else.
    FlexFecHeader h;
    REQUIRE(hc::flexfec_parse(a, 0U, h) == FlexFecStatus::Ok);
    Bytes out = filled(a.size(), kSentinel);
    CHECK(hc::flexfec_recover({a.data(), 11U}, h, {}, {}, 5U, 20U, out, written) == FlexFecStatus::Truncated);
    CHECK(hc::flexfec_recover(a, h, {}, with_b, 5U, 20U, out, written) == FlexFecStatus::WrongCount);
    CHECK(hc::flexfec_recover(a, h, {}, {}, 5U, 21U, out, written) == FlexFecStatus::NotProtected);
    CHECK(hc::flexfec_recover(a, h, {}, {}, 5U, 20U, {out.data(), a.size() - 1U}, written) == FlexFecStatus::NoSpace);
    CHECK(untouched(out));
}
