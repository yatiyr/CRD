// crd-net -- HTTP/2 on the stream path: a TLS connection that agrees on h2 bears WebSocket tunnels (RFC 8441), each a
// connection of the host (4.2a; docs/design/network-foundation.md#h2-bearer).

#include "ingress_fixture.hpp"
#include "loop_fixture.hpp"
#include "quic_connection_fixture.hpp"
#include "quic_stream_fixture.hpp"
#include "stream_pipe_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/net/hpack.hpp>
#include <crd/net/http2.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/qmux_host.hpp>
#include <crd/net/ticket_cache.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::crypto::TlsError;
using crd::crypto::TlsSession;
using crd::crypto::TlsState;

using Bytes = crd::containers::Array<u8>;

constexpr u64 kNow = 1'000'000'000U;
constexpr u64 kMsNs = 1'000'000U;
constexpr usize kBuffer = 16U * 1024U;
constexpr StringView kH2Protocols[] = {"h2"};
constexpr StringView kHttp11Protocols[] = {"http/1.1"};
constexpr StringView kServerProtocols[] = {"cerid-game/1", "http/1.1", "h2"};
constexpr u8 kFrameData = 0x0U;
constexpr u8 kFrameHeaders = 0x1U;
constexpr u8 kFrameSettings = 0x4U;
constexpr u8 kFrameGoAway = 0x7U;
constexpr u8 kEndHeaders = 0x4U;
constexpr char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

ConstSpan<u8> span_of(const char* text)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text), std::strlen(text));
}

void append(Bytes& to, ConstSpan<u8> more)
{
    const usize old = to.size();
    to.resize(old + more.size());
    if (!more.empty())
    {
        std::memcpy(to.data() + old, more.data(), more.size());
    }
}

ConstSpan<u8> view(const Bytes& bytes)
{
    return ConstSpan<u8>(bytes.data(), bytes.size());
}

HttpField field(const char* name, const char* value)
{
    HttpField out;
    out.name = span_of(name);
    out.value = span_of(value);
    return out;
}

// The contexts: clients asking for h2, for QMux and for http/1.1, an h2 client asking for early data (a raw session's:
// Cerid's own refuses it), and a server listing all three.
struct H2Identity
{
    crypto::SigningKey key;
    crypto::TlsCredential credential;
    crypto::TlsContext h2_client;
    crypto::TlsContext qmux_client;
    crypto::TlsContext ws_client;
    crypto::TlsContext early_client;
    crypto::TlsContext server;

    explicit H2Identity(crypto::ReplayFilter* early = nullptr)
    {
        u8 seed[32] = {9U, 9U, 9U};
        REQUIRE(key.from_seed(crypto::AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) ==
                crypto::CryptoError::None);
        REQUIRE(credential.init(key, kServerName) == TlsError::None);
        const crypto::KeyId pin = credential.key_id();
        crypto::TlsConfig tls;
        tls.role = crypto::TlsRole::Client;
        tls.groups = kGroups;
        tls.signatures = kSignatures;
        tls.aeads = kAeads;
        tls.pinned = ConstSpan<crypto::KeyId>(&pin, 1U);
        tls.server_name = kServerName;
        tls.application_protocols = kH2Protocols;
        REQUIRE(h2_client.init(tls) == TlsError::None);
        tls.application_protocols = kProtocols;
        REQUIRE(qmux_client.init(tls) == TlsError::None);
        tls.application_protocols = kHttp11Protocols;
        REQUIRE(ws_client.init(tls) == TlsError::None);
        tls.application_protocols = kH2Protocols;
        tls.early_data = true;
        REQUIRE(early_client.init(tls) == TlsError::None);
        crypto::TlsConfig answer;
        answer.role = crypto::TlsRole::Server;
        answer.groups = kGroups;
        answer.aeads = kAeads;
        answer.credential = &credential;
        answer.server_name = kServerName;
        answer.application_protocols = kServerProtocols;
        if (early != nullptr)
        {
            answer.ticket_key = ConstSpan<u8>(kTicketKey);
            answer.early_data = true;
            answer.early_data_filter = early;
        }
        REQUIRE(server.init(answer) == TlsError::None);
    }
};

// One WebSocket policy for both ends, and an HTTP/2 configuration for each.
struct Setup
{
    ConstSpan<u8> protocols[1];
    WebSocketConfig websocket;
    Http2Config server_http2;
    Http2Config client_http2;

