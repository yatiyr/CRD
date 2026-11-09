// crd-net -- QMux over TLS (4.2a; docs/design/network-foundation.md#qmux): the frames and records
// draft-ietf-quic-qmux-02 allows and refuses, QUIC's streams and datagrams over it, time, closing, and a run over the
// host's TCP.

#include "ingress_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/register_ids.hpp>
#include <crd/net/stream_socket.hpp>
#include <crd/net/ticket_cache.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <utility>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::WireReader;
using crd::containers::WireWriter;
using crd::crypto::TlsError;
using crd::crypto::TlsSession;
using crd::crypto::TlsState;

using Bytes = crd::containers::Array<u8>;
using Parameters = crd::containers::Array<std::pair<u64, u64>>;

constexpr u64 kNow = 1000000000U;
constexpr usize kBuffer = 16U * 1024U;

Bytes sized(usize size, u8 fill = 0U)
{
    Bytes bytes;
    bytes.resize(size, fill);
    return bytes;
}

QuicStreamLimits stream_limits(u32 buffer = 64U * 1024U)
{
    QuicStreamLimits limits;
    limits.peer_bidi = 8U;
    limits.peer_uni = 4U;
    limits.local_bidi = 8U;
    limits.local_uni = 4U;
    limits.receive_bytes = buffer;
    limits.send_bytes = buffer;
    limits.stream_window = buffer / 2U;
    limits.connection_window = buffer;
    limits.connection_max_window = u64{buffer} * 4U;
    return limits;
}

QuicDatagramLimits datagram_limits()
{
    QuicDatagramLimits limits;
    limits.max_frame_bytes = 1200U;
    limits.receive_bytes = 8192U;
    limits.receive_count = 32U;
    limits.send_bytes = 8192U;
    limits.send_count = 32U;
    limits.outcomes = true;
    return limits;
}

QmuxConfig config_of(const crypto::TlsContext& tls, u64 idle_ms = 30000U)
{
    QmuxConfig config;
    config.tls = &tls;
    config.streams = stream_limits();
    config.datagrams = datagram_limits();
    config.idle_timeout_ns = idle_ms * kMs;
    return config;
}

// Moves bytes both ways until neither side has any, at most `chunk` bytes per call into receive().
void pump(QmuxConnection& a, QmuxConnection& b, u64 now, usize chunk = kBuffer)
{
    u8 bytes[kBuffer];
    for (bool moved = true; moved;)
    {
        moved = false;
        for (QmuxConnection* from : {&a, &b})
        {
            QmuxConnection& to = (from == &a) ? b : a;
            for (usize got = from->take(Span<u8>(bytes), now); got != 0U; got = from->take(Span<u8>(bytes), now))
            {
                moved = true;
                for (usize at = 0U; at < got; at += chunk)
                {
                    to.receive(ConstSpan<u8>(bytes + at, (got - at < chunk) ? got - at : chunk), now);
                }
            }
        }
    }
}

struct Pair
{
    Identity identity;
    CountingAllocator allocator;
    QmuxConnection client;
    QmuxConnection server;

    explicit Pair(u64 client_idle_ms = 30000U, u64 server_idle_ms = 30000U)
    {
        REQUIRE(client.init(allocator, config_of(identity.client, client_idle_ms)));
        REQUIRE(server.init(allocator, config_of(identity.server, server_idle_ms)));
    }

    void connect(u64 now = kNow, usize chunk = kBuffer)
    {
        REQUIRE(client.start(now));
        REQUIRE(server.start(now));
        pump(client, server, now, chunk);
        REQUIRE(client.state() == QmuxState::Established);
        REQUIRE(server.state() == QmuxState::Established);
        REQUIRE(client.peer_parameters_known());
        REQUIRE(server.peer_parameters_known());
    }
};

// A TLS client that speaks to a QMux server by hand: every malformed thing a real QMux client never sends.
struct Raw
{
    Identity identity;
    CountingAllocator allocator;
    TlsSession peer;
    QmuxConnection server;
    u64 now = kNow;

    Raw()
    {
        REQUIRE(server.init(allocator, config_of(identity.server)));
        REQUIRE(server.start(kNow));
        REQUIRE(peer.init(identity.client) == TlsError::None);
        flush();
        REQUIRE(peer.state() == TlsState::Established);
        REQUIRE(server.state() == QmuxState::Established);
    }

    void flush()
    {
        u8 bytes[kBuffer];
        for (bool moved = true; moved;)
        {
            moved = false;
            for (;;)
            {
                usize got = 0U;
                REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
                if (got == 0U)
                {
                    break;
                }
                moved = true;
                server.receive(ConstSpan<u8>(bytes, got), now);
            }
            if (peer.state() == TlsState::Failed)
            {
                return;
            }
            for (usize out = server.take(Span<u8>(bytes), now); out != 0U; out = server.take(Span<u8>(bytes), now))
            {
                moved = true;
                const TlsError error = peer.receive(ConstSpan<u8>(bytes, out));
                REQUIRE((error == TlsError::None || error == TlsError::Closed));
            }
        }
    }

    void record(ConstSpan<u8> frames)
    {
        Bytes bytes = sized(frames.size() + 8U);
        WireWriter writer{Span<u8>(bytes.data(), bytes.size())};
        put_quic_varint(writer, frames.size());
        writer.put_bytes(frames);
        REQUIRE(peer.send(ConstSpan<u8>(bytes.data(), writer.written())) == TlsError::None);
        flush();
    }

    void record(const Bytes& frames) { record(ConstSpan<u8>(frames.data(), frames.size())); }

    // A transport parameters frame with just these (id, value) pairs.
    void parameters(const Parameters& list) { record(tp_frame(list)); }

    static Bytes tp_frame(const Parameters& list)
    {
        Bytes body = sized(16U * list.size() + 32U);
        WireWriter inner{Span<u8>(body.data(), body.size())};
        for (const auto& parameter : list)
        {
            put_quic_varint(inner, parameter.first);
            put_quic_varint(inner, quic_varint_bytes(parameter.second));
            put_quic_varint(inner, parameter.second);
        }
        Bytes frame = sized(inner.written() + 16U);
        WireWriter outer{Span<u8>(frame.data(), frame.size())};
        put_quic_varint(outer, iana::kQuicFrameQxTransportParameters);
        put_quic_varint(outer, inner.written());
        outer.put_bytes(ConstSpan<u8>(body.data(), inner.written()));
        frame.resize(outer.written());
        return frame;
    }

    void open()
    {
        parameters({{0x04U, 1U << 20U}, {0x05U, 1U << 16U}, {0x06U, 1U << 16U}, {0x08U, 16U}});
        REQUIRE(server.peer_parameters_known());
    }

    // Reads everything the server sent back and finds its CONNECTION_CLOSE; the frames before it are skipped.
    QuicFrame close_frame()
    {
        Bytes plain = sized(64U * 1024U);
        usize fill = 0U;
        for (;;)
        {
            usize got = 0U;
            const TlsError error = peer.read(Span<u8>(plain.data() + fill, plain.size() - fill), got);
            fill += got;
            if (got == 0U || error != TlsError::None)
            {
                break;
            }
        }
        // Stays alive for the views the frame holds.
        kept = static_cast<Bytes&&>(plain);
        WireReader in(ConstSpan<u8>(kept.data(), fill));
        while (!in.done())
        {
            u64 size = 0U;
            REQUIRE(take_quic_varint(in, size));
            ConstSpan<u8> frames;
            REQUIRE(in.take_bytes(static_cast<usize>(size), frames));
            WireReader inside(frames);
            while (!inside.done())
            {
                WireReader probe(inside.rest());
                u64 type = 0U;
                REQUIRE(take_quic_varint(probe, type));
                if (type == iana::kQuicFrameQxTransportParameters)
                {
                    u64 length = 0U;
                    REQUIRE(take_quic_varint(probe, length));
                    ConstSpan<u8> skipped;
                    REQUIRE(inside.take_bytes(probe.consumed() + static_cast<usize>(length), skipped));
                    continue;
                }
                if (type == iana::kQuicFrameQxPingRequest || type == iana::kQuicFrameQxPingResponse)
                {
                    u64 sequence = 0U;
                    REQUIRE(take_quic_varint(probe, sequence));
                    ConstSpan<u8> skipped;
                    REQUIRE(inside.take_bytes(probe.consumed(), skipped));
                    continue;
                }
                QuicFrame frame;
                REQUIRE(read_quic_frame(inside, frame) == QuicTransportError::NoError);
                if (frame.kind == QuicFrameKind::ConnectionClose)
                {
                    return frame;
                }
            }
        }
        FAIL("no CONNECTION_CLOSE");
        return QuicFrame{};
    }

    void expect_error(QuicTransportError error, u64 frame_type = 0U)
    {
        CHECK(server.state() == QmuxState::Closing);
        CHECK(server.close_info().reason == QmuxCloseReason::Error);
        CHECK(server.close_info().error_code == static_cast<u64>(error));
        flush();
        CHECK(server.send_finished());
        const QuicFrame frame = close_frame();
        CHECK(frame.error_code == static_cast<u64>(error));
        CHECK_FALSE(frame.application);
        if (frame_type != 0U)
        {
            CHECK(frame.frame_type == frame_type);
        }
    }

    Bytes kept;
};

Bytes stream_frame(u64 id, u64 offset, const Bytes& data, bool fin = false)
{
    QuicFrame frame;
    frame.kind = QuicFrameKind::Stream;
    frame.stream_id = id;
    frame.offset = offset;
    frame.data = ConstSpan<u8>(data.data(), data.size());
    frame.fin = fin;
    Bytes bytes = sized(data.size() + 32U);
    const usize written = write_quic_frame(frame, Span<u8>(bytes.data(), bytes.size()));
    REQUIRE(written != 0U);
    bytes.resize(written);
    return bytes;
}

