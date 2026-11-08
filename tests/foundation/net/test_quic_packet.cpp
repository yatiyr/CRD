// crd-net -- QUIC packets per encryption level (4.2a; docs/design/network-foundation.md#quic-packets): RFC 9001's and
// RFC 9369's sample packets written and read byte for byte, padding to the sample, a channel's block and the Initial
// floor with the shortest Length, every refusal of the writer, coalesced datagrams and their foreign packets, the key
// discards of RFC 9001 section 4.9, nothing under protection trusted before the AEAD opens (reserved bits, empty
// packets, the QUIC bit, forgeries, duplicates, stale packets, the integrity limit), key updates initiated, followed
// and checked (section 6), a key's own update interval, and a real TLS handshake carried in these packets to 1-RTT in
// both versions.

#include "ingress_fixture.hpp"
#include "quic_vectors.hpp"

#include <crd/containers/array.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/quic_packet.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::containers::WireReader;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd::net::testing;
using namespace crd::quic_vectors;

using Bytes = crd::containers::Array<u8>;

constexpr usize kCid = 8U; // the connection ID length both ends of a pair issue
constexpr u8 kDcid[kCid] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr u8 kOtherDcid[kCid] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7};
constexpr u8 kPing[1] = {0x01};
constexpr u8 kSecrets[4][32] = {{0x11}, {0x22}, {0x33}, {0x44}}; // client and server Handshake, then 1-RTT
constexpr usize kRoom = 1500U;
constexpr QuicVersion kVersions[] = {QuicVersion::V1, QuicVersion::V2};

ConstSpan<u8> secret(usize index)
{
    return ConstSpan<u8>(kSecrets[index], sizeof(kSecrets[index]));
}

// Two ends with every level's keys from fixed secrets.
struct Pair
{
    QuicPacketProtection client;
    QuicPacketProtection server;
};

void start(Pair& pair, QuicVersion version = QuicVersion::V1, AlgorithmId aead = AlgorithmId::Aes128Gcm)
{
    const ConstSpan<u8> dcid(kDcid, kCid);
    REQUIRE(pair.client.start(QuicSide::Client, version, dcid) == CryptoError::None);
    REQUIRE(pair.server.start(QuicSide::Server, version, dcid) == CryptoError::None);
    for (const QuicLevel level : {QuicLevel::Handshake, QuicLevel::Application})
    {
        const usize first = (level == QuicLevel::Handshake) ? 0U : 2U;
        REQUIRE(pair.client.install(level, true, aead, secret(first)) == CryptoError::None);
        REQUIRE(pair.client.install(level, false, aead, secret(first + 1U)) == CryptoError::None);
        REQUIRE(pair.server.install(level, false, aead, secret(first)) == CryptoError::None);
        REQUIRE(pair.server.install(level, true, aead, secret(first + 1U)) == CryptoError::None);
    }
}

QuicPacketSpec spec_of(QuicPacketType type, u64 number, QuicVersion version = QuicVersion::V1)
{
    QuicPacketSpec spec;
    spec.type = type;
    spec.version = static_cast<crd::u32>(version);
    spec.dcid = ConstSpan<u8>(kDcid, kCid);
    if (type != QuicPacketType::OneRtt)
    {
        spec.scid = ConstSpan<u8>(kOtherDcid, kCid);
    }
    spec.number = number;
    return spec;
}

struct Buffer
{
    u8 bytes[20000] = {};
    usize size = 0U;

    [[nodiscard]] Span<u8> rest(usize room = kRoom) { return Span<u8>(bytes + size, room); }
    [[nodiscard]] Span<u8> all() { return Span<u8>(bytes, size); }
};

// Writes one frame byte at the start of an open writer's frames.
void put_frame(const QuicPacketWriter& writer, u8 frame)
{
    const Span<u8> room = writer.frames();
    REQUIRE_FALSE(room.empty());
    room[0] = frame;
}

// Appends one packet of `spec` carrying `frames` to `datagram`; the error of the seal.
QuicPacketError append(QuicPacketProtection& from, Buffer& datagram, const QuicPacketSpec& spec, ConstSpan<u8> frames,
                       QuicPadding padding = {}, usize room = kRoom)
{
    QuicPacketWriter writer;
    REQUIRE(writer.begin(spec, datagram.rest(room)));
    REQUIRE(frames.size() <= writer.frames().size());
    std::memcpy(writer.frames().data(), frames.data(), frames.size());
    usize written = 0U;
    const QuicPacketError error = from.seal(writer, frames.size(), padding, written);
    if (error == QuicPacketError::None)
    {
        datagram.size += written;
    }
    return error;
}

// Opens the first packet of `datagram` at `to` (a copy, so the datagram can be delivered again).
QuicPacketError deliver(QuicPacketProtection& to, const Buffer& datagram, QuicOpened& out, Bytes& copy)
{
    copy.resize(datagram.size);
    std::memcpy(copy.data(), datagram.bytes, datagram.size);
    QuicDatagramPackets packets(Span<u8>(copy.data(), copy.size()), kCid);
    QuicHeader header;
    Span<u8> packet;
    REQUIRE(packets.next(header, packet));
    return to.open(header, packet, out);
}

QuicPacketError deliver(QuicPacketProtection& to, const Buffer& datagram)
{
    QuicOpened opened;
    Bytes copy;
    return deliver(to, datagram, opened, copy);
}

// A 1-RTT packet sealed directly under `key` with the key phase bit `phase`: a peer this side cannot otherwise make.
Buffer crafted(crd::crypto::QuicPacketKey& key, const QuicHeaderKey& header, bool phase, u64 number)
{
    Buffer datagram;
    QuicPacketWriter writer;
    QuicPacketSpec spec = spec_of(QuicPacketType::OneRtt, number);
    spec.number_bytes = 4U;
    REQUIRE(writer.begin(spec, datagram.rest()));
    put_frame(writer, kPing[0]);
    REQUIRE(writer.seal(key, header, phase, 1U, {}, datagram.size) == CryptoError::None);
    return datagram;
}
} // namespace

