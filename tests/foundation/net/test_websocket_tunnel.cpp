// crd-net -- WebSockets over HTTP/2 (RFC 8441) and HTTP/3 (RFC 9220), each a QMux connection (4.2a;
// docs/design/network-foundation.md#websocket-tunnels): the extended CONNECT's policy and every refusal by status,
// tunnels carrying streams, datagrams and a ChannelLink past every window over both versions with the wire cut
// anywhere, frames in fragments with control frames between them, every way a tunnel ends, its timers, the pool's bound
// and what the owner keeps.

#include "channel_link_fixture.hpp"
#include "http3_fixture.hpp"
#include "ingress_fixture.hpp"
#include "quic_stream_fixture.hpp"
#include "text_fixture.hpp"

#include <crd/containers/wire_bytes.hpp>
#include <crd/net/http2.hpp>
#include <crd/net/register_ids.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <memory>
#include <string_view>

namespace
{
using namespace crd;
using namespace crd::net;
using namespace crd::net::testing;
using crd::containers::ConstSpan;
using crd::containers::Span;
namespace cont = crd::containers;

constexpr usize kClient = 0U;
constexpr usize kServer = 1U;
constexpr u64 kSecond = 1'000'000'000U;
constexpr const char* kProtocol = "qmux-02.game";
constexpr usize kMaxSeen = 64U;
constexpr usize kMaxFields = 12U;

ConstSpan<u8> span_of(const char* text)
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text), std::strlen(text));
}

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

// One policy for both ends: a client's host and Origin, a server's list.
struct Policy
{
    ConstSpan<u8> protocols[2];
    ConstSpan<u8> origins[1];
    WebSocketConfig config;

    Policy()
    {
        protocols[0] = span_of(kProtocol);
        protocols[1] = span_of("qmux-01.game");
        origins[0] = span_of("https://game.example");
        config.protocols = ConstSpan<ConstSpan<u8>>(protocols, 2U);
        config.origins = ConstSpan<ConstSpan<u8>>(origins, 1U);
        config.path = span_of("/game");
        config.host = span_of("game.cerid.test");
        config.origin = span_of("https://game.example");
    }
};

WsTunnelConfig tunnel_config(const WebSocketConfig& websocket, u32 tunnels = 4U)
{
    WsTunnelConfig config;
    config.tunnels = tunnels;
    QmuxConfig& connection = config.connection;
    connection.websocket = &websocket;
    connection.streams.peer_bidi = 8U;
    connection.streams.peer_uni = 32U;
    connection.streams.local_bidi = 8U;
    connection.streams.local_uni = 32U;
    connection.streams.receive_bytes = 64U * 1024U;
    connection.streams.send_bytes = 64U * 1024U;
    connection.streams.stream_window = 32U * 1024U;
    connection.streams.connection_window = 64U * 1024U;
    connection.streams.connection_max_window = 256U * 1024U;
    connection.datagrams.max_frame_bytes = 1200U;
    connection.datagrams.receive_bytes = 8192U;
    connection.datagrams.receive_count = 32U;
    connection.datagrams.send_bytes = 8192U;
    connection.datagrams.send_count = 32U;
    return config;
}

// A request or response written by hand.
struct Section
{
    HttpField fields[kMaxFields];
    usize count = 0U;

    Section& add(const char* name, const char* value)
    {
        fields[count].name = span_of(name);
        fields[count].value = span_of(value);
        ++count;
        return *this;
    }
    [[nodiscard]] ConstSpan<HttpField> span() const { return ConstSpan<HttpField>(fields, count); }
};

Section connect_request(const char* origin = "https://game.example", const char* path = "/game")
{
    Section section;
    section.add(":method", "CONNECT").add(":protocol", "websocket").add(":scheme", "https");
    section.add(":authority", "game.cerid.test").add(":path", path).add("sec-websocket-version", "13");
    section.add("sec-websocket-protocol", "other, qmux-02.game");
    if (origin != nullptr)
    {
        section.add("origin", origin);
    }
    return section;
}

struct Seen
{
    WsTunnelEventKind kind = WsTunnelEventKind::Opened;
    QuicHandle tunnel;
    u64 stream = 0U;
    u16 status = 0U;
    u64 error = 0U;
    QmuxCloseInfo close; // Closed: how the tunnel's connection ended
};

// One end: its tunnels and what reached its owner.
struct End
{
    WebSocketTunnels tunnels;
    Seen seen[kMaxSeen];
    usize seen_count = 0U;
    usize services = 0U; // Service events, counted only
    u64 request_stream = ~u64{0}; // the last request its owner saw
    u16 status = 0U;              // and the last response
    Text response;                // its fields
    Text body;                    // what came on streams no tunnel holds
    u64 aborted = ~u64{0};        // the last Aborted error the owner saw
    usize finished = 0U;

    void record(const WsTunnelEvent& event)
    {
        REQUIRE(event.connection != nullptr);
        REQUIRE((event.kind != WsTunnelEventKind::Refused || event.connection->state() == QmuxState::Closed));
        if (event.kind == WsTunnelEventKind::Service)
        {
            ++services;
            return;
        }
        REQUIRE(seen_count < kMaxSeen);
        Seen& slot = seen[seen_count++];
        slot.kind = event.kind;
        slot.tunnel = event.tunnel;
        slot.stream = event.stream;
        slot.status = event.status;
        slot.error = event.error;
        if (event.kind == WsTunnelEventKind::Closed)
        {
            slot.close = event.connection->close_info();
        }
    }

    [[nodiscard]] usize count(WsTunnelEventKind kind) const
    {
        usize total = 0U;
        for (usize at = 0U; at < seen_count; ++at)
        {
            total += (seen[at].kind == kind) ? 1U : 0U;
        }
        return total;
    }

    [[nodiscard]] const Seen* last(WsTunnelEventKind kind) const
    {
        for (usize at = seen_count; at != 0U; --at)
        {
            if (seen[at - 1U].kind == kind)
            {
                return &seen[at - 1U];
            }
        }
        return nullptr;
    }

    // The tunnel the last Opened named.
    [[nodiscard]] QmuxConnection* opened()
    {
        const Seen* const found = last(WsTunnelEventKind::Opened);
        return (found == nullptr) ? nullptr : tunnels.connection(found->tunnel);
    }
};