Bytes ping_frame(u64 type, u64 sequence)
{
    Bytes bytes = sized(16U);
    WireWriter writer{Span<u8>(bytes.data(), bytes.size())};
    put_quic_varint(writer, type);
    put_quic_varint(writer, sequence);
    bytes.resize(writer.written());
    return bytes;
}

TEST_CASE("QMux: both ends carry transport parameters first and open", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    CHECK(pair.client.peer_parameters().initial_max_data == stream_limits().connection_window);
    CHECK(pair.server.peer_parameters().initial_max_streams_bidi == 8U);
    CHECK(pair.client.peer_parameters().max_datagram_frame_size == 1200U);
    CHECK(pair.client.peer_parameters().max_idle_timeout == 30000U);
    CHECK(pair.client.counters().records_sent >= 1U);
    CHECK((pair.client.handshake().application_protocol() == kProtocols[0]));
    CHECK(pair.client.next_timer_ns() == kNow + 30000U * kMs);
}

TEST_CASE("QMux: the handshake and records survive being split at every byte", "[net][qmux]")
{
    Pair pair;
    pair.connect(kNow, 1U);
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    const u8 message[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    usize taken = 0U;
    REQUIRE(pair.client.streams().write(id, ConstSpan<u8>(message), true, taken) == QuicStreamError::None);
    pump(pair.client, pair.server, kNow, 1U);
    const QuicStreamRead read = pair.server.streams().peek(id);
    REQUIRE(read.data.size() == sizeof(message));
    CHECK(std::memcmp(read.data.data(), message, sizeof(message)) == 0);
    CHECK(read.fin);
}

TEST_CASE("QMux: a transfer past every window arrives whole, with flow control both ways", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    u64 up = 0U;
    REQUIRE(pair.client.streams().open(true, up) == QuicStreamError::None);
    u64 down = 0U;
    REQUIRE(pair.server.streams().open(false, down) == QuicStreamError::None);
    const u64 size = 2U * 1024U * 1024U;
    u64 up_written = 0U;
    u64 down_written = 0U;
    u64 up_read = 0U;
    u64 down_read = 0U;
    bool up_done = false;
    bool down_done = false;
    u64 now = kNow;
    for (usize turn = 0U; turn < 20000U && !(up_done && down_done); ++turn)
    {
        write_pattern(pair.client.streams(), up, up_written, size, 3U);
        write_pattern(pair.server.streams(), down, down_written, size, 5U);
        pump(pair.client, pair.server, now);
        up_done = drain(pair.server.streams(), up, up_read, 3U);
        down_done = drain(pair.client.streams(), down, down_read, 5U);
        now += kMs;
    }
    CHECK(up_done);
    CHECK(down_done);
    CHECK(up_read == size);
    CHECK(down_read == size);
    CHECK(pair.client.state() == QmuxState::Established);
}

TEST_CASE("QMux: a stream's reset and its stop request cross the record layer", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    const u8 bytes[100] = {};
    usize taken = 0U;
    REQUIRE(pair.client.streams().write(id, ConstSpan<u8>(bytes), false, taken) == QuicStreamError::None);
    pump(pair.client, pair.server, kNow);
    REQUIRE(pair.server.streams().stop_sending(id, 77U) == QuicStreamError::None);
    pump(pair.client, pair.server, kNow);
    QuicStreamEvent event;
    bool stopped = false;
    while (pair.client.streams().poll(event))
    {
        stopped = stopped || (event.events & kQuicStreamStopSending) != 0U;
    }
    CHECK(stopped);
    CHECK(pair.client.state() == QmuxState::Established);
    CHECK(pair.server.state() == QmuxState::Established);
    REQUIRE(pair.client.streams().send_state(id) != QuicSendState::Ready);
}

TEST_CASE("QMux: datagrams arrive in order and are reported delivered once handed on", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    CHECK(pair.client.datagrams().max_payload() == 1197U);
    const u8 payload[256] = {9U};
    u64 ids[3] = {};
    for (u64& id : ids)
    {
        REQUIRE(pair.client.datagrams().queue(ConstSpan<u8>(payload), kNow, id) == QuicDatagramError::None);
    }
    pump(pair.client, pair.server, kNow);
    for (usize i = 0U; i < 3U; ++i)
    {
        ConstSpan<u8> held;
        REQUIRE(pair.server.datagrams().peek(held));
        CHECK(held.size() == sizeof(payload));
        pair.server.datagrams().pop();
        QuicDatagramReport report;
        REQUIRE(pair.client.datagrams().poll(report));
        CHECK(report.id == ids[i]);
        CHECK(report.outcome == QuicDatagramOutcome::Acked);
    }
}

TEST_CASE("QMux: a datagram frame stays within the peer's limit and a record", "[net][qmux]")
{
    Identity identity;
    CountingAllocator allocator;
    QmuxConfig large = config_of(identity.server);
    large.datagrams.max_frame_bytes = 64U * 1024U - 1U;
    large.datagrams.receive_bytes = large.datagrams.max_frame_bytes;
    QmuxConfig sender = config_of(identity.client);
    sender.datagrams.send_bytes = 64U * 1024U;
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(allocator, sender));
    REQUIRE(server.init(allocator, large));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    pump(client, server, kNow);
    usize limit = 0U;
    REQUIRE(quic_datagram_payload_limit(kQmuxMaxRecord, limit));
    CHECK(client.datagrams().max_payload() == limit); // the record, not the peer's larger limit
    Bytes payload = sized(limit, u8{0x5A});
    u64 id = 0U;
    REQUIRE(client.datagrams().queue(ConstSpan<u8>(payload.data(), payload.size()), kNow, id) ==
            QuicDatagramError::None);
    pump(client, server, kNow);
    ConstSpan<u8> held;
    REQUIRE(server.datagrams().peek(held));
    CHECK(held.size() == limit);
    CHECK(server.state() == QmuxState::Established);
}

TEST_CASE("QMux: a peer's first frame must be its transport parameters", "[net][qmux]")
{
    SECTION("PADDING first")
    {
        Raw raw;
        raw.record(Bytes{0x00U});
        raw.expect_error(QuicTransportError::ProtocolViolation);
    }
    SECTION("a stream first")
    {
        Raw raw;
        raw.record(stream_frame(0U, 0U, {1U}));
        raw.expect_error(QuicTransportError::ProtocolViolation);
    }
    SECTION("QX_PING first")
    {
        Raw raw;
        raw.record(ping_frame(iana::kQuicFrameQxPingRequest, 0U));
        raw.expect_error(QuicTransportError::ProtocolViolation);
    }
}

TEST_CASE("QMux: a second transport parameters frame is refused", "[net][qmux]")
{
    Raw raw;
    raw.open();
    raw.parameters({{0x04U, 1U}});
    raw.expect_error(QuicTransportError::ProtocolViolation, iana::kQuicFrameQxTransportParameters);
}

TEST_CASE("QMux: every QUIC parameter that speaks of paths or IDs is refused", "[net][qmux]")
{
    for (const u64 id : {0x00ULL, 0x02ULL, 0x03ULL, 0x0AULL, 0x0BULL, 0x0CULL, 0x0DULL, 0x0EULL, 0x0FULL, 0x10ULL})
    {
        CAPTURE(id);
        Raw raw;
        raw.parameters({{id, 1U}});
        raw.expect_error(QuicTransportError::TransportParameterError);
    }
}

TEST_CASE("QMux: parameters are held to their encodings and limits", "[net][qmux]")
{
    SECTION("max_record_size below the default")
    {
        Raw raw;
        raw.parameters({{iana::kQuicTpMaxRecordSize, kQmuxMaxRecord - 1U}});
        raw.expect_error(QuicTransportError::TransportParameterError);
    }
    SECTION("max_record_size at and above the default")
    {
        Raw raw;
        raw.parameters({{iana::kQuicTpMaxRecordSize, kQmuxMaxRecord}});
        CHECK(raw.server.peer_parameters_known());
        CHECK(raw.server.state() == QmuxState::Established);
    }
    SECTION("a parameter sent twice")
    {
        Raw raw;
        raw.parameters({{0x04U, 1U}, {0x04U, 2U}});
        raw.expect_error(QuicTransportError::TransportParameterError);
    }
    SECTION("streams beyond 2^60")
    {
        Raw raw;
        raw.parameters({{0x08U, (u64{1} << 60U) + 1U}});
        raw.expect_error(QuicTransportError::TransportParameterError);
    }
    SECTION("a value with bytes after its varint")
    {
        Raw raw;
        const u8 body[] = {0x04U, 0x02U, 0x01U, 0x00U};
        Bytes frame = sized(16U);
        WireWriter writer{Span<u8>(frame.data(), frame.size())};
        put_quic_varint(writer, iana::kQuicFrameQxTransportParameters);
        put_quic_varint(writer, sizeof(body));
        writer.put_bytes(ConstSpan<u8>(body));
        frame.resize(writer.written());
        raw.record(frame);
        raw.expect_error(QuicTransportError::TransportParameterError);
    }
    SECTION("another document's parameter is ignored, even twice")
    {
        Raw raw;
        raw.parameters({{0x31U, 1U}, {0x31U, 2U}, {0x04U, 7U}});
        CHECK(raw.server.peer_parameters().initial_max_data == 7U);
        CHECK(raw.server.state() == QmuxState::Established);
    }
    SECTION("a frame shorter than its length")
    {
        Raw raw;
        Bytes frame = sized(16U);
        WireWriter writer{Span<u8>(frame.data(), frame.size())};
        put_quic_varint(writer, iana::kQuicFrameQxTransportParameters);
        put_quic_varint(writer, 200U);
        frame.resize(writer.written());
        raw.record(frame);
        raw.expect_error(QuicTransportError::FrameEncodingError);
    }
}

