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
