// crd-net -- QUIC frames (4.2a; docs/design/network-foundation.md#quic-frames): RFC 9001's appendix A payloads read
// and written byte for byte, every frame kind through a round trip in its shortest encoding, every refusal of
// sections 12.4 and 19 with nothing consumed, ACK ranges against a reference walk, and the frame placement table of
// section 12.4 restated.

#include "ingress_fixture.hpp"
#include "quic_vectors.hpp"

#include <crd/containers/array.hpp>
#include <crd/net/quic_frame.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <random>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using namespace crd::net;
using namespace crd::net::testing;
using namespace crd::quic_vectors;

constexpr u8 kToken[3]       = {0xA1, 0xA2, 0xA3};
constexpr u8 kData[5]        = {0x10, 0x20, 0x30, 0x40, 0x50};
constexpr u8 kId[8]          = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
constexpr u8 kReset[16]      = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
constexpr u8 kPath[8]        = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58};
constexpr u8 kReason[4]      = {'b', 'y', 'e', '!'};
constexpr u8 kZeros[3]       = {}; // what a PADDING frame reads as

bool same_frame(const QuicFrame& a, const QuicFrame& b)
{
    return a.kind == b.kind && a.stream_id == b.stream_id && a.offset == b.offset && a.value == b.value &&
           a.error_code == b.error_code && a.frame_type == b.frame_type && a.sequence == b.sequence &&
           a.retire_prior_to == b.retire_prior_to && same(a.data, b.data) && same(a.reset_token, b.reset_token) &&
           a.fin == b.fin && a.to_end == b.to_end && a.bidirectional == b.bidirectional &&
           a.application == b.application;
}

// One frame of every kind write_quic_frame writes, each with its fields away from zero.
crd::containers::Array<QuicFrame> every_frame()
{
    crd::containers::Array<QuicFrame> frames;
    const auto add = [&](QuicFrameKind kind) -> QuicFrame&
    {
        QuicFrame frame;
        frame.kind = kind;
        frames.push_back(frame);
        return frames.back();
    };
    add(QuicFrameKind::Padding).data = ConstSpan<u8>(kZeros, sizeof(kZeros));
    add(QuicFrameKind::Ping);
    {
        QuicFrame& f = add(QuicFrameKind::ResetStream);
        f.stream_id = 4U;
        f.error_code = 0x1234U;
        f.value = 1U << 20U;
    }
    {
        QuicFrame& f = add(QuicFrameKind::StopSending);
        f.stream_id = 8U;
        f.error_code = 77U;
    }
    {
        QuicFrame& f = add(QuicFrameKind::Crypto);
        f.offset = 1500U;
        f.data = ConstSpan<u8>(kData, sizeof(kData));
    }
    add(QuicFrameKind::NewToken).data = ConstSpan<u8>(kToken, sizeof(kToken));
    for (u8 bits = 0U; bits < 8U; ++bits)
    {
        QuicFrame& f = add(QuicFrameKind::Stream);
        f.stream_id = 0x3FU + bits;
        f.offset = (bits & kQuicStreamBitOffset) != 0U ? 70000U : 0U;
        f.to_end = (bits & kQuicStreamBitLength) == 0U;
        f.fin = (bits & kQuicStreamBitFin) != 0U;
        f.data = ConstSpan<u8>(kData, bits % 2U == 0U ? sizeof(kData) : 0U);
    }
    add(QuicFrameKind::MaxData).value = crd::net::kQuicMaxVarint;
    {
        QuicFrame& f = add(QuicFrameKind::MaxStreamData);
        f.stream_id = 2U;
        f.value = 65536U;
    }
    for (const bool bidirectional : {true, false})
    {
        QuicFrame& limit = add(QuicFrameKind::MaxStreams);
        limit.bidirectional = bidirectional;
        limit.value = kQuicMaxStreams;
        QuicFrame& blocked = add(QuicFrameKind::StreamsBlocked);
        blocked.bidirectional = bidirectional;
        blocked.value = 100U;
    }
    add(QuicFrameKind::DataBlocked).value = 16384U;
    {
        QuicFrame& f = add(QuicFrameKind::StreamDataBlocked);
        f.stream_id = 6U;
        f.value = 1U;
    }
    {
        QuicFrame& f = add(QuicFrameKind::NewConnectionId);
        f.sequence = 9U;
        f.retire_prior_to = 9U;
        f.data = ConstSpan<u8>(kId, sizeof(kId));
        f.reset_token = ConstSpan<u8>(kReset, sizeof(kReset));
    }
    add(QuicFrameKind::RetireConnectionId).sequence = 3U;
    add(QuicFrameKind::PathChallenge).data = ConstSpan<u8>(kPath, sizeof(kPath));
    add(QuicFrameKind::PathResponse).data = ConstSpan<u8>(kPath, sizeof(kPath));
    for (const bool application : {false, true})
    {
        QuicFrame& f = add(QuicFrameKind::ConnectionClose);
        f.application = application;
        f.error_code = application ? 0x4000U : static_cast<u64>(QuicTransportError::ProtocolViolation);
        f.frame_type = application ? 0U : 0x06U;
        f.data = ConstSpan<u8>(kReason, sizeof(kReason));
    }
    add(QuicFrameKind::HandshakeDone);
    for (const bool to_end : {false, true})
    {
        QuicFrame& f = add(QuicFrameKind::Datagram);
        f.to_end = to_end;
        f.data = ConstSpan<u8>(kData, sizeof(kData));
    }
    return frames;
}