TEST_CASE("QMux: the frames QUIC needs for packets and paths are refused", "[net][qmux]")
{
    const Bytes forbidden[] = {
        {0x01U},                                 // PING
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U},     // ACK
        {0x06U, 0x00U, 0x01U, 0xAAU},            // CRYPTO
        {0x07U, 0x01U, 0xAAU},                   // NEW_TOKEN
        {0x19U, 0x00U},                          // RETIRE_CONNECTION_ID
        {0x1AU, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}, // PATH_CHALLENGE
        {0x1BU, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}, // PATH_RESPONSE
        {0x1EU},                                 // HANDSHAKE_DONE
        {0x40U, 0x40U},                          // no frame: the type 0x40
    };
    for (const Bytes& frame : forbidden)
    {
        CAPTURE(frame[0]);
        Raw raw;
        raw.open();
        raw.record(frame);
        raw.expect_error(QuicTransportError::FrameEncodingError);
    }
}

TEST_CASE("QMux: records are held to their size and their frames to their record", "[net][qmux]")
{
    SECTION("a record over the maximum")
    {
        Raw raw;
        raw.open();
        u8 header[8];
        WireWriter writer{Span<u8>(header)};
        put_quic_varint(writer, kQmuxMaxRecord + 1U);
        REQUIRE(raw.peer.send(ConstSpan<u8>(header, writer.written())) == TlsError::None);
        raw.flush();
        raw.expect_error(QuicTransportError::FrameEncodingError);
    }
    SECTION("a record of no frames")
    {
        Raw raw;
        raw.open();
        const u8 zero = 0U;
        REQUIRE(raw.peer.send(ConstSpan<u8>(&zero, 1U)) == TlsError::None);
        raw.flush();
        raw.expect_error(QuicTransportError::ProtocolViolation);
    }
    SECTION("a frame cut off by the record's end")
    {
        Raw raw;
        raw.open();
        Bytes frame = stream_frame(0U, 0U, {1U, 2U, 3U});
        frame.pop_back();
        raw.record(frame);
        raw.expect_error(QuicTransportError::FrameEncodingError);
    }
    SECTION("the largest record is accepted")
    {
        Raw raw;
        raw.open();
        Bytes padding = sized(kQmuxMaxRecord, u8{0});
        raw.record(padding);
        CHECK(raw.server.state() == QmuxState::Established);
    }
    SECTION("a record arriving in pieces, the Size split")
    {
        Raw raw;
        raw.open();
        Bytes frames = sized(300U, u8{0});
        u8 header[8];
        WireWriter writer{Span<u8>(header)};
        put_quic_varint(writer, frames.size());
        const u64 before = raw.server.counters().records_received;
        REQUIRE(raw.peer.send(ConstSpan<u8>(header, 1U)) == TlsError::None);
        raw.flush();
        REQUIRE(raw.peer.send(ConstSpan<u8>(header + 1U, writer.written() - 1U)) == TlsError::None);
        REQUIRE(raw.peer.send(ConstSpan<u8>(frames.data(), 150U)) == TlsError::None);
        raw.flush();
        CHECK(raw.server.counters().records_received == before);
        REQUIRE(raw.peer.send(ConstSpan<u8>(frames.data() + 150U, 150U)) == TlsError::None);
        raw.flush();
        CHECK(raw.server.counters().records_received == before + 1U);
    }
}

TEST_CASE("QMux: stream data must continue its stream", "[net][qmux]")
{
    SECTION("in order is accepted, frame by frame")
    {
        Raw raw;
        raw.open();
        raw.record(stream_frame(0U, 0U, {1U, 2U}));
        raw.record(stream_frame(0U, 2U, {3U}, true));
        CHECK(raw.server.state() == QmuxState::Established);
        const QuicStreamRead read = raw.server.streams().peek(0U);
        CHECK(read.data.size() == 3U);
        CHECK(read.fin);
    }
    SECTION("a gap")
    {
        Raw raw;
        raw.open();
        raw.record(stream_frame(0U, 0U, {1U}));
        raw.record(stream_frame(0U, 2U, {3U}));
        raw.expect_error(QuicTransportError::ProtocolViolation);
    }
    SECTION("repeated data")
    {
        Raw raw;
        raw.open();
        raw.record(stream_frame(0U, 0U, {1U, 2U}));
        raw.record(stream_frame(0U, 1U, {2U, 3U}));
        raw.expect_error(QuicTransportError::ProtocolViolation);
    }
    SECTION("another stream's frames interleave freely")
    {
        Raw raw;
        raw.open();
        raw.record(stream_frame(0U, 0U, {1U}));
        raw.record(stream_frame(4U, 0U, {2U}));
        raw.record(stream_frame(0U, 1U, {3U}));
        CHECK(raw.server.state() == QmuxState::Established);
    }
}

TEST_CASE("QMux: QX_PING is answered, and held to its sequence", "[net][qmux]")
{
    SECTION("a response per largest request; sequences must grow")
    {
        Raw raw;
        raw.open();
        Bytes pings = ping_frame(iana::kQuicFrameQxPingRequest, 3U);
        const Bytes second = ping_frame(iana::kQuicFrameQxPingRequest, 9U);
        for (const u8 byte : second)
        {
            pings.push_back(byte);
        }
        raw.record(pings);
        CHECK(raw.server.state() == QmuxState::Established);
        raw.record(ping_frame(iana::kQuicFrameQxPingRequest, 9U));
        raw.expect_error(QuicTransportError::ProtocolViolation, iana::kQuicFrameQxPingRequest);
    }
    SECTION("a response to a request never sent")
    {
        Raw raw;
        raw.open();
        raw.record(ping_frame(iana::kQuicFrameQxPingResponse, 5U));
        raw.expect_error(QuicTransportError::ProtocolViolation, iana::kQuicFrameQxPingResponse);
    }
}

TEST_CASE("QMux: pings measure the RTT that tunes windows", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    u8 bytes[kBuffer];
    pair.client.ping();
    const usize sent = pair.client.take(Span<u8>(bytes), kNow);
    REQUIRE(sent != 0U);
    pair.server.receive(ConstSpan<u8>(bytes, sent), kNow + 10U * kMs);
    const usize answer = pair.server.take(Span<u8>(bytes), kNow + 10U * kMs);
    REQUIRE(answer != 0U);
    pair.client.receive(ConstSpan<u8>(bytes, answer), kNow + 20U * kMs);
    CHECK(pair.client.rtt_ns() == 20U * kMs);
    CHECK(pair.client.counters().pongs_received >= 1U);
}

TEST_CASE("QMux: the idle timeout is the smaller offered and only records restart it", "[net][qmux]")
{
    Pair pair(30000U, 5000U);
    pair.connect();
    CHECK(pair.client.next_timer_ns() == kNow + 5000U * kMs);
    pump(pair.client, pair.server, kNow + 1000U * kMs); // nothing moved: no record restarts it
    CHECK(pair.client.next_timer_ns() == kNow + 5000U * kMs);
    pair.client.ping();
    pump(pair.client, pair.server, kNow + 1000U * kMs);
    CHECK(pair.client.next_timer_ns() == kNow + 6000U * kMs);
    pair.client.on_timeout(kNow + 5999U * kMs);
    CHECK(pair.client.state() == QmuxState::Established);
    pair.client.on_timeout(kNow + 6000U * kMs);
    CHECK(pair.client.state() == QmuxState::Closed);
    CHECK(pair.client.close_info().reason == QmuxCloseReason::IdleTimeout);
    CHECK(pair.client.next_timer_ns() == kQmuxNoTimer);
    // The idle shutdown carries no CONNECTION_CLOSE, just close_notify.
    pump(pair.client, pair.server, kNow + 6000U * kMs);
    CHECK(pair.server.close_info().reason == QmuxCloseReason::PeerShutdown);
    CHECK(pair.client.send_finished());
}

TEST_CASE("QMux: only a whole record received restarts the idle timer", "[net][qmux]")
{
    Raw raw;
    raw.open();
    const u64 idle = 30000U * kMs;
    raw.now = kNow + 1000U * kMs;
    u8 header[8];
    WireWriter writer{Span<u8>(header)};
    const Bytes frames = sized(64U, u8{0});
    put_quic_varint(writer, frames.size());
    REQUIRE(raw.peer.send(ConstSpan<u8>(header, writer.written())) == TlsError::None);
    REQUIRE(raw.peer.send(ConstSpan<u8>(frames.data(), 32U)) == TlsError::None);
    raw.flush();
    CHECK(raw.server.next_timer_ns() == kNow + idle); // half a record restarts nothing
    raw.now = kNow + 2000U * kMs;
    REQUIRE(raw.peer.send(ConstSpan<u8>(frames.data() + 32U, 32U)) == TlsError::None);
    raw.flush();
    CHECK(raw.server.next_timer_ns() == raw.now + idle);
}