// Both ends over HTTP/2, the wire moving `cut` bytes at a time.
class H2Rig
{
public:
    static constexpr bool kHttp3 = false;

    explicit H2Rig(usize cut = ~usize{0}, u32 server_tunnels = 4U, bool tunnels_on_server = true)
        : m_cut(cut)
    {
        for (usize side = 0U; side < 2U; ++side)
        {
            Http2Config config;
            config.side = (side == kClient) ? Http2Side::Client : Http2Side::Server;
            config.connect_protocol = side == kServer;
            config.stream_window = 16384U;
            config.connection_window = kHttp2InitialWindow;
            ok = conn[side].init(allocator, config, now) && ok;
        }
        ok = ok && end[kClient].tunnels.init(allocator, conn[kClient], tunnel_config(policy.config));
        if (tunnels_on_server)
        {
            ok = ok && end[kServer].tunnels.init(allocator, conn[kServer], tunnel_config(policy.config, server_tunnels));
        }
        m_server_tunnels = tunnels_on_server;
        settle();
    }

    void pump(usize side)
    {
        End& here = end[side];
        const bool tunnels = side == kClient || m_server_tunnels;
        for (;;)
        {
            usize taken = 0U;
            Http2Event event;
            const bool got = conn[side].poll(view(in[side]), now, taken, event);
            if (got && !(tunnels && here.tunnels.on_event(event, now)))
            {
                owner(side, event);
            }
            erase_front(in[side], taken);
            if (!got)
            {
                break;
            }
        }
        WsTunnelEvent event;
        while (tunnels && here.tunnels.poll(event, now))
        {
            here.record(event);
        }
    }

    void settle()
    {
        for (u32 round = 0U; round < 200000U; ++round)
        {
            pump(kClient);
            pump(kServer);
            if (conn[kClient].output().empty() && conn[kServer].output().empty())
            {
                return;
            }
            move(kClient);
            move(kServer);
        }
        FAIL("the wire never settled");
    }

    [[nodiscard]] u64 raw_open(const Section& section)
    {
        u32 id = 0U;
        REQUIRE(conn[kClient].open_request(section.span(), false, id) == Http2Error::None);
        return id;
    }
    void raw_respond(u64 stream, const Section& section, bool fin)
    {
        REQUIRE(conn[kServer].send_response(static_cast<u32>(stream), section.span(), fin) == Http2Error::None);
    }
    void raw_send(u64 stream, ConstSpan<u8> bytes, bool fin = false)
    {
        usize sent = 0U;
        while (sent < bytes.size() || (fin && sent == bytes.size() && bytes.empty()))
        {
            usize taken = 0U;
            REQUIRE(conn[kClient].send_body(static_cast<u32>(stream), bytes.subspan(sent), fin, taken) ==
                    Http2Error::None);
            sent += taken;
            if (bytes.empty())
            {
                break;
            }
        }
    }
    void raw_cancel(u64 stream) { REQUIRE(conn[kClient].cancel(static_cast<u32>(stream), kH2Cancel) == Http2Error::None); }
    void raw_finish(u64 stream) { REQUIRE(conn[kClient].finish(static_cast<u32>(stream)) == Http2Error::None); }
    [[nodiscard]] u64 refused_code() const { return kH2RefusedStream; }

    void flush()
    {
        end[kClient].tunnels.flush(now);
        if (m_server_tunnels)
        {
            end[kServer].tunnels.flush(now);
        }
    }

    CountingAllocator allocator;
    Policy policy;
    Http2Connection conn[2];
    End end[2];
    Text in[2];
    u64 now = 1000U * kSecond;
    bool ok = true;

private:
    void owner(usize side, const Http2Event& event)
    {
        End& here = end[side];
        switch (event.kind)
        {
            case Http2EventKind::Headers:
                here.request_stream = event.stream;
                here.status = event.info.status;
                here.response = joined(event.fields);
                break;
            case Http2EventKind::Body:
                here.body += text_of(event.payload);
                static_cast<void>(conn[side].consume_body(event.stream, event.payload.size()));
                break;
            case Http2EventKind::Aborted:
                here.aborted = event.error;
                break;
            case Http2EventKind::Finished:
                ++here.finished;
                break;
            default:
                break;
        }
    }

    void move(usize from)
    {
        const ConstSpan<u8> bytes = conn[from].output();
        const usize count = (bytes.size() < m_cut) ? bytes.size() : m_cut;
        in[1U - from] += text_of(bytes.first(count));
        conn[from].output_taken(count);
    }

    usize m_cut;
    bool m_server_tunnels = true;
};

// Both ends over HTTP/3 on QUIC's stream sets, every frame delivered.
class H3TunnelRig
{
public:
    static constexpr bool kHttp3 = true;

    explicit H3TunnelRig(usize cut = ~usize{0}, u32 server_tunnels = 4U, bool tunnels_on_server = true)
    {
        static_cast<void>(cut); // QUIC's frames are what the rig cuts
        Http3Config server = h3_config(crypto::QuicSide::Server);
        server.connect_protocol = true;
        QuicStreamLimits limits = h3_limits();
        limits.receive_bytes = 32768U;
        limits.send_bytes = 32768U;
        limits.stream_window = 32768U;
        wire = std::make_unique<H3Rig>(allocator, true, true, h3_config(crypto::QuicSide::Client), server, limits,
                                       false);
        ok = wire->ok && end[kClient].tunnels.init(allocator, wire->h3[kClient], tunnel_config(policy.config));
        if (tunnels_on_server)
        {
            ok = ok &&
                 end[kServer].tunnels.init(allocator, wire->h3[kServer], tunnel_config(policy.config, server_tunnels));
        }
        m_server_tunnels = tunnels_on_server;
        settle();
    }

    void pump(usize side)
    {
        End& here = end[side];
        const bool tunnels = side == kClient || m_server_tunnels;
        Http3Event event;
        while (wire->h3[side].poll(event))
        {
            if (mark_early && event.kind == Http3EventKind::Headers)
            {
                event.early = true;
            }
            if (!(tunnels && here.tunnels.on_event(event, now)))
            {
                owner(side, event);
            }
        }
        WsTunnelEvent tunnel;
        while (tunnels && here.tunnels.poll(tunnel, now))
        {
            here.record(tunnel);
        }
    }

