// crd-fuzz-hesap-flexfec: hesap-comms' FlexFEC parity (RFC 8627; HESAP-NET; crd/hesap/comms/flexfec.hpp) under the
// bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input. The first byte picks the mode, the rest is its data.
//   0  a FEC header: a stream count, then the bytes after the repair packet's RTP header;
//   1  a repair packet: the variant, up to three streams' blocks, each member's payload length and a fault (a member
//      dropped, repeated or foreign, a packet not RTP, one byte too little room), over generated RTP packets;
//   2  a 2-D block of L columns and D rows with its row and column repair packets, the packets and repairs lost by
//      the input's bits, through the window decoder.
//
// Oracles, none of them the codec's own verdict:
//   * a parser restated bit by bit from Figures 10 to 15 decides every header's status, fields and protected set;
//   * the fault decides every encode's and recovery's status, and a refused one writes nothing;
//   * a repair packet is §6.2's bit string, built here from each packet literally;
//   * every member dropped in turn is recovered byte for byte from the others;
//   * the window decoder recovers exactly the packets a peeling of the index sets reaches, each equal to its
//     original, and nothing else.
#include <crd/fuzz/harness.hpp>

#include <crd/hesap/comms/flexfec.hpp>

#include <cstring>
#include <initializer_list>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::comms;

constexpr u8 kSentinel = 0xA5U;
constexpr usize kMaxPayload = 48U;
constexpr usize kMaxPacket = 12U + kMaxPayload;
constexpr u32 kMaxMembers = 3U * 255U;

struct Reader
{
    const u8* data;
    usize size;

    u8 next()
    {
        if (size == 0U)
        {
            return 0U;
        }
        --size;
        return *data++;
    }
};

struct Lcg
{
    u64 state;

    u8 next()
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        return static_cast<u8>(state >> 56U);
    }
};

// ---- Mode 0: the header, restated bit by bit ----------------------------------------------------------------------

struct Bits
{
    const u8* data;
    usize bytes;
    usize at = 0U; // in bits

    bool has(usize count) const { return (at + count) <= bytes * 8U; }

    u64 take(u32 count)
    {
        u64 v = 0U;
        for (u32 i = 0U; i < count; ++i)
        {
            v = (v << 1U) | ((data[at >> 3U] >> (7U - (at & 7U))) & 1U);
            ++at;
        }
        return v;
    }
};

bool mask_has(const FlexFecBlock& b, u32 j)
{
    return ((b.mask[j / 64U] >> (j % 64U)) & 1U) != 0U;
}

FlexFecStatus restated_parse(const u8* data, usize size, u32 stream_count, FlexFecHeader& h)
{
    Bits bits{data, size};
    if (!bits.has(2U))
    {
        return FlexFecStatus::Truncated;
    }
    const u64 r = bits.take(1U);
    const u64 f = bits.take(1U);
    if (r == 1U && f == 1U)
    {
        return FlexFecStatus::Reserved;
    }
    if (r == 1U)
    {
        if (!bits.has(94U))
        {
            return FlexFecStatus::Truncated;
        }
        h.variant = FlexFecVariant::Retransmission;
        h.recovery = static_cast<u16>(bits.take(14U));
        h.sequence = static_cast<u16>(bits.take(16U));
        h.ts_recovery = static_cast<u32>(bits.take(32U));
        h.ssrc = static_cast<u32>(bits.take(32U));
        h.header_bytes = 12U;
        return FlexFecStatus::Ok;
    }
    if (!bits.has(62U))
    {
        return FlexFecStatus::Truncated;
    }
    if (stream_count == 0U || stream_count > 15U)
    {
        return FlexFecStatus::WrongCount;
    }
    h.variant = f == 1U ? FlexFecVariant::Fixed : FlexFecVariant::Mask;
    h.recovery = static_cast<u16>(bits.take(14U));
    h.length_recovery = static_cast<u16>(bits.take(16U));
    h.ts_recovery = static_cast<u32>(bits.take(32U));
    h.stream_count = stream_count;
    for (u32 i = 0U; i < stream_count; ++i)
    {
        FlexFecBlock& b = h.blocks[i];
        if (!bits.has(32U))
        {
            return FlexFecStatus::Truncated;
        }
        b.sn_base = static_cast<u16>(bits.take(16U));
        if (f == 1U)
        {
            b.columns = static_cast<u8>(bits.take(8U));
            b.rows = static_cast<u8>(bits.take(8U));
            if (b.columns == 0U)
            {
                return FlexFecStatus::Reserved;
            }
            continue;
        }
        // Figure 12: k, 15 bits; k, 31 bits; 64 bits.
        const u32 widths[3] = {15U, 31U, 64U};
        u32 j = 0U;
        for (u32 part = 0U; part < 3U; ++part)
        {
            if (part > 0U && !bits.has(part == 1U ? 32U : 64U))
            {
                return FlexFecStatus::Truncated;
            }
            const bool more = part < 2U && bits.take(1U) == 1U;
            for (u32 t = 0U; t < widths[part]; ++t, ++j)
            {
                if (bits.take(1U) == 1U)
                {
                    b.mask[j / 64U] |= u64{1U} << (j % 64U);
                }
            }
            if (!more)
            {
                break;
            }
        }
        if (b.mask[0] == 0U && b.mask[1] == 0U)
        {
            return FlexFecStatus::Reserved;
        }
    }
    h.header_bytes = bits.at / 8U;
    return FlexFecStatus::Ok;
}

