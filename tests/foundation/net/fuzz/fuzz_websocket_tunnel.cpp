// crd-fuzz-net-websocket-tunnel: WebSockets over HTTP/2 and HTTP/3 in crd-net (4.2a;
// docs/design/network-foundation.md#websocket-tunnels) under the bounded harness (REPO.DEV.9;
// docs/design/test-instruments.md).
//
// Input: a mode byte, then operations.
//   0  a client honest in HTTP/2 and hostile above it, at a server with tunnels: extended CONNECTs good and bad, any
//      bytes on their streams, ends and resets, the wire cut anywhere and time moving.
//   1  the same over HTTP/3, on QUIC's stream sets.
//   2  two honest ends over HTTP/2 cut anywhere: a tunnel carries a stream both ways, every byte arriving in order.
//
// Oracles: the HTTP connections never fail, as nothing the tunnels do may break them; the pool's bound holds; an
// Opened tunnel is Established and a Closed one Closed, a Refused one never on a server; once every stream ended the
// pool is empty; and in mode 2 both directions arrive whole.

#include "text_fixture.hpp"

#include <crd/containers/array.hpp>
#include <crd/containers/wire_bytes.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include <algorithm>
#include <cstring>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::fuzz::BudgetAllocator;
using namespace crd::net;
using namespace crd::net::testing;
namespace cont = crd::containers;

constexpr u64 kStepNs = 100'000'000U;
constexpr u32 kTunnels = 3U;
constexpr usize kMaxStreams = 16U;
constexpr usize kMaxOps = 1024U;
constexpr u32 kMaxCancels = 48U; // under HTTP/2's reset gate: a client resetting faster is refused, rightly

struct Bytes
{
    ConstSpan<u8> data;
    usize at = 0U;

    [[nodiscard]] u8 next() noexcept { return (at < data.size()) ? data[at++] : u8{0}; }
    [[nodiscard]] bool done() const noexcept { return at >= data.size(); }
};

const ConstSpan<u8> kProtocols[] = {http_text("qmux-02.game"), http_text("qmux-01.game")};
const ConstSpan<u8> kOrigins[] = {http_text("https://game.example")};

WebSocketConfig policy()
{
    WebSocketConfig config;
    config.protocols = ConstSpan<ConstSpan<u8>>(kProtocols, 2U);
    config.origins = ConstSpan<ConstSpan<u8>>(kOrigins, 1U);
    config.path = http_text("/game");
    config.host = http_text("game.cerid.test");
    config.origin = http_text("https://game.example");
    return config;
}

WsTunnelConfig tunnel_config(const WebSocketConfig& websocket)
{
    WsTunnelConfig config;
    config.tunnels = kTunnels;
    QmuxConfig& connection = config.connection;
    connection.websocket = &websocket;
    connection.streams.peer_bidi = 4U;
    connection.streams.peer_uni = 4U;
    connection.streams.local_bidi = 4U;
    connection.streams.local_uni = 4U;
    connection.streams.receive_bytes = 16384U;
    connection.streams.send_bytes = 16384U;
    connection.streams.stream_window = 8192U;
    connection.streams.connection_window = 16384U;
    connection.streams.connection_max_window = 65536U;
    connection.datagrams.max_frame_bytes = 1200U;
    connection.datagrams.receive_bytes = 4096U;
    connection.datagrams.receive_count = 8U;
    connection.datagrams.send_bytes = 4096U;
    connection.datagrams.send_count = 8U;
    return config;
}