    void settle()
    {
        for (u32 round = 0U; round < 200000U; ++round)
        {
            wire->now = now;
            pump(kClient);
            pump(kServer);
            const bool a = wire->deliver(kClient);
            const bool b = wire->deliver(kServer);
            if (!a && !b)
            {
                pump(kClient);
                pump(kServer);
                if (!wire->deliver(kClient) && !wire->deliver(kServer))
                {
                    return;
                }
            }
        }
        FAIL("the wire never settled");
    }

    [[nodiscard]] u64 raw_open(const Section& section)
    {
        u64 id = 0U;
        REQUIRE(wire->h3[kClient].open_request(section.span(), false, id) == Http3Error::None);
        return id;
    }
    void raw_respond(u64 stream, const Section& section, bool fin)
    {
        REQUIRE(wire->h3[kServer].send_response(stream, section.span(), fin) == Http3Error::None);
    }
    void raw_cancel(u64 stream) { REQUIRE(wire->h3[kClient].cancel(stream, kH3RequestCancelled) == Http3Error::None); }
    void raw_finish(u64 stream) { REQUIRE(wire->h3[kClient].finish(stream) == Http3Error::None); }
    [[nodiscard]] u64 refused_code() const { return kH3RequestRejected; }

    void flush()
    {
        end[kClient].tunnels.flush(now);
        if (m_server_tunnels)
        {
            end[kServer].tunnels.flush(now);
        }
    }

    CountingAllocator allocator;
    Policy policy;
    std::unique_ptr<H3Rig> wire;
    End end[2];
    u64 now = 1000U * kSecond;
    bool ok = true;
    bool mark_early = false; // the owner's view of a request in 0-RTT

private:
    void owner(usize side, const Http3Event& event)
    {
        End& here = end[side];
        switch (event.kind)
        {
            case Http3EventKind::Headers:
                here.request_stream = event.stream;
                here.status = event.info.status;
                here.response = joined(event.fields);
                break;
            case Http3EventKind::Body:
                for (Http3Body body = wire->h3[side].peek_body(event.stream); !body.data.empty();
                     body = wire->h3[side].peek_body(event.stream))
                {
                    here.body += text_of(body.data);
                    static_cast<void>(wire->h3[side].consume_body(event.stream, body.data.size()));
                }
                break;
            case Http3EventKind::Aborted:
                here.aborted = event.error;
                break;
            case Http3EventKind::Finished:
                ++here.finished;
                break;
            default:
                break;
        }
    }

    bool m_server_tunnels = true;
};

// Opens one tunnel and returns both ends' connections.
template <typename Rig> void open_tunnel(Rig& rig, QmuxConnection*& client, QmuxConnection*& server)
{
    QuicHandle handle;
    REQUIRE(rig.end[kClient].tunnels.open(rig.now, handle) == WsTunnelError::None);
    rig.settle();
    client = rig.end[kClient].tunnels.connection(handle);
    server = rig.end[kServer].opened();
    REQUIRE(client != nullptr);
    REQUIRE(server != nullptr);
}

} // namespace

TEST_CASE("WebSocket tunnels: the extended CONNECT's policy", "[net][ws-tunnel]")
{
    const Policy policy;
    const auto status = [&policy](const Section& section)
    {
        HttpMessageInfo info;
        REQUIRE(http_check_request(section.span(), info));
        WsHandshake handshake;
        return ws_check_connect(section.span(), info, policy.config, handshake);
    };
    {
        const Section good = connect_request();
        HttpMessageInfo info;
        REQUIRE(http_check_request(good.span(), info));
        CHECK(ws_is_connect(info));
        WsHandshake handshake;
        CHECK(ws_check_connect(good.span(), info, policy.config, handshake) == kWsConnectAccepted);
        CHECK(handshake.protocol == 0U);
    }
    CHECK(status(connect_request(nullptr)) == kWsConnectAccepted); // no Origin: not a browser
    CHECK(status(connect_request("https://evil.example")) == 403U);
    CHECK(status(connect_request("https://game.example", "/other")) == 404U);
    CHECK(status(connect_request("https://game.example", "/game?room=4")) == kWsConnectAccepted);
    Section key = connect_request();
    key.add("sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==").add("sec-websocket-key", "twice");
    CHECK(status(key) == kWsConnectAccepted); // RFC 8441 section 5: nothing reads it
    Section twice = connect_request();
    twice.add("origin", "https://game.example");
    CHECK(status(twice) == 400U);
    Section version;
    version.add(":method", "CONNECT").add(":protocol", "websocket").add(":scheme", "https");
    version.add(":authority", "game.cerid.test").add(":path", "/game").add("sec-websocket-protocol", kProtocol);
    CHECK(status(version) == 400U);
    version.add("sec-websocket-version", "8");
    CHECK(status(version) == 400U);
    Section unknown;
    unknown.add(":method", "CONNECT").add(":protocol", "websocket").add(":scheme", "https");
    unknown.add(":authority", "game.cerid.test").add(":path", "/game").add("sec-websocket-version", "13");
    unknown.add("sec-websocket-protocol", "chat, superchat");
    CHECK(status(unknown) == 400U);
    Section other;
    other.add(":method", "CONNECT").add(":protocol", "webtransport").add(":scheme", "https");
    other.add(":authority", "game.cerid.test").add(":path", "/game");
    HttpMessageInfo other_info;
    REQUIRE(http_check_request(other.span(), other_info));
    CHECK_FALSE(ws_is_connect(other_info));
    WsHandshake ignored;
    CHECK(ws_check_connect(other.span(), other_info, policy.config, ignored) == 400U);
}