TEST_CASE("QUIC packets: RFC 9001's and RFC 9369's sample packets written and read byte for byte",
          "[net][4.2a][quic-packet]")
{
    u8 dcid[8];
    u8 scid[8];
    REQUIRE(hex(kClientDcid, dcid) == 8U);
    REQUIRE(hex("f067a5502a4262b5", scid) == 8U);
    u8 crypto_frame[512];
    u8 server_payload[256];
    const usize crypto_bytes = hex(kClientCryptoFrame, crypto_frame);
    const usize server_bytes = hex(kServerPayload, server_payload);

    for (const QuicVersion version : kVersions)
    {
        const bool v2 = version == QuicVersion::V2;
        static u8 expected[1300];
        Buffer datagram;
        QuicKeys keys;
        REQUIRE(quic_initial_keys(version, QuicSide::Client, ConstSpan<u8>(dcid, 8U), keys) == CryptoError::None);

        // Appendix A.2: the client's Initial, packet 2 in four bytes, its CRYPTO frame padded to 1200 bytes.
        QuicPacketSpec spec;
        spec.type = QuicPacketType::Initial;
        spec.version = static_cast<crd::u32>(version);
        spec.dcid = ConstSpan<u8>(dcid, 8U);
        spec.number = 2U;
        spec.number_bytes = 4U;
        QuicPacketWriter writer;
        REQUIRE(writer.begin(spec, datagram.rest()));
        std::memcpy(writer.frames().data(), crypto_frame, crypto_bytes);
        REQUIRE(writer.seal(keys.packet, keys.header, false, crypto_bytes, {quic_datagram_floor(true, 0U), 0U},
                            datagram.size) == CryptoError::None);
        REQUIRE(hex(v2 ? kV2ClientProtected : kV1ClientProtected, expected) == datagram.size);
        CHECK(datagram.size == kQuicMinInitialDatagram);
        CHECK(same(datagram.all(), ConstSpan<u8>(expected, datagram.size)));

        QuicPacketProtection server;
        REQUIRE(server.start(QuicSide::Server, version, ConstSpan<u8>(dcid, 8U)) == CryptoError::None);
        QuicOpened opened;
        Bytes copy;
        REQUIRE(deliver(server, datagram, opened, copy) == QuicPacketError::None);
        CHECK(opened.type == QuicPacketType::Initial);
        CHECK(opened.number == 2U);
        REQUIRE(opened.payload.size() == kClientPayloadBytes);
        CHECK(same(opened.payload.first(crypto_bytes), ConstSpan<u8>(crypto_frame, crypto_bytes)));
        WireReader in(opened.payload);
        QuicFrame frame;
        REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
        CHECK(frame.kind == QuicFrameKind::Crypto);
        REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
        CHECK(frame.kind == QuicFrameKind::Padding);
        CHECK(in.done());

        // Appendix A.3: the server's Initial, packet 1 in two bytes, its ACK and CRYPTO frames.
        QuicKeys server_keys;
        REQUIRE(quic_initial_keys(version, QuicSide::Server, ConstSpan<u8>(dcid, 8U), server_keys) ==
                CryptoError::None);
        Buffer answer;
        spec.dcid = {};
        spec.scid = ConstSpan<u8>(scid, 8U);
        spec.number = 1U;
        spec.number_bytes = 2U;
        REQUIRE(writer.begin(spec, answer.rest()));
        std::memcpy(writer.frames().data(), server_payload, server_bytes);
        REQUIRE(writer.seal(server_keys.packet, server_keys.header, false, server_bytes, {}, answer.size) ==
                CryptoError::None);
        REQUIRE(hex(v2 ? kV2ServerProtected : kV1ServerProtected, expected) == answer.size);
        CHECK(same(answer.all(), ConstSpan<u8>(expected, answer.size)));

        QuicPacketProtection client;
        REQUIRE(client.start(QuicSide::Client, version, ConstSpan<u8>(dcid, 8U)) == CryptoError::None);
        REQUIRE(deliver(client, answer, opened, copy) == QuicPacketError::None);
        CHECK(opened.number == 1U);
        CHECK(same(opened.payload, ConstSpan<u8>(server_payload, server_bytes)));

        // Appendix A.5: the ChaCha20-Poly1305 short header packet, a PING as packet 654360564 in three bytes.
        u8 chacha_secret[32];
        REQUIRE(hex(kChachaSecret, chacha_secret) == 32U);
        QuicKeys chacha;
        REQUIRE(chacha.init(version, AlgorithmId::Chacha20Poly1305, ConstSpan<u8>(chacha_secret, 32U)) ==
                CryptoError::None);
        QuicPacketSpec short_spec;
        short_spec.number = kChachaPacketNumber;
        short_spec.number_bytes = 3U;
        short_spec.largest_acked = kChachaPacketNumber - 1U;
        short_spec.acknowledged = true;
        Buffer short_packet;
        REQUIRE(writer.begin(short_spec, short_packet.rest()));
        put_frame(writer, kPing[0]);
        REQUIRE(writer.seal(chacha.packet, chacha.header, false, 1U, {}, short_packet.size) == CryptoError::None);
        REQUIRE(hex(v2 ? kV2ChachaPacket : kV1ChachaPacket, expected) == short_packet.size);
        CHECK(same(short_packet.all(), ConstSpan<u8>(expected, short_packet.size)));
    }
}

