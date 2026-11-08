// crd-fuzz-net-ingress: QUIC headers and admission stages S1 to S3 of crd-net (4.2a;
// docs/design/network-foundation.md#ingress) under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: byte 0 picks the length of the server's connection IDs (8 + byte % 13), whether every address must be proven
// (bit 7) and the peer's family (bit 6); the rest is one datagram.
//
// Oracles. A header that parses holds views inside the datagram, a packet extent within it and, for a protected
// packet, room for the header protection sample. A varint read from the datagram writes back in its shortest form and
// reads again to the same value. Through a server: Route goes only to a connection the table holds, Admit only for a
// client Initial of at least 1200 bytes with an 8-byte Destination Connection ID, and every answer is a datagram of
// at least 1200 bytes answered in at most three times its size (RFC 9000 section 8), which parses back as Version
// Negotiation (listing the server's versions and one reserved version) or as a Retry whose integrity tag verifies.
// A Retry token that does not validate is answered, below its datagram's size, with one server Initial that the client
// opens under the keys of the Destination Connection ID it chose, holding CONNECTION_CLOSE with INVALID_TOKEN and
// PADDING alone; an admitted Initial a server refuses gets the same close with CONNECTION_REFUSED. Early goes only to
// 0-RTT for no connection under a Destination Connection ID of at least 8 bytes, and is never answered.
// A stateless reset answers only a short header for no connection: shorter than it (section 10.3.3), one byte
// shorter up to 43 bytes, at least 21 and at most kQuicMaxResetAnswerBytes, with the fixed bits 01 and the token of
// its Destination Connection ID last.
#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/quic_packet.hpp>

#include <cstring>

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using namespace crd::net;

constexpr u8 kKey[crd::crypto::kPrfKeyBytes] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                                 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x0F};
constexpr u8 kTokenKey[crd::crypto::kQuicTokenKeyBytes] = {
    0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70};
constexpr u8 kResetKey[crd::crypto::kQuicResetKeyBytes] = {
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80,
    0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90};
constexpr u64 kUnix = 1793577600U;
constexpr u8 kRouted = 0x5AU; // every ID of this byte repeated routes to connection 7

bool inside(ConstSpan<u8> view, ConstSpan<u8> datagram)
{
    return view.empty() ||
           (view.data() >= datagram.data() && view.data() + view.size() <= datagram.data() + datagram.size());
}

void check_header(ConstSpan<u8> datagram, usize cid_bytes)
{
    QuicHeader header;
    if (parse_quic_header(datagram, cid_bytes, header) != QuicHeaderError::None)
    {
        return;
    }
    CRD_FUZZ_REQUIRE(inside(header.dcid, datagram) && inside(header.scid, datagram) && inside(header.token, datagram));
    CRD_FUZZ_REQUIRE(header.packet_bytes <= datagram.size());
    CRD_FUZZ_REQUIRE(header.first == datagram[0]);
    switch (header.type)
    {
    case QuicPacketType::OneRtt:
        CRD_FUZZ_REQUIRE(header.dcid.size() == cid_bytes);
        [[fallthrough]];
    case QuicPacketType::Initial:
    case QuicPacketType::ZeroRtt:
    case QuicPacketType::Handshake:
        CRD_FUZZ_REQUIRE(header.number_offset + kQuicMinProtectedBytes <= header.packet_bytes);
        break;
    case QuicPacketType::Retry:
    case QuicPacketType::VersionNegotiation:
    case QuicPacketType::OtherVersion:
        CRD_FUZZ_REQUIRE(header.packet_bytes == datagram.size());
        break;
    }
    if (header.type != QuicPacketType::OneRtt && crd::crypto::quic_version_supported(header.version))
    {
        CRD_FUZZ_REQUIRE(header.dcid.size() <= crd::crypto::kQuicMaxConnectionIdBytes);
        CRD_FUZZ_REQUIRE(header.scid.size() <= crd::crypto::kQuicMaxConnectionIdBytes);
    }
}