TEST_CASE("WebSocket tunnels: a client's reading of the answer", "[net][ws-tunnel]")
{
    const Policy policy;
    const auto read = [&policy](const Section& section)
    {
        HttpMessageInfo info;
        REQUIRE(http_check_response(section.span(), info));
        WsHandshake handshake;
        ws_read_connect_response(section.span(), info, policy.config, handshake);
        return handshake;
    };
    Section good;
    good.add(":status", "200").add("sec-websocket-protocol", "qmux-01.game");
    CHECK(read(good).status == kWsConnectAccepted);
    CHECK(read(good).protocol == 1U);
    Section created;
    created.add(":status", "204").add("sec-websocket-protocol", kProtocol);
    CHECK(read(created).status == kWsConnectAccepted);
    Section none;
    none.add(":status", "200");
    CHECK(read(none).status == 0U);
    Section foreign;
    foreign.add(":status", "200").add("sec-websocket-protocol", "chat");
    CHECK(read(foreign).status == 0U);
    Section both = good;
    both.add("sec-websocket-protocol", kProtocol);
    CHECK(read(both).status == 0U);
    Section extension = good;
    extension.add("sec-websocket-extensions", "permessage-deflate");
    CHECK(read(extension).status == 0U);
    Section refused;
    refused.add(":status", "403");
    CHECK(read(refused).status == 403U);
}

TEST_CASE("WebSocket tunnels: a configuration out of range holds nothing", "[net][ws-tunnel]")
{
    const Policy policy;
    CountingAllocator allocator;
    Http2Connection client;
    Http2Connection server;
    Http2Connection plain;
    Http2Config config;
    REQUIRE(client.init(allocator, config, 0U));
    config.side = Http2Side::Server;
    REQUIRE(plain.init(allocator, config, 0U));
    config.connect_protocol = true;
    REQUIRE(server.init(allocator, config, 0U));
    const usize before = allocator.calls();
    const auto refused = [&](Http2Connection& http, auto change)
    {
        WsTunnelConfig tunnel = tunnel_config(policy.config);
        change(tunnel);
        WebSocketTunnels tunnels;
        return !tunnels.init(allocator, http, tunnel);
    };
    CHECK(refused(plain, [](WsTunnelConfig&) {})); // a server that does not take extended CONNECT
    CHECK(refused(server, [](WsTunnelConfig& c) { c.tunnels = 0U; }));
    CHECK(refused(server, [](WsTunnelConfig& c) { c.tunnels = kWsMaxTunnels + 1U; }));
    CHECK(refused(server, [](WsTunnelConfig& c) { c.connection.websocket = nullptr; }));
    CHECK(refused(server, [](WsTunnelConfig& c) { c.connection.tunnel = QmuxTunnel::Server; }));
    CHECK(refused(server, [](WsTunnelConfig& c) { c.connection.streams.stream_window = 0U; }));
    CHECK(refused(server, [](WsTunnelConfig& c) { c.connection.streams.reset_stream_at = true; })); // QMux is in order
    WebSocketConfig hostless = policy.config;
    hostless.host = {};
    CHECK(refused(client, [&hostless](WsTunnelConfig& c) { c.connection.websocket = &hostless; }));
    CHECK(allocator.calls() == before);

    WebSocketTunnels twice;
    REQUIRE(twice.init(allocator, server, tunnel_config(policy.config)));
    CHECK_FALSE(twice.init(allocator, server, tunnel_config(policy.config)));
    QuicHandle handle;
    CHECK(twice.open(0U, handle) == WsTunnelError::Failed); // a server opens none
    CHECK_FALSE(handle.valid());

    QmuxConnection tunnel;
    QmuxConfig bare = tunnel_config(policy.config).connection;
    CHECK_FALSE(tunnel.init(allocator, bare)); // neither TLS nor a tunnel
    bare.tunnel = QmuxTunnel::Server;
    bare.websocket = nullptr;
    CHECK_FALSE(tunnel.init(allocator, bare));
    bare.websocket = &policy.config;
    REQUIRE(tunnel.init(allocator, bare));
    CHECK_FALSE(tunnel.start(0U));
    CHECK_FALSE(tunnel.start_tunnel(2U, 0U)); // no such subprotocol
    REQUIRE(tunnel.start_tunnel(1U, 0U));
    CHECK_FALSE(tunnel.start_tunnel(1U, 0U));
    CHECK(tunnel.state() == QmuxState::Established);
    CHECK(same(tunnel.websocket_protocol(), span_of("qmux-01.game")));
}

TEMPLATE_TEST_CASE("WebSocket tunnels: a tunnel opens and carries streams and datagrams past every window",
                   "[net][ws-tunnel]", H2Rig, H3TunnelRig)
{
    const usize cut = GENERATE(usize{1U}, usize{7U}, ~usize{0});
    if (TestType::kHttp3 && cut != ~usize{0})
    {
        return;
    }
    CAPTURE(cut);
    TestType rig(cut);
    REQUIRE(rig.ok);
    QuicHandle handle;
    CHECK(rig.end[kServer].tunnels.open(rig.now, handle) == WsTunnelError::Failed);
    QmuxConnection* client = nullptr;
    QmuxConnection* server = nullptr;
    open_tunnel(rig, client, server);
    CHECK(rig.end[kClient].count(WsTunnelEventKind::Opened) == 1U);
    CHECK(rig.end[kServer].count(WsTunnelEventKind::Opened) == 1U);
    CHECK(client->state() == QmuxState::Established);
    CHECK(client->websocket());
    CHECK(same(client->websocket_protocol(), span_of(kProtocol)));
    CHECK(same(server->websocket_protocol(), span_of(kProtocol)));
    CHECK(client->peer_parameters_known());
    CHECK(server->peer_parameters_known());
    CHECK(rig.end[kClient].tunnels.live() == 1U);

    u64 id = 0U;
    REQUIRE(client->streams().open(true, id) == QuicStreamError::None);
    constexpr u64 total = 200U * 1024U;
    u64 client_written = 0U;
    u64 server_written = 0U;
    u64 client_read = 0U;
    u64 server_read = 0U;
    static_cast<void>(write_pattern(client->streams(), id, client_written, total, 3U));
    rig.flush();
    rig.settle();
    const usize allocations = rig.allocator.calls();
    bool client_done = false;
    bool server_done = false;
    for (usize turn = 0U; turn < 20000U && !(client_done && server_done); ++turn)
    {
        static_cast<void>(write_pattern(client->streams(), id, client_written, total, 3U));
        static_cast<void>(write_pattern(server->streams(), id, server_written, total, 5U));
        rig.flush();
        rig.settle();
        client_done = client_done || drain(client->streams(), id, client_read, 5U);
        server_done = server_done || drain(server->streams(), id, server_read, 3U);
    }
    CHECK(client_read == total);
    CHECK(server_read == total);
    CHECK(rig.allocator.calls() == allocations); // nothing allocates once a tunnel is open
    CHECK(rig.end[kServer].services != 0U);

    const usize limit = client->datagrams().max_payload();
    REQUIRE(limit != 0U);
    cont::Array<u8> message;
    message.resize(limit, static_cast<u8>(0x5AU));
    u64 datagram = 0U;
    REQUIRE(client->datagrams().queue(ConstSpan<u8>(message.data(), message.size()), rig.now, datagram) ==
            QuicDatagramError::None);
    REQUIRE(server->datagrams().queue(ConstSpan<u8>(message.data(), message.size()), rig.now, datagram) ==
            QuicDatagramError::None);
    rig.flush();
    rig.settle();
    for (QmuxConnection* connection : {client, server})
    {
        ConstSpan<u8> received;
        REQUIRE(connection->datagrams().peek(received));
        CHECK(received.size() == limit);
        connection->datagrams().pop();
    }
}