TEST_CASE("QUIC packets: the writer pads to the sample, a block and the Initial floor with the shortest Length",
          "[net][4.2a][quic-packet]")
{
    Pair pair;
    start(pair);

    SECTION("a PING with a one-byte packet number is padded until header protection can sample it")
    {
        Buffer datagram;
        REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, 0U), kPing) == QuicPacketError::None);
        CHECK(datagram.size == 1U + kCid + 1U + 3U + kQuicTagBytes);
        QuicOpened opened;
        Bytes copy;
        REQUIRE(deliver(pair.server, datagram, opened, copy) == QuicPacketError::None);
        const u8 payload[3] = {0x01, 0x00, 0x00};
        CHECK(same(opened.payload, ConstSpan<u8>(payload, 3U)));
    }
    SECTION("a channel's block pads the packet to its next multiple, never past the room")
    {
        Buffer datagram;
        REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, 0U), kPing, {0U, 64U}) ==
                QuicPacketError::None);
        CHECK(datagram.size == 64U);
        Buffer tight;
        REQUIRE(append(pair.client, tight, spec_of(QuicPacketType::OneRtt, 1U), kPing, {0U, 64U}, 40U) ==
                QuicPacketError::None);
        CHECK(tight.size == 40U);
        CHECK(deliver(pair.server, datagram) == QuicPacketError::None);
        CHECK(deliver(pair.server, tight) == QuicPacketError::None);
    }
    SECTION("a small long header packet takes a one-byte Length, a large one up to four")
    {
        constexpr usize before = 1U + 4U + 1U + kCid + 1U + kCid;
        Buffer small;
        REQUIRE(append(pair.client, small, spec_of(QuicPacketType::Handshake, 0U), kPing) == QuicPacketError::None);
        CHECK(small.size == before + 1U + 1U + 3U + kQuicTagBytes);
        CHECK(small.bytes[before] == 1U + 3U + kQuicTagBytes);
        CHECK(deliver(pair.server, small) == QuicPacketError::None);

        Buffer large;
        REQUIRE(append(pair.client, large, spec_of(QuicPacketType::Handshake, 1U), kPing, {17000U, 0U}, 20000U) ==
                QuicPacketError::None);
        CHECK(large.size == 17000U);
        CHECK((large.bytes[before] >> 6U) == 2U);
        CHECK(deliver(pair.server, large) == QuicPacketError::None);

        Buffer mid;
        REQUIRE(append(pair.client, mid, spec_of(QuicPacketType::Handshake, 2U), kPing, {300U, 0U}, 20000U) ==
                QuicPacketError::None);
        CHECK((mid.bytes[before] >> 6U) == 1U);
        CHECK(deliver(pair.server, mid) == QuicPacketError::None);
    }
    SECTION("a datagram carrying an Initial reaches 1200 bytes")
    {
        Buffer datagram;
        REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::Initial, 0U), kPing,
                       {quic_datagram_floor(true, datagram.size), 0U}) == QuicPacketError::None);
        CHECK(datagram.size == kQuicMinInitialDatagram);
        CHECK(quic_datagram_floor(true, 1300U) == 0U);
        CHECK(quic_datagram_floor(false, 0U) == 0U);
    }
    SECTION("padding past the room and frames past frames() are refused")
    {
        Buffer datagram;
        CHECK(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, 0U), kPing, {kRoom + 1U, 0U}) ==
              QuicPacketError::Refused);
        QuicPacketWriter writer;
        REQUIRE(writer.begin(spec_of(QuicPacketType::OneRtt, 0U), datagram.rest()));
        usize written = 0U;
        CHECK(pair.client.seal(writer, writer.frames().size() + 1U, {}, written) == QuicPacketError::Refused);
        QuicKeys keys;
        REQUIRE(keys.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret(2U)) == CryptoError::None);
        QuicPacketWriter unopened;
        CHECK(unopened.frames().empty());
        CHECK(unopened.seal(keys.packet, keys.header, false, 0U, {}, written) == CryptoError::NotReady);
    }
    SECTION("a four-byte packet number needs no padding, and a packet without frames is refused when read")
    {
        Buffer datagram;
        QuicPacketSpec spec = spec_of(QuicPacketType::OneRtt, 0U);
        spec.number_bytes = 4U;
        REQUIRE(append(pair.client, datagram, spec, {}) == QuicPacketError::None);
        CHECK(datagram.size == 1U + kCid + 4U + kQuicTagBytes);
        CHECK(deliver(pair.server, datagram) == QuicPacketError::Empty);
        CHECK(quic_packet_close(QuicPacketError::Empty) == QuicTransportError::ProtocolViolation);
    }
}

TEST_CASE("QUIC packets: the writer refuses what no packet may carry", "[net][4.2a][quic-packet]")
{
    Buffer datagram;
    QuicPacketWriter writer;
    const u8 long_id[21] = {};
    const u8 token[4] = {1, 2, 3, 4};

    QuicPacketSpec spec = spec_of(QuicPacketType::Retry, 0U);
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec.type = QuicPacketType::VersionNegotiation;
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::Handshake, 0U);
    spec.dcid = ConstSpan<u8>(long_id, sizeof(long_id));
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::Handshake, 0U);
    spec.scid = ConstSpan<u8>(long_id, sizeof(long_id));
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::Handshake, 0U);
    spec.token = ConstSpan<u8>(token, sizeof(token));
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::Initial, 0U);
    spec.version = 0x0A0A0A0AU;
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::OneRtt, kQuicMaxPacketNumber + 1U);
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::OneRtt, 1000U);
    spec.number_bytes = 1U; // 1000 packets unacknowledged need two bytes
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec.number_bytes = 5U;
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::OneRtt, 5U);
    spec.largest_acked = 5U;
    spec.acknowledged = true;
    CHECK_FALSE(writer.begin(spec, datagram.rest()));
    spec = spec_of(QuicPacketType::OneRtt, 0U);
    CHECK_FALSE(writer.begin(spec, datagram.rest(1U + kCid + 1U + 3U + kQuicTagBytes - 1U)));
    CHECK(writer.begin(spec, datagram.rest(1U + kCid + 1U + 3U + kQuicTagBytes)));
    spec = spec_of(QuicPacketType::Handshake, 0U);
    CHECK_FALSE(writer.begin(spec, datagram.rest(23U + 1U + 3U + kQuicTagBytes + 1U)));
    CHECK(writer.begin(spec, datagram.rest(23U + 2U + 1U + 3U + kQuicTagBytes)));

    // An Initial's token sits after its connection IDs, as S1 reads it.
    spec = spec_of(QuicPacketType::Initial, 0U);
    spec.token = ConstSpan<u8>(token, sizeof(token));
    Pair pair;
    start(pair);
    REQUIRE(append(pair.client, datagram, spec, kPing, {kQuicMinInitialDatagram, 0U}) == QuicPacketError::None);
    QuicHeader header;
    REQUIRE(parse_quic_header(datagram.all(), kCid, header) == QuicHeaderError::None);
    CHECK(same(header.token, ConstSpan<u8>(token, sizeof(token))));
    CHECK(deliver(pair.server, datagram) == QuicPacketError::None);
}