TEST_CASE("QMux: the idle timer restarts only when the last byte has been handed on", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    const u64 idle = 30000U * kMs;
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    write_pattern(pair.client.streams(), id, written, 4096U, 1U);
    u8 bytes[kBuffer];
    const u64 later = kNow + 5000U * kMs;
    REQUIRE(pair.client.take(Span<u8>(bytes, 64U), later) == 64U); // part of the records
    CHECK(pair.client.next_timer_ns() == kNow + idle);
    while (pair.client.take(Span<u8>(bytes), later) != 0U)
    {
    }
    CHECK(pair.client.next_timer_ns() == later + idle);
}

TEST_CASE("QMux: a peer that stops reading does not hold a connection open", "[net][qmux]")
{
    Pair pair(1000U, 1000U);
    pair.connect();
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    write_pattern(pair.client.streams(), id, written, 64U * 1024U, 1U);
    // Records are assembled but no byte is taken: nothing has been handed to the transport.
    CHECK(pair.client.wants_write(kNow));
    pair.client.on_timeout(kNow + 1000U * kMs);
    CHECK(pair.client.state() == QmuxState::Closed);
    CHECK(pair.client.close_info().reason == QmuxCloseReason::IdleTimeout);
}

TEST_CASE("QMux: keep-alives go out before the idle timeout", "[net][qmux]")
{
    Identity identity;
    CountingAllocator allocator;
    QmuxConfig config = config_of(identity.client, 3000U);
    config.keepalive_ns = 1000U * kMs;
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(allocator, config));
    REQUIRE(server.init(allocator, config_of(identity.server, 3000U)));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    pump(client, server, kNow);
    u64 now = kNow;
    for (usize turn = 0U; turn < 10U; ++turn)
    {
        now = client.next_timer_ns();
        CHECK(now == kNow + (turn + 1U) * 1000U * kMs);
        client.on_timeout(now);
        server.on_timeout(now);
        pump(client, server, now);
        CHECK(client.state() == QmuxState::Established);
        CHECK(server.state() == QmuxState::Established);
    }
    CHECK(client.counters().pings_sent >= 11U);
}

TEST_CASE("QMux: closing sends CONNECTION_CLOSE, then ends the sending side", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    const char reason[] = "goodbye";
    pair.client.close(42U, ConstSpan<u8>(reinterpret_cast<const u8*>(reason), 7U), kNow);
    CHECK(pair.client.state() == QmuxState::Closing);
    CHECK_FALSE(pair.client.send_finished());
    pump(pair.client, pair.server, kNow);
    CHECK(pair.client.send_finished());
    CHECK(pair.server.state() == QmuxState::Closed);
    CHECK(pair.server.close_info().reason == QmuxCloseReason::Peer);
    CHECK(pair.server.close_info().application);
    CHECK(pair.server.close_info().error_code == 42U);
    REQUIRE(pair.server.close_reason().size() == 7U);
    CHECK(std::memcmp(pair.server.close_reason().data(), reason, 7U) == 0);
    CHECK(pair.server.send_finished());
    // The closing side discards what still comes and ends with the peer's end or its wait.
    CHECK(pair.client.next_timer_ns() == kNow + 1000U * kMs);
    pair.client.on_timeout(kNow + 1000U * kMs);
    CHECK(pair.client.state() == QmuxState::Closed);
    CHECK(pair.client.close_info().reason == QmuxCloseReason::Local);
}

TEST_CASE("QMux: a closing side ends at the peer's clean end, and long reasons are cut", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    const Bytes reason = sized(kQmuxCloseReasonBytes + 50U, u8{'x'});
    pair.server.close(1U, ConstSpan<u8>(reason.data(), reason.size()), kNow);
    pump(pair.client, pair.server, kNow);
    CHECK(pair.client.close_reason().size() == kQmuxCloseReasonBytes);
    pair.server.transport_ended(true, kNow);
    CHECK(pair.server.state() == QmuxState::Closed);
    CHECK(pair.server.close_info().reason == QmuxCloseReason::Local);
}

TEST_CASE("QMux: a stream's end without close_notify is a truncation", "[net][qmux]")
{
    {
        Pair pair;
        pair.connect();
        pair.client.transport_ended(true, kNow);
        CHECK(pair.client.state() == QmuxState::Closed);
        CHECK(pair.client.close_info().reason == QmuxCloseReason::Truncated);
        CHECK_FALSE(pair.client.wants_write(kNow));
    }
    {
        Pair pair;
        pair.connect();
        pair.client.transport_ended(false, kNow);
        CHECK(pair.client.close_info().reason == QmuxCloseReason::Reset);
        u8 out[64];
        CHECK(pair.client.take(Span<u8>(out), kNow) == 0U);
    }
}

TEST_CASE("QMux: the transport is read while the application is not", "[net][qmux]")
{
    Pair pair;
    pair.connect();
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    write_pattern(pair.client.streams(), id, written, 8U * 1024U * 1024U, 1U);
    pump(pair.client, pair.server, kNow);
    // The server never reads the stream: its flow control fills, and the ping behind it is still answered.
    const u64 before = pair.client.counters().pongs_received;
    pair.client.ping();
    pump(pair.client, pair.server, kNow);
    CHECK(pair.client.counters().pongs_received == before + 1U);
    CHECK(pair.client.state() == QmuxState::Established);
    CHECK(pair.server.state() == QmuxState::Established);
}

TEST_CASE("QMux: records pad to the block with PADDING", "[net][qmux]")
{
    Identity identity;
    CountingAllocator allocator;
    QmuxConfig padded = config_of(identity.client);
    padded.padding_block = 256U;
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(allocator, padded));
    REQUIRE(server.init(allocator, config_of(identity.server)));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    pump(client, server, kNow);
    const u64 records = client.counters().records_sent;
    const u64 pad = client.counters().bytes_padded;
    CHECK(pad != 0U);
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    const u8 bytes[10] = {1U};
    usize taken = 0U;
    REQUIRE(client.streams().write(id, ConstSpan<u8>(bytes), true, taken) == QuicStreamError::None);
    pump(client, server, kNow);
    CHECK(client.counters().records_sent == records + 1U);
    CHECK(client.counters().bytes_padded > pad);
    CHECK(server.state() == QmuxState::Established);
    CHECK(server.streams().peek(id).data.size() == 10U);
}

TEST_CASE("QMux: the handshake's failures end the connection", "[net][qmux]")
{
    SECTION("a server whose key is not the pinned one")
    {
        Identity identity(true);
        CountingAllocator allocator;
        QmuxConnection client;
        QmuxConnection server;
        REQUIRE(client.init(allocator, config_of(identity.client)));
        REQUIRE(server.init(allocator, config_of(identity.server)));
        REQUIRE(client.start(kNow));
        REQUIRE(server.start(kNow));
        pump(client, server, kNow);
        CHECK(client.state() == QmuxState::Closed);
        CHECK(client.close_info().reason == QmuxCloseReason::TlsFailed);
        CHECK(client.tls_error() == TlsError::PeerRefused);
        CHECK(server.state() == QmuxState::Closed);
        CHECK(server.close_info().reason == QmuxCloseReason::TlsFailed);
        CHECK(client.send_finished());
    }
    SECTION("the handshake's deadline")
    {
        Pair pair;
        REQUIRE(pair.client.start(kNow));
        CHECK(pair.client.next_timer_ns() == kNow + 10000U * kMs);
        pair.client.on_timeout(kNow + 10000U * kMs - 1U);
        CHECK(pair.client.state() == QmuxState::Handshaking);
        pair.client.on_timeout(kNow + 10000U * kMs);
        CHECK(pair.client.state() == QmuxState::Closed);
        CHECK(pair.client.close_info().reason == QmuxCloseReason::HandshakeTimeout);
    }
    SECTION("closed during the handshake")
    {
        Pair pair;
        REQUIRE(pair.client.start(kNow));
        pair.client.close(9U, {}, kNow);
        CHECK(pair.client.state() == QmuxState::Closed);
        CHECK(pair.client.close_info().reason == QmuxCloseReason::Local);
    }
    SECTION("a truncated handshake")
    {
        Pair pair;
        REQUIRE(pair.client.start(kNow));
        pair.client.transport_ended(true, kNow);
        CHECK(pair.client.close_info().reason == QmuxCloseReason::Truncated);
    }
    SECTION("garbage in place of TLS")
    {
        Pair pair;
        REQUIRE(pair.server.start(kNow));
        const u8 garbage[64] = {0x16U, 0x03U, 0x01U, 0xFFU, 0xFFU};
        pair.server.receive(ConstSpan<u8>(garbage), kNow);
        CHECK(pair.server.state() == QmuxState::Closed);
        CHECK(pair.server.close_info().reason == QmuxCloseReason::TlsFailed);
    }
}

TEST_CASE("QMux: a connection needs ALPN and a usable configuration", "[net][qmux]")
{
    CountingAllocator allocator;
    {
        QmuxConnection connection;
        CHECK_FALSE(connection.start(kNow));
        CHECK_FALSE(connection.init(allocator, QmuxConfig{}));
    }
    Identity identity;
    {
        QmuxConfig bad = config_of(identity.client);
        bad.streams.receive_bytes = 1000U;
        QmuxConnection connection;
        CHECK_FALSE(connection.init(allocator, bad));
    }
    {
        QmuxConnection connection;
        REQUIRE(connection.init(allocator, config_of(identity.client)));
        CHECK_FALSE(connection.init(allocator, config_of(identity.client)));
    }
    {
        crypto::TlsConfig tls;
        tls.role = crypto::TlsRole::Client;
        tls.groups = kGroups;
        tls.signatures = kSignatures;
        tls.aeads = kAeads;
        const crypto::KeyId pin = identity.credential.key_id();
        tls.pinned = ConstSpan<crypto::KeyId>(&pin, 1U);
        tls.server_name = kServerName;
        crypto::TlsContext no_alpn;
        REQUIRE(no_alpn.init(tls) == TlsError::None);
        crypto::TlsConfig server_tls;
        server_tls.role = crypto::TlsRole::Server;
        server_tls.groups = kGroups;
        server_tls.aeads = kAeads;
        server_tls.credential = &identity.credential;
        server_tls.server_name = kServerName;
        crypto::TlsContext server_no_alpn;
        REQUIRE(server_no_alpn.init(server_tls) == TlsError::None);
        QmuxConnection client;
        QmuxConnection server;
        REQUIRE(client.init(allocator, config_of(no_alpn)));
        REQUIRE(server.init(allocator, config_of(server_no_alpn)));
        REQUIRE(client.start(kNow));
        REQUIRE(server.start(kNow));
        pump(client, server, kNow);
        CHECK(client.state() == QmuxState::Closed);
        CHECK(client.close_info().reason == QmuxCloseReason::TlsFailed);
        CHECK(client.tls_error() == TlsError::PeerRefused);
    }
}