// The sequence numbers a block protects, from the RFC's wording: SN base + j for each set mask bit j; SN, SN + 1, ..,
// SN + (L - 1) for D <= 1; SN, SN + L, .., SN + (D - 1)·L for D > 1.
u32 restated_set(FlexFecVariant variant, const FlexFecBlock& b, u16* out)
{
    u32 n = 0U;
    if (variant == FlexFecVariant::Mask)
    {
        for (u32 j = 0U; j < 110U; ++j)
        {
            if (mask_has(b, j))
            {
                out[n++] = static_cast<u16>(b.sn_base + j);
            }
        }
        return n;
    }
    const u32 count = b.rows <= 1U ? b.columns : b.rows;
    for (u32 i = 0U; i < count; ++i)
    {
        out[n++] = static_cast<u16>(b.sn_base + (b.rows <= 1U ? i : i * b.columns));
    }
    return n;
}

void parse_request(Reader r)
{
    const u32 stream_count = r.next() % 17U;
    FlexFecHeader got;
    FlexFecHeader want;
    const FlexFecStatus status = flexfec_parse({r.data, r.size}, stream_count, got);
    CRD_FUZZ_REQUIRE(status == restated_parse(r.data, r.size, stream_count, want));
    if (status != FlexFecStatus::Ok)
    {
        return;
    }
    CRD_FUZZ_REQUIRE(got.variant == want.variant && got.recovery == want.recovery &&
                     got.length_recovery == want.length_recovery && got.ts_recovery == want.ts_recovery &&
                     got.sequence == want.sequence && got.ssrc == want.ssrc && got.header_bytes == want.header_bytes);
    if (got.variant == FlexFecVariant::Retransmission)
    {
        return;
    }
    CRD_FUZZ_REQUIRE(got.stream_count == want.stream_count);
    for (u32 i = 0U; i < got.stream_count; ++i)
    {
        u16 a[255];
        u16 b[255];
        const u32 n = flexfec_protected(got, i, a);
        CRD_FUZZ_REQUIRE(n == restated_set(want.variant, want.blocks[i], b));
        CRD_FUZZ_REQUIRE(n == 0U || std::memcmp(a, b, n * sizeof(u16)) == 0);
    }
}

// ---- Mode 1: a repair packet over generated sources ---------------------------------------------------------------

u8 g_packets[kMaxMembers + 1U][kMaxPacket];
usize g_lengths[kMaxMembers + 1U];
u8 g_out[FlexFecHeader::kMaxBytes + kMaxPayload + 16U];