TEST_CASE("QUIC packets: a coalesced datagram is walked packet by packet and another connection's packet ends it",
          "[net][4.2a][quic-packet]")
{
    Pair pair;
    start(pair);
    Buffer datagram;
    REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::Initial, 0U), kPing) == QuicPacketError::None);
    REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::Handshake, 0U), kPing) == QuicPacketError::None);
    REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, 0U), kPing,
                   {quic_datagram_floor(true, datagram.size), 0U}) == QuicPacketError::None);
    CHECK(datagram.size == kQuicMinInitialDatagram);

    QuicDatagramPackets packets(datagram.all(), kCid);
    QuicHeader header;
    Span<u8> packet;
    QuicOpened opened;
    const QuicPacketType order[3] = {QuicPacketType::Initial, QuicPacketType::Handshake, QuicPacketType::OneRtt};
    for (const QuicPacketType type : order)
    {
        REQUIRE(packets.next(header, packet));
        CHECK(header.type == type);
        REQUIRE(pair.server.open(header, packet, opened) == QuicPacketError::None);
        CHECK(opened.type == type);
        CHECK(opened.payload[0] == kPing[0]);
    }
    CHECK_FALSE(packets.next(header, packet));
    CHECK(packets.error() == QuicHeaderError::None);
    CHECK_FALSE(packets.foreign());

    // A packet of another connection ends the walk; so does one that does not parse.
    Pair other;
    start(other);
    Buffer mixed;
    QuicPacketSpec foreign = spec_of(QuicPacketType::Handshake, 0U);
    foreign.dcid = ConstSpan<u8>(kOtherDcid, kCid);
    REQUIRE(append(other.client, mixed, spec_of(QuicPacketType::Initial, 0U), kPing) == QuicPacketError::None);
    REQUIRE(append(other.client, mixed, foreign, kPing) == QuicPacketError::None);
    QuicDatagramPackets walk(mixed.all(), kCid);
    REQUIRE(walk.next(header, packet));
    CHECK_FALSE(walk.next(header, packet));
    CHECK(walk.foreign());
    CHECK_FALSE(walk.next(header, packet));

    Buffer cut;
    REQUIRE(append(other.client, cut, spec_of(QuicPacketType::Handshake, 1U), kPing) == QuicPacketError::None);
    cut.bytes[cut.size] = 0xE0U; // a long header that ends at its first byte
    ++cut.size;
    QuicDatagramPackets broken(cut.all(), kCid);
    REQUIRE(broken.next(header, packet));
    CHECK_FALSE(broken.next(header, packet));
    CHECK(broken.error() == QuicHeaderError::Truncated);
}