TEST_CASE("QMux: resumption on the stream path uses its own tickets", "[net][qmux]")
{
    Identity identity;
    CountingAllocator allocator;
    TicketCache tickets(allocator, TicketCacheConfig{});
    for (usize run = 0U; run < 2U; ++run)
    {
        QmuxConfig config = config_of(identity.client);
        config.tickets = &tickets;
        QmuxConnection client;
        QmuxConnection server;
        REQUIRE(client.init(allocator, config));
        REQUIRE(server.init(allocator, config_of(identity.server)));
        REQUIRE(client.start(kNow));
        REQUIRE(server.start(kNow));
        pump(client, server, kNow);
        CHECK(client.handshake().resumed() == (run == 1U));
    }
}

TEST_CASE("QMux: nothing allocates after init", "[net][qmux]")
{
    Pair pair;
    const usize calls = pair.allocator.calls();
    pair.connect();
    u64 id = 0U;
    REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    write_pattern(pair.client.streams(), id, written, 8192U, 1U);
    pump(pair.client, pair.server, kNow);
    pair.client.close(0U, {}, kNow);
    pump(pair.client, pair.server, kNow);
    CHECK(pair.allocator.calls() == calls);
}

// What waits on one direction of a byte path of seeded chunks and stalls: TCP's only hostility is timing.
struct Line
{
    Bytes bytes;
    usize head = 0U;
};

TEST_CASE("QMux: streams and datagrams cross a path of seeded stalls and chunks", "[net][qmux]")
{
    for (u64 seed = 1U; seed <= 8U; ++seed)
    {
        CAPTURE(seed);
        Rng rng{seed};
        Pair pair;
        REQUIRE(pair.client.start(kNow));
        REQUIRE(pair.server.start(kNow));
        Line to_server;
        Line to_client;
        u64 up = ~u64{0};
        u64 written = 0U;
        u64 read = 0U;
        u64 datagrams = 0U;
        u64 received = 0U;
        const u64 size = 256U * 1024U;
        u64 now = kNow;
        bool done = false;
        const auto carry = [&](QmuxConnection& from, QmuxConnection& to, Line& line)
        {
            u8 bytes[kBuffer];
            for (usize got = from.take(Span<u8>(bytes), now); got != 0U; got = from.take(Span<u8>(bytes), now))
            {
                for (usize at = 0U; at < got; ++at)
                {
                    line.bytes.push_back(bytes[at]);
                }
            }
            const usize waiting = line.bytes.size() - line.head;
            if (rng.next() % 4U != 0U && waiting != 0U)
            {
                const usize chunk = 1U + static_cast<usize>(rng.next() % waiting);
                to.receive(ConstSpan<u8>(line.bytes.data() + line.head, chunk), now);
                line.head += chunk;
                if (line.head == line.bytes.size())
                {
                    line.bytes.clear();
                    line.head = 0U;
                }
            }
        };
        for (usize turn = 0U; turn < 60000U && !done; ++turn)
        {
            carry(pair.client, pair.server, to_server);
            carry(pair.server, pair.client, to_client);
            now += 100U * kMs / 1000U; // 0.1 ms a turn
            if (pair.client.state() == QmuxState::Established && pair.client.peer_parameters_known() && up == ~u64{0})
            {
                REQUIRE(pair.client.streams().open(true, up) == QuicStreamError::None);
            }
            if (up != ~u64{0})
            {
                write_pattern(pair.client.streams(), up, written, size, 7U);
                done = pair.server.streams().recv_state(up) != QuicRecvState::None &&
                       drain(pair.server.streams(), up, read, 7U) && read == size;
            }
            if (pair.client.peer_parameters_known() && datagrams < 200U)
            {
                const u8 payload[64] = {static_cast<u8>(datagrams)};
                u64 id = 0U;
                if (pair.client.datagrams().queue(ConstSpan<u8>(payload), now, id) == QuicDatagramError::None)
                {
                    ++datagrams;
                }
            }
            ConstSpan<u8> held;
            while (pair.server.datagrams().peek(held))
            {
                ++received;
                pair.server.datagrams().pop();
            }
            QuicDatagramReport report;
            while (pair.client.datagrams().poll(report))
            {
                CHECK(report.outcome == QuicDatagramOutcome::Acked);
            }
        }
        CHECK(done);
        CHECK(read == size);
        CHECK(received <= datagrams);
        CHECK(pair.client.state() == QmuxState::Established);
        CHECK(pair.server.state() == QmuxState::Established);
    }
}

TEST_CASE("QMux: a run over the host's TCP", "[net][qmux][tcp]")
{
    SocketError error = SocketError::None;
    TcpListenOptions listen;
    listen.bind.address = IpAddress::v4_loopback();
    TcpListener listener = TcpListener::open(listen, error);
    REQUIRE(error == SocketError::None);
    TcpSocket client_socket = TcpSocket::connect(listener.local_endpoint(), TcpOptions{}, error);
    REQUIRE(error == SocketError::None);
    REQUIRE(listener.wait_readable(5000U * kMs));
    TcpSocket server_socket = listener.accept(error);
    REQUIRE(error == SocketError::None);
    REQUIRE(client_socket.wait(kStreamWritable, 5000U * kMs) != 0U);
    REQUIRE(client_socket.connected() == SocketError::None);

    Pair pair;
    REQUIRE(pair.client.start(kNow));
    REQUIRE(pair.server.start(kNow));
    u64 id = ~u64{0};
    u64 written = 0U;
    u64 read = 0U;
    const u64 size = 2U * 1024U * 1024U;
    bool done = false;
    const auto service = [&](QmuxConnection& side, TcpSocket& socket)
    {
        u8 bytes[kBuffer];
        for (;;)
        {
            const StreamResult result = socket.read(Span<u8>(bytes));
            if (result.bytes != 0U)
            {
                side.receive(ConstSpan<u8>(bytes, result.bytes), kNow);
            }
            if (result.bytes == 0U)
            {
                break;
            }
        }
        for (usize got = side.take(Span<u8>(bytes), kNow); got != 0U; got = side.take(Span<u8>(bytes), kNow))
        {
            usize at = 0U;
            while (at < got)
            {
                const StreamResult sent = socket.write(ConstSpan<u8>(bytes + at, got - at));
                REQUIRE((sent.error == SocketError::None || sent.error == SocketError::Busy));
                at += sent.bytes;
                if (sent.error == SocketError::Busy)
                {
                    REQUIRE(socket.wait(kStreamWritable, 5000U * kMs) != 0U);
                }
            }
        }
    };
    for (usize turn = 0U; turn < 200000U && !done; ++turn)
    {
        service(pair.client, client_socket);
        service(pair.server, server_socket);
        if (id == ~u64{0} && pair.client.peer_parameters_known())
        {
            REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None);
        }
        if (id != ~u64{0})
        {
            write_pattern(pair.client.streams(), id, written, size, 11U);
            done = pair.server.streams().recv_state(id) != QuicRecvState::None &&
                   drain(pair.server.streams(), id, read, 11U) && read == size;
        }
    }
    CHECK(done);
    pair.client.close(0U, {}, kNow);
    u8 bytes[kBuffer];
    for (usize got = pair.client.take(Span<u8>(bytes), kNow); got != 0U; got = pair.client.take(Span<u8>(bytes), kNow))
    {
        usize at = 0U;
        while (at < got)
        {
            const StreamResult sent = client_socket.write(ConstSpan<u8>(bytes + at, got - at));
            REQUIRE(sent.error == SocketError::None);
            at += sent.bytes;
        }
    }
    REQUIRE(pair.client.send_finished());
    REQUIRE(client_socket.shutdown_send() == SocketError::None);
    for (usize turn = 0U; turn < 1000U && pair.server.state() != QmuxState::Closed; ++turn)
    {
        REQUIRE(server_socket.wait(kStreamReadable, 5000U * kMs) != 0U);
        const StreamResult result = server_socket.read(Span<u8>(bytes));
        if (result.bytes != 0U)
        {
            pair.server.receive(ConstSpan<u8>(bytes, result.bytes), kNow);
        }
        if (result.ended)
        {
            pair.server.transport_ended(true, kNow);
        }
    }
    CHECK(pair.server.state() == QmuxState::Closed);
    CHECK(pair.server.close_info().reason == QmuxCloseReason::Peer);
}

} // namespace

// ---- 0-RTT (draft section 8.2) ----

namespace
{
using crd::crypto::TlsEarlyData;

constexpr u8 kRawContext[] = {'r', 'a', 'w'};

// One fleet's 0-RTT: a replay filter, contexts that allow early data and the client's tickets.
struct Early
{
    ReadyFilter ready;
    Identity identity{false, &ready.filter};
    CountingAllocator allocator;
    TicketCache tickets{allocator, TicketCacheConfig{}};