TEMPLATE_TEST_CASE("WebSocket tunnels: a reader that stops holds the writer by flow control, never by failing",
                   "[net][ws-tunnel]", H2Rig, H3TunnelRig)
{
    TestType rig;
    REQUIRE(rig.ok);
    QmuxConnection* client = nullptr;
    QmuxConnection* server = nullptr;
    open_tunnel(rig, client, server);
    u64 id = 0U;
    REQUIRE(client->streams().open(true, id) == QuicStreamError::None);
    constexpr u64 total = 512U * 1024U;
    u64 written = 0U;
    for (usize turn = 0U; turn < 64U; ++turn)
    {
        static_cast<void>(write_pattern(client->streams(), id, written, total, 9U));
        rig.flush();
        rig.settle();
    }
    // The server's application has read nothing: QMux's windows stopped the client well short of the whole.
    const u64 held = written;
    CHECK(held < total);
    CHECK(held <= 2U * tunnel_config(rig.policy.config).connection.streams.connection_max_window);
    CHECK(client->state() == QmuxState::Established);
    CHECK(server->state() == QmuxState::Established);
    CHECK(rig.end[kServer].count(WsTunnelEventKind::Closed) == 0U);
    u64 read = 0U;
    bool done = false;
    for (usize turn = 0U; turn < 20000U && !done; ++turn)
    {
        static_cast<void>(write_pattern(client->streams(), id, written, total, 9U));
        rig.flush();
        rig.settle();
        done = drain(server->streams(), id, read, 9U);
    }
    CHECK(read == total);
}

TEMPLATE_TEST_CASE("WebSocket tunnels: a ChannelLink runs over a tunnel unchanged", "[net][ws-tunnel]", H2Rig,
                   H3TunnelRig)
{
    TestType rig;
    REQUIRE(rig.ok);
    QmuxConnection* connections[2] = {};
    open_tunnel(rig, connections[kClient], connections[kServer]);
    std::unique_ptr<LinkEnd> ends[2];
    for (usize side = 0U; side < 2U; ++side)
    {
        ends[side] = std::make_unique<LinkEnd>(QuotaLimits{});
        REQUIRE(ends[side]->bound);
        ends[side]->link = std::make_unique<ChannelLink>();
        REQUIRE(ends[side]->link->init(rig.allocator, ends[side]->admission,
                                       ChannelCarrier(connections[side]->streams(), connections[side]->datagrams()),
                                       ChannelLinkConfig{}));
    }
    u8 body[3000];
    for (usize at = 0U; at < sizeof(body); ++at)
    {
        body[at] = static_cast<u8>(at * 13U);
    }
    u8 frame[4096];
    const usize used = link_frame(kLinkNote, ConstSpan<u8>(body, sizeof(body)), Span<u8>(frame, sizeof(frame)));
    REQUIRE(used != 0U);
    for (usize side = 0U; side < 2U; ++side)
    {
        usize taken = 0U;
        REQUIRE(ends[side]->link->write(kLinkEvent, ConstSpan<u8>(frame, used), taken) == ChannelError::None);
        REQUIRE(taken == used);
    }
    rig.flush();
    rig.settle();
    for (usize side = 0U; side < 2U; ++side)
    {
        bool got = false;
        ChannelEvent event;
        while (ends[side]->link->next(event, rig.now))
        {
            if (event.kind == ChannelEventKind::Frame)
            {
                CHECK(event.channel == kLinkEvent);
                CHECK(event.frame.message->id == kLinkNote);
                CHECK(same(event.frame.body, ConstSpan<u8>(body, sizeof(body))));
                got = true;
            }
        }
        CHECK(got);
        CHECK_FALSE(ends[side]->link->failed());
    }
}

TEMPLATE_TEST_CASE("WebSocket tunnels: every refusal is answered by status and never reaches the application",
                   "[net][ws-tunnel]", H2Rig, H3TunnelRig)
{
    TestType rig;
    REQUIRE(rig.ok);
    const auto answer = [&rig](const Section& section)
    {
        rig.end[kClient].status = 0U;
        static_cast<void>(rig.raw_open(section));
        rig.settle();
        return rig.end[kClient].status;
    };
    CHECK(answer(connect_request("https://evil.example")) == 403U);
    CHECK(answer(connect_request("https://game.example", "/lobby")) == 404U);
    Section version;
    version.add(":method", "CONNECT").add(":protocol", "websocket").add(":scheme", "https");
    version.add(":authority", "game.cerid.test").add(":path", "/game").add("sec-websocket-version", "12");
    version.add("sec-websocket-protocol", kProtocol);
    CHECK(answer(version) == 400U);
    CHECK(contains(rig.end[kClient].response, "sec-websocket-version=13"));
    CHECK(rig.end[kServer].seen_count == 0U); // nothing of it reached the server's application
    CHECK(rig.end[kServer].tunnels.live() == 0U);
    // Another :protocol and a plain request are the owner's.
    Section get;
    get.add(":method", "GET").add(":scheme", "https").add(":authority", "game.cerid.test").add(":path", "/");
    static_cast<void>(rig.raw_open(get));
    rig.settle();
    CHECK(rig.end[kServer].request_stream != ~u64{0});
    CHECK(answer(connect_request()) == kWsConnectAccepted);
    CHECK(rig.end[kServer].count(WsTunnelEventKind::Opened) == 1U);
    if constexpr (TestType::kHttp3)
    {
        rig.mark_early = true;
        CHECK(answer(connect_request()) == 425U); // RFC 8470: an opening handshake that could be replayed is none
        CHECK(rig.end[kServer].count(WsTunnelEventKind::Opened) == 1U);
    }
}