TEST_CASE("QUIC packets: Initial, Handshake and 0-RTT keys are discarded at RFC 9001's moments",
          "[net][4.2a][quic-packet]")
{
    Pair pair;
    start(pair);
    // A client with 1-RTT keys has let its 0-RTT keys go.
    CHECK(pair.client.install(QuicLevel::Early, true, AlgorithmId::Aes128Gcm, secret(3U)) == CryptoError::NotReady);
    Pair early;
    const ConstSpan<u8> dcid(kDcid, kCid);
    REQUIRE(early.client.start(QuicSide::Client, QuicVersion::V1, dcid) == CryptoError::None);
    REQUIRE(early.server.start(QuicSide::Server, QuicVersion::V1, dcid) == CryptoError::None);

    SECTION("0-RTT keys: a client's to send until its 1-RTT keys arrive, a server's to open until it lets them go")
    {
        CHECK(early.client.install(QuicLevel::Early, false, AlgorithmId::Aes128Gcm, secret(0U)) ==
              CryptoError::Overlap);
        CHECK(early.server.install(QuicLevel::Early, true, AlgorithmId::Aes128Gcm, secret(0U)) == CryptoError::Overlap);
        REQUIRE(early.client.install(QuicLevel::Early, true, AlgorithmId::Aes128Gcm, secret(0U)) == CryptoError::None);
        CHECK(early.client.install(QuicLevel::Early, true, AlgorithmId::Aes128Gcm, secret(0U)) == CryptoError::Exists);
        REQUIRE(early.server.install(QuicLevel::Early, false, AlgorithmId::Aes128Gcm, secret(0U)) == CryptoError::None);
        CHECK_FALSE(early.client.can_open(QuicPacketType::ZeroRtt));
        CHECK_FALSE(early.server.can_send(QuicPacketType::ZeroRtt));
        Buffer datagram;
        REQUIRE(append(early.client, datagram, spec_of(QuicPacketType::ZeroRtt, 0U), kPing) == QuicPacketError::None);
        CHECK(deliver(early.server, datagram) == QuicPacketError::None);
        u64 largest = 9U;
        REQUIRE(early.server.largest_opened(QuicSpace::Application, largest));
        CHECK(largest == 0U);
        CHECK_FALSE(early.server.largest_opened(QuicSpace::Handshake, largest));

        REQUIRE(early.client.install(QuicLevel::Application, true, AlgorithmId::Aes128Gcm, secret(2U)) ==
                CryptoError::None);
        CHECK_FALSE(early.client.can_send(QuicPacketType::ZeroRtt));
        CHECK(early.client.install(QuicLevel::Early, true, AlgorithmId::Aes128Gcm, secret(0U)) ==
              CryptoError::NotReady);
        early.server.discard(QuicLevel::Early);
        CHECK(deliver(early.server, datagram) == QuicPacketError::NoKeys);
    }
    SECTION("Initial keys: a client's go with its first Handshake packet, a server's with the first it opens")
    {
        CHECK(pair.client.can_send(QuicPacketType::Initial));
        Buffer datagram;
        REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::Handshake, 0U), kPing) == QuicPacketError::None);
        CHECK_FALSE(pair.client.can_send(QuicPacketType::Initial));
        CHECK_FALSE(pair.client.can_open(QuicPacketType::Initial));
        Buffer initial;
        QuicPacketWriter writer;
        REQUIRE(writer.begin(spec_of(QuicPacketType::Initial, 1U), initial.rest()));
        usize written = 0U;
        CHECK(pair.client.seal(writer, 0U, {}, written) == QuicPacketError::NoKeys);
        CHECK(pair.client.rekey_initial(QuicVersion::V1, dcid) == CryptoError::NotReady);

        CHECK(pair.server.can_open(QuicPacketType::Initial));
        CHECK(deliver(pair.server, datagram) == QuicPacketError::None);
        CHECK_FALSE(pair.server.can_open(QuicPacketType::Initial));
        CHECK_FALSE(pair.server.can_send(QuicPacketType::Initial));
    }
    SECTION("Handshake keys go when the handshake is confirmed")
    {
        CHECK_FALSE(pair.client.confirmed());
        pair.client.confirm();
        pair.server.confirm();
        CHECK(pair.client.confirmed());
        for (const QuicPacketProtection* end : {&pair.client, &pair.server})
        {
            CHECK_FALSE(end->can_send(QuicPacketType::Handshake));
            CHECK_FALSE(end->can_open(QuicPacketType::Handshake));
            CHECK(end->can_send(QuicPacketType::OneRtt));
        }
        CHECK(pair.client.install(QuicLevel::Handshake, true, AlgorithmId::Aes128Gcm, secret(0U)) ==
              CryptoError::NotReady);
        pair.client.discard(QuicLevel::Application);
        CHECK_FALSE(pair.client.can_send(QuicPacketType::OneRtt));
        CHECK_FALSE(pair.client.can_open(QuicPacketType::OneRtt));
    }
    SECTION("a Retry's new connection ID and a compatible version change re-derive the Initial keys")
    {
        const ConstSpan<u8> retried(kOtherDcid, kCid);
        REQUIRE(pair.client.rekey_initial(QuicVersion::V2, retried) == CryptoError::None);
        QuicPacketProtection server;
        REQUIRE(server.start(QuicSide::Server, QuicVersion::V2, retried) == CryptoError::None);
        Buffer datagram;
        REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::Initial, 0U, QuicVersion::V2), kPing) ==
                QuicPacketError::None);
        CHECK(deliver(server, datagram) == QuicPacketError::None);
        CHECK(deliver(pair.server, datagram) == QuicPacketError::Forged); // the old keys no longer open it
    }
    SECTION("installs that do not fit the connection are refused")
    {
        QuicPacketProtection idle;
        CHECK(idle.install(QuicLevel::Handshake, true, AlgorithmId::Aes128Gcm, secret(0U)) == CryptoError::NotReady);
        CHECK(idle.rekey_initial(QuicVersion::V1, dcid) == CryptoError::NotReady);
        CHECK(pair.client.install(QuicLevel::Initial, true, AlgorithmId::Aes128Gcm, secret(0U)) ==
              CryptoError::Unsupported);
        CHECK(pair.client.install(QuicLevel::Handshake, true, AlgorithmId::Aes128Gcm, secret(0U)) ==
              CryptoError::Exists);
        CHECK(pair.client.install(QuicLevel::Application, false, AlgorithmId::Aes128Gcm, secret(0U)) ==
              CryptoError::Exists);
        CHECK(early.client.install(QuicLevel::Application, true, AlgorithmId::Ed25519, secret(0U)) ==
              CryptoError::Unsupported);
        CHECK(early.client.install(QuicLevel::Handshake, true, AlgorithmId::Aes256Gcm, secret(0U)) ==
              CryptoError::BadLength);
        CHECK_FALSE(early.client.can_send(QuicPacketType::OneRtt));
        CHECK_FALSE(early.client.can_send(QuicPacketType::Retry));
        CHECK_FALSE(early.client.can_open(QuicPacketType::Retry));
    }
}

TEST_CASE("QUIC packets: nothing read under protection is trusted before the AEAD opens", "[net][4.2a][quic-packet]")
{
    Pair pair;
    start(pair);

    SECTION("reserved bits set by the sender are PROTOCOL_VIOLATION, after the packet opens")
    {
        for (const QuicPacketType type : {QuicPacketType::Handshake, QuicPacketType::OneRtt})
        {
            Buffer datagram;
            QuicPacketWriter writer;
            REQUIRE(writer.begin(spec_of(type, 0U), datagram.rest()));
            put_frame(writer, kPing[0]);
            datagram.bytes[0] = static_cast<u8>(datagram.bytes[0] | (type == QuicPacketType::OneRtt ? 0x10U : 0x08U));
            REQUIRE(pair.client.seal(writer, 1U, {}, datagram.size) == QuicPacketError::None);
            CHECK(deliver(pair.server, datagram) == QuicPacketError::ReservedBits);
        }
        CHECK(quic_packet_close(QuicPacketError::ReservedBits) == QuicTransportError::ProtocolViolation);
    }
    SECTION("a cleared QUIC bit is dropped until this side sent grease_quic_bit")
    {
        CHECK_FALSE(quic_grease_bit_clear(false, 1U));
        CHECK_FALSE(quic_grease_bit_clear(true, 2U));
        CHECK(quic_grease_bit_clear(true, 3U));
        Buffer datagram;
        QuicPacketSpec spec = spec_of(QuicPacketType::OneRtt, 0U);
        spec.clear_quic_bit = true;
        spec.spin = true;
        REQUIRE(append(pair.client, datagram, spec, kPing) == QuicPacketError::None);
        CHECK((datagram.bytes[0] & 0x40U) == 0U);
        CHECK(deliver(pair.server, datagram) == QuicPacketError::QuicBit);
        pair.server.accept_cleared_quic_bit(true);
        QuicOpened opened;
        Bytes copy;
        REQUIRE(deliver(pair.server, datagram, opened, copy) == QuicPacketError::None);
        CHECK(opened.spin);
    }
    SECTION("a forged packet moves nothing, and failed opens reach the integrity limit")
    {
        Buffer datagram;
        Buffer forged;
        REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, 0U), kPing) == QuicPacketError::None);
        REQUIRE(append(pair.client, forged, spec_of(QuicPacketType::OneRtt, 1U), kPing) == QuicPacketError::None);
        forged.bytes[forged.size - 1U] ^= 1U;
        CHECK(deliver(pair.server, forged) == QuicPacketError::Forged);
        CHECK(pair.server.failed_opens() == 1U);
        u64 largest = 0U;
        CHECK_FALSE(pair.server.largest_opened(QuicSpace::Application, largest));
        CHECK(deliver(pair.server, datagram) == QuicPacketError::None);
        CHECK(deliver(pair.server, datagram) == QuicPacketError::Duplicate);

        pair.server.limit_failed_opens(3U);
        CHECK(deliver(pair.server, forged) == QuicPacketError::Forged);
        CHECK(deliver(pair.server, forged) == QuicPacketError::IntegrityLimit);
        CHECK(quic_packet_close(QuicPacketError::IntegrityLimit) == QuicTransportError::AeadLimitReached);
        CHECK(quic_packet_close(QuicPacketError::Forged) == QuicTransportError::NoError);
    }
    SECTION("a packet older than the window is stale, and an unexpected type or extent is refused")
    {
        for (const u64 number : {0U, 300U})
        {
            Buffer datagram;
            REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, number), kPing) ==
                    QuicPacketError::None);
            REQUIRE(deliver(pair.server, datagram) == QuicPacketError::None);
        }
        Buffer late;
        QuicPacketSpec spec = spec_of(QuicPacketType::OneRtt, 50U);
        spec.number_bytes = 2U;
        REQUIRE(append(pair.client, late, spec, kPing) == QuicPacketError::None);
        CHECK(deliver(pair.server, late) == QuicPacketError::Stale);

        QuicHeader header;
        Buffer handshake;
        REQUIRE(append(pair.client, handshake, spec_of(QuicPacketType::Handshake, 0U), kPing) == QuicPacketError::None);
        REQUIRE(parse_quic_header(handshake.all(), kCid, header) == QuicHeaderError::None);
        QuicOpened opened;
        CHECK(pair.server.open(header, handshake.all().first(handshake.size - 1U), opened) ==
              QuicPacketError::Unprotectable);
        header.type = QuicPacketType::Retry;
        CHECK(pair.server.open(header, handshake.all(), opened) == QuicPacketError::NoKeys);
        QuicPacketProtection fresh;
        REQUIRE(fresh.start(QuicSide::Server, QuicVersion::V1, ConstSpan<u8>(kDcid, kCid)) == CryptoError::None);
        CHECK(deliver(fresh, handshake) == QuicPacketError::NoKeys);
    }
}