    [[nodiscard]] QmuxConfig client_config(crypto::TlsTicketStore* store = nullptr)
    {
        QmuxConfig config = config_of(identity.client);
        config.tickets = (store != nullptr) ? store : &tickets;
        return config;
    }

    [[nodiscard]] QmuxConfig server_config() { return config_of(identity.server); }

    // A full handshake, which leaves a ticket that remembers the server's parameters.
    void first_connection(crypto::TlsTicketStore* store = nullptr)
    {
        QmuxConnection client;
        QmuxConnection server;
        REQUIRE(client.init(allocator, client_config(store)));
        REQUIRE(server.init(allocator, server_config()));
        REQUIRE(client.start(kNow));
        REQUIRE(server.start(kNow));
        pump(client, server, kNow);
        REQUIRE(client.state() == QmuxState::Established);
        REQUIRE(client.peer_parameters_known());
    }
};

// Hands what `from` has to send to `to`, once.
void hand(QmuxConnection& from, QmuxConnection& to, u64 now = kNow)
{
    u8 bytes[kBuffer];
    for (usize got = from.take(Span<u8>(bytes), now); got != 0U; got = from.take(Span<u8>(bytes), now))
    {
        to.receive(ConstSpan<u8>(bytes, got), now);
    }
}

bool finished(QuicStreamSet& set, u64 id)
{
    bool seen = false;
    QuicStreamEvent event;
    while (set.poll(event))
    {
        seen = seen || (event.id == id && (event.events & kQuicStreamFinished) != 0U);
    }
    return seen;
}

// Hands a ticket back changed: another allowance, or parameters that no longer parse.
class EditedTickets final : public crypto::TlsTicketStore
{
public:
    void keep(const crypto::TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        m_inner.keep(binding, ticket);
    }

    ConstSpan<u8> take(const crypto::TlsBinding& binding) noexcept override
    {
        const ConstSpan<u8> ticket = m_inner.take(binding);
        if (ticket.empty())
        {
            return ticket;
        }
        std::memcpy(m_edited, ticket.data(), ticket.size());
        if (allowance != 0U)
        {
            m_edited[3] = static_cast<u8>(allowance >> 8U);
            m_edited[4] = static_cast<u8>(allowance);
        }
        if (spoil_parameters)
        {
            m_edited[7] = 0xFFU;
        }
        return ConstSpan<u8>(m_edited, ticket.size());
    }

    u32 allowance = 0U;
    bool spoil_parameters = false;

private:
    Tickets m_inner;
    u8 m_edited[4096] = {};
};

// A TLS server that speaks QMux by hand and accepts early data under a context of its own.
struct RawServer
{
    TlsSession session;

    explicit RawServer(const crypto::TlsContext& context)
    {
        REQUIRE(session.init(context, nullptr, {}, ConstSpan<u8>(kRawContext)) == TlsError::None);
    }

    void read_everything()
    {
        u8 bytes[kBuffer];
        usize got = 1U;
        while (got != 0U)
        {
            const bool early =
                session.state() == TlsState::Handshaking && session.early_data() == TlsEarlyData::Accepted;
            const Span<u8> room(bytes);
            const TlsError error = early ? session.read_early(room, got) : session.read(room, got);
            REQUIRE((error == TlsError::None || error == TlsError::NotReady || error == TlsError::Closed));
            got = (error == TlsError::None) ? got : 0U;
        }
    }

    // Takes the client's first flight and answers nothing yet: the server stands at 0.5-RTT.
    void first_flight(QmuxConnection& client, u64 now = kNow)
    {
        u8 bytes[kBuffer];
        const usize got = client.take(Span<u8>(bytes), now);
        REQUIRE(got != 0U);
        REQUIRE(session.receive(ConstSpan<u8>(bytes, got)) == TlsError::None);
        read_everything();
    }

    void exchange(QmuxConnection& client, u64 now = kNow)
    {
        u8 bytes[kBuffer];
        for (bool moved = true; moved;)
        {
            moved = false;
            for (usize got = client.take(Span<u8>(bytes), now); got != 0U; got = client.take(Span<u8>(bytes), now))
            {
                moved = true;
                REQUIRE(session.receive(ConstSpan<u8>(bytes, got)) == TlsError::None);
            }
            read_everything();
            usize out = 0U;
            REQUIRE(session.take(Span<u8>(bytes), out) == TlsError::None);
            while (out != 0U)
            {
                moved = true;
                client.receive(ConstSpan<u8>(bytes, out), now);
                REQUIRE(session.take(Span<u8>(bytes), out) == TlsError::None);
            }
        }
    }

    // A record of these parameters, sent as 1-RTT or, when `half_rtt`, as 0.5-RTT data.
    void parameters(const Parameters& list, bool half_rtt = false)
    {
        const Bytes frame = Raw::tp_frame(list);
        Bytes bytes = sized(frame.size() + 8U);
        WireWriter writer{Span<u8>(bytes.data(), bytes.size())};
        put_quic_varint(writer, frame.size());
        writer.put_bytes(ConstSpan<u8>(frame.data(), frame.size()));
        const ConstSpan<u8> record(bytes.data(), writer.written());
        REQUIRE((half_rtt ? session.send_half_rtt(record) : session.send(record)) == TlsError::None);
    }
};

const Parameters kRawLimits = {{0x04U, 1U << 16U}, {0x05U, 1U << 15U}, {0x06U, 1U << 15U}, {0x08U, 8U}};

// A connection to a RawServer that leaves a ticket remembering `limits`.
void remember(Early& early, const Parameters& limits)
{
    QmuxConnection client;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(client.start(kNow));
    RawServer server(early.identity.server);
    server.exchange(client);
    REQUIRE(client.state() == QmuxState::Established);
    server.parameters(limits);
    server.exchange(client);
    REQUIRE(client.peer_parameters_known());
}
} // namespace

TEST_CASE("QMux 0-RTT: a resumed client's frames reach its server early and marked, answered at 0.5-RTT",
          "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    REQUIRE(client.handshake().early_data() == TlsEarlyData::Offered);
    CHECK(client.state() == QmuxState::Handshaking);

    // Under the limits the ticket remembers, before the server has said anything.
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    REQUIRE(write_pattern(client.streams(), id, written, 4000U, 3U));
    const u8 payload[100] = {7U};
    u64 datagram = 0U;
    REQUIRE(client.datagrams().queue(ConstSpan<u8>(payload), kNow, datagram) == QuicDatagramError::None);
    CHECK(client.wants_write(kNow));

    hand(client, server);
    REQUIRE(server.state() == QmuxState::Established);
    CHECK(server.handshake().state() == TlsState::Handshaking);
    CHECK(server.handshake().early_data() == TlsEarlyData::Accepted);
    CHECK(server.peer_parameters_known());
    u64 read = 0U;
    u64 marked = 0U;
    CHECK(drain(server.streams(), id, read, 3U, &marked));
    CHECK(read == 4000U);
    CHECK(marked == 4000U);
    CHECK(server.datagrams().oldest_early());
    server.datagrams().pop();

    // Nothing counts as delivered until the server has answered.
    QuicDatagramReport report;
    CHECK_FALSE(client.datagrams().poll(report));
    CHECK_FALSE(finished(client.streams(), id));

    // The answer goes before the client's Finished, a round trip sooner than the handshake allows.
    u64 answered = 0U;
    REQUIRE(write_pattern(server.streams(), id, answered, 2000U, 5U));
    hand(server, client);
    REQUIRE(client.state() == QmuxState::Established);
    CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
    CHECK(client.peer_parameters_known());
    REQUIRE(client.datagrams().poll(report));
    CHECK(report.id == datagram);
    CHECK(report.outcome == QuicDatagramOutcome::Acked);
    CHECK(finished(client.streams(), id));
    u64 back = 0U;
    CHECK(drain(client.streams(), id, back, 5U));
    CHECK(back == 2000U);

    CHECK(server.handshake().state() == TlsState::Handshaking);
    hand(client, server);
    CHECK(server.handshake().state() == TlsState::Established);
    CHECK(server.next_timer_ns() != kNow + QmuxConfig{}.handshake_timeout_ns);

    // Once the handshake is over, data carries no mark.
    u64 later = 0U;
    REQUIRE(client.streams().open(true, later) == QuicStreamError::None);
    u64 later_written = 0U;
    REQUIRE(write_pattern(client.streams(), later, later_written, 1000U, 9U));
    pump(client, server, kNow);
    u64 later_read = 0U;
    u64 later_marked = 0U;
    CHECK(drain(server.streams(), later, later_read, 9U, &later_marked));
    CHECK(later_read == 1000U);
    CHECK(later_marked == 0U);
    CHECK(client.state() == QmuxState::Established);
    CHECK(server.state() == QmuxState::Established);
}

TEST_CASE("QMux 0-RTT: every cut of the flights still delivers it", "[net][qmux][qmux-early]")
{
    for (const usize chunk : {usize{1U}, usize{3U}, usize{7U}, usize{64U}, usize{1000U}})
    {
        CAPTURE(chunk);
        Early early;
        early.first_connection();
        QmuxConnection client;
        QmuxConnection server;
        REQUIRE(client.init(early.allocator, early.client_config()));
        REQUIRE(server.init(early.allocator, early.server_config()));
        REQUIRE(client.start(kNow));
        REQUIRE(server.start(kNow));
        u64 id = 0U;
        REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
        u64 written = 0U;
        REQUIRE(write_pattern(client.streams(), id, written, 9000U, 4U));
        pump(client, server, kNow, chunk);
        CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
        u64 read = 0U;
        u64 marked = 0U;
        CHECK(drain(server.streams(), id, read, 4U, &marked));
        CHECK(read == 9000U);
        CHECK(marked == 9000U);
    }
}