QuicTransportError read_one(ConstSpan<u8> bytes, QuicFrame& frame, usize& consumed)
{
    WireReader in(bytes);
    const QuicTransportError error = read_quic_frame(in, frame);
    consumed = in.consumed();
    return error;
}

// ---- RFC 9001's payloads ----

TEST_CASE("QUIC frames: RFC 9001's Initial payloads read and write byte for byte", "[net][quic-frame]")
{
    u8 client[300] = {};
    const usize client_bytes = hex(kClientCryptoFrame, client);
    WireReader in(ConstSpan<u8>(client, client_bytes));
    QuicFrame frame;
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CHECK(in.done());
    CHECK(frame.kind == QuicFrameKind::Crypto);
    CHECK(frame.offset == 0U);
    CHECK(frame.data.size() == 241U);
    CHECK(frame.data.data() == client + 4U); // a view into the payload
    u8 written[300] = {};
    REQUIRE(write_quic_frame(frame, Span<u8>(written, sizeof(written))) == client_bytes);
    CHECK(same(ConstSpan<u8>(written, client_bytes), ConstSpan<u8>(client, client_bytes)));

    u8 server[120] = {};
    const usize server_bytes = hex(kServerPayload, server);
    WireReader payload(ConstSpan<u8>(server, server_bytes));
    REQUIRE(read_quic_frame(payload, frame) == QuicTransportError::NoError);
    REQUIRE(frame.kind == QuicFrameKind::Ack);
    CHECK(frame.ack.largest == 0U);
    CHECK(frame.ack.delay == 0U);
    CHECK(frame.ack.range_count == 0U);
    CHECK(frame.ack.first_range == 0U);
    CHECK_FALSE(frame.ack.ecn);
    QuicAckRanges ranges(frame.ack);
    QuicAckRange range;
    REQUIRE(ranges.next(range));
    CHECK(range.smallest == 0U);
    CHECK(range.largest == 0U);
    CHECK_FALSE(ranges.next(range));
    const QuicAckRange acked[1] = {{0U, 0U}};
    REQUIRE(write_quic_ack(ConstSpan<QuicAckRange>(acked, 1U), 0U, nullptr, Span<u8>(written, sizeof(written))) == 5U);
    CHECK(same(ConstSpan<u8>(written, 5U), ConstSpan<u8>(server, 5U)));

    REQUIRE(read_quic_frame(payload, frame) == QuicTransportError::NoError);
    CHECK(frame.kind == QuicFrameKind::Crypto);
    CHECK(frame.data.size() == 0x5AU);
    CHECK(payload.done());
}

// ---- Round trips ----

TEST_CASE("QUIC frames: every kind round trips in its shortest encoding", "[net][quic-frame]")
{
    for (const QuicFrame& frame : every_frame())
    {
        const usize bytes = quic_frame_bytes(frame);
        REQUIRE(bytes != 0U);
        u8 out[128] = {};
        REQUIRE(write_quic_frame(frame, Span<u8>(out, sizeof(out))) == bytes);
        // One byte short writes nothing.
        u8 short_out[128];
        std::memset(short_out, 0xEE, sizeof(short_out));
        CHECK(write_quic_frame(frame, Span<u8>(short_out, bytes - 1U)) == 0U);
        bool untouched = true;
        for (const u8 byte : short_out)
        {
            untouched = untouched && byte == 0xEEU;
        }
        CHECK(untouched);

        QuicFrame read;
        usize consumed = 0U;
        REQUIRE(read_one(ConstSpan<u8>(out, bytes), read, consumed) == QuicTransportError::NoError);
        CHECK(consumed == bytes);
        CHECK(same_frame(read, frame));
        CHECK(quic_frame_bytes(read) == bytes);
    }
}