TEMPLATE_TEST_CASE("WebSocket tunnels: past the pool a CONNECT is refused as a stream", "[net][ws-tunnel]", H2Rig,
                   H3TunnelRig)
{
    TestType rig(~usize{0}, 1U);
    REQUIRE(rig.ok);
    QuicHandle first;
    QuicHandle second;
    REQUIRE(rig.end[kClient].tunnels.open(rig.now, first) == WsTunnelError::None);
    REQUIRE(rig.end[kClient].tunnels.open(rig.now, second) == WsTunnelError::None);
    rig.settle();
    CHECK(rig.end[kServer].count(WsTunnelEventKind::Opened) == 1U);
    CHECK(rig.end[kServer].tunnels.live() == 1U);
    CHECK(rig.end[kClient].count(WsTunnelEventKind::Opened) == 1U);
    const Seen* const refused = rig.end[kClient].last(WsTunnelEventKind::Refused);
    REQUIRE(refused != nullptr);
    CHECK(refused->status == 0U);
    CHECK(refused->error == rig.refused_code());
    CHECK(rig.end[kClient].tunnels.connection(refused->tunnel) == nullptr);
    CHECK(rig.end[kClient].tunnels.live() == 1U);
    CHECK(rig.end[kClient].tunnels.connection(first) != nullptr);
    CHECK(rig.end[kClient].tunnels.connection(second) == nullptr); // stale once forgotten

    // The pool's own bound on the client.
    QuicHandle more;
    for (u32 index = 0U; index < 3U; ++index)
    {
        REQUIRE(rig.end[kClient].tunnels.open(rig.now, more) == WsTunnelError::None);
    }
    CHECK(rig.end[kClient].tunnels.open(rig.now, more) == WsTunnelError::NoTunnel);
}

TEMPLATE_TEST_CASE("WebSocket tunnels: a client refuses an answer that does not open one", "[net][ws-tunnel]", H2Rig,
                   H3TunnelRig)
{
    struct Case
    {
        const char* status;
        const char* protocol;
        u16 expected;
    };
    const Case cases[] = {{"200", nullptr, 0U}, {"200", "chat", 0U}, {"403", nullptr, 403U}, {"302", nullptr, 302U}};
    for (const Case& item : cases)
    {
        CAPTURE(item.status);
        TestType rig(~usize{0}, 4U, false);
        REQUIRE(rig.ok);
        QuicHandle handle;
        REQUIRE(rig.end[kClient].tunnels.open(rig.now, handle) == WsTunnelError::None);
        rig.settle();
        REQUIRE(rig.end[kServer].request_stream != ~u64{0});
        Section response;
        response.add(":status", item.status);
        if (item.protocol != nullptr)
        {
            response.add("sec-websocket-protocol", item.protocol);
        }
        rig.raw_respond(rig.end[kServer].request_stream, response, false);
        rig.settle();
        const Seen* const refused = rig.end[kClient].last(WsTunnelEventKind::Refused);
        REQUIRE(refused != nullptr);
        CHECK(refused->status == item.expected);
        CHECK(rig.end[kClient].count(WsTunnelEventKind::Opened) == 0U);
        CHECK(rig.end[kClient].tunnels.live() == 0U);
    }
}

TEST_CASE("WebSocket tunnels: a peer that stops reading still delivers what it sent", "[net][ws-tunnel]")
{
    // STOP_SENDING ends this side's direction alone (RFC 9000 section 3.5): a peer may stop reading once it has closed,
    // and what it sent behind the frame, its Close among it, is still read.
    for (const bool server_stops : {true, false})
    {
        CAPTURE(server_stops);
        H3TunnelRig rig;
        REQUIRE(rig.ok);
        QmuxConnection* client = nullptr;
        QmuxConnection* server = nullptr;
        open_tunnel(rig, client, server);
        const usize stopper = server_stops ? kServer : kClient;
        const usize other = server_stops ? kClient : kServer;
        QmuxConnection* const closer = server_stops ? server : client;
        const Seen* const opened = rig.end[kServer].last(WsTunnelEventKind::Opened);
        REQUIRE(opened != nullptr);
        REQUIRE(rig.wire->streams[stopper].stop_sending(opened->stream, 0U) == QuicStreamError::None);
        closer->close(7U, span_of("bye"), rig.now);
        rig.flush();
        rig.settle();
        const Seen* const closed = rig.end[other].last(WsTunnelEventKind::Closed);
        REQUIRE(closed != nullptr);
        CHECK(closed->close.reason == QmuxCloseReason::Peer);
        CHECK(closed->close.application);
        CHECK(closed->close.error_code == 7U);
        CHECK(rig.end[stopper].count(WsTunnelEventKind::Closed) == 1U);
        CHECK(rig.end[kClient].tunnels.live() == 0U);
        CHECK(rig.end[kServer].tunnels.live() == 0U);
    }
}

TEST_CASE("WebSocket tunnels: a request stopped before its answer still takes the answer", "[net][ws-tunnel]")
{
    // A server may stop reading a request and then answer it complete (RFC 9114 section 4.1.1), its STOP_SENDING
    // arriving first: Quinn sends the frame ahead of the response's stream data.
    struct Case
    {
        const char* status;
        const char* protocol;
        u16 expected;
    };
    const Case cases[] = {{"400", nullptr, 400U}, {"200", kProtocol, 0U}, {nullptr, nullptr, 0U}};
    for (const Case& item : cases)
    {
        CAPTURE(item.status);
        H3TunnelRig rig(~usize{0}, 4U, false);
        REQUIRE(rig.ok);
        QuicHandle handle;
        REQUIRE(rig.end[kClient].tunnels.open(rig.now, handle) == WsTunnelError::None);
        rig.settle();
        const u64 stream = rig.end[kServer].request_stream;
        REQUIRE(stream != ~u64{0});
        REQUIRE(rig.wire->streams[kServer].stop_sending(stream, 0U) == QuicStreamError::None);
        rig.settle();
        CHECK(rig.end[kClient].count(WsTunnelEventKind::Refused) == 0U); // still waiting for the answer
        if (item.status == nullptr)
        {
            rig.now = rig.end[kClient].tunnels.next_deadline_ns(); // no answer: the handshake timeout still bounds it
        }
        else
        {
            Section response;
            response.add(":status", item.status);
            if (item.protocol != nullptr)
            {
                response.add("sec-websocket-protocol", item.protocol);
            }
            rig.raw_respond(stream, response, true);
        }
        rig.settle();
        const Seen* const refused = rig.end[kClient].last(WsTunnelEventKind::Refused);
        REQUIRE(refused != nullptr);
        CHECK(refused->status == item.expected);
        CHECK(rig.end[kClient].count(WsTunnelEventKind::Opened) == 0U); // a 2xx it could not answer opens none
        CHECK(rig.end[kClient].tunnels.live() == 0U);
    }
}