void check_varint(ConstSpan<u8> datagram)
{
    WireReader in(datagram);
    u64 value = 0U;
    if (!take_quic_varint(in, value))
    {
        CRD_FUZZ_REQUIRE(in.consumed() == 0U);
        return;
    }
    u8 bytes[8] = {};
    WireWriter out{Span<u8>(bytes)};
    put_quic_varint(out, value);
    CRD_FUZZ_REQUIRE(out.written() == quic_varint_bytes(value) && out.written() <= in.consumed());
    WireReader again(ConstSpan<u8>(bytes, out.written()));
    u64 read = 0U;
    CRD_FUZZ_REQUIRE(take_quic_varint(again, read) && read == value && again.done());
}

void check_reset(const IngressDecision& decision, ConstSpan<u8> datagram, const crd::crypto::QuicResetKey& key,
                 ConstSpan<u8> answer)
{
    CRD_FUZZ_REQUIRE(decision.header.type == QuicPacketType::OneRtt && decision.datagram_bytes == datagram.size());
    CRD_FUZZ_REQUIRE(answer.size() < datagram.size() && answer.size() >= kQuicMinResetBytes);
    CRD_FUZZ_REQUIRE(answer.size() <= kQuicMaxResetAnswerBytes);
    CRD_FUZZ_REQUIRE(datagram.size() > 43U || answer.size() == datagram.size() - 1U);
    CRD_FUZZ_REQUIRE(datagram.size() <= 43U || answer.size() >= 43U);
    CRD_FUZZ_REQUIRE((answer[0] & 0xC0U) == 0x40U);
    u8 token[crd::crypto::kQuicResetTokenBytes];
    CRD_FUZZ_REQUIRE(key.token(decision.header.dcid, Span<u8>(token)) == crd::crypto::CryptoError::None);
    CRD_FUZZ_REQUIRE(std::memcmp(token, answer.data() + answer.size() - sizeof(token), sizeof(token)) == 0);
}

void check_answer(const IngressDecision& decision, ConstSpan<u8> datagram, const IngressConfig& config,
                  ConstSpan<u8> answer)
{
    CRD_FUZZ_REQUIRE(datagram.size() >= kQuicMinInitialDatagram);
    CRD_FUZZ_REQUIRE(answer.size() <= 3U * datagram.size()); // RFC 9000 section 8's amplification limit
    QuicHeader read;
    CRD_FUZZ_REQUIRE(parse_quic_header(answer, config.local_cid_bytes, read) == QuicHeaderError::None);
    if (decision.action == IngressAction::NegotiateVersion)
    {
        CRD_FUZZ_REQUIRE(read.type == QuicPacketType::VersionNegotiation);
        CRD_FUZZ_REQUIRE(read.dcid.size() == decision.header.scid.size());
        const usize list = 1U + 4U + 1U + read.dcid.size() + 1U + read.scid.size();
        CRD_FUZZ_REQUIRE(answer.size() == list + (4U * (config.version_count + 1U)));
        usize reserved = 0U;
        for (usize at = list; at < answer.size(); at += 4U)
        {
            reserved += quic_version_reserved(crd::containers::wire_load<u32>(answer, at)) ? 1U : 0U;
        }
        CRD_FUZZ_REQUIRE(reserved == 1U);
        return;
    }
    CRD_FUZZ_REQUIRE(read.type == QuicPacketType::Retry);
    CRD_FUZZ_REQUIRE(read.scid.size() == config.local_cid_bytes);
    CRD_FUZZ_REQUIRE(crd::crypto::quic_retry_verify(static_cast<crd::crypto::QuicVersion>(decision.header.version),
                                                    decision.header.dcid, answer) == crd::crypto::CryptoError::None);
}