// A request, good or spoiled by `variant`.
usize request(u8 variant, HttpField (&out)[8])
{
    const ConstSpan<u8> origin = ((variant & 1U) != 0U) ? http_text("https://evil.example") : kOrigins[0];
    const ConstSpan<u8> path = ((variant & 2U) != 0U) ? http_text("/other") : http_text("/game");
    const ConstSpan<u8> version = ((variant & 4U) != 0U) ? http_text("8") : http_text("13");
    const ConstSpan<u8> protocol = ((variant & 8U) != 0U) ? http_text("chat") : http_text("x, qmux-01.game");
    const ConstSpan<u8> upgrade = ((variant & 16U) != 0U) ? http_text("webtransport") : http_text("websocket");
    out[0] = HttpField{http_text(":method"), http_text("CONNECT"), false};
    out[1] = HttpField{http_text(":protocol"), upgrade, false};
    out[2] = HttpField{http_text(":scheme"), http_text("https"), false};
    out[3] = HttpField{http_text(":authority"), http_text("game.cerid.test"), false};
    out[4] = HttpField{http_text(":path"), path, false};
    out[5] = HttpField{http_text("sec-websocket-version"), version, false};
    out[6] = HttpField{http_text("sec-websocket-protocol"), protocol, false};
    out[7] = HttpField{http_text("origin"), origin, false};
    return ((variant & 32U) != 0U) ? 7U : 8U;
}

// What a server's application does: reads everything its tunnels have, and checks every event.
void serve(WebSocketTunnels& tunnels, u64 now)
{
    WsTunnelEvent event;
    while (tunnels.poll(event, now))
    {
        CRD_FUZZ_REQUIRE(event.kind != WsTunnelEventKind::Refused);
        CRD_FUZZ_REQUIRE(event.connection != nullptr);
        CRD_FUZZ_REQUIRE(tunnels.live() <= kTunnels);
        if (event.kind == WsTunnelEventKind::Opened)
        {
            // Started: what arrived behind the CONNECT may already have ended it, and its Closed follows.
            CRD_FUZZ_REQUIRE(event.connection->state() != QmuxState::Idle &&
                             event.connection->state() != QmuxState::Handshaking);
            CRD_FUZZ_REQUIRE(tunnels.connection(event.tunnel) == event.connection);
        }
        if (event.kind == WsTunnelEventKind::Closed)
        {
            CRD_FUZZ_REQUIRE(event.connection->state() == QmuxState::Closed);
            continue;
        }
        QuicStreamSet& streams = event.connection->streams();
        QuicStreamEvent stream;
        while (streams.poll(stream))
        {
            for (QuicStreamRead read = streams.peek(stream.id); !read.data.empty(); read = streams.peek(stream.id))
            {
                static_cast<void>(streams.consume(stream.id, read.data.size()));
            }
        }
        ConstSpan<u8> datagram;
        while (event.connection->datagrams().peek(datagram))
        {
            event.connection->datagrams().pop();
        }
    }
}

// The hostile client's streams.
struct Opened
{
    u64 ids[kMaxStreams] = {};
    usize count = 0U;
    u32 cancels = 0U;

    [[nodiscard]] u64 pick(u8 choice) const noexcept { return ids[choice % count]; }
};

// ---- Mode 0: HTTP/2 ----

struct H2End
{
    Http2Connection connection;
    cont::Array<u8> in;
};

void h2_move(H2End& from, H2End& to, usize cut)
{
    const ConstSpan<u8> bytes = from.connection.output();
    const usize count = std::min(bytes.size(), cut);
    for (usize at = 0U; at < count; ++at)
    {
        to.in.push_back(bytes[at]);
    }
    from.connection.output_taken(count);
}

// Polls one end; `tunnels` takes what is theirs.
void h2_pump(H2End& end, WebSocketTunnels* tunnels, u64 now)
{
    for (;;)
    {
        usize taken = 0U;
        Http2Event event;
        const bool more = end.connection.poll(ConstSpan<u8>(end.in.data(), end.in.size()), now, taken, event);
        if (more && !(tunnels != nullptr && tunnels->on_event(event, now)) && event.kind == Http2EventKind::Body)
        {
            static_cast<void>(end.connection.consume_body(event.stream, event.payload.size()));
        }
        if (taken != 0U)
        {
            std::memmove(end.in.data(), end.in.data() + taken, end.in.size() - taken);
            end.in.resize(end.in.size() - taken);
        }
        if (!more)
        {
            return;
        }
    }
}