TEST_CASE("QUIC packets: key updates are initiated, followed and checked as RFC 9001 section 6 asks",
          "[net][4.2a][quic-packet]")
{
    Pair pair;
    start(pair);
    QuicKeys raw; // the client's 1-RTT keys of phase 0, for packets the client itself would never send
    REQUIRE(raw.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret(2U)) == CryptoError::None);
    crd::crypto::QuicPacketKey phase1;
    crd::crypto::QuicPacketKey phase2;
    REQUIRE(phase1.update(raw.packet) == CryptoError::None);
    REQUIRE(phase2.update(phase1) == CryptoError::None);

    const auto send = [](QuicPacketProtection& from, u64 number)
    {
        Buffer datagram;
        REQUIRE(append(from, datagram, spec_of(QuicPacketType::OneRtt, number), kPing) == QuicPacketError::None);
        return datagram;
    };

    SECTION("an update waits for confirmation and an acknowledgement in the current phase, then both ends move")
    {
        const Buffer first = send(pair.client, 0U);
        const Buffer third = send(pair.client, 2U);
        const Buffer delayed = send(pair.client, 1U);
        REQUIRE(deliver(pair.server, first) == QuicPacketError::None);
        REQUIRE(deliver(pair.server, third) == QuicPacketError::None);
        pair.client.confirm();
        pair.server.confirm();
        CHECK_FALSE(pair.client.update_keys()); // nothing of phase 0 acknowledged
        pair.client.acknowledged(0U);
        REQUIRE(pair.client.can_update_keys());
        REQUIRE(pair.client.update_keys());
        CHECK(pair.client.send_phase() == 1U);
        CHECK_FALSE(pair.client.can_update_keys()); // nothing of phase 1 acknowledged, the peer has not answered

        const Buffer updated = send(pair.client, 3U);
        QuicOpened opened;
        Bytes copy;
        REQUIRE(deliver(pair.server, updated, opened, copy) == QuicPacketError::None);
        CHECK(opened.key_update);
        CHECK(pair.server.receive_phase() == 1U);
        CHECK(pair.server.send_phase() == 1U); // it answers in the new phase
        CHECK(pair.server.previous_held());
        REQUIRE(deliver(pair.server, delayed, opened, copy) == QuicPacketError::None); // a late phase 0 packet
        CHECK_FALSE(opened.key_update);

        const Buffer answer = send(pair.server, 0U);
        REQUIRE(deliver(pair.client, answer, opened, copy) == QuicPacketError::None);
        CHECK(opened.key_update);
        CHECK(pair.client.send_phase() == 1U);
        CHECK(pair.client.receive_phase() == 1U);
        pair.client.discard_previous();
        CHECK_FALSE(pair.client.previous_held());
        pair.client.acknowledged(2U); // a phase 0 packet: phase 1 is not acknowledged yet
        CHECK_FALSE(pair.client.can_update_keys());
        pair.client.acknowledged(3U);
        CHECK(pair.client.can_update_keys());
    }
    SECTION("an update waits for the handshake's confirmation and for the previous receive key to go")
    {
        REQUIRE(deliver(pair.server, send(pair.client, 0U)) == QuicPacketError::None);
        pair.client.acknowledged(0U);
        CHECK_FALSE(pair.client.update_keys()); // not confirmed
        pair.client.confirm();
        pair.server.confirm();
        REQUIRE(pair.client.update_keys());
        REQUIRE(deliver(pair.server, send(pair.client, 1U)) == QuicPacketError::None);
        QuicOpened opened;
        Bytes copy;
        REQUIRE(deliver(pair.client, send(pair.server, 0U), opened, copy) == QuicPacketError::None);
        pair.client.acknowledged(1U);
        CHECK_FALSE(pair.client.can_update_keys()); // its previous receive key is still held
        pair.client.discard_previous();
        CHECK(pair.client.update_keys());
        CHECK(pair.client.send_phase() == 2U);
    }
    SECTION("a second update before this side acknowledged the first in the new phase is KEY_UPDATE_ERROR")
    {
        REQUIRE(deliver(pair.server, crafted(raw.packet, raw.header, false, 0U)) == QuicPacketError::None);
        REQUIRE(deliver(pair.server, crafted(phase1, raw.header, true, 1U)) == QuicPacketError::None);
        pair.server.discard_previous();
        const Buffer second = crafted(phase2, raw.header, false, 2U);
        CHECK(deliver(pair.server, second) == QuicPacketError::KeyUpdate);
        CHECK(quic_packet_close(QuicPacketError::KeyUpdate) == QuicTransportError::KeyUpdateError);
        pair.server.sent_acknowledgement();
        QuicOpened opened;
        Bytes copy;
        REQUIRE(deliver(pair.server, second, opened, copy) == QuicPacketError::None);
        CHECK(opened.key_update);
        CHECK(pair.server.receive_phase() == 2U);
        CHECK(pair.server.send_phase() == 2U);
    }
    SECTION("newer keys on a lower packet number than older keys used is KEY_UPDATE_ERROR")
    {
        REQUIRE(deliver(pair.server, crafted(raw.packet, raw.header, false, 0U)) == QuicPacketError::None);
        REQUIRE(deliver(pair.server, crafted(raw.packet, raw.header, false, 5U)) == QuicPacketError::None);
        CHECK(deliver(pair.server, crafted(phase1, raw.header, true, 3U)) == QuicPacketError::KeyUpdate);

        REQUIRE(deliver(pair.server, crafted(phase1, raw.header, true, 6U)) == QuicPacketError::None);
        CHECK(deliver(pair.server, crafted(phase1, raw.header, true, 4U)) == QuicPacketError::KeyUpdate);
        // An older key above the newer key's first packet is tried as the next phase's and so never opens.
        CHECK(deliver(pair.server, crafted(raw.packet, raw.header, false, 9U)) == QuicPacketError::Forged);
        CHECK(deliver(pair.server, crafted(raw.packet, raw.header, false, 2U)) == QuicPacketError::None);
        pair.server.discard_previous();
        CHECK(deliver(pair.server, crafted(raw.packet, raw.header, false, 1U)) == QuicPacketError::Forged);
    }
}