    Setup()
    {
        protocols[0] = span_of("qmux-02.game");
        websocket.protocols = ConstSpan<ConstSpan<u8>>(protocols, 1U);
        websocket.path = span_of("/game");
        websocket.host = span_of(kServerName);
        websocket.any_origin = true;
        server_http2.connect_protocol = true;
    }

    [[nodiscard]] QmuxConfig config(const crypto::TlsContext* tls, bool client, u64 idle_ms = 30000U) const
    {
        QmuxConfig config;
        config.tls = tls;
        config.websocket = &websocket;
        config.http2 = client ? &client_http2 : &server_http2;
        config.http2_tunnels = 4U;
        config.streams.peer_bidi = 8U;
        config.streams.peer_uni = 4U;
        config.streams.local_bidi = 8U;
        config.streams.local_uni = 4U;
        config.streams.receive_bytes = 64U * 1024U;
        config.streams.send_bytes = 64U * 1024U;
        config.streams.stream_window = 32U * 1024U;
        config.streams.connection_window = 64U * 1024U;
        config.streams.connection_max_window = 256U * 1024U;
        config.datagrams.max_frame_bytes = 1200U;
        config.datagrams.receive_bytes = 8192U;
        config.datagrams.receive_count = 32U;
        config.datagrams.send_bytes = 8192U;
        config.datagrams.send_count = 32U;
        config.idle_timeout_ns = idle_ms * kMsNs;
        return config;
    }
};

void pump(QmuxConnection& a, QmuxConnection& b, u64 now)
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
                to.receive(ConstSpan<u8>(bytes, got), now);
            }
        }
    }
}

// Every tunnel event of `connection` until none is left; the last one of `kind`, if any.
struct TunnelLog
{
    usize opened = 0U;
    usize closed = 0U;
    usize refused = 0U;
    QmuxConnection* last_opened = nullptr;
    QmuxCloseInfo last_end;

    void poll(QmuxConnection& connection, u64 now)
    {
        WsTunnelEvent event;
        while (connection.poll_tunnel(event, now))
        {
            REQUIRE(event.connection != nullptr);
            if (event.kind == WsTunnelEventKind::Opened)
            {
                ++opened;
                last_opened = event.connection;
            }
            else if (event.kind == WsTunnelEventKind::Closed || event.kind == WsTunnelEventKind::Refused)
            {
                closed += (event.kind == WsTunnelEventKind::Closed) ? 1U : 0U;
                refused += (event.kind == WsTunnelEventKind::Refused) ? 1U : 0U;
                REQUIRE(event.connection->state() == QmuxState::Closed);
                last_end = event.connection->close_info();
            }
        }
    }
};

// ---- A peer of its own: TLS and HTTP/2 frames by hand, for what Cerid's client never sends ----

Bytes frame(u8 type, u8 flags, u32 stream, ConstSpan<u8> payload)
{
    Bytes out;
    out.resize(kHttp2FrameHeaderBytes + payload.size());
    out[0] = static_cast<u8>(payload.size() >> 16U);
    out[1] = static_cast<u8>(payload.size() >> 8U);
    out[2] = static_cast<u8>(payload.size());
    out[3] = type;
    out[4] = flags;
    out[5] = static_cast<u8>(stream >> 24U);
    out[6] = static_cast<u8>(stream >> 16U);
    out[7] = static_cast<u8>(stream >> 8U);
    out[8] = static_cast<u8>(stream);
    if (!payload.empty())
    {
        std::memcpy(out.data() + kHttp2FrameHeaderBytes, payload.data(), payload.size());
    }
    return out;
}

// A client's preface and an empty SETTINGS.
Bytes opening()
{
    Bytes out;
    append(out, span_of(kPreface));
    append(out, view(frame(kFrameSettings, 0U, 0U, {})));
    return out;
}

const HttpField kWebSocketConnect[] = {field(":method", "CONNECT"),     field(":protocol", "websocket"),
                                       field(":scheme", "https"),       field(":authority", kServerName),
                                       field(":path", "/game"),         field("sec-websocket-version", "13"),
                                       field("sec-websocket-protocol", "qmux-02.game")};
const HttpField kGet[] = {field(":method", "GET"), field(":scheme", "https"), field(":authority", kServerName),
                          field(":path", "/index.html")};

struct RawPeer
{
    CountingAllocator allocator;
    HpackEncoder encoder; // one table for every request, as the server's decoder keeps one
    TlsSession tls;
    Bytes plain; // what the server sent, decrypted
    bool closed = false;