TEST_CASE("QUIC frames: a run of PADDING is one frame and the next frame follows it", "[net][quic-frame]")
{
    const u8 payload[5] = {0x00, 0x00, 0x00, 0x01, 0x00};
    WireReader in(ConstSpan<u8>(payload, sizeof(payload)));
    QuicFrame frame;
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CHECK(frame.kind == QuicFrameKind::Padding);
    CHECK(frame.data.size() == 3U);
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CHECK(frame.kind == QuicFrameKind::Ping);
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CHECK(frame.data.size() == 1U);
    CHECK(in.done());

    QuicFrame padding;
    padding.kind = QuicFrameKind::Padding;
    CHECK(quic_frame_bytes(padding) == 0U); // an empty PADDING frame is refused
    u8 out[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    padding.data = ConstSpan<u8>(kData, 3U);
    REQUIRE(write_quic_frame(padding, Span<u8>(out, sizeof(out))) == 3U);
    CHECK((out[0] == 0U && out[1] == 0U && out[2] == 0U && out[3] == 0xFFU));
}

// ---- Refusals ----

TEST_CASE("QUIC frames: every truncation is FRAME_ENCODING_ERROR and consumes nothing", "[net][quic-frame]")
{
    for (const QuicFrame& frame : every_frame())
    {
        // Frames without a length end where the packet does, so no cut of theirs is malformed.
        if (frame.to_end || frame.kind == QuicFrameKind::Padding)
        {
            continue;
        }
        u8 out[128] = {};
        const usize bytes = write_quic_frame(frame, Span<u8>(out, sizeof(out)));
        REQUIRE(bytes != 0U);
        for (usize cut = 1U; cut < bytes; ++cut)
        {
            QuicFrame read;
            usize consumed = 99U;
            CHECK(read_one(ConstSpan<u8>(out, cut), read, consumed) == QuicTransportError::FrameEncodingError);
            CHECK(consumed == 0U);
        }
    }
}

TEST_CASE("QUIC frames: unknown and longer-encoded types and section 19's bounds are refused", "[net][quic-frame]")
{
    struct Case
    {
        const char*        bytes;
        QuicTransportError error;
    };
    const Case cases[] = {
        {"1f", QuicTransportError::FrameEncodingError},             // past HANDSHAKE_DONE
        {"20", QuicTransportError::FrameEncodingError},
        {"2f", QuicTransportError::FrameEncodingError},
        {"32", QuicTransportError::FrameEncodingError},             // past RFC 9221's DATAGRAM
        {"4001", QuicTransportError::ProtocolViolation},            // PING in two bytes
        {"800000", QuicTransportError::FrameEncodingError},         // a truncated four-byte type
        {"0200000001", QuicTransportError::FrameEncodingError},     // first range 1 above largest 0
        {"02050001040000", QuicTransportError::FrameEncodingError}, // a range after one ending at packet 1
        {"02050001010201", QuicTransportError::FrameEncodingError}, // a second range running below packet 0
        {"0600", QuicTransportError::FrameEncodingError},           // CRYPTO without a length
        {"06ffffffffffffffff0100", QuicTransportError::FrameEncodingError}, // CRYPTO past 2^62 - 1
        {"0e00ffffffffffffffff0100", QuicTransportError::FrameEncodingError}, // STREAM: offset 2^62 - 1, 1 byte
        {"0700", QuicTransportError::FrameEncodingError},                   // an empty NEW_TOKEN token
        {"12d000000000000001", QuicTransportError::FrameEncodingError},     // MAX_STREAMS 2^60 + 1
        {"17d000000000000001", QuicTransportError::FrameEncodingError},     // STREAMS_BLOCKED 2^60 + 1
        {"18010000", QuicTransportError::FrameEncodingError},               // NEW_CONNECTION_ID, empty ID
        {"180102" "08c0c1c2c3c4c5c6c7" "0102030405060708090a0b0c0d0e0f10",
         QuicTransportError::FrameEncodingError},                           // retire prior to above sequence
        {"180100" "15c0c1c2c3c4c5c6c7c0c1c2c3c4c5c6c7c0c1c2c3c4" "0102030405060708090a0b0c0d0e0f10",
         QuicTransportError::FrameEncodingError},                           // a 21-byte ID
        {"1a01020304050607", QuicTransportError::FrameEncodingError},       // 7 bytes of path data
        {"1c0a0605626f", QuicTransportError::FrameEncodingError},           // a reason past the packet
        {"3106aabbcc", QuicTransportError::FrameEncodingError},             // a DATAGRAM past the packet
    };
    for (const Case& test : cases)
    {
        u8 bytes[64] = {};
        crd::containers::Array<char> text;
        for (const char* c = test.bytes; *c != '\0'; ++c)
        {
            if (*c != ' ')
            {
                text.push_back(*c);
            }
        }
        text.push_back('\0');
        const usize size = hex(text.data(), bytes);
        QuicFrame frame;
        usize consumed = 99U;
        INFO(test.bytes);
        CHECK(read_one(ConstSpan<u8>(bytes, size), frame, consumed) == test.error);
        CHECK(consumed == 0U);
    }
}

TEST_CASE("QUIC frames: writers refuse what no reader would accept", "[net][quic-frame]")
{
    const auto refused = [](const QuicFrame& frame)
    {
        u8 out[64] = {0xEE};
        return quic_frame_bytes(frame) == 0U && write_quic_frame(frame, Span<u8>(out, sizeof(out))) == 0U &&
               out[0] == 0xEEU;
    };
    QuicFrame frame;
    frame.kind = QuicFrameKind::Ack;
    CHECK(refused(frame)); // ACK frames are write_quic_ack's
    frame.kind = QuicFrameKind::MaxData;
    frame.value = crd::net::kQuicMaxVarint + 1U;
    CHECK(refused(frame));
    frame.kind = QuicFrameKind::MaxStreams;
    frame.value = kQuicMaxStreams + 1U;
    CHECK(refused(frame));
    frame = QuicFrame{};
    frame.kind = QuicFrameKind::NewToken;
    CHECK(refused(frame));
    frame.kind = QuicFrameKind::Crypto;
    frame.offset = crd::net::kQuicMaxVarint;
    frame.data = ConstSpan<u8>(kData, 1U);
    CHECK(refused(frame));
    frame.kind = QuicFrameKind::Stream;
    CHECK(refused(frame));
    frame = QuicFrame{};
    frame.kind = QuicFrameKind::PathChallenge;
    frame.data = ConstSpan<u8>(kPath, 7U);
    CHECK(refused(frame));
    frame = QuicFrame{};
    frame.kind = QuicFrameKind::NewConnectionId;
    frame.data = ConstSpan<u8>(kId, sizeof(kId));
    frame.reset_token = ConstSpan<u8>(kReset, 15U);
    CHECK(refused(frame));
    frame.reset_token = ConstSpan<u8>(kReset, sizeof(kReset));
    frame.retire_prior_to = 1U;
    CHECK(refused(frame));
    frame.retire_prior_to = 0U;
    frame.data = ConstSpan<u8>{};
    CHECK(refused(frame));
    frame.kind = QuicFrameKind::ConnectionClose;
    frame.error_code = crd::net::kQuicMaxVarint + 1U;
    CHECK(refused(frame));
}

// ---- ACK ranges ----

TEST_CASE("QUIC frames: ACK ranges round trip against a reference set and refuse bad orders", "[net][quic-frame]")
{
    std::mt19937_64 random(0xA11CE5U);
    for (int round = 0; round < 2000; ++round)
    {
        // Ranges built from the top down with gaps of at least one unacknowledged packet number.
        crd::containers::Array<QuicAckRange> ranges;
        u64 top = (round % 3 == 0) ? crd::net::kQuicMaxVarint : (random() % 100000U) + 40U;
        const usize count = 1U + static_cast<usize>(random() % 12U);
        for (usize index = 0U; index < count; ++index)
        {
            const u64 length = random() % 5U;
            if (top < length)
            {
                break;
            }
            ranges.push_back(QuicAckRange{top - length, top});
            const u64 gap = 2U + random() % 7U;
            if (top - length < gap)
            {
                break;
            }
            top = top - length - gap;
        }
        const EcnCounts counts{random() % 1000U, random() % 1000U, random() % 1000U};
        const EcnCounts* ecn = (round % 2 == 0) ? &counts : nullptr;
        const u64 delay = random() % 70000U;
        const ConstSpan<QuicAckRange> view(ranges.data(), ranges.size());
        u8 out[256] = {};
        const usize bytes = write_quic_ack(view, delay, ecn, Span<u8>(out, sizeof(out)));
        REQUIRE(bytes == quic_ack_bytes(view, delay, ecn));
        REQUIRE(bytes != 0U);

        QuicFrame frame;
        usize consumed = 0U;
        REQUIRE(read_one(ConstSpan<u8>(out, bytes), frame, consumed) == QuicTransportError::NoError);
        CHECK(consumed == bytes);
        CHECK(frame.ack.largest == ranges[0].largest);
        CHECK(frame.ack.delay == delay);
        CHECK(frame.ack.ecn == (ecn != nullptr));
        if (ecn != nullptr)
        {
            CHECK((frame.ack.ect0 == counts.ect0 && frame.ack.ect1 == counts.ect1 && frame.ack.ce == counts.ce));
        }
        QuicAckRanges walk(frame.ack);
        QuicAckRange range;
        for (const QuicAckRange& expected : ranges)
        {
            REQUIRE(walk.next(range));
            CHECK((range.smallest == expected.smallest && range.largest == expected.largest));
        }
        CHECK_FALSE(walk.next(range));
    }

    const auto refused = [](std::initializer_list<QuicAckRange> list)
    {
        const crd::containers::Array<QuicAckRange> ranges(list);
        return quic_ack_bytes(ConstSpan<QuicAckRange>(ranges.data(), ranges.size()), 0U) == 0U;
    };
    CHECK(quic_ack_bytes(ConstSpan<QuicAckRange>{}, 0U) == 0U); // no range
    CHECK(refused({{5U, 4U}}));                                 // smallest above largest
    CHECK(refused({{5U, 9U}, {3U, 4U}}));                       // adjacent: 4 and 5 are one range
    CHECK(refused({{5U, 9U}, {3U, 6U}}));                       // overlapping
    CHECK(refused({{1U, 2U}, {5U, 9U}}));                       // smallest first
    CHECK(refused({{0U, crd::net::kQuicMaxVarint + 1U}}));
    CHECK_FALSE(refused({{5U, 9U}, {0U, 3U}}));
    const EcnCounts too_large{crd::net::kQuicMaxVarint + 1U, 0U, 0U};
    const QuicAckRange one[1] = {{0U, 0U}};
    CHECK(quic_ack_bytes(ConstSpan<QuicAckRange>(one, 1U), 0U, &too_large) == 0U);
    CHECK(quic_ack_bytes(ConstSpan<QuicAckRange>(one, 1U), crd::net::kQuicMaxVarint + 1U) == 0U);

    // A range count past what the frame can hold is refused before any walk.
    const u8 huge[] = {0x02, 0x05, 0x00, 0x80, 0x10, 0x00, 0x00, 0x00, 0x00};
    QuicFrame frame;
    usize consumed = 0U;
    CHECK(read_one(ConstSpan<u8>(huge, sizeof(huge)), frame, consumed) == QuicTransportError::FrameEncodingError);
}

// ---- Placement ----

TEST_CASE("QUIC frames: placement follows RFC 9000's table 3, RFC 9221 and the server-only frames",
          "[net][quic-frame]")
{
    // Restated from section 12.4 table 3 ("IH01"), RFC 9221 section 4 and sections 19.7 and 19.20.
    struct Row
    {
        QuicFrameKind kind;
        bool          application;
        const char*   packets;     // I, H, 0 and 1 in that order, '_' where not allowed
        bool          server_only; // NEW_TOKEN and HANDSHAKE_DONE
    };
    const Row rows[] = {
        {QuicFrameKind::Padding, false, "IH01", false},
        {QuicFrameKind::Ping, false, "IH01", false},
        {QuicFrameKind::Ack, false, "IH_1", false},
        {QuicFrameKind::ResetStream, false, "__01", false},
        {QuicFrameKind::ResetStreamAt, false, "__01", false},
        {QuicFrameKind::StopSending, false, "__01", false},
        {QuicFrameKind::Crypto, false, "IH_1", false},
        {QuicFrameKind::NewToken, false, "___1", true},
        {QuicFrameKind::Stream, false, "__01", false},
        {QuicFrameKind::MaxData, false, "__01", false},
        {QuicFrameKind::MaxStreamData, false, "__01", false},
        {QuicFrameKind::MaxStreams, false, "__01", false},
        {QuicFrameKind::DataBlocked, false, "__01", false},
        {QuicFrameKind::StreamDataBlocked, false, "__01", false},
        {QuicFrameKind::StreamsBlocked, false, "__01", false},
        {QuicFrameKind::NewConnectionId, false, "__01", false},
        {QuicFrameKind::RetireConnectionId, false, "__01", false},
        {QuicFrameKind::PathChallenge, false, "__01", false},
        {QuicFrameKind::PathResponse, false, "___1", false},
        {QuicFrameKind::ConnectionClose, false, "IH01", false},
        {QuicFrameKind::ConnectionClose, true, "__01", false},
        {QuicFrameKind::HandshakeDone, false, "___1", true},
        {QuicFrameKind::Datagram, false, "__01", false},
    };
    const QuicPacketType packets[4] = {QuicPacketType::Initial, QuicPacketType::Handshake, QuicPacketType::ZeroRtt,
                                       QuicPacketType::OneRtt};
    for (const Row& row : rows)
    {
        for (usize index = 0U; index < 4U; ++index)
        {
            for (const bool from_server : {false, true})
            {
                bool expected = row.packets[index] != '_';
                expected = expected && !(row.server_only && !from_server);
                expected = expected && !(packets[index] == QuicPacketType::ZeroRtt && from_server);
                INFO(static_cast<int>(row.kind) << " in " << index << (from_server ? " from server" : " from client"));
                CHECK(quic_frame_allowed(row.kind, row.application, packets[index], from_server) == expected);
            }
        }
        for (const QuicPacketType none :
             {QuicPacketType::Retry, QuicPacketType::VersionNegotiation, QuicPacketType::OtherVersion})
        {
            CHECK_FALSE(quic_frame_allowed(row.kind, row.application, none, true));
        }
    }

    CHECK_FALSE(quic_frame_ack_eliciting(QuicFrameKind::Ack));
    CHECK_FALSE(quic_frame_ack_eliciting(QuicFrameKind::Padding));
    CHECK_FALSE(quic_frame_ack_eliciting(QuicFrameKind::ConnectionClose));
    CHECK(quic_frame_ack_eliciting(QuicFrameKind::Ping));
    CHECK(quic_frame_ack_eliciting(QuicFrameKind::Datagram));
    CHECK_FALSE(quic_frame_congestion_controlled(QuicFrameKind::Ack));
    CHECK(quic_frame_congestion_controlled(QuicFrameKind::Padding));
    CHECK(quic_frame_probing(QuicFrameKind::PathChallenge));
    CHECK(quic_frame_probing(QuicFrameKind::NewConnectionId));
    CHECK_FALSE(quic_frame_probing(QuicFrameKind::Stream));
}
} // namespace