TEST_CASE("QUIC packets: a key reaching its update interval updates itself, or refuses once it cannot",
          "[net][4.2a][quic-packet]")
{
    Pair pair;
    start(pair);
    pair.client.confirm();
    pair.server.confirm();
    pair.client.set_key_update_interval(8U);
    u64 number = 0U;
    for (; number < 6U; ++number)
    {
        Buffer datagram;
        REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, number), kPing) == QuicPacketError::None);
        REQUIRE(deliver(pair.server, datagram) == QuicPacketError::None);
        pair.client.acknowledged(number);
    }
    CHECK(pair.client.send_phase() == 0U);
    Buffer datagram;
    REQUIRE(append(pair.client, datagram, spec_of(QuicPacketType::OneRtt, number++), kPing) == QuicPacketError::None);
    CHECK(pair.client.send_phase() == 1U); // three quarters of the interval sealed: the update came first
    REQUIRE(deliver(pair.server, datagram) == QuicPacketError::None);

    // Nothing of phase 1 is acknowledged, so it cannot update again and its key refuses at the interval.
    for (usize sealed = 0U; sealed < 7U; ++sealed)
    {
        Buffer more;
        REQUIRE(append(pair.client, more, spec_of(QuicPacketType::OneRtt, number++), kPing) == QuicPacketError::None);
    }
    Buffer refused;
    CHECK(append(pair.client, refused, spec_of(QuicPacketType::OneRtt, number), kPing) == QuicPacketError::Exhausted);
    CHECK(quic_packet_close(QuicPacketError::Exhausted) == QuicTransportError::AeadLimitReached);
    pair.client.set_key_update_interval(0U);
    CHECK(append(pair.client, refused, spec_of(QuicPacketType::OneRtt, number), kPing) == QuicPacketError::None);
}

namespace
{
constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519Mlkem768, AlgorithmId::X25519};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Chacha20Poly1305};
constexpr StringView kProtocols[] = {"cerid-game/1"};
constexpr const char* kServerName = "game.cerid.test";
constexpr u8 kParameters[] = {0x04U, 0x04U, 0x80U, 0x10U, 0x00U, 0x00U}; // initial_max_data, as bytes
constexpr QuicLevel kCryptoLevels[] = {QuicLevel::Initial, QuicLevel::Handshake, QuicLevel::Application};

// One end of a connection: its TLS handshake and its packet protection, and the CRYPTO stream offsets per level.
struct End
{
    SigningKey key;
    TlsCredential credential;
    TlsContext context;
    QuicHandshake handshake;
    QuicPacketProtection protection;
    u64 sent[3] = {};
    u64 received[3] = {};
    u64 next[3] = {}; // the next packet number per space
};

QuicPacketType packet_type(QuicLevel level)
{
    if (level == QuicLevel::Initial)
    {
        return QuicPacketType::Initial;
    }
    return (level == QuicLevel::Handshake) ? QuicPacketType::Handshake : QuicPacketType::OneRtt;
}

void configure(End& client, End& server)
{
    u8 seed[32];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(i + 1U);
    }
    REQUIRE(server.key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) == CryptoError::None);
    REQUIRE(server.credential.init(server.key, kServerName) == TlsError::None);
    const KeyId pin = server.credential.key_id();

    TlsConfig client_config;
    client_config.role = TlsRole::Client;
    client_config.groups = kGroups;
    client_config.signatures = kSignatures;
    client_config.aeads = kAeads;
    client_config.pinned = ConstSpan<KeyId>(&pin, 1U);
    client_config.server_name = kServerName;
    client_config.application_protocols = kProtocols;
    REQUIRE(client.context.init(client_config) == TlsError::None);

    TlsConfig server_config;
    server_config.role = TlsRole::Server;
    server_config.groups = kGroups;
    server_config.aeads = kAeads;
    server_config.credential = &server.credential;
    server_config.server_name = kServerName;
    server_config.application_protocols = kProtocols;
    REQUIRE(server.context.init(server_config) == TlsError::None);
}