void check_initial_close(const IngressDecision& decision, ConstSpan<u8> datagram, Span<u8> answer,
                         QuicTransportError error)
{
    CRD_FUZZ_REQUIRE(decision.header.type == QuicPacketType::Initial && datagram.size() >= kQuicMinInitialDatagram);
    CRD_FUZZ_REQUIRE(error != QuicTransportError::InvalidToken ||
                     crd::crypto::quic_token_kind(decision.header.token) == crd::crypto::QuicTokenKind::Retry);
    CRD_FUZZ_REQUIRE(answer.size() < datagram.size());
    QuicPacketProtection client;
    CRD_FUZZ_REQUIRE(client.start(crd::crypto::QuicSide::Client,
                                  static_cast<crd::crypto::QuicVersion>(decision.header.version),
                                  decision.header.dcid) == crd::crypto::CryptoError::None);
    QuicDatagramPackets packets(answer, 0U);
    QuicHeader header;
    Span<u8> packet;
    CRD_FUZZ_REQUIRE(packets.next(header, packet) && header.type == QuicPacketType::Initial);
    CRD_FUZZ_REQUIRE(header.version == decision.header.version);
    CRD_FUZZ_REQUIRE(header.dcid.size() == decision.header.scid.size());
    CRD_FUZZ_REQUIRE(header.dcid.empty() ||
                     std::memcmp(header.dcid.data(), decision.header.scid.data(), header.dcid.size()) == 0);
    QuicOpened opened;
    CRD_FUZZ_REQUIRE(client.open(header, packet, opened) == QuicPacketError::None);
    WireReader frames(opened.payload);
    QuicFrame frame;
    CRD_FUZZ_REQUIRE(read_quic_frame(frames, frame) == QuicTransportError::NoError);
    CRD_FUZZ_REQUIRE(frame.kind == QuicFrameKind::ConnectionClose && !frame.application);
    CRD_FUZZ_REQUIRE(frame.error_code == static_cast<u64>(error));
    while (!frames.done())
    {
        CRD_FUZZ_REQUIRE(read_quic_frame(frames, frame) == QuicTransportError::NoError);
        CRD_FUZZ_REQUIRE(frame.kind == QuicFrameKind::Padding);
    }
    CRD_FUZZ_REQUIRE(!packets.next(header, packet));
}