TEMPLATE_TEST_CASE("WebSocket tunnels: no answer within the handshake timeout refuses the tunnel", "[net][ws-tunnel]",
                   H2Rig, H3TunnelRig)
{
    TestType rig(~usize{0}, 4U, false);
    REQUIRE(rig.ok);
    QuicHandle handle;
    REQUIRE(rig.end[kClient].tunnels.open(rig.now, handle) == WsTunnelError::None);
    rig.settle();
    CHECK(rig.end[kClient].tunnels.connection(handle) == nullptr); // not open yet
    const u64 due = rig.end[kClient].tunnels.next_deadline_ns();
    CHECK(due == rig.now + tunnel_config(rig.policy.config).connection.handshake_timeout_ns);
    rig.now = due - 1U;
    rig.settle();
    CHECK(rig.end[kClient].count(WsTunnelEventKind::Refused) == 0U);
    rig.now = due;
    rig.settle();
    const Seen* const refused = rig.end[kClient].last(WsTunnelEventKind::Refused);
    REQUIRE(refused != nullptr);
    CHECK(refused->status == 0U);
    CHECK(refused->error == 0U);
    CHECK(rig.end[kClient].tunnels.live() == 0U);
    CHECK(rig.end[kClient].tunnels.next_deadline_ns() == kQmuxNoTimer);
    CHECK(rig.end[kServer].aborted != ~u64{0}); // the server's owner saw the request cancelled
}

TEMPLATE_TEST_CASE("WebSocket tunnels: a close goes as CONNECTION_CLOSE, Close and the stream's end", "[net][ws-tunnel]",
                   H2Rig, H3TunnelRig)
{
    TestType rig;
    REQUIRE(rig.ok);
    QmuxConnection* client = nullptr;
    QmuxConnection* server = nullptr;
    open_tunnel(rig, client, server);
    client->close(7U, span_of("bye"), rig.now);
    rig.flush();
    rig.settle();
    REQUIRE(rig.end[kServer].count(WsTunnelEventKind::Closed) == 1U);
    REQUIRE(rig.end[kClient].count(WsTunnelEventKind::Closed) == 1U);
    CHECK(rig.end[kServer].tunnels.live() == 0U);
    CHECK(rig.end[kClient].tunnels.live() == 0U);
    CHECK(rig.end[kClient].tunnels.next_deadline_ns() == kQmuxNoTimer);
    if constexpr (!TestType::kHttp3)
    {
        CHECK(rig.conn[kClient].open_streams() == 0U);
        CHECK(rig.conn[kServer].open_streams() == 0U);
        CHECK(rig.conn[kClient].error() == 0U);
    }
}

TEST_CASE("WebSocket tunnels: what each end saw of a close", "[net][ws-tunnel]")
{
    // A recorder of the reason at the Closed event: the connection is destroyed by the next poll.
    H2Rig rig;
    REQUIRE(rig.ok);
    QmuxConnection* client = nullptr;
    QmuxConnection* server = nullptr;
    open_tunnel(rig, client, server);
    client->close(7U, span_of("bye"), rig.now);
    rig.flush();
    for (usize turn = 0U; turn < 64U; ++turn)
    {
        rig.pump(kClient);
        const ConstSpan<u8> bytes = rig.conn[kClient].output();
        rig.in[kServer] += text_of(bytes);
        rig.conn[kClient].output_taken(bytes.size());
        // Read the server's events here, before a poll destroys a Closed tunnel.
        usize taken = 0U;
        Http2Event event;
        while (rig.conn[kServer].poll(view(rig.in[kServer]), rig.now, taken, event))
        {
            static_cast<void>(rig.end[kServer].tunnels.on_event(event, rig.now));
            erase_front(rig.in[kServer], taken);
            taken = 0U;
        }
        erase_front(rig.in[kServer], taken);
        WsTunnelEvent tunnel;
        while (rig.end[kServer].tunnels.poll(tunnel, rig.now))
        {
            if (tunnel.kind == WsTunnelEventKind::Closed)
            {
                const QmuxCloseInfo& info = tunnel.connection->close_info();
                CHECK(info.reason == QmuxCloseReason::Peer);
                CHECK(info.application);
                CHECK(info.error_code == 7U);
                CHECK(same(tunnel.connection->close_reason(), span_of("bye")));
                return;
            }
        }
    }
    FAIL("the server's tunnel never closed");
}

TEMPLATE_TEST_CASE("WebSocket tunnels: a reset of the stream and its end without Close", "[net][ws-tunnel]", H2Rig,
                   H3TunnelRig)
{
    for (const bool reset : {true, false})
    {
        CAPTURE(reset);
        TestType rig;
        REQUIRE(rig.ok);
        const u64 stream = rig.raw_open(connect_request());
        rig.settle();
        REQUIRE(rig.end[kClient].status == kWsConnectAccepted);
        REQUIRE(rig.end[kServer].count(WsTunnelEventKind::Opened) == 1U);
        if (reset)
        {
            rig.raw_cancel(stream);
        }
        else
        {
            rig.raw_finish(stream);
        }
        rig.settle();
        CHECK(rig.end[kServer].count(WsTunnelEventKind::Closed) == 1U);
        CHECK(rig.end[kServer].tunnels.live() == 0U);
        if (!reset)
        {
            CHECK(rig.end[kClient].aborted == (TestType::kHttp3 ? kH3RequestCancelled : kH2Cancel));
        }
    }
}