    RawPeer() { REQUIRE(encoder.init(allocator, kHpackDefaultTableSize)); }

    // One request on `stream` that ends its headers.
    [[nodiscard]] Bytes request(u32 stream, ConstSpan<HttpField> fields)
    {
        u8 block[1024];
        const usize used = encoder.encode(fields, Span<u8>(block, sizeof(block)));
        REQUIRE(used != 0U);
        return frame(kFrameHeaders, kEndHeaders, stream, ConstSpan<u8>(block, used));
    }

    // Bytes both ways until neither side moves.
    void exchange(QmuxConnection& server, u64 now)
    {
        u8 bytes[kBuffer];
        for (bool moved = true; moved;)
        {
            moved = false;
            usize got = 0U;
            REQUIRE(tls.take(Span<u8>(bytes), got) == TlsError::None);
            if (got != 0U)
            {
                moved = true;
                server.receive(ConstSpan<u8>(bytes, got), now);
            }
            const usize out = server.take(Span<u8>(bytes), now);
            if (out != 0U)
            {
                moved = true;
                const TlsError received = tls.receive(ConstSpan<u8>(bytes, out));
                closed = closed || received == TlsError::Closed;
            }
            for (usize read = 1U; read != 0U && tls.state() == TlsState::Established;)
            {
                read = 0U;
                const TlsError error = tls.read(Span<u8>(bytes), read);
                closed = closed || error == TlsError::Closed;
                append(plain, ConstSpan<u8>(bytes, read));
            }
        }
    }

    // The :status of the response on `stream`; 0 when none came.
    [[nodiscard]] u32 status(u32 stream) const
    {
        CountingAllocator tables;
        HpackDecoder decoder;
        REQUIRE(decoder.init(tables, kHpackDefaultTableSize));
        for (usize at = 0U; at + kHttp2FrameHeaderBytes <= plain.size();)
        {
            const usize length = (usize{plain[at]} << 16U) | (usize{plain[at + 1U]} << 8U) | plain[at + 2U];
            const u32 id = (u32{plain[at + 5U]} << 24U) | (u32{plain[at + 6U]} << 16U) | (u32{plain[at + 7U]} << 8U) |
                           plain[at + 8U];
            const ConstSpan<u8> payload(plain.data() + at + kHttp2FrameHeaderBytes, length);
            if (plain[at + 3U] == kFrameHeaders)
            {
                HttpField fields[16];
                u8 text[1024];
                usize count = 0U;
                REQUIRE(decoder.decode(payload, Span<HttpField>(fields, 16U), Span<u8>(text, sizeof(text)), count) ==
                        HpackStatus::Complete);
                if (id == stream && count != 0U && same(fields[0].name, span_of(":status")))
                {
                    return (u32{fields[0].value[0]} - '0') * 100U + (u32{fields[0].value[1]} - '0') * 10U +
                           (u32{fields[0].value[2]} - '0');
                }
            }
            at += kHttp2FrameHeaderBytes + length;
        }
        return 0U;
    }

    // The error code of the first GOAWAY the server sent; ~0 when none came.
    [[nodiscard]] u32 goaway() const
    {
        for (usize at = 0U; at + kHttp2FrameHeaderBytes <= plain.size();)
        {
            const usize length = (usize{plain[at]} << 16U) | (usize{plain[at + 1U]} << 8U) | plain[at + 2U];
            if (plain[at + 3U] == kFrameGoAway && length >= 8U)
            {
                const u8* const code = plain.data() + at + kHttp2FrameHeaderBytes + 4U;
                return (u32{code[0]} << 24U) | (u32{code[1]} << 16U) | (u32{code[2]} << 8U) | code[3];
            }
            at += kHttp2FrameHeaderBytes + length;
        }
        return ~u32{0};
    }
};

// A raw h2 client established against `server`, with `bytes` sent once the handshake is done.
void raw_session(RawPeer& peer, const crypto::TlsContext& context, QmuxConnection& server, ConstSpan<u8> bytes)
{
    REQUIRE(peer.tls.init(context) == TlsError::None);
    REQUIRE(server.start(kNow));
    peer.exchange(server, kNow);
    REQUIRE(peer.tls.state() == TlsState::Established);
    REQUIRE(server.http2());
    REQUIRE(peer.tls.send(bytes) == TlsError::None);
    peer.exchange(server, kNow);
}

// ---- Hosts over pipes ----