TEST_CASE("QMux 0-RTT: what the ticket does not allow goes after the handshake, in order", "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    REQUIRE(write_pattern(client.streams(), id, written, 30000U, 6U));
    pump(client, server, kNow);
    u64 read = 0U;
    u64 marked = 0U;
    CHECK(drain(server.streams(), id, read, 6U, &marked));
    CHECK(read == 30000U);
    CHECK(marked > 0U);
    CHECK(marked <= crypto::kTlsEarlyDataBytes);
}

TEST_CASE("QMux 0-RTT: a server that refuses it gets everything again, parameters first", "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    const bool plain = GENERATE(false, true);
    const bool other_limits = GENERATE(false, true);
    const bool other_application = GENERATE(false, true);
    CAPTURE(plain, other_limits, other_application);
    const u8 schema[] = {2U};
    QmuxConfig server_config = plain ? config_of(early.identity.plain) : early.server_config();
    if (other_limits)
    {
        server_config.streams.stream_window /= 2U;
    }
    if (other_application && !plain)
    {
        server_config.early_context = ConstSpan<u8>(schema);
    }
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, server_config));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    REQUIRE(client.handshake().early_data() == TlsEarlyData::Offered);

    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    REQUIRE(write_pattern(client.streams(), id, written, 3000U, 3U));
    const u8 payload[100] = {7U};
    u64 sent = 0U;
    REQUIRE(client.datagrams().queue(ConstSpan<u8>(payload), kNow, sent) == QuicDatagramError::None);
    pump(client, server, kNow);

    const bool refused = plain || other_limits || other_application;
    CHECK(client.handshake().early_data() == (refused ? TlsEarlyData::Rejected : TlsEarlyData::Accepted));
    REQUIRE(client.state() == QmuxState::Established);
    REQUIRE(server.state() == QmuxState::Established);
    CHECK(client.peer_parameters_known());
    CHECK(server.peer_parameters_known());
    QuicDatagramReport report;
    REQUIRE(client.datagrams().poll(report));
    CHECK(report.id == sent);
    CHECK(report.outcome == (refused ? QuicDatagramOutcome::Lost : QuicDatagramOutcome::Acked));
    if (!refused)
    {
        return;
    }
    CHECK(server.handshake().resumed()); // a ticket's early data is what was refused, never its resumption
    CHECK_FALSE(finished(client.streams(), id));
    CHECK(client.streams().send_state(id) == QuicSendState::None);
    CHECK(server.streams().recv_state(id) == QuicRecvState::None);
    ConstSpan<u8> held;
    CHECK_FALSE(server.datagrams().peek(held));

    // The application starts again, now under what the server said.
    u64 again = 0U;
    REQUIRE(client.streams().open(true, again) == QuicStreamError::None);
    CHECK(again == id);
    written = 0U;
    REQUIRE(write_pattern(client.streams(), again, written, 3000U, 8U));
    REQUIRE(client.datagrams().queue(ConstSpan<u8>(payload), kNow, sent) == QuicDatagramError::None);
    pump(client, server, kNow);
    u64 read = 0U;
    u64 marked = 0U;
    CHECK(drain(server.streams(), again, read, 8U, &marked));
    CHECK(read == 3000U);
    CHECK(marked == 0U);
    REQUIRE(server.datagrams().peek(held));
    CHECK_FALSE(server.datagrams().oldest_early());
    CHECK(client.state() == QmuxState::Established);
    CHECK(server.state() == QmuxState::Established);
}

TEST_CASE("QMux 0-RTT: a replayed first flight delivers nothing a second time", "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    QmuxConnection client;
    QmuxConnection server;
    QmuxConnection replayed;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(replayed.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    REQUIRE(replayed.start(kNow));
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    REQUIRE(write_pattern(client.streams(), id, written, 1000U, 3U));
    u8 flight[kBuffer];
    const usize bytes = client.take(Span<u8>(flight), kNow);
    REQUIRE(bytes != 0U);
    server.receive(ConstSpan<u8>(flight, bytes), kNow);
    CHECK(server.handshake().early_data() == TlsEarlyData::Accepted);
    replayed.receive(ConstSpan<u8>(flight, bytes), kNow);
    CHECK(replayed.handshake().early_data() == TlsEarlyData::Replayed);
    CHECK(replayed.state() == QmuxState::Handshaking);
    CHECK_FALSE(replayed.peer_parameters_known());
    CHECK(replayed.streams().recv_state(id) == QuicRecvState::None);
}

TEST_CASE("QMux 0-RTT: a server's parameters may not fall below what the early frames kept to",
          "[net][qmux][qmux-early]")
{
    Early early;
    remember(early, kRawLimits);
    struct Case
    {
        u64 max_data;
        u64 bidi;
        bool refused;
    };
    const Case item = GENERATE(Case{1U << 16U, 1U << 15U, false}, Case{1U << 17U, 1U << 16U, false},
                               Case{(1U << 16U) - 1U, 1U << 15U, true}, Case{1U << 16U, (1U << 15U) - 1U, true});
    CAPTURE(item.max_data, item.bidi);
    QmuxConnection client;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(client.handshake().early_data() == TlsEarlyData::Offered);
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    RawServer server(early.identity.server);
    server.first_flight(client);
    REQUIRE(server.session.early_data() == TlsEarlyData::Accepted);
    server.parameters({{0x04U, item.max_data}, {0x05U, 1U << 15U}, {0x06U, item.bidi}, {0x08U, 8U}}, true);
    server.exchange(client);
    if (item.refused)
    {
        CHECK(client.state() == QmuxState::Closing);
        CHECK(client.close_info().reason == QmuxCloseReason::Error);
        CHECK(client.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
    }
    else
    {
        CHECK(client.state() == QmuxState::Established);
        CHECK(client.peer_parameters().initial_max_data == item.max_data);
    }
}

TEST_CASE("QMux 0-RTT: a ticket carries the server's parameters, and they grant the client's early streams",
          "[net][qmux][qmux-early]")
{
    Early early;
    Tickets store;
    early.first_connection(&store);
    CHECK(store.holds());
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config(&store)));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    CHECK(client.handshake().early_data() == TlsEarlyData::Offered);
    CHECK_FALSE(client.handshake().remembered_parameters().empty());
    CHECK(client.wants_write(kNow));
    CHECK(client.streams().send_state(0U) == QuicSendState::None);
    // The ticket's own grants, before any byte from the server.
    u64 id = 0U;
    CHECK(client.streams().open(true, id) == QuicStreamError::None);
    pump(client, server, kNow);
    CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
    CHECK(store.holds()); // the server's next ticket
}

TEST_CASE("QMux 0-RTT: a client without a usable ticket sends nothing early and starts normally",
          "[net][qmux][qmux-early]")
{
    Early early;
    EditedTickets store;
    early.first_connection(&store);
    const usize edit = GENERATE(0U, 1U, 2U);
    CAPTURE(edit);
    if (edit == 0U)
    {
        store.allowance = 64U; // less than the transport parameters frame
    }
    else if (edit == 1U)
    {
        store.spoil_parameters = true;
    }
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config(&store)));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    CHECK(client.handshake().early_data() == TlsEarlyData::Offered);
    u64 id = 0U;
    const bool early_credit = edit == 2U;
    CHECK((client.streams().open(true, id) == QuicStreamError::None) == early_credit);
    hand(client, server);
    CHECK(server.peer_parameters_known() == early_credit);
    pump(client, server, kNow);
    CHECK(client.state() == QmuxState::Established);
    CHECK(server.state() == QmuxState::Established);
    CHECK(client.peer_parameters_known());
    CHECK(server.peer_parameters_known());
}

TEST_CASE("QMux 0-RTT: a ticket without parameters offers no early frames", "[net][qmux][qmux-early]")
{
    Early early;
    Tickets store;
    {
        TlsSession plain;
        RawServer server(early.identity.server);
        REQUIRE(plain.init(early.identity.client, &store) == TlsError::None);
        u8 bytes[kBuffer];
        usize got = 0U;
        for (usize round = 0U; round < 6U; ++round)
        {
            REQUIRE(plain.take(Span<u8>(bytes), got) == TlsError::None);
            REQUIRE(server.session.receive(ConstSpan<u8>(bytes, got)) == TlsError::None);
            REQUIRE(server.session.take(Span<u8>(bytes), got) == TlsError::None);
            REQUIRE(plain.receive(ConstSpan<u8>(bytes, got)) == TlsError::None);
            usize none = 0U;
            REQUIRE(((plain.state() != TlsState::Established) || plain.read(Span<u8>(bytes), none) == TlsError::None));
        }
    }
    REQUIRE(store.holds());
    QmuxConnection client;
    REQUIRE(client.init(early.allocator, early.client_config(&store)));
    REQUIRE(client.start(kNow));
    CHECK(client.handshake().early_data() == TlsEarlyData::Offered);
    CHECK(client.handshake().remembered_parameters().empty());
    u64 id = 0U;
    CHECK(client.streams().open(true, id) != QuicStreamError::None);
    u8 bytes[kBuffer];
    CHECK(client.take(Span<u8>(bytes), kNow) != 0U); // the ClientHello alone
    CHECK(client.counters().records_sent == 0U);
}

