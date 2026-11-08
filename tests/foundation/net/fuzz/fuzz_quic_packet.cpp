// crd-fuzz-net-quic-packet: QUIC packets per encryption level of crd-net (4.2a;
// docs/design/network-foundation.md#quic-packets) under the bounded fuzz harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: byte 0 picks the mode (bit 0: a round trip, else a received datagram), the version (bit 1) and whether a
// cleared QUIC bit is accepted (bit 2); bytes 1 to 8 are entropy; the rest is a datagram, or a round trip's frames.
// Both ends hold every level's keys from fixed secrets, so seeds carry packets that open.
//
// Oracles. Received: the walk yields packets inside the datagram, in order, without overlap; an opened packet's
// payload lies inside it and is not empty, its number is the largest its space opened or below, and the same bytes
// again are a duplicate; a refusal is one of the errors open() names, never a crash. Round trip: the writer refuses
// exactly what its contract refuses (restated); an accepted packet reaches its floor and its block (when they fit),
// carries the shortest Length a long header can, walks as one packet and opens at the peer to its frames followed by
// PADDING, under the key phase the sender sealed it in.
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_packet.hpp>

#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::net;

constexpr usize kHeaderBytes = 9U;
constexpr usize kCid = 8U;
constexpr usize kRoom = 1500U;
constexpr u8 kDcid[kCid] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr u8 kScid[kCid] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7};
constexpr u8 kSecrets[5][32] = {{0x11}, {0x22}, {0x33}, {0x44}, {0x55}}; // Handshake, 1-RTT, then 0-RTT

struct Ends
{
    QuicPacketProtection client;
    QuicPacketProtection server;
};

ConstSpan<u8> secret(usize index)
{
    return ConstSpan<u8>(kSecrets[index], sizeof(kSecrets[index]));
}