// The application's side of a host: every connection it reports, bearers and tunnels alike. A tunnel carries
// `transfer` bytes each way on the client's first stream.
struct App final : QmuxHostEvents
{
    struct Flow
    {
        bool tunnel = false;
        bool opened = false;
        bool read_done = false;
        u64 written = 0U;
        u64 read = 0U;
    };

    struct Ended
    {
        QuicHandle handle;
        QmuxCloseInfo info;
        bool bearer = false;
    };

    static constexpr usize kMost = 16U;

    void on_accept(QuicHandle handle, QmuxConnection& connection, const Endpoint& peer) noexcept override
    {
        ++accepted;
        last_peer = peer;
        flows[handle.slot] = Flow{};
        flows[handle.slot].tunnel = connection.websocket();
        tunnels_accepted += connection.websocket() ? 1U : 0U;
    }

    void on_service(QuicHandle handle, QmuxConnection& connection, u64) noexcept override
    {
        const bool ended_here = connection.state() == QmuxState::Closed;
        closed_services += ended_here ? 1U : 0U;
        if (connection.http2() || transfer == 0U ||
            (!ended_here && (connection.state() != QmuxState::Established || !connection.peer_parameters_known())))
        {
            return;
        }
        Flow& flow = flows[handle.slot];
        if (client && !flow.opened)
        {
            u64 id = 0U;
            flow.opened = connection.streams().open(true, id) == QuicStreamError::None;
        }
        flow.opened = flow.opened || connection.streams().recv_state(0U) != QuicRecvState::None;
        if (flow.opened && flow.written < transfer)
        {
            static_cast<void>(write_pattern(connection.streams(), 0U, flow.written, transfer, client ? 3U : 5U));
        }
        if (flow.opened && !flow.read_done)
        {
            flow.read_done = drain(connection.streams(), 0U, flow.read, client ? 5U : 3U);
        }
    }

    void on_closed(QuicHandle handle, const QmuxConnection& connection, SocketError) noexcept override
    {
        REQUIRE(ended_count < kMost);
        ended[ended_count++] = Ended{handle, connection.close_info(), connection.http2()};
    }

    [[nodiscard]] bool done(u32 slot) const noexcept { return flows[slot].read_done && flows[slot].read == transfer; }

    bool client = false;
    u64 transfer = 0U;
    usize accepted = 0U;
    usize tunnels_accepted = 0U;
    usize closed_services = 0U;
    Endpoint last_peer;
    Flow flows[kMost];
    Ended ended[kMost];
    usize ended_count = 0U;
};

struct Hosts
{
    H2Identity identity;
    Setup setup;
    CountingAllocator allocator;
    TestClock clock;
    PipeNetwork network;
    Endpoint server_address{IpAddress::v4_loopback(), 4433U};
    PipePoller server_poller{network, server_address};
    PipePoller client_poller{network, Endpoint{IpAddress::v4_loopback(), 50000U}};
    HandshakeBuckets buckets;
    App server_events;
    App client_events;
    std::unique_ptr<QmuxHost> server;
    std::unique_ptr<QmuxHost> client;

    explicit Hosts(u32 server_connections = 8U) : buckets(allocator, HandshakeRules{})
    {
        network.listener = &server_poller;
        client_events.client = true;
        QmuxHostConfig server_config;
        server_config.connection = setup.config(&identity.server, false);
        server_config.connections = server_connections;
        QmuxHostConfig client_config;
        client_config.connection = setup.config(nullptr, true);
        client_config.connections = 8U;
        REQUIRE(buckets.ready());
        server = std::make_unique<QmuxHost>(allocator, server_poller, clock, &buckets, server_events, server_config);
        client = std::make_unique<QmuxHost>(allocator, client_poller, clock, nullptr, client_events, client_config);
        REQUIRE(server->ready());
        REQUIRE(client->ready());
    }

    QuicHandle connect(const crypto::TlsContext& context)
    {
        SocketError error = SocketError::None;
        const QuicHandle handle = client->connect(QmuxConnect{server_address, &context}, clock.now, error);
        REQUIRE(error == SocketError::None);
        REQUIRE(handle.valid());
        return handle;
    }

    void turns(usize count)
    {
        for (usize turn = 0U; turn < count; ++turn)
        {
            static_cast<void>(server->poll(0U));
            static_cast<void>(client->poll(0U));
            clock.now += kMsNs;
        }
    }