void make_packet(usize slot, u32 ssrc, u16 sequence, usize payload, Lcg& lcg)
{
    u8* p = g_packets[slot];
    for (usize i = 0U; i < 12U + payload; ++i)
    {
        p[i] = lcg.next();
    }
    p[0] = static_cast<u8>(0x80U | (p[0] & 0x3FU));
    p[2] = static_cast<u8>(sequence >> 8U);
    p[3] = static_cast<u8>(sequence);
    p[8] = static_cast<u8>(ssrc >> 24U);
    p[9] = static_cast<u8>(ssrc >> 16U);
    p[10] = static_cast<u8>(ssrc >> 8U);
    p[11] = static_cast<u8>(ssrc);
    g_lengths[slot] = 12U + payload;
}

std::span<const u8> packet(usize slot)
{
    return {g_packets[slot], g_lengths[slot]};
}

bool untouched(const u8* p, usize n)
{
    for (usize i = 0U; i < n; ++i)
    {
        if (p[i] != kSentinel)
        {
            return false;
        }
    }
    return true;
}

// §6.2's bit string of `count` packets, literally, into `out`; returns its length.
usize bit_string(const std::span<const u8>* packets, usize count, bool fixed, u8* out)
{
    usize longest = 0U;
    for (usize i = 0U; i < count; ++i)
    {
        longest = packets[i].size() - 12U > longest ? packets[i].size() - 12U : longest;
    }
    std::memset(out, 0, 8U + longest);
    for (usize i = 0U; i < count; ++i)
    {
        const std::span<const u8> p = packets[i];
        const usize y = p.size() - 12U;
        const u8 fields[8] = {p[0], p[1], static_cast<u8>(y >> 8U), static_cast<u8>(y), p[4], p[5], p[6], p[7]};
        for (usize t = 0U; t < 8U; ++t)
        {
            out[t] = static_cast<u8>(out[t] ^ fields[t]);
        }
        for (usize t = 0U; t < y; ++t)
        {
            out[8U + t] = static_cast<u8>(out[8U + t] ^ p[12U + t]);
        }
    }
    out[0] = static_cast<u8>((out[0] & 0x3FU) | (fixed ? 0x40U : 0U));
    return 8U + longest;
}