TEST_CASE("QUIC frames: RESET_STREAM_AT reads, writes and refuses a reliable size past the final size",
          "[net][4.2a][quic-frames]")
{
    // Type 0x24, stream 4, error 0x0102 in two bytes, final size 300, reliable size 7.
    const u8 wire[] = {0x24U, 0x04U, 0x41U, 0x02U, 0x41U, 0x2CU, 0x07U};
    WireReader in(ConstSpan<u8>(wire, sizeof(wire)));
    QuicFrame frame;
    REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
    CHECK(in.done());
    CHECK(frame.kind == QuicFrameKind::ResetStreamAt);
    CHECK(frame.stream_id == 4U);
    CHECK(frame.error_code == 0x0102U);
    CHECK(frame.value == 300U);
    CHECK(frame.offset == 7U);
    CHECK(quic_frame_bytes(frame) == sizeof(wire));
    u8 out[sizeof(wire)] = {};
    REQUIRE(write_quic_frame(frame, Span<u8>(out, sizeof(out))) == sizeof(wire));
    CHECK(std::memcmp(out, wire, sizeof(wire)) == 0);

    frame.offset = 301U;
    CHECK(quic_frame_bytes(frame) == 0U); // never written
    const u8 past[] = {0x24U, 0x04U, 0x00U, 0x05U, 0x06U};
    WireReader bad(ConstSpan<u8>(past, sizeof(past)));
    QuicFrame refused;
    CHECK(read_quic_frame(bad, refused) == QuicTransportError::FrameEncodingError);
    const u8 equal[] = {0x24U, 0x04U, 0x00U, 0x05U, 0x05U};
    WireReader same(ConstSpan<u8>(equal, sizeof(equal)));
    CHECK(read_quic_frame(same, refused) == QuicTransportError::NoError);
}