void check_ingress(ConstSpan<u8> datagram, u8 control)
{
    // One arena for every input: what an input takes, its objects give back when it ends.
    static crd::memory::TlsfAllocator heap(64U * 1024U, nullptr, "fuzz-ingress");
    ConnectionIdTable routes(heap, 32U, ConstSpan<u8>(kKey));
    HandshakeRules rules;
    rules.always_retry = (control & 0x80U) != 0U;
    rules.prefix_buckets = 16U;
    HandshakeBuckets buckets(heap, rules, ConstSpan<u8>(kKey));
    crd::crypto::QuicTokenKeys tokens;
    crd::crypto::ReplayFilter replay;
    crd::crypto::QuicResetKey reset;
    CRD_FUZZ_REQUIRE(routes.ready() && buckets.ready());
    CRD_FUZZ_REQUIRE(reset.init(ConstSpan<u8>(kResetKey)) == crd::crypto::CryptoError::None);
    CRD_FUZZ_REQUIRE(tokens.init(ConstSpan<u8>(kTokenKey), {}) == crd::crypto::CryptoError::None);
    CRD_FUZZ_REQUIRE(replay.init(64U, crd::crypto::kQuicNewTokenLifetime, kUnix) == crd::crypto::CryptoError::None);
    u8 routed[crd::crypto::kQuicMaxConnectionIdBytes];
    std::memset(routed, kRouted, sizeof(routed));
    for (usize length = 0U; length <= sizeof(routed); ++length)
    {
        CRD_FUZZ_REQUIRE(routes.insert(ConstSpan<u8>(routed, length), 7U) == RouteError::None);
    }
    IngressConfig config;
    config.local_cid_bytes = 8U + (control % 13U);
    Ingress ingress(config, routes, buckets, &tokens, &replay, &reset);
    CRD_FUZZ_REQUIRE(ingress.ready());

    const u8 v6[16] = {0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const Endpoint peer{((control & 0x40U) != 0U) ? IpAddress::v6(v6) : IpAddress::v4(192, 0, 2, 1), 4433U};
    const IngressDecision decision = ingress.classify(datagram, false, peer, 1000000000U, kUnix);
    switch (decision.action)
    {
    case IngressAction::Drop:
        CRD_FUZZ_REQUIRE(decision.reason < DropReason::Count);
        break;
    case IngressAction::Route:
        CRD_FUZZ_REQUIRE(decision.connection == 7U && routes.find(decision.header.dcid) == 7U);
        break;
    case IngressAction::Admit:
    {
        CRD_FUZZ_REQUIRE(decision.header.type == QuicPacketType::Initial);
        CRD_FUZZ_REQUIRE(datagram.size() >= kQuicMinInitialDatagram);
        CRD_FUZZ_REQUIRE(decision.header.dcid.size() >= kQuicMinClientDcidBytes);
        u8 answer[kQuicMaxStatelessAnswerBytes] = {};
        const usize written = ingress.refuse(decision, 1000000000U, Span<u8>(answer));
        CRD_FUZZ_REQUIRE(written != 0U);
        check_initial_close(decision, datagram, Span<u8>(answer, written), QuicTransportError::ConnectionRefused);
        break;
    }
    case IngressAction::Early:
        CRD_FUZZ_REQUIRE(decision.header.type == QuicPacketType::ZeroRtt);
        CRD_FUZZ_REQUIRE(decision.header.dcid.size() >= kQuicMinClientDcidBytes);
        CRD_FUZZ_REQUIRE(routes.find(decision.header.dcid) == kNoConnection);
        break;
    case IngressAction::Retry:
    case IngressAction::NegotiateVersion:
    {
        u8 answer[kQuicMaxStatelessAnswerBytes] = {};
        const usize written = ingress.answer(decision, peer, kUnix, Span<u8>(answer));
        CRD_FUZZ_REQUIRE(written != 0U);
        check_answer(decision, datagram, config, ConstSpan<u8>(answer, written));
        break;
    }
    case IngressAction::StatelessReset:
    {
        u8 answer[kQuicMaxStatelessAnswerBytes] = {};
        const usize written = ingress.answer(decision, peer, kUnix, Span<u8>(answer));
        CRD_FUZZ_REQUIRE(written != 0U);
        check_reset(decision, datagram, reset, ConstSpan<u8>(answer, written));
        break;
    }
    case IngressAction::InvalidToken:
    {
        u8 answer[kQuicMaxStatelessAnswerBytes] = {};
        const usize written = ingress.answer(decision, peer, kUnix, Span<u8>(answer));
        CRD_FUZZ_REQUIRE(written != 0U);
        check_initial_close(decision, datagram, Span<u8>(answer, written), QuicTransportError::InvalidToken);
        break;
    }
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const ConstSpan<u8> datagram(data + 1U, size - 1U);
    check_header(datagram, 8U + (data[0] % 13U));
    check_varint(datagram);
    check_ingress(datagram, data[0]);
    return 0;
}

namespace
{
// A long header packet of `total` bytes after the control byte, its Length covering the rest; an Initial carries
// `token_bytes` of a token that claims to be a Retry token.
usize long_seed(u8* out, u8 control, u8 first, u32 version, usize dcid_bytes, usize total, usize token_bytes = 0U)
{
    out[0] = control;
    WireWriter writer(Span<u8>(out + 1U, total));
    writer.put_u8(first);
    writer.put_u32(version);
    writer.put_u8(static_cast<u8>(dcid_bytes));
    for (usize index = 0U; index < dcid_bytes; ++index)
    {
        writer.put_u8(static_cast<u8>(0x80U + index));
    }
    writer.put_u8(4U);
    writer.put_u32(0xC1C2C3C4U);
    if ((first & 0x30U) == ((version == static_cast<u32>(crd::crypto::QuicVersion::V2)) ? 0x10U : 0x00U))
    {
        writer.put_u8(static_cast<u8>(token_bytes)); // a one-byte varint: under 64
        for (usize index = 0U; index < token_bytes; ++index)
        {
            writer.put_u8((index == 0U) ? static_cast<u8>(crd::crypto::QuicTokenKind::Retry) : u8{0x5C});
        }
    }
    const usize header = writer.written() + 2U;
    writer.put_u16(static_cast<u16>(0x4000U | (total - header)));
    while (writer.written() < total)
    {
        writer.put_u8(0xA5U);
    }
    return 1U + total;
}
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    static u8 seed[1U + 1300U];
    const u32 v1 = static_cast<u32>(crd::crypto::QuicVersion::V1);
    const u32 v2 = static_cast<u32>(crd::crypto::QuicVersion::V2);
    sink.add("initial-v1", seed, long_seed(seed, 8U, 0xC0U, v1, 8U, 1200U));
    sink.add("initial-v2", seed, long_seed(seed, 8U, 0xD0U, v2, 8U, 1200U));
    sink.add("initial-v1-retry", seed, long_seed(seed, 0x88U, 0xC0U, v1, 8U, 1200U));
    sink.add("initial-v2-retry-v6", seed, long_seed(seed, 0xC8U, 0xD0U, v2, 20U, 1300U));
    sink.add("initial-v1-forged-retry-token", seed, long_seed(seed, 8U, 0xC0U, v1, 8U, 1200U, 40U));
    sink.add("initial-small", seed, long_seed(seed, 8U, 0xC0U, v1, 8U, 1199U));
    sink.add("initial-short-dcid", seed, long_seed(seed, 8U, 0xC0U, v1, 7U, 1200U));
    sink.add("handshake-v1", seed, long_seed(seed, 8U, 0xE0U, v1, 8U, 64U));
    sink.add("zero-rtt-v2", seed, long_seed(seed, 8U, 0xE0U, v2, 8U, 64U));
    sink.add("unsupported-version", seed, long_seed(seed, 8U, 0xC0U, 0x1A2A3A4AU, 8U, 1200U));
    sink.add("unsupported-small", seed, long_seed(seed, 8U, 0xC0U, 0x00000002U, 8U, 300U));
    sink.add("version-negotiation", seed, long_seed(seed, 8U, 0x80U, 0U, 8U, 40U));

    // A short header to a routed ID of each length, and to an unknown one.
    for (u8 control = 0U; control < 13U; control += 4U)
    {
        const usize cid = 8U + control;
        seed[0] = control;
        seed[1] = 0x41U;
        std::memset(seed + 2U, kRouted, cid);
        std::memset(seed + 2U + cid, 0x33, 30U);
        char name[32] = "short-routed-00";
        name[13] = static_cast<char>('0' + (cid / 10U));
        name[14] = static_cast<char>('0' + (cid % 10U));
        sink.add(name, seed, 2U + cid + 30U);
    }
    seed[0] = 8U;
    seed[1] = 0x40U;
    std::memset(seed + 2U, 0x01, 8U + 30U);
    sink.add("short-unknown", seed, 2U + 8U + 30U);
    std::memset(seed + 2U, 0x02, 8U + 190U);
    sink.add("short-unknown-long", seed, 2U + 8U + 190U); // a stateless reset of a random length

    // Variable-length integers of every length.
    const u8 varints[] = {0x00U, 0x25U, 0x7BU, 0xBDU, 0x9DU, 0x7FU, 0x3EU, 0x7DU, 0xC2U, 0x19U, 0x7CU, 0x5EU,
                          0xFFU, 0x14U, 0xE8U, 0x8CU};
    sink.add("varints", varints, sizeof(varints));
}