void h2_settle(H2End& client, H2End& server, WebSocketTunnels& tunnels, u64 now)
{
    for (u32 round = 0U; round < 4096U; ++round)
    {
        h2_pump(client, nullptr, now);
        h2_pump(server, &tunnels, now);
        serve(tunnels, now);
        if (client.connection.output().empty() && server.connection.output().empty())
        {
            return;
        }
        h2_move(client, server, ~usize{0});
        h2_move(server, client, ~usize{0});
    }
}

void hostile_h2(Bytes& in)
{
    BudgetAllocator allocator;
    const WebSocketConfig websocket = policy();
    H2End client;
    H2End server;
    Http2Config config;
    config.stream_window = 1U + (u32{in.next()} * 257U);
    if (!client.connection.init(allocator, config, 1U))
    {
        return;
    }
    config.side = Http2Side::Server;
    config.connect_protocol = true;
    WebSocketTunnels tunnels;
    if (!server.connection.init(allocator, config, 1U) ||
        !tunnels.init(allocator, server.connection, tunnel_config(websocket)))
    {
        return;
    }
    u64 now = 1U;
    h2_settle(client, server, tunnels, now);
    Opened opened;
    for (usize op = 0U; op < kMaxOps && !in.done(); ++op)
    {
        const u8 choice = in.next();
        switch (choice % 7U)
        {
            case 0U:
            {
                HttpField fields[8];
                const usize count = request(in.next(), fields);
                u32 id = 0U;
                if (opened.count < kMaxStreams &&
                    client.connection.open_request(ConstSpan<HttpField>(fields, count), false, id) == Http2Error::None)
                {
                    opened.ids[opened.count++] = id;
                }
                break;
            }
            case 1U:
            {
                const usize length = 1U + in.next();
                cont::Array<u8> bytes;
                for (usize at = 0U; at < length; ++at)
                {
                    bytes.push_back(in.next());
                }
                usize taken = 0U;
                if (opened.count != 0U)
                {
                    static_cast<void>(client.connection.send_body(static_cast<u32>(opened.pick(in.next())),
                                                                  ConstSpan<u8>(bytes.data(), bytes.size()), false,
                                                                  taken));
                }
                break;
            }
            case 2U:
                if (opened.count != 0U)
                {
                    static_cast<void>(client.connection.finish(static_cast<u32>(opened.pick(in.next()))));
                }
                break;
            case 3U:
                if (opened.count != 0U && opened.cancels < kMaxCancels)
                {
                    ++opened.cancels;
                    static_cast<void>(client.connection.cancel(static_cast<u32>(opened.pick(in.next())), kH2Cancel));
                }
                break;
            case 4U:
                h2_settle(client, server, tunnels, now);
                now += kStepNs * (1U + in.next());
                break;
            default:
            {
                const usize cut = 1U + (usize{in.next()} * 37U);
                h2_move(client, server, cut);
                h2_pump(server, &tunnels, now);
                serve(tunnels, now);
                h2_move(server, client, cut);
                h2_pump(client, nullptr, now);
                break;
            }
        }
        CRD_FUZZ_REQUIRE(server.connection.error() == 0U);
        CRD_FUZZ_REQUIRE(client.connection.error() == 0U);
    }
    // Every stream ends: the pool empties.
    h2_settle(client, server, tunnels, now);
    for (usize index = 0U; index < opened.count; ++index)
    {
        static_cast<void>(client.connection.cancel(static_cast<u32>(opened.ids[index]), kH2Cancel));
        h2_settle(client, server, tunnels, now);
    }
    CRD_FUZZ_REQUIRE(server.connection.error() == 0U);
    CRD_FUZZ_REQUIRE(tunnels.live() == 0U);
}

// ---- Mode 1: HTTP/3 ----

struct QuicWire
{
    QuicStreamSet streams[2];
    Http3Connection http3[2];
    bool ok = false;