    // Turns until the bearer takes a tunnel, which it refuses with NotReady until the server's SETTINGS came.
    QuicHandle open(QuicHandle bearer)
    {
        for (usize turn = 0U; turn < 64U; ++turn)
        {
            WsTunnelError error = WsTunnelError::None;
            const QuicHandle tunnel = client->open_tunnel(bearer, clock.now, error);
            if (tunnel.valid())
            {
                REQUIRE(error == WsTunnelError::None);
                return tunnel;
            }
            REQUIRE((error == WsTunnelError::NotReady || error == WsTunnelError::Refused));
            turns(1U);
        }
        FAIL("no tunnel");
        return QuicHandle{};
    }
};

} // namespace

TEST_CASE("HTTP/2: one server context serves QMux, WebSocket and HTTP/2 by ALPN", "[net][qmux-h2]")
{
    H2Identity identity;
    Setup setup;
    CountingAllocator allocator;
    const crypto::TlsContext* const clients[] = {&identity.qmux_client, &identity.ws_client, &identity.h2_client};
    for (usize which = 0U; which < 3U; ++which)
    {
        QmuxConnection client;
        QmuxConnection server;
        REQUIRE(client.init(allocator, setup.config(clients[which], true)));
        REQUIRE(server.init(allocator, setup.config(&identity.server, false)));
        REQUIRE(client.start(kNow));
        REQUIRE(server.start(kNow));
        pump(client, server, kNow);
        CHECK(client.state() == QmuxState::Established);
        CHECK(server.state() == QmuxState::Established);
        CHECK(server.http2() == (which == 2U));
        CHECK(client.http2() == (which == 2U));
        CHECK(server.websocket() == (which == 1U));
        CHECK(server.peer_parameters_known() == (which != 2U)); // HTTP/2 carries no QMux parameters
        CHECK_FALSE(client.wants_write(kNow));
    }
}

TEST_CASE("HTTP/2: a configuration is checked for its side", "[net][qmux-h2]")
{
    H2Identity identity;
    Setup setup;
    CountingAllocator allocator;
    const auto takes = [&allocator](const QmuxConfig& config)
    {
        QmuxConnection connection;
        return connection.init(allocator, config);
    };
    CHECK(takes(setup.config(&identity.server, false)));
    CHECK(takes(setup.config(&identity.h2_client, true)));
    QmuxConfig config = setup.config(&identity.server, false);
    config.websocket = nullptr;
    CHECK_FALSE(takes(config)); // the tunnels need a WebSocket policy
    config = setup.config(&identity.server, false);
    config.http2_tunnels = 0U;
    CHECK_FALSE(takes(config));
    config.http2_tunnels = kWsMaxTunnels + 1U;
    CHECK_FALSE(takes(config));
    config.http2_tunnels = kWsMaxTunnels;
    CHECK(takes(config));
    config.http2 = &setup.client_http2; // a server that would not take extended CONNECT
    CHECK_FALSE(takes(config));
    Http2Config large = setup.server_http2;
    large.frame_bytes = kHttp2MinFrameBytes + 1U; // more than one TLS record
    config.http2 = &large;
    CHECK_FALSE(takes(config));
    Http2Config bad = setup.server_http2;
    bad.streams = 0U;
    config.http2 = &bad;
    CHECK_FALSE(takes(config));
    CHECK_FALSE(takes(setup.config(&identity.early_client, true))); // a client asking for 0-RTT
    CHECK_FALSE(takes(setup.config(&identity.h2_client, false)));   // a client's context where a server's belongs
    config = setup.config(nullptr, true);
    config.tunnel = QmuxTunnel::Client;
    CHECK_FALSE(takes(config)); // HTTP/2 inside a tunnel
}

TEST_CASE("HTTP/2: a request that is no tunnel's is answered 404, and a peer's GOAWAY ends the connection",
          "[net][qmux-h2]")
{
    H2Identity identity;
    Setup setup;
    CountingAllocator allocator;
    QmuxConnection server;
    REQUIRE(server.init(allocator, setup.config(&identity.server, false)));
    RawPeer peer;
    Bytes bytes = opening();
    append(bytes, view(peer.request(1U, ConstSpan<HttpField>(kGet, 4U))));
    raw_session(peer, identity.h2_client, server, view(bytes));
    CHECK(peer.status(1U) == 404U);
    CHECK(server.state() == QmuxState::Established);
    CHECK(server.tunnels_live() == 0U);

    u8 goaway[8] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    REQUIRE(peer.tls.send(view(frame(kFrameGoAway, 0U, 0U, ConstSpan<u8>(goaway, sizeof(goaway))))) ==
            TlsError::None);
    peer.exchange(server, kNow);
    CHECK(server.state() == QmuxState::Closing);
    CHECK(server.close_info().reason == QmuxCloseReason::PeerShutdown);
    CHECK(peer.goaway() == kH2NoError);
    CHECK(peer.closed); // close_notify behind it
    CHECK(server.send_finished());
    server.transport_ended(true, kNow);
    CHECK(server.state() == QmuxState::Closed);
}