TEST_CASE("QMux 0-RTT: what is written after the first flight still goes early", "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    hand(client, server);
    CHECK_FALSE(client.wants_write(kNow));

    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    REQUIRE(write_pattern(client.streams(), id, written, 500U, 3U));
    CHECK(client.wants_write(kNow));
    hand(client, server);
    CHECK(client.handshake().early_data() == TlsEarlyData::Offered);
    u64 read = 0U;
    u64 marked = 0U;
    CHECK(drain(server.streams(), id, read, 3U, &marked));
    CHECK(read == 500U);
    CHECK(marked == 500U);

    // A stream reset early counts as delivered only once the server has answered, and then frees its slot.
    u64 dropped = 0U;
    REQUIRE(client.streams().open(true, dropped) == QuicStreamError::None);
    REQUIRE(client.streams().reset(dropped, 11U) == QuicStreamError::None);
    hand(client, server);
    CHECK(client.streams().send_state(dropped) == QuicSendState::ResetSent);
    pump(client, server, kNow);
    CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
    CHECK(client.streams().send_state(dropped) == QuicSendState::ResetRecvd);
}

TEST_CASE("QMux 0-RTT: a frame too large for the ticket's allowance waits; records stay within it",
          "[net][qmux][qmux-early]")
{
    Early early;
    EditedTickets store;
    early.first_connection(&store);
    store.allowance = 200U;
    QmuxConfig client_config = early.client_config(&store);
    client_config.padding_block = 4096U;
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, client_config));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    const u8 payload[400] = {7U};
    u64 datagram = 0U;
    REQUIRE(client.datagrams().queue(ConstSpan<u8>(payload), kNow, datagram) == QuicDatagramError::None);
    u8 bytes[kBuffer];
    usize got = client.take(Span<u8>(bytes), kNow);
    REQUIRE(got != 0U);
    CHECK(client.state() == QmuxState::Handshaking);
    CHECK(client.counters().records_sent == 1U);
    CHECK(client.handshake().early_data_room() < 200U);
    CHECK_FALSE(client.wants_write(kNow)); // nothing more fits: no busy turns
    server.receive(ConstSpan<u8>(bytes, got), kNow);
    CHECK(server.peer_parameters_known());
    ConstSpan<u8> held;
    CHECK_FALSE(server.datagrams().peek(held));
    pump(client, server, kNow);
    REQUIRE(server.datagrams().peek(held));
    CHECK(held.size() == sizeof(payload));
    CHECK_FALSE(server.datagrams().oldest_early());
    QuicDatagramReport report;
    REQUIRE(client.datagrams().poll(report));
    CHECK(report.outcome == QuicDatagramOutcome::Acked);
}

TEST_CASE("QMux 0-RTT: a record cut by the end of the early data counts as early", "[net][qmux][qmux-early]")
{
    Early early;
    Tickets store;
    early.first_connection(&store);
    QmuxConnection server;
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(server.start(kNow));
    TlsSession peer;
    REQUIRE(peer.init(early.identity.client, &store) == TlsError::None);
    REQUIRE(peer.early_data() == TlsEarlyData::Offered);

    const Bytes parameters = Raw::tp_frame({{0x04U, 1U << 20U}, {0x05U, 1U << 16U}, {0x06U, 1U << 16U}, {0x08U, 16U}});
    const Bytes data = sized(200U, 0x5AU);
    const Bytes stream = stream_frame(0U, 0U, data);
    Bytes record = sized(parameters.size() + stream.size() + 8U);
    WireWriter writer{Span<u8>(record.data(), record.size())};
    put_quic_varint(writer, parameters.size() + stream.size());
    writer.put_bytes(ConstSpan<u8>(parameters.data(), parameters.size()));
    writer.put_bytes(ConstSpan<u8>(stream.data(), stream.size()));
    const ConstSpan<u8> whole(record.data(), writer.written());
    const usize cut = whole.size() / 2U;
    REQUIRE(peer.send_early(whole.first(cut)) == TlsError::None);

    u8 bytes[kBuffer];
    usize got = 0U;
    REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
    server.receive(ConstSpan<u8>(bytes, got), kNow);
    CHECK(server.state() == QmuxState::Established);
    CHECK_FALSE(server.peer_parameters_known()); // half a record
    for (usize out = server.take(Span<u8>(bytes), kNow); out != 0U; out = server.take(Span<u8>(bytes), kNow))
    {
        REQUIRE(peer.receive(ConstSpan<u8>(bytes, out)) == TlsError::None);
    }
    REQUIRE(peer.state() == TlsState::Established);
    REQUIRE(peer.send(whole.subspan(cut)) == TlsError::None);
    REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
    server.receive(ConstSpan<u8>(bytes, got), kNow);
    REQUIRE(server.handshake().state() == TlsState::Established);
    CHECK(server.peer_parameters_known());
    const QuicStreamRead read = server.streams().peek(0U);
    CHECK(read.data.size() == data.size());
    CHECK(read.early_bytes == data.size());
}

TEST_CASE("QMux 0-RTT: the first early frame must be the transport parameters", "[net][qmux][qmux-early]")
{
    Early early;
    Tickets store;
    early.first_connection(&store);
    QmuxConnection server;
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(server.start(kNow));
    TlsSession peer;
    REQUIRE(peer.init(early.identity.client, &store) == TlsError::None);
    const Bytes data = sized(16U, 1U);
    const Bytes stream = stream_frame(0U, 0U, data);
    Bytes record = sized(stream.size() + 8U);
    WireWriter writer{Span<u8>(record.data(), record.size())};
    put_quic_varint(writer, stream.size());
    writer.put_bytes(ConstSpan<u8>(stream.data(), stream.size()));
    REQUIRE(peer.send_early(ConstSpan<u8>(record.data(), writer.written())) == TlsError::None);
    u8 bytes[kBuffer];
    usize got = 0U;
    REQUIRE(peer.take(Span<u8>(bytes), got) == TlsError::None);
    server.receive(ConstSpan<u8>(bytes, got), kNow);
    CHECK(server.state() == QmuxState::Closing);
    CHECK(server.close_info().reason == QmuxCloseReason::Error);
    CHECK(server.close_info().error_code == static_cast<u64>(QuicTransportError::ProtocolViolation));
    // The CONNECTION_CLOSE goes at 0.5-RTT, the handshake still unfinished.
    const usize out = server.take(Span<u8>(bytes), kNow);
    REQUIRE(out != 0U);
    CHECK(server.send_finished());
    REQUIRE(peer.receive(ConstSpan<u8>(bytes, out)) == TlsError::None);
    REQUIRE(peer.state() == TlsState::Established);
    u8 plain[256];
    REQUIRE(peer.read(Span<u8>(plain), got) == TlsError::None);
    CHECK(got != 0U);
}

TEST_CASE("QMux 0-RTT: a client that never finishes its handshake times out", "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    hand(client, server);
    REQUIRE(server.state() == QmuxState::Established);
    REQUIRE(server.handshake().state() == TlsState::Handshaking);
    const u64 deadline = kNow + QmuxConfig{}.handshake_timeout_ns;
    CHECK(server.next_timer_ns() == deadline);
    server.on_timeout(deadline - 1U);
    CHECK(server.state() == QmuxState::Established);
    server.on_timeout(deadline);
    CHECK(server.state() == QmuxState::Closed);
    CHECK(server.close_info().reason == QmuxCloseReason::HandshakeTimeout);
}

TEST_CASE("QMux 0-RTT: a server may close during its 0.5-RTT", "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, early.server_config()));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    hand(client, server);
    REQUIRE(server.state() == QmuxState::Established);
    server.close(9U, {}, kNow);
    hand(server, client);
    CHECK(client.state() == QmuxState::Closed);
    CHECK(client.close_info().reason == QmuxCloseReason::Peer);
    CHECK(client.close_info().application);
    CHECK(client.close_info().error_code == 9U);
    CHECK(server.send_finished());
}

TEST_CASE("QMux 0-RTT: configurations are held to where early data is accepted", "[net][qmux][qmux-early]")
{
    Early early;
    const u8 schema[] = {1U};
    const Bytes huge = sized(kQuicMaxEarlyContextBytes + 1U);
    QmuxConfig config = early.server_config();
    config.early_context = ConstSpan<u8>(huge.data(), huge.size());
    QmuxConnection oversized;
    CHECK_FALSE(oversized.init(early.allocator, config));
    config.early_context = ConstSpan<u8>(huge.data(), huge.size() - 1U);
    QmuxConnection largest;
    REQUIRE(largest.init(early.allocator, config));
    CHECK(largest.start(kNow));
    config = early.client_config();
    config.early_context = ConstSpan<u8>(schema);
    QmuxConnection client;
    CHECK_FALSE(client.init(early.allocator, config));
    config = config_of(early.identity.plain);
    config.early_context = ConstSpan<u8>(schema);
    QmuxConnection plain;
    CHECK_FALSE(plain.init(early.allocator, config));
}

TEST_CASE("QMux 0-RTT: nothing allocates after init", "[net][qmux][qmux-early]")
{
    Early early;
    early.first_connection();
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(early.allocator, early.client_config()));
    REQUIRE(server.init(early.allocator, early.server_config()));
    const usize calls = early.allocator.calls();
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    u64 id = 0U;
    REQUIRE(client.streams().open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    REQUIRE(write_pattern(client.streams(), id, written, 2000U, 3U));
    pump(client, server, kNow);
    CHECK(client.handshake().early_data() == TlsEarlyData::Accepted);
    CHECK(early.allocator.calls() - calls <= 2U); // the ticket cache's copy of the new ticket, and nothing else
}