void start(Ends& ends, QuicVersion version)
{
    const ConstSpan<u8> dcid(kDcid, kCid);
    CRD_FUZZ_REQUIRE(ends.client.start(QuicSide::Client, version, dcid) == CryptoError::None);
    CRD_FUZZ_REQUIRE(ends.server.start(QuicSide::Server, version, dcid) == CryptoError::None);
    CRD_FUZZ_REQUIRE(ends.client.install(QuicLevel::Early, true, AlgorithmId::Aes128Gcm, secret(4U)) ==
                     CryptoError::None);
    CRD_FUZZ_REQUIRE(ends.server.install(QuicLevel::Early, false, AlgorithmId::Aes128Gcm, secret(4U)) ==
                     CryptoError::None);
    for (const QuicLevel level : {QuicLevel::Handshake, QuicLevel::Application})
    {
        const usize first = (level == QuicLevel::Handshake) ? 0U : 2U;
        CRD_FUZZ_REQUIRE(ends.server.install(level, false, AlgorithmId::Aes128Gcm, secret(first)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(ends.server.install(level, true, AlgorithmId::Aes128Gcm, secret(first + 1U)) ==
                         CryptoError::None);
        CRD_FUZZ_REQUIRE(ends.client.install(level, false, AlgorithmId::Aes128Gcm, secret(first + 1U)) ==
                         CryptoError::None);
    }
}

// The client's Handshake and 1-RTT send keys end its 0-RTT and Initial keys; they come last, after its 0-RTT and
// Initial packets.
void finish_client(Ends& ends)
{
    for (const QuicLevel level : {QuicLevel::Handshake, QuicLevel::Application})
    {
        CRD_FUZZ_REQUIRE(ends.client.install(level, true, AlgorithmId::Aes128Gcm,
                                             secret(level == QuicLevel::Handshake ? 0U : 2U)) == CryptoError::None);
    }
}

bool inside(ConstSpan<u8> view, ConstSpan<u8> whole)
{
    return view.empty() || (view.data() >= whole.data() && view.data() + view.size() <= whole.data() + whole.size());
}

// Whether every packet of the datagram opened.
bool check_received(ConstSpan<u8> body, u8 control)
{
    static u8 datagram[crd::fuzz::kMaxInputBytes];
    static u8 again[crd::fuzz::kMaxInputBytes];
    Ends ends;
    start(ends, (control & 2U) != 0U ? QuicVersion::V2 : QuicVersion::V1);
    ends.server.accept_cleared_quic_bit((control & 4U) != 0U);
    std::memcpy(datagram, body.data(), body.size());
    const Span<u8> whole(datagram, body.size());

    QuicDatagramPackets packets(whole, kCid);
    QuicHeader header;
    Span<u8> packet;
    const u8* end = datagram;
    bool opened_all = true;
    usize walked = 0U;
    while (packets.next(header, packet))
    {
        CRD_FUZZ_REQUIRE(packet.data() == end && inside(packet, whole) && !packet.empty());
        end = packet.data() + packet.size();
        ++walked;
        std::memcpy(again, packet.data(), packet.size());
        QuicOpened opened;
        const QuicPacketError error = ends.server.open(header, packet, opened);
        if (error != QuicPacketError::None)
        {
            CRD_FUZZ_REQUIRE(error == QuicPacketError::NoKeys || error == QuicPacketError::QuicBit ||
                             error == QuicPacketError::Unprotectable || error == QuicPacketError::Forged ||
                             error == QuicPacketError::Duplicate || error == QuicPacketError::Stale ||
                             error == QuicPacketError::ReservedBits || error == QuicPacketError::Empty ||
                             error == QuicPacketError::KeyUpdate);
            opened_all = false;
            continue;
        }
        CRD_FUZZ_REQUIRE(opened.type == header.type && inside(opened.payload, packet) && !opened.payload.empty());
        u64 largest = 0U;
        CRD_FUZZ_REQUIRE(ends.server.largest_opened(quic_space_of(opened.type), largest) && opened.number <= largest);
        QuicHeader again_header = header;
        QuicOpened twice;
        CRD_FUZZ_REQUIRE(ends.server.open(again_header, Span<u8>(again, packet.size()), twice) ==
                             QuicPacketError::Duplicate ||
                         !ends.server.can_open(header.type));
    }
    CRD_FUZZ_REQUIRE(end <= datagram + body.size());
    return opened_all && walked != 0U;
}

// Whether the packet was written and opened.
bool check_round_trip(ConstSpan<u8> frames, u8 control, u64 entropy)
{
    const QuicVersion version = (control & 2U) != 0U ? QuicVersion::V2 : QuicVersion::V1;
    Ends ends;
    start(ends, version);
    constexpr QuicPacketType types[4] = {QuicPacketType::Initial, QuicPacketType::ZeroRtt, QuicPacketType::Handshake,
                                         QuicPacketType::OneRtt};
    QuicPacketSpec spec;
    spec.type = types[entropy & 3U];
    spec.version = static_cast<u32>(version);
    spec.dcid = ConstSpan<u8>(kDcid, (entropy >> 2U) % (kCid + 1U));
    spec.scid = spec.type == QuicPacketType::OneRtt ? ConstSpan<u8>{} : ConstSpan<u8>(kScid, kCid);
    spec.number = (entropy >> 8U) & 0xFFFFU;
    spec.number_bytes = (entropy >> 24U) % 5U;
    spec.spin = ((entropy >> 27U) & 1U) != 0U;
    spec.clear_quic_bit = ((entropy >> 28U) & 1U) != 0U;
    const usize room = 64U + static_cast<usize>((entropy >> 32U) % (kRoom - 63U));
    const QuicPadding padding{static_cast<usize>((entropy >> 44U) % (kRoom + 64U)),
                              static_cast<u32>((entropy >> 56U) % 80U)};
    ends.server.accept_cleared_quic_bit(true);
    if (spec.type == QuicPacketType::OneRtt || spec.type == QuicPacketType::Handshake)
    {
        finish_client(ends);
    }
    const bool update = spec.type == QuicPacketType::OneRtt && ((entropy >> 29U) & 1U) != 0U;
    if (update)
    {
        ends.client.confirm();
        ends.server.confirm();
        // A first packet acknowledged in phase 0 lets the client update; the server follows at the next one.
        u8 first[kRoom];
        QuicPacketWriter writer;
        QuicPacketSpec ping = spec;
        ping.number = spec.number + 1U;
        ping.number_bytes = 4U;
        CRD_FUZZ_REQUIRE(writer.begin(ping, Span<u8>(first, kRoom)));
        writer.frames()[0] = 0x01U;
        usize written = 0U;
        CRD_FUZZ_REQUIRE(ends.client.seal(writer, 1U, {}, written) == QuicPacketError::None);
        ends.client.acknowledged(ping.number);
        CRD_FUZZ_REQUIRE(ends.client.update_keys());
        spec.number = ping.number + 1U;
    }

    static u8 datagram[kRoom];
    QuicPacketWriter writer;
    const usize header = (spec.type == QuicPacketType::OneRtt)
                             ? 1U + spec.dcid.size()
                             : 1U + 4U + 1U + spec.dcid.size() + 1U + spec.scid.size() +
                                   (spec.type == QuicPacketType::Initial ? 1U : 0U);
    const usize needed = quic_packet_number_bytes(spec.number, 0U, false);
    const usize number = (spec.number_bytes == 0U) ? needed : spec.number_bytes;
    const usize length = (spec.type == QuicPacketType::OneRtt) ? 0U : 2U;
    const bool fits = number >= needed && number <= 4U &&
                      room >= header + length + number + (number < 4U ? 4U - number : 0U) + kQuicTagBytes;
    const bool opened = writer.begin(spec, Span<u8>(datagram, room));
    CRD_FUZZ_REQUIRE(opened == fits);
    if (!opened)
    {
        return false;
    }
    const usize used = (frames.size() < writer.frames().size()) ? frames.size() : writer.frames().size();
    std::memcpy(writer.frames().data(), frames.data(), used);
    usize size = 0U;
    const QuicPacketError error = ends.client.seal(writer, used, padding, size);
    if (padding.min_bytes > room)
    {
        CRD_FUZZ_REQUIRE(error == QuicPacketError::Refused);
        return false;
    }
    CRD_FUZZ_REQUIRE(error == QuicPacketError::None && size <= room && size >= padding.min_bytes);
    CRD_FUZZ_REQUIRE(padding.block == 0U || size % padding.block == 0U || size == room || size == padding.min_bytes);

    QuicDatagramPackets packets(Span<u8>(datagram, size), spec.dcid.size());
    QuicHeader read;
    Span<u8> packet;
    CRD_FUZZ_REQUIRE(packets.next(read, packet) && packet.size() == size && read.type == spec.type);
    CRD_FUZZ_REQUIRE(!packets.next(read, packet) && packets.error() == QuicHeaderError::None);
    if (spec.type != QuicPacketType::OneRtt)
    {
        const u64 declared = size - read.number_offset;
        CRD_FUZZ_REQUIRE(read.number_offset - header == quic_varint_bytes(declared) ||
                         read.number_offset - header == quic_varint_bytes(declared + 1U)); // a padded boundary
    }
    QuicOpened out;
    const QuicPacketError opened_error = ends.server.open(read, packet, out);
    if (used == 0U && number == 4U && size == read.number_offset + 4U + kQuicTagBytes)
    {
        CRD_FUZZ_REQUIRE(opened_error == QuicPacketError::Empty);
        return false;
    }
    CRD_FUZZ_REQUIRE(opened_error == QuicPacketError::None && out.number == spec.number);
    CRD_FUZZ_REQUIRE(out.key_update == update && out.payload.size() >= used);
    CRD_FUZZ_REQUIRE(used == 0U || std::memcmp(out.payload.data(), frames.data(), used) == 0);
    for (usize i = used; i < out.payload.size(); ++i)
    {
        CRD_FUZZ_REQUIRE(out.payload[i] == 0U);
    }
    CRD_FUZZ_REQUIRE(out.spin == (spec.spin && spec.type == QuicPacketType::OneRtt));
    return true;
}

// Runs one input: whether it reached an opened packet (every packet of a received datagram).
bool run(const u8* data, usize size)
{
    if (size < kHeaderBytes || size > crd::fuzz::kMaxInputBytes)
    {
        return false;
    }
    u64 entropy = 0U;
    for (usize index = 1U; index < kHeaderBytes; ++index)
    {
        entropy = (entropy << 8U) | data[index];
    }
    const ConstSpan<u8> body(data + kHeaderBytes, size - kHeaderBytes);
    return ((data[0] & 1U) != 0U) ? check_round_trip(body, data[0], entropy) : check_received(body, data[0]);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    (void)run(data, size);
    return 0;
}

namespace
{
void add_seed(crd::fuzz::SeedSink& sink, const char* name, u8 control, u64 entropy, ConstSpan<u8> body)
{
    static u8 seed[kHeaderBytes + kRoom * 3U];
    seed[0] = control;
    for (usize index = 1U; index < kHeaderBytes; ++index)
    {
        seed[index] = static_cast<u8>(entropy >> (8U * (kHeaderBytes - 1U - index)));
    }
    std::memcpy(seed + kHeaderBytes, body.data(), body.size());
    sink.add(name, seed, kHeaderBytes + body.size());
}

// Appends a packet from the client to `out` at `at`; returns the new end.
usize append(Ends& ends, QuicPacketSpec spec, u8* out, usize at, usize min_bytes)
{
    QuicPacketWriter writer;
    CRD_FUZZ_REQUIRE(writer.begin(spec, Span<u8>(out + at, kRoom)));
    writer.frames()[0] = 0x01U;
    usize written = 0U;
    CRD_FUZZ_REQUIRE(ends.client.seal(writer, 1U, {min_bytes, 0U}, written) == QuicPacketError::None);
    return at + written;
}
} // namespace

// Every seed opens but the cleared QUIC bit toward a server that never sent grease_quic_bit (VerdictSink checks each).
void crd_fuzz_seeds(crd::fuzz::SeedSink& next)
{
    static const char* const kAccepted[] = {
        "received-coalesced-v1",      "received-coalesced-v2",     "received-cleared-bit", "round-trip-initial",
        "round-trip-handshake-block", "round-trip-one-rtt-update", "round-trip-zero-rtt"};
    crd::fuzz::VerdictSink sink(next, run, kAccepted, sizeof(kAccepted) / sizeof(kAccepted[0]));
    static u8 body[kRoom * 3U];
    for (const u8 version : {u8{0}, u8{2}})
    {
        Ends ends;
        start(ends, version != 0U ? QuicVersion::V2 : QuicVersion::V1);
        QuicPacketSpec spec;
        spec.version = version != 0U ? static_cast<u32>(QuicVersion::V2) : static_cast<u32>(QuicVersion::V1);
        spec.dcid = ConstSpan<u8>(kDcid, kCid);
        spec.scid = ConstSpan<u8>(kScid, kCid);
        spec.type = QuicPacketType::ZeroRtt;
        usize at = append(ends, spec, body, 0U, 0U);
        spec.type = QuicPacketType::Initial;
        at = append(ends, spec, body, at, 0U);
        finish_client(ends);
        spec.type = QuicPacketType::Handshake;
        at = append(ends, spec, body, at, 0U);
        spec.type = QuicPacketType::OneRtt;
        spec.scid = {};
        spec.number = 1U; // 0-RTT's packet 0 shares the application space
        at = append(ends, spec, body, at, kQuicMinInitialDatagram - at);
        add_seed(sink, version != 0U ? "received-coalesced-v2" : "received-coalesced-v1", version, 0U,
                 ConstSpan<u8>(body, at));
        if (version == 0U)
        {
            spec.clear_quic_bit = true;
            const usize cleared = append(ends, spec, body, 0U, 0U);
            add_seed(sink, "received-cleared-bit", 0x04U, 0U, ConstSpan<u8>(body, cleared));
            add_seed(sink, "received-cleared-bit-refused", 0x00U, 0U, ConstSpan<u8>(body, cleared));
        }
    }
    const u8 frames[6] = {0x01U, 0x00U, 0x02U, 0x05U, 0x00U, 0x00U};
    add_seed(sink, "round-trip-initial", 0x01U, 0x0000020000000020ULL, ConstSpan<u8>(frames, 1U));
    add_seed(sink, "round-trip-handshake-block", 0x03U, 0x80BD8B389E276576ULL, ConstSpan<u8>(frames, 6U));
    add_seed(sink, "round-trip-one-rtt-update", 0x01U, 0xF04C9A92BDBDD3DBULL, ConstSpan<u8>(frames, 6U));
    add_seed(sink, "round-trip-zero-rtt", 0x01U, 0x50781FCA41B810C1ULL, ConstSpan<u8>(frames, 0U));
}