TEST_CASE("HTTP/2: a connection error is a GOAWAY with its code, then close_notify", "[net][qmux-h2]")
{
    H2Identity identity;
    Setup setup;
    CountingAllocator allocator;
    QmuxConnection server;
    REQUIRE(server.init(allocator, setup.config(&identity.server, false)));
    RawPeer peer;
    Bytes bytes = opening();
    append(bytes, view(frame(kFrameData, 0U, 0U, span_of("x")))); // DATA on stream 0 (RFC 9113 section 6.1)
    raw_session(peer, identity.h2_client, server, view(bytes));
    CHECK(server.state() == QmuxState::Closing);
    CHECK(server.close_info().reason == QmuxCloseReason::Http2Failed);
    CHECK(server.close_info().error_code == kH2ProtocolError);
    CHECK(peer.goaway() == kH2ProtocolError);
    CHECK(peer.closed);
    // What still arrives is not read, and the close wait ends it.
    peer.exchange(server, kNow);
    server.on_timeout(server.next_timer_ns());
    CHECK(server.state() == QmuxState::Closed);
}

TEST_CASE("HTTP/2: an extended CONNECT in TLS early data is answered 425 and opens nothing", "[net][qmux-h2]")
{
    ReadyFilter ready;
    H2Identity identity(&ready.filter);
    Setup setup;
    CountingAllocator allocator;
    TicketCache tickets{allocator, TicketCacheConfig{}};
    REQUIRE(tickets.ready());
    {
        QmuxConnection server;
        REQUIRE(server.init(allocator, setup.config(&identity.server, false)));
        RawPeer first;
        REQUIRE(first.tls.init(identity.early_client, &tickets) == TlsError::None);
        REQUIRE(server.start(kNow));
        first.exchange(server, kNow);
        REQUIRE(first.tls.state() == TlsState::Established);
        REQUIRE(server.http2());
    }
    QmuxConnection server;
    REQUIRE(server.init(allocator, setup.config(&identity.server, false)));
    REQUIRE(server.start(kNow));
    RawPeer peer;
    REQUIRE(peer.tls.init(identity.early_client, &tickets) == TlsError::None);
    REQUIRE(peer.tls.early_data() == crypto::TlsEarlyData::Offered);
    Bytes early = opening();
    append(early, view(peer.request(1U, ConstSpan<HttpField>(kWebSocketConnect, 7U))));
    REQUIRE(peer.tls.send_early(view(early)) == TlsError::None);
    peer.exchange(server, kNow);
    REQUIRE(peer.tls.early_data() == crypto::TlsEarlyData::Accepted);
    CHECK(server.http2());
    CHECK(peer.status(1U) == 425U);
    TunnelLog log;
    log.poll(server, kNow);
    CHECK(log.opened == 0U);
    CHECK(server.tunnels_live() == 0U);

    // The same request after the handshake opens a tunnel.
    REQUIRE(peer.tls.send(view(peer.request(3U, ConstSpan<HttpField>(kWebSocketConnect, 7U)))) == TlsError::None);
    peer.exchange(server, kNow);
    CHECK(peer.status(3U) == 200U);
    log.poll(server, kNow);
    CHECK(log.opened == 1U);
}