// Sends what `from` has to send, every level's CRYPTO data in packets of one datagram each (an Initial datagram
// padded to 1200 bytes), and has `to` open them, provide the CRYPTO data and install the keys it gains. The datagrams
// sent.
usize exchange(End& from, End& to, QuicSide side)
{
    usize datagrams = 0U;
    REQUIRE(from.protection.install(from.handshake) == CryptoError::None);
    for (const QuicLevel level : kCryptoLevels)
    {
        const usize index = static_cast<usize>(level);
        while (from.handshake.pending(level) > 0U)
        {
            Buffer datagram;
            QuicPacketWriter writer;
            QuicPacketSpec spec = spec_of(packet_type(level), from.next[index]++);
            REQUIRE(writer.begin(spec, datagram.rest()));
            u8 chunk[1000];
            usize taken = 0U;
            REQUIRE(from.handshake.take(level, Span<u8>(chunk, sizeof(chunk)), taken) == TlsError::None);
            QuicFrame frame;
            frame.kind = QuicFrameKind::Crypto;
            frame.offset = from.sent[index];
            frame.data = ConstSpan<u8>(chunk, taken);
            const usize used = write_quic_frame(frame, writer.frames());
            REQUIRE(used != 0U);
            from.sent[index] += taken;
            const bool initial = level == QuicLevel::Initial;
            REQUIRE(from.protection.seal(writer, used, {quic_datagram_floor(initial, 0U), 0U}, datagram.size) ==
                    QuicPacketError::None);
            ++datagrams;

            QuicDatagramPackets packets(datagram.all(), kCid);
            QuicHeader header;
            Span<u8> packet;
            QuicOpened opened;
            REQUIRE(packets.next(header, packet));
            REQUIRE(to.protection.open(header, packet, opened) == QuicPacketError::None);
            WireReader in(opened.payload);
            while (!in.done())
            {
                QuicFrame read;
                REQUIRE(read_quic_frame(in, read) == QuicTransportError::NoError);
                REQUIRE(quic_frame_allowed(read.kind, read.application, opened.type, side == QuicSide::Server));
                if (read.kind == QuicFrameKind::Crypto)
                {
                    REQUIRE(read.offset == to.received[index]);
                    to.received[index] += read.data.size();
                    REQUIRE(to.handshake.provide(level, read.data) == TlsError::None);
                }
            }
            REQUIRE(to.protection.install(to.handshake) == CryptoError::None);
        }
    }
    return datagrams;
}

// A 1-RTT packet carrying `frame` from one end to the other; what the receiver read.
QuicFrameKind one_rtt(End& from, End& to, const QuicFrame& frame)
{
    Buffer datagram;
    QuicPacketWriter writer;
    REQUIRE(writer.begin(spec_of(QuicPacketType::OneRtt, from.next[2]++), datagram.rest()));
    const usize used = write_quic_frame(frame, writer.frames());
    REQUIRE(from.protection.seal(writer, used, {}, datagram.size) == QuicPacketError::None);
    QuicOpened opened;
    Bytes copy;
    REQUIRE(deliver(to.protection, datagram, opened, copy) == QuicPacketError::None);
    WireReader in(opened.payload);
    QuicFrame read;
    REQUIRE(read_quic_frame(in, read) == QuicTransportError::NoError);
    return read.kind;
}
} // namespace

TEST_CASE("QUIC packets: a TLS handshake carried in these packets reaches 1-RTT in both versions",
          "[net][4.2a][quic-packet]")
{
    for (const QuicVersion version : kVersions)
    {
        End client;
        End server;
        configure(client, server);
        const ConstSpan<u8> dcid(kDcid, kCid);
        REQUIRE(client.handshake.init(client.context, ConstSpan<u8>(kParameters)) == TlsError::None);
        REQUIRE(server.handshake.init(server.context, ConstSpan<u8>(kParameters)) == TlsError::None);
        REQUIRE(client.protection.start(QuicSide::Client, version, dcid) == CryptoError::None);
        REQUIRE(server.protection.start(QuicSide::Server, version, dcid) == CryptoError::None);

        // The hybrid ClientHello takes two Initial datagrams; the server answers at Initial and Handshake.
        CHECK(exchange(client, server, QuicSide::Client) == 2U);
        CHECK(exchange(server, client, QuicSide::Server) >= 2U);
        CHECK(client.protection.can_open(QuicPacketType::Handshake));
        CHECK(server.protection.can_send(QuicPacketType::OneRtt)); // half-RTT keys come before the client's Finished
        CHECK_FALSE(server.protection.can_open(QuicPacketType::OneRtt));
        CHECK(exchange(client, server, QuicSide::Client) >= 1U);
        CHECK(client.handshake.state() == TlsState::Established);
        CHECK(server.handshake.state() == TlsState::Established);
        (void)exchange(server, client, QuicSide::Server); // the server's tickets at the Application level

        CHECK_FALSE(client.protection.can_send(QuicPacketType::Initial)); // its Handshake packet ended them
        CHECK_FALSE(server.protection.can_open(QuicPacketType::Initial)); // the first Handshake packet it opened

        // The server confirms on completion and says so with HANDSHAKE_DONE; the client confirms on reading it.
        server.protection.confirm();
        QuicFrame done;
        done.kind = QuicFrameKind::HandshakeDone;
        REQUIRE(one_rtt(server, client, done) == QuicFrameKind::HandshakeDone);
        client.protection.confirm();
        for (const End* end : {&client, &server})
        {
            CHECK_FALSE(end->protection.can_send(QuicPacketType::Handshake));
            CHECK_FALSE(end->protection.can_open(QuicPacketType::Handshake));
        }
        QuicFrame ping;
        ping.kind = QuicFrameKind::Ping;
        CHECK(one_rtt(client, server, ping) == QuicFrameKind::Ping);
        CHECK(one_rtt(server, client, ping) == QuicFrameKind::Ping);
    }
}