    QuicWire(BudgetAllocator& allocator, const QuicStreamLimits& limits)
    {
        ok = streams[0].init(allocator, limits, crd::crypto::QuicSide::Client) &&
             streams[1].init(allocator, limits, crd::crypto::QuicSide::Server);
        if (!ok)
        {
            return;
        }
        QuicTransportParameters parameters[2];
        streams[0].fill_parameters(parameters[0]);
        streams[1].fill_parameters(parameters[1]);
        streams[0].set_peer_parameters(parameters[1]);
        streams[1].set_peer_parameters(parameters[0]);
        Http3Config config;
        config.side = crd::crypto::QuicSide::Client;
        ok = http3[0].init(allocator, streams[0], nullptr, config) && http3[0].start();
        config.side = crd::crypto::QuicSide::Server;
        config.connect_protocol = true;
        ok = ok && http3[1].init(allocator, streams[1], nullptr, config) && http3[1].start();
    }

    // Every frame `from` has, delivered; true when anything moved.
    bool deliver(usize from, u64 now)
    {
        bool moved = false;
        QuicFrame frame;
        QuicStreamRecord record;
        while (streams[from].next_frame(1200U, now, 20'000'000U, frame, record))
        {
            CRD_FUZZ_REQUIRE(streams[1U - from].on_frame(frame) == QuicTransportError::NoError);
            streams[from].on_acked(record);
            moved = true;
        }
        return moved;
    }
};

void h3_pump(Http3Connection& connection, WebSocketTunnels* tunnels, u64 now)
{
    Http3Event event;
    while (connection.poll(event))
    {
        if (!(tunnels != nullptr && tunnels->on_event(event, now)) && event.kind == Http3EventKind::Body)
        {
            for (Http3Body body = connection.peek_body(event.stream); !body.data.empty();
                 body = connection.peek_body(event.stream))
            {
                static_cast<void>(connection.consume_body(event.stream, body.data.size()));
            }
        }
    }
}

void h3_settle(QuicWire& wire, WebSocketTunnels& tunnels, u64 now)
{
    for (u32 round = 0U; round < 4096U; ++round)
    {
        h3_pump(wire.http3[0], nullptr, now);
        h3_pump(wire.http3[1], &tunnels, now);
        serve(tunnels, now);
        const bool a = wire.deliver(0U, now);
        const bool b = wire.deliver(1U, now);
        if (!a && !b)
        {
            return;
        }
    }
}

void hostile_h3(Bytes& in)
{
    BudgetAllocator allocator;
    const WebSocketConfig websocket = policy();
    QuicStreamLimits limits;
    limits.peer_bidi = kMaxStreams;
    limits.peer_uni = 8U;
    limits.local_bidi = kMaxStreams;
    limits.local_uni = 8U;
    limits.receive_bytes = 16384U;
    limits.send_bytes = 16384U;
    limits.stream_window = 1024U + (u32{in.next()} * 32U);
    limits.connection_window = 1U << 20U;
    limits.connection_max_window = 1U << 22U;
    QuicWire wire(allocator, limits);
    WebSocketTunnels tunnels;
    if (!wire.ok || !tunnels.init(allocator, wire.http3[1], tunnel_config(websocket)))
    {
        return;
    }
    u64 now = 1U;
    h3_settle(wire, tunnels, now);
    Opened opened;
    Http3Connection& client = wire.http3[0];
    for (usize op = 0U; op < kMaxOps && !in.done(); ++op)
    {
        const u8 choice = in.next();
        switch (choice % 6U)
        {
            case 0U:
            {
                HttpField fields[8];
                const usize count = request(in.next(), fields);
                u64 id = 0U;
                if (opened.count < kMaxStreams &&
                    client.open_request(ConstSpan<HttpField>(fields, count), false, id) == Http3Error::None)
                {
                    opened.ids[opened.count++] = id;
                }
                break;
            }
            case 1U:
            {
                const usize length = 1U + in.next();
                cont::Array<u8> bytes;
                for (usize at = 0U; at < length; ++at)
                {
                    bytes.push_back(in.next());
                }
                usize taken = 0U;
                if (opened.count != 0U)
                {
                    static_cast<void>(
                        client.send_body(opened.pick(in.next()), ConstSpan<u8>(bytes.data(), bytes.size()), false, taken));
                }
                break;
            }
            case 2U:
                if (opened.count != 0U)
                {
                    static_cast<void>(client.finish(opened.pick(in.next())));
                }
                break;
            case 3U:
                if (opened.count != 0U)
                {
                    static_cast<void>(client.cancel(opened.pick(in.next()), kH3RequestCancelled));
                }
                break;
            case 4U:
                now += kStepNs * (1U + in.next());
                h3_settle(wire, tunnels, now);
                break;
            default:
                h3_settle(wire, tunnels, now);
                break;
        }
        CRD_FUZZ_REQUIRE(wire.http3[1].error() == 0U);
        CRD_FUZZ_REQUIRE(client.error() == 0U);
    }
    h3_settle(wire, tunnels, now);
    for (usize index = 0U; index < opened.count; ++index)
    {
        static_cast<void>(client.cancel(opened.ids[index], kH3RequestCancelled));
        h3_settle(wire, tunnels, now);
    }
    CRD_FUZZ_REQUIRE(wire.http3[1].error() == 0U);
    CRD_FUZZ_REQUIRE(tunnels.live() == 0U);
}