void encode_request(Reader r)
{
    const u8 shape = r.next();
    const bool fixed = (shape & 1U) != 0U;
    const FlexFecVariant variant = fixed ? FlexFecVariant::Fixed : FlexFecVariant::Mask;
    const u32 streams = 1U + ((shape >> 1U) % 3U);
    const u32 ssrcs[3] = {0x10000001U, 0x20000002U, 0x30000003U};
    FlexFecBlock blocks[3];
    bool reserved = false;
    for (u32 s = 0U; s < streams; ++s)
    {
        FlexFecBlock& b = blocks[s];
        b.sn_base = static_cast<u16>((u32{r.next()} << 8U) | r.next());
        if (fixed)
        {
            b.columns = static_cast<u8>(r.next() % 16U);
            b.rows = static_cast<u8>(r.next() % 6U);
            reserved = reserved || b.columns == 0U;
            continue;
        }
        // A mask byte per 8 bits of the first 15, then one byte choosing a wide bit or none.
        const u32 low = (u32{r.next()} << 8U) | r.next();
        for (u32 j = 0U; j < 15U; ++j)
        {
            b.mask[0] |= ((low >> j) & 1U) != 0U ? u64{1U} << j : 0U;
        }
        const u32 wide = r.next();
        if (wide >= 15U && wide < 110U)
        {
            b.mask[wide / 64U] |= u64{1U} << (wide % 64U);
        }
        reserved = reserved || (b.mask[0] == 0U && b.mask[1] == 0U);
    }
    const u8 fault = r.next() % 7U;
    Lcg lcg{(u64{r.next()} << 8U) | r.next()};
    usize count = 0U;
    for (u32 s = 0U; !reserved && s < streams; ++s)
    {
        u16 sequences[255];
        const u32 n = restated_set(variant, blocks[s], sequences);
        for (u32 t = 0U; t < n; ++t)
        {
            make_packet(count++, ssrcs[s], sequences[t], r.next() % (kMaxPayload + 1U), lcg);
        }
    }
    std::span<const u8> sources[kMaxMembers + 1U];
    for (usize i = 0U; i < count; ++i)
    {
        sources[i] = packet(i);
    }
    usize given = count;
    FlexFecStatus want = reserved ? FlexFecStatus::Reserved : FlexFecStatus::Ok;
    usize longest = 0U;
    for (usize i = 0U; i < count; ++i)
    {
        longest = g_lengths[i] - 12U > longest ? g_lengths[i] - 12U : longest;
    }
    usize header_bytes = 8U;
    for (u32 s = 0U; s < streams; ++s)
    {
        const FlexFecBlock& b = blocks[s];
        const bool wide = b.mask[1] != 0U || (b.mask[0] >> 15U) != 0U;
        const bool widest = b.mask[1] != 0U || (b.mask[0] >> 46U) != 0U;
        header_bytes += fixed || !wide ? 4U : (widest ? 16U : 8U);
    }
    usize room = header_bytes + longest;
    if (want == FlexFecStatus::Ok)
    {
        switch (fault)
        {
        case 1U: // a member dropped
            --given;
            want = FlexFecStatus::WrongCount;
            break;
        case 2U: // a member repeated at the end
            sources[given++] = sources[0];
            want = FlexFecStatus::Duplicate;
            break;
        case 3U: // a foreign stream's packet at the end
            make_packet(count, 0x40000004U, 0U, 3U, lcg);
            sources[given++] = packet(count);
            want = FlexFecStatus::NotProtected;
            break;
        case 4U: // the first member not RTP version 2
            g_packets[0][0] = static_cast<u8>(g_packets[0][0] ^ 0xC0U);
            want = FlexFecStatus::NotRtp;
            break;
        case 5U:
            --room;
            want = FlexFecStatus::NoSpace;
            break;
        default:
            break;
        }
    }
    std::memset(g_out, kSentinel, sizeof(g_out));
    usize written = 0U;
    const FlexFecStatus status =
        flexfec_encode({sources, given}, {ssrcs, streams}, variant, {blocks, streams}, {g_out, room}, written);
    CRD_FUZZ_REQUIRE(status == want);
    if (status != FlexFecStatus::Ok)
    {
        CRD_FUZZ_REQUIRE(untouched(g_out, sizeof(g_out)));
        return;
    }
    CRD_FUZZ_REQUIRE(written == room && untouched(g_out + written, sizeof(g_out) - written));
    u8 x[8U + kMaxPayload];
    const usize x_bytes = bit_string(sources, count, fixed, x);
    CRD_FUZZ_REQUIRE(std::memcmp(g_out, x, 8U) == 0);
    CRD_FUZZ_REQUIRE(written - header_bytes == x_bytes - 8U);
    CRD_FUZZ_REQUIRE(x_bytes == 8U || std::memcmp(g_out + header_bytes, x + 8U, x_bytes - 8U) == 0);
    FlexFecHeader header;
    CRD_FUZZ_REQUIRE(flexfec_parse({g_out, written}, streams, header) == FlexFecStatus::Ok);
    CRD_FUZZ_REQUIRE(header.header_bytes == header_bytes);
    // Each member back from the others; the last loses its place to the first so the received list stays whole.
    for (usize lost = 0U; lost < count; ++lost)
    {
        std::span<const u8> received[kMaxMembers];
        usize n = 0U;
        for (usize i = 0U; i < count; ++i)
        {
            if (i != lost)
            {
                received[n++] = sources[i];
            }
        }
        const std::span<const u8> p = sources[lost];
        const u32 ssrc = (u32{p[8]} << 24U) | (u32{p[9]} << 16U) | (u32{p[10]} << 8U) | p[11];
        const u16 sequence = static_cast<u16>((p[2] << 8U) | p[3]);
        u8 out[kMaxPacket + 4U];
        std::memset(out, kSentinel, sizeof(out));
        usize got = 0U;
        CRD_FUZZ_REQUIRE(flexfec_recover({g_out, written}, header, {ssrcs, streams}, {received, n}, ssrc, sequence,
                                         {out, p.size()}, got) == FlexFecStatus::Ok);
        CRD_FUZZ_REQUIRE(got == p.size() && std::memcmp(out, p.data(), got) == 0 && untouched(out + got, 4U));
        if (p.size() > 12U)
        {
            std::memset(out, kSentinel, sizeof(out));
            CRD_FUZZ_REQUIRE(flexfec_recover({g_out, written}, header, {ssrcs, streams}, {received, n}, ssrc,
                                             sequence, {out, p.size() - 1U}, got) == FlexFecStatus::NoSpace);
            CRD_FUZZ_REQUIRE(untouched(out, sizeof(out)));
        }
    }
}