TEST_CASE("HTTP/2: the connection idles only without tunnels, ending with GOAWAY and close_notify", "[net][qmux-h2]")
{
    H2Identity identity;
    Setup setup;
    CountingAllocator allocator;
    QmuxConnection client;
    QmuxConnection server;
    REQUIRE(client.init(allocator, setup.config(&identity.h2_client, true, 1000U)));
    REQUIRE(server.init(allocator, setup.config(&identity.server, false, 1000U)));
    REQUIRE(client.start(kNow));
    REQUIRE(server.start(kNow));
    pump(client, server, kNow);
    REQUIRE(client.http2());
    QuicHandle handle;
    REQUIRE(client.open_tunnel(kNow, handle) == WsTunnelError::None);
    pump(client, server, kNow);
    TunnelLog server_log;
    TunnelLog client_log;
    server_log.poll(server, kNow);
    pump(client, server, kNow);
    client_log.poll(client, kNow);
    REQUIRE(server_log.opened == 1U);
    REQUIRE(client_log.opened == 1U);
    CHECK(server.tunnels_live() == 1U);

    // At the idle deadline the tunnel ends on its own timer; the connection carrying it does not.
    u64 now = kNow + 1000U * kMsNs;
    client.on_timeout(now);
    server.on_timeout(now);
    CHECK(client.state() == QmuxState::Established);
    CHECK(server.state() == QmuxState::Established);
    client_log.poll(client, now);
    server_log.poll(server, now);
    pump(client, server, now);
    client_log.poll(client, now);
    server_log.poll(server, now);
    CHECK(client_log.closed == 1U);
    CHECK(server_log.closed == 1U);
    CHECK(client_log.last_end.reason == QmuxCloseReason::IdleTimeout);
    client_log.poll(client, now); // the ended tunnel is forgotten
    server_log.poll(server, now);
    CHECK(client.tunnels_live() == 0U);
    CHECK(server.tunnels_live() == 0U);

    // Without a tunnel its own idle timer runs.
    now = client.next_timer_ns();
    CHECK(now == kNow + 2000U * kMsNs);
    client.on_timeout(now);
    CHECK(client.state() == QmuxState::Closed);
    CHECK(client.close_info().reason == QmuxCloseReason::IdleTimeout);
    CHECK(client.send_finished() == false); // GOAWAY and close_notify wait for their one try
    u8 bytes[kBuffer];
    const usize got = client.take(Span<u8>(bytes), now);
    REQUIRE(got != 0U);
    server.receive(ConstSpan<u8>(bytes, got), now);
    CHECK(server.state() == QmuxState::Closed);
    CHECK(server.close_info().reason == QmuxCloseReason::PeerShutdown);
    CHECK(client.send_finished());
}

TEST_CASE("HTTP/2: tunnels are connections of the host, served, closed and reported before their bearer",
          "[net][qmux-h2]")
{
    Hosts hosts;
    hosts.client_events.transfer = 256U * 1024U;
    hosts.server_events.transfer = 256U * 1024U;
    const QuicHandle bearer = hosts.connect(hosts.identity.h2_client);
    WsTunnelError error = WsTunnelError::None;
    CHECK_FALSE(hosts.client->open_tunnel(bearer, hosts.clock.now, error).valid());
    CHECK(error == WsTunnelError::Refused); // not HTTP/2 yet
    const QuicHandle first = hosts.open(bearer);
    const QuicHandle second = hosts.open(bearer);
    CHECK(first != second);
    CHECK(hosts.client->connection(first) == nullptr); // not open yet
    CHECK_FALSE(hosts.client->open_tunnel(first, hosts.clock.now, error).valid());
    CHECK(error == WsTunnelError::Refused); // a tunnel bears none
    CHECK(hosts.client->live() == 3U);

    const auto finished = [&hosts, first, second]
    {
        return hosts.client_events.done(first.slot) && hosts.client_events.done(second.slot) &&
               hosts.server_events.tunnels_accepted == 2U && hosts.server_events.done(1U) &&
               hosts.server_events.done(2U);
    };
    for (usize turn = 0U; turn < 20000U && !finished(); ++turn)
    {
        hosts.turns(1U);
    }
    REQUIRE(finished());
    CHECK(hosts.server_events.accepted == 3U); // the bearer, then its two tunnels
    CHECK(hosts.server_events.last_peer.port == 50000U);
    CHECK(hosts.server->live() == 3U);
    CHECK(hosts.server->counters().tunnels == 2U);
    CHECK(hosts.client->counters().tunnels == 2U);

    // An established turn allocates nothing.
    const usize calls = hosts.allocator.calls();
    hosts.turns(32U);
    CHECK(hosts.allocator.calls() == calls);

    // One tunnel closes: both ends report it, and its handle goes stale.
    QmuxConnection* const tunnel = hosts.client->connection(first);
    REQUIRE(tunnel != nullptr);
    CHECK(tunnel->websocket());
    tunnel->close(4U, ConstSpan<u8>(), hosts.clock.now);
    hosts.client->wake(first);
    hosts.turns(16U);
    REQUIRE(hosts.client_events.ended_count == 1U);
    REQUIRE(hosts.server_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].handle == first);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::Local);
    CHECK(hosts.server_events.ended[0].info.reason == QmuxCloseReason::Peer);
    CHECK(hosts.server_events.ended[0].info.error_code == 4U);
    CHECK(hosts.client->connection(first) == nullptr);
    CHECK(hosts.client->live() == 2U);
    CHECK(hosts.server->live() == 2U);

    // The bearer closes: its last tunnel is reported closed before it, on both ends.
    QmuxConnection* const connection = hosts.client->connection(bearer);
    REQUIRE(connection != nullptr);
    REQUIRE(connection->http2());
    connection->close(0U, ConstSpan<u8>(), hosts.clock.now);
    hosts.client->wake(bearer);
    hosts.turns(16U);
    REQUIRE(hosts.client_events.ended_count == 3U);
    REQUIRE(hosts.server_events.ended_count == 3U);
    CHECK(hosts.client_events.ended[1].handle == second);
    CHECK_FALSE(hosts.client_events.ended[1].bearer);
    CHECK(hosts.client_events.ended[2].handle == bearer);
    CHECK(hosts.client_events.ended[2].info.reason == QmuxCloseReason::Local);
    CHECK_FALSE(hosts.server_events.ended[1].bearer);
    CHECK(hosts.server_events.ended[2].bearer);
    CHECK(hosts.server_events.ended[2].info.reason == QmuxCloseReason::PeerShutdown);
    CHECK(hosts.client->live() == 0U);
    CHECK(hosts.server->live() == 0U);
    CHECK(hosts.client_poller.fins == 1U);
    CHECK(hosts.client_poller.resets == 0U);
    CHECK(hosts.client->connection(second) == nullptr);
}