// ---- Mode 2: both ends honest ----

u8 pattern(u64 seed, u64 offset)
{
    return static_cast<u8>((offset * 31U) + (seed * 7U) + (offset >> 8U));
}

// Writes the pattern from `written` up to `total`, with FIN at the end.
void write_pattern(QuicStreamSet& streams, u64 id, u64 seed, u64& written, u64 total)
{
    u8 chunk[512];
    while (written < total)
    {
        const usize want = static_cast<usize>(std::min<u64>(sizeof(chunk), total - written));
        for (usize at = 0U; at < want; ++at)
        {
            chunk[at] = pattern(seed, written + at);
        }
        usize taken = 0U;
        if (streams.write(id, ConstSpan<u8>(chunk, want), written + want == total, taken) != QuicStreamError::None ||
            taken == 0U)
        {
            return;
        }
        written += taken;
    }
}

// Reads and checks what arrived; true once the FIN was read.
bool read_pattern(QuicStreamSet& streams, u64 id, u64 seed, u64& read)
{
    for (QuicStreamRead data = streams.peek(id);; data = streams.peek(id))
    {
        for (usize at = 0U; at < data.data.size(); ++at)
        {
            CRD_FUZZ_REQUIRE(data.data[at] == pattern(seed, read + at));
        }
        read += data.data.size();
        static_cast<void>(streams.consume(id, data.data.size()));
        if (data.fin)
        {
            return true;
        }
        if (data.data.empty())
        {
            return false;
        }
    }
}