TEMPLATE_TEST_CASE("WebSocket tunnels: the idle timeout ends both ends", "[net][ws-tunnel]", H2Rig, H3TunnelRig)
{
    TestType rig;
    REQUIRE(rig.ok);
    QmuxConnection* client = nullptr;
    QmuxConnection* server = nullptr;
    open_tunnel(rig, client, server);
    const u64 due = rig.end[kClient].tunnels.next_deadline_ns();
    REQUIRE(due != kQmuxNoTimer);
    CHECK(due > rig.now);
    rig.now = due + 1U;
    rig.settle();
    CHECK(rig.end[kClient].count(WsTunnelEventKind::Closed) == 1U);
    CHECK(rig.end[kServer].count(WsTunnelEventKind::Closed) == 1U);
    CHECK(rig.end[kClient].tunnels.live() == 0U);
    CHECK(rig.end[kServer].tunnels.live() == 0U);
}

TEST_CASE("WebSocket tunnels: fragments with control frames between them, a Ping answered, a bad frame failing it",
          "[net][ws-tunnel]")
{
    const usize cut = GENERATE(usize{1U}, usize{5U}, ~usize{0});
    CAPTURE(cut);
    H2Rig rig(cut);
    REQUIRE(rig.ok);
    const u64 stream = rig.raw_open(connect_request());
    rig.settle();
    REQUIRE(rig.end[kServer].count(WsTunnelEventKind::Opened) == 1U);
    QmuxConnection* const server = rig.end[kServer].opened();
    REQUIRE(server != nullptr);

    // The parameters frame in two fragments, a masked Ping between them.
    u8 parameters[16];
    cont::WireWriter writer{Span<u8>(parameters, sizeof(parameters))};
    put_quic_varint(writer, iana::kQuicFrameQxTransportParameters);
    put_quic_varint(writer, 0U);
    const usize size = writer.written();
    const auto frame_of = [](WsOpcode opcode, bool fin, ConstSpan<u8> payload, bool masked)
    {
        constexpr u8 mask[kWsMaskBytes] = {0x11U, 0x22U, 0x33U, 0x44U};
        Text bytes;
        u8 head[kWsMaxHeaderBytes];
        const usize used = ws_frame_header_bytes(payload.size(), masked);
        ws_write_frame_header(Span<u8>(head, used), opcode, fin, payload.size(), masked ? mask : nullptr);
        bytes += text_of(ConstSpan<u8>(head, used));
        u8 body[kWsMaxControlBytes];
        std::memcpy(body, payload.data(), payload.size());
        if (masked)
        {
            ws_mask(Span<u8>(body, payload.size()), mask);
        }
        bytes += text_of(ConstSpan<u8>(body, payload.size()));
        return bytes;
    };
    Text sequence = frame_of(WsOpcode::Binary, false, ConstSpan<u8>(parameters, 1U), true);
    sequence += frame_of(WsOpcode::Ping, true, span_of("hi"), true);
    sequence += frame_of(WsOpcode::Continuation, true, ConstSpan<u8>(parameters + 1U, size - 1U), true);
    rig.raw_send(stream, view(sequence));
    rig.settle();
    CHECK(server->state() == QmuxState::Established);
    CHECK(server->peer_parameters_known());
    // The server's answer: its own record, and a Pong with the Ping's payload, unmasked.
    const Text pong = frame_of(WsOpcode::Pong, true, span_of("hi"), false);
    CHECK(contains(rig.end[kClient].body, pong));

    // A frame a client must mask, unmasked: the server fails the tunnel with 1002.
    rig.raw_send(stream, view(frame_of(WsOpcode::Ping, true, span_of("x"), false)));
    rig.settle();
    REQUIRE(rig.end[kServer].count(WsTunnelEventKind::Closed) == 1U);
    const Text close = frame_of(WsOpcode::Close, true, ConstSpan<u8>(reinterpret_cast<const u8*>("\x03\xea"), 2U), false);
    CHECK(contains(rig.end[kClient].body, close));
    CHECK(rig.end[kClient].finished == 1U); // Close, then the stream's end
}

TEST_CASE("WebSocket tunnels: a tunnel ended by what arrived behind its CONNECT is reported Opened, then Closed",
          "[net][ws-tunnel]")
{
    H2Rig rig;
    REQUIRE(rig.ok);
    const u64 stream = rig.raw_open(connect_request());
    u8 frame[kWsMaxHeaderBytes + 1U];
    const usize head = ws_frame_header_bytes(1U, false);
    ws_write_frame_header(Span<u8>(frame, head), WsOpcode::Ping, true, 1U, nullptr); // unmasked: a client's must be
    frame[head] = 'x';
    rig.raw_send(stream, ConstSpan<u8>(frame, head + 1U));
    rig.settle();
    REQUIRE(rig.end[kServer].seen_count == 2U);
    CHECK(rig.end[kServer].seen[0].kind == WsTunnelEventKind::Opened);
    CHECK(rig.end[kServer].seen[1].kind == WsTunnelEventKind::Closed);
    CHECK(rig.end[kServer].seen[0].tunnel == rig.end[kServer].seen[1].tunnel);
    CHECK(rig.end[kServer].tunnels.live() == 0U);
    CHECK(rig.end[kClient].status == kWsConnectAccepted);
    CHECK(rig.conn[kServer].error() == 0U);
}

TEST_CASE("WebSocket tunnels: a failed HTTP/2 connection ends every tunnel", "[net][ws-tunnel]")
{
    H2Rig rig;
    REQUIRE(rig.ok);
    QmuxConnection* client = nullptr;
    QmuxConnection* server = nullptr;
    open_tunnel(rig, client, server);
    // DATA on stream 0 is a connection error (RFC 9113 section 6.1).
    const u8 bad[] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    rig.in[kServer] += text_of(ConstSpan<u8>(bad, sizeof(bad)));
    rig.pump(kServer);
    CHECK(rig.conn[kServer].error() == kH2ProtocolError);
    CHECK(rig.end[kServer].count(WsTunnelEventKind::Closed) == 1U);
    CHECK(rig.end[kServer].tunnels.live() == 0U);
    CHECK(rig.end[kServer].tunnels.next_deadline_ns() == kQmuxNoTimer);
}