TEST_CASE("HTTP/2: a host with no slot left refuses a tunnel with CONNECTION_REFUSED", "[net][qmux-h2]")
{
    Hosts hosts(2U); // the bearer and one tunnel
    const QuicHandle bearer = hosts.connect(hosts.identity.h2_client);
    const QuicHandle first = hosts.open(bearer);
    const QuicHandle second = hosts.open(bearer);
    hosts.turns(32U);
    CHECK(hosts.server->counters().tunnels == 1U);
    CHECK(hosts.server->counters().tunnels_refused == 1U);
    CHECK(hosts.server->live() == 2U);
    CHECK(hosts.client->connection(first) != nullptr);
    REQUIRE(hosts.client_events.ended_count == 1U);
    CHECK(hosts.client_events.ended[0].handle == second);
    CHECK(hosts.client_events.ended[0].info.reason == QmuxCloseReason::Peer);
    CHECK_FALSE(hosts.client_events.ended[0].info.application);
    CHECK(hosts.client_events.ended[0].info.error_code == static_cast<u64>(QuicTransportError::ConnectionRefused));
    CHECK(hosts.server_events.ended_count == 0U); // never the application's
}

TEST_CASE("HTTP/2: a refused tunnel is reported closed with its status, and the client's slots bound it",
          "[net][qmux-h2]")
{
    Hosts hosts;
    Setup offer; // the client offers a subprotocol the server does not take
    offer.protocols[0] = span_of("other.game");
    QmuxHostConfig client_config;
    client_config.connection = offer.config(nullptr, true);
    client_config.connections = 2U;
    App client_events;
    client_events.client = true;
    hosts.client = std::make_unique<QmuxHost>(hosts.allocator, hosts.client_poller, hosts.clock, nullptr,
                                              client_events, client_config);
    REQUIRE(hosts.client->ready());

    const QuicHandle bearer = hosts.connect(hosts.identity.h2_client);
    const QuicHandle tunnel = hosts.open(bearer);
    WsTunnelError error = WsTunnelError::None;
    CHECK_FALSE(hosts.client->open_tunnel(bearer, hosts.clock.now, error).valid());
    CHECK(error == WsTunnelError::NoTunnel); // both slots in use
    hosts.turns(16U);
    REQUIRE(client_events.ended_count == 1U);
    CHECK(client_events.ended[0].handle == tunnel);
    CHECK(client_events.ended[0].info.reason == QmuxCloseReason::UpgradeRefused);
    CHECK(client_events.ended[0].info.error_code == 400U);
    CHECK(hosts.client->live() == 1U);
    CHECK(hosts.server->live() == 1U);
    CHECK(hosts.server_events.tunnels_accepted == 0U);
    CHECK(hosts.client->connection(tunnel) == nullptr);
}