void honest(Bytes& in)
{
    BudgetAllocator allocator;
    const WebSocketConfig websocket = policy();
    H2End ends[2];
    Http2Config config;
    // A window of a few bytes still splits every frame header; one byte would only make the transfer outlast the rounds.
    config.stream_window = 64U + (u32{in.next()} * 257U);
    WebSocketTunnels tunnels[2];
    const bool client_ok = ends[0].connection.init(allocator, config, 1U) &&
                           tunnels[0].init(allocator, ends[0].connection, tunnel_config(websocket));
    config.side = Http2Side::Server;
    config.connect_protocol = true;
    if (!client_ok || !ends[1].connection.init(allocator, config, 1U) ||
        !tunnels[1].init(allocator, ends[1].connection, tunnel_config(websocket)))
    {
        return;
    }
    const u64 total = 1U + (u64{in.next()} * 211U);
    const u64 now = 1U;
    QuicHandle handle;
    QmuxConnection* connections[2] = {};
    u64 stream = 0U;
    bool stream_open = false;
    u64 written[2] = {};
    u64 read[2] = {};
    bool done[2] = {};
    bool opened = false;
    for (u32 round = 0U; round < 100000U && !(done[0] && done[1]); ++round)
    {
        if (!opened)
        {
            opened = tunnels[0].open(now, handle) == WsTunnelError::None;
        }
        for (usize side = 0U; side < 2U; ++side)
        {
            h2_pump(ends[side], &tunnels[side], now);
            WsTunnelEvent event;
            while (tunnels[side].poll(event, now))
            {
                CRD_FUZZ_REQUIRE(event.kind != WsTunnelEventKind::Refused && event.kind != WsTunnelEventKind::Closed);
                if (event.kind == WsTunnelEventKind::Opened)
                {
                    connections[side] = event.connection;
                }
            }
        }
        if (connections[0] != nullptr && !stream_open && connections[0]->peer_parameters_known())
        {
            CRD_FUZZ_REQUIRE(connections[0]->streams().open(true, stream) == QuicStreamError::None);
            stream_open = true;
        }
        for (usize side = 0U; side < 2U && stream_open; ++side)
        {
            if (connections[side] == nullptr)
            {
                continue;
            }
            QuicStreamSet& streams = connections[side]->streams();
            done[side] = done[side] || read_pattern(streams, stream, 1U - side, read[side]);
            if (side == 0U || read[1] != 0U || done[1]) // the server's half exists once the client's bytes came
            {
                write_pattern(streams, stream, side, written[side], total);
            }
            tunnels[side].flush(now);
        }
        // Cut anywhere; time stands still, so no timer ends an honest transfer however slow the wire (the hostile modes
        // run the timers).
        const usize cut = 64U + (usize{in.next()} * 61U);
        h2_move(ends[0], ends[1], cut);
        h2_move(ends[1], ends[0], cut);
        CRD_FUZZ_REQUIRE(ends[0].connection.error() == 0U && ends[1].connection.error() == 0U);
    }
    CRD_FUZZ_REQUIRE(read[0] == total && read[1] == total);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const u8* data, size_t size)
{
    if (size == 0U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    Bytes in{ConstSpan<u8>(data + 1U, size - 1U)};
    switch (data[0] % 3U)
    {
        case 0U:
            hostile_h2(in);
            break;
        case 1U:
            hostile_h3(in);
            break;
        default:
            honest(in);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A good CONNECT, then the client's transport parameters frame (0xff51... and no parameters) in a masked Binary
    // message and a masked Ping, settling between, then a cut wire, a second CONNECT, its end and a reset.
    const u8 h2[] = {
        0x00U, 0x40U, 0x00U, 0x00U, 0x04U, 0x00U, 0x01U, 0x16U,                      //
        0x82U, 0x89U, 0x11U, 0x22U, 0x33U, 0x44U, 0xEEU, 0x73U, 0x60U, 0x74U, 0x1CU, //
        0x28U, 0x3EU, 0x4EU, 0x11U,                                                  //
        0x89U, 0x82U, 0x11U, 0x22U, 0x33U, 0x44U, 0x79U, 0x4BU,                      //
        0x00U, 0x04U, 0x00U, 0x05U, 0x03U, 0x00U, 0x01U, 0x04U, 0x00U, 0x02U, 0x00U, //
        0x04U, 0x00U, 0x03U, 0x00U, 0x04U, 0x00U,
    };
    sink.add("http2", h2, sizeof(h2));
    // The same over HTTP/3, then a spoiled CONNECT (another Origin).
    const u8 h3[] = {
        0x01U, 0x20U, 0x00U, 0x00U, 0x04U, 0x00U, 0x01U, 0x0EU,                      //
        0x82U, 0x89U, 0x11U, 0x22U, 0x33U, 0x44U, 0xEEU, 0x73U, 0x60U, 0x74U, 0x1CU, //
        0x28U, 0x3EU, 0x4EU, 0x11U, 0x00U, 0x04U, 0x00U, 0x00U, 0x01U, 0x05U, 0x02U, //
        0x00U, 0x04U, 0x00U,
    };
    sink.add("http3", h3, sizeof(h3));
    // Two honest ends: the window, the size, then cuts.
    const u8 honest[] = {0x02U, 0x40U, 0x30U, 0x00U, 0x07U, 0x01U, 0xFFU, 0x03U, 0x00U, 0x07U, 0x01U, 0xFFU, 0x03U};
    sink.add("honest", honest, sizeof(honest));
}