// ---- Mode 2: the window decoder over a 2-D block ------------------------------------------------------------------

constexpr u32 kMaxL = 8U;
constexpr u32 kMaxD = 6U;
u8 g_repairs[kMaxL + kMaxD][12U + kMaxPayload];
usize g_repair_lengths[kMaxL + kMaxD];
u8 g_window[kMaxL * kMaxD][kMaxPacket];

void window_request(Reader r)
{
    const u32 l = 1U + (r.next() % kMaxL);
    const u32 d = 1U + (r.next() % kMaxD);
    const u16 base = static_cast<u16>((u32{r.next()} << 8U) | r.next());
    constexpr u32 kSsrc = 0x77U;
    Lcg lcg{(u64{r.next()} << 8U) | r.next()};
    const u32 total = l * d;
    for (u32 k = 0U; k < total; ++k)
    {
        make_packet(k, kSsrc, static_cast<u16>(base + k), r.next() % (kMaxPayload + 1U), lcg);
    }
    // Row repairs (L, D = 1 when columns follow, else D = 0), then column repairs (L, D) when D > 1.
    const u32 ssrc[1] = {kSsrc};
    u32 sets[kMaxL + kMaxD][kMaxL > kMaxD ? kMaxL : kMaxD];
    u32 set_sizes[kMaxL + kMaxD];
    u32 repairs = 0U;
    for (u32 row = 0U; row < d; ++row, ++repairs)
    {
        std::span<const u8> members[kMaxL];
        for (u32 c = 0U; c < l; ++c)
        {
            members[c] = packet((row * l) + c);
            sets[repairs][c] = (row * l) + c;
        }
        set_sizes[repairs] = l;
        const FlexFecBlock block[1] = {{static_cast<u16>(base + (row * l)), static_cast<u8>(l),
                                        static_cast<u8>(d > 1U ? 1U : 0U), {}}};
        CRD_FUZZ_REQUIRE(flexfec_encode({members, l}, ssrc, FlexFecVariant::Fixed, block, g_repairs[repairs],
                                        g_repair_lengths[repairs]) == FlexFecStatus::Ok);
    }
    for (u32 c = 0U; d > 1U && c < l; ++c, ++repairs)
    {
        std::span<const u8> members[kMaxD];
        for (u32 row = 0U; row < d; ++row)
        {
            members[row] = packet((row * l) + c);
            sets[repairs][row] = (row * l) + c;
        }
        set_sizes[repairs] = d;
        const FlexFecBlock block[1] = {{static_cast<u16>(base + c), static_cast<u8>(l), static_cast<u8>(d), {}}};
        CRD_FUZZ_REQUIRE(flexfec_encode({members, d}, ssrc, FlexFecVariant::Fixed, block, g_repairs[repairs],
                                        g_repair_lengths[repairs]) == FlexFecStatus::Ok);
    }
    // One bit per packet, then per repair: set means lost.
    bool present[kMaxL * kMaxD];
    FlexFecSlot slots[kMaxL * kMaxD];
    for (u32 k = 0U; k < total; ++k)
    {
        present[k] = (r.next() & 1U) == 0U;
        std::memset(g_window[k], kSentinel, sizeof(g_window[k]));
        slots[k] = {g_window[k], sizeof(g_window[k]), 0U, present[k]};
        if (present[k])
        {
            std::memcpy(g_window[k], g_packets[k], g_lengths[k]);
            slots[k].length = g_lengths[k];
        }
    }
    FlexFecRepair list[kMaxL + kMaxD];
    bool usable[kMaxL + kMaxD];
    u32 listed = 0U;
    for (u32 q = 0U; q < repairs; ++q)
    {
        usable[q] = (r.next() & 1U) == 0U;
        if (usable[q])
        {
            list[listed++] = {{g_repairs[q], g_repair_lengths[q]}, ssrc};
        }
    }
    // The peeling of the index sets: a repair with one member missing restores it, until nothing changes.
    bool reach[kMaxL * kMaxD];
    std::memcpy(reach, present, sizeof(reach));
    u32 expected = 0U;
    for (bool again = true; again;)
    {
        again = false;
        for (u32 q = 0U; q < repairs; ++q)
        {
            u32 missing = 0U;
            u32 which = 0U;
            for (u32 t = 0U; usable[q] && t < set_sizes[q]; ++t)
            {
                if (!reach[sets[q][t]])
                {
                    ++missing;
                    which = sets[q][t];
                }
            }
            if (usable[q] && missing == 1U)
            {
                reach[which] = true;
                ++expected;
                again = true;
            }
        }
    }
    const FlexFecStream stream[1] = {{kSsrc, base, {slots, total}}};
    CRD_FUZZ_REQUIRE(flexfec_recover_window(stream, {list, listed}) == expected);
    for (u32 k = 0U; k < total; ++k)
    {
        CRD_FUZZ_REQUIRE(slots[k].present == reach[k]);
        if (reach[k])
        {
            CRD_FUZZ_REQUIRE(slots[k].length == g_lengths[k] &&
                             std::memcmp(g_window[k], g_packets[k], g_lengths[k]) == 0);
        }
        else
        {
            CRD_FUZZ_REQUIRE(untouched(g_window[k], sizeof(g_window[k])));
        }
    }
}

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    u8 buffer[96];
    usize size = 0U;
    for (const u8 b : bytes)
    {
        buffer[size++] = b;
    }
    sink.add(name, buffer, size);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const Reader rest{data + 1U, size - 1U};
    switch (data[0] % 3U)
    {
    case 0U:
        parse_request(rest);
        break;
    case 1U:
        encode_request(rest);
        break;
    default:
        window_request(rest);
        break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // Headers (a stream count, then the bytes): Figure 12's three mask widths, Figure 13's L and D, a retransmission,
    // and the refusals: R=1 with F=1, L = 0, an empty mask, a cut mask chain, a stream count of 0 and of 16.
    add(sink, "parse-mask-15", {0U, 1U, 0x00U, 0x10U, 0x00U, 0x20U, 1U, 2U, 3U, 4U, 0x12U, 0x34U, 0x40U, 0x01U, 9U});
    add(sink, "parse-mask-46", {0U, 1U, 0x00U, 0x10U, 0x00U, 0x20U, 1U, 2U, 3U, 4U, 0x12U, 0x34U, 0xC0U, 0x00U, 0x40U,
                                0x00U, 0x00U, 0x01U});
    add(sink, "parse-mask-110", {0U, 1U, 0x00U, 0x10U, 0x00U, 0x20U, 1U, 2U, 3U, 4U, 0xFFU, 0xFFU, 0x80U, 0x01U, 0x80U,
                                 0x00U, 0x00U, 0x00U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U});
    add(sink, "parse-fixed-two", {0U, 2U, 0x40U, 0x10U, 0x00U, 0x20U, 1U, 2U, 3U, 4U, 0x00U, 0x05U, 4U, 3U, 0xFFU,
                                  0xF0U, 10U, 0U});
    add(sink, "parse-retransmission", {0U, 0U, 0x80U, 0x60U, 0x12U, 0x34U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U});
    add(sink, "parse-reserved-rf", {0U, 1U, 0xC0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});
    add(sink, "parse-reserved-l0", {0U, 1U, 0x40U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 3U});
    add(sink, "parse-empty-mask", {0U, 1U, 0x00U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U});
    add(sink, "parse-cut-chain", {0U, 1U, 0x00U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0x80U, 0U, 0x80U, 0U, 0U, 0U, 1U});
    add(sink, "parse-no-streams", {0U, 0U, 0x00U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0x40U, 0U});
    add(sink, "parse-sixteen-streams", {0U, 16U, 0x00U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0x40U, 0U});
    // Repair packets (shape: bit 0 fixed, then streams - 1; blocks; the fault; a seed; payload lengths).
    add(sink, "encode-mask-one", {1U, 0U, 0xFFU, 0xFAU, 0x5AU, 0x5AU, 100U, 0U, 1U, 2U, 9U, 40U, 0U, 48U, 7U, 7U, 7U});
    add(sink, "encode-mask-three", {1U, 4U, 0U, 1U, 0x01U, 0x01U, 20U, 0U, 9U, 0x80U, 0x01U, 50U, 1U, 0U, 0x40U, 0U,
                                    109U, 0U, 3U, 4U, 30U, 31U, 32U, 33U, 34U, 35U});
    add(sink, "encode-fixed-rows", {1U, 1U, 0xFFU, 0xFEU, 5U, 0U, 0U, 7U, 1U, 2U, 3U, 4U, 5U});
    add(sink, "encode-fixed-columns", {1U, 3U, 0U, 10U, 3U, 4U, 1U, 1U, 2U, 1U, 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U});
    add(sink, "encode-dropped", {1U, 1U, 0U, 1U, 4U, 0U, 1U, 0U, 3U, 9U, 9U, 9U, 9U});
    add(sink, "encode-repeated", {1U, 1U, 0U, 1U, 4U, 0U, 2U, 0U, 3U, 9U, 9U, 9U, 9U});
    add(sink, "encode-foreign", {1U, 1U, 0U, 1U, 4U, 0U, 3U, 0U, 3U, 9U, 9U, 9U, 9U});
    add(sink, "encode-not-rtp", {1U, 0U, 0U, 1U, 0x00U, 0x07U, 200U, 4U, 0U, 3U, 9U, 9U, 9U});
    add(sink, "encode-no-room", {1U, 1U, 0U, 1U, 4U, 0U, 5U, 0U, 3U, 9U, 9U, 9U, 9U});
    add(sink, "encode-reserved", {1U, 1U, 0U, 1U, 0U, 2U, 0U, 0U, 3U});
    // Windows (L - 1, D - 1, the base, a seed, payload lengths, then loss bits for packets and repairs): Figure 16's
    // 4 x 3 block losing 1, 2, 10 and 11; Figure 7's losing 2, 3, 10 and 11; a block across the wrap with repairs lost.
    add(sink, "window-figure-16", {2U, 3U, 2U, 0x03U, 0xE9U, 1U, 2U, 10U, 20U, 30U, 40U, 11U, 21U, 31U, 41U, 12U, 22U,
                                   32U, 42U, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U});
    add(sink, "window-figure-7", {2U, 3U, 2U, 0x03U, 0xE9U, 1U, 2U, 10U, 20U, 30U, 40U, 11U, 21U, 31U, 41U, 12U, 22U,
                                  32U, 42U, 0U, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0U});
    add(sink, "window-wrap", {2U, 2U, 1U, 0xFFU, 0xFEU, 3U, 4U, 0U, 1U, 2U, 3U, 4U, 5U, 1U, 0U, 1U, 0U, 1U, 0U, 0U, 1U,
                              0U, 0U, 0U});
    add(sink, "window-rows-only", {2U, 4U, 0U, 0U, 0U, 9U, 9U, 1U, 2U, 3U, 4U, 5U, 0U, 1U, 0U, 0U, 0U, 0U});
}
