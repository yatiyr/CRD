// crd-net interoperability test (4.2a) -- a QMux endpoint over crd-net's QmuxHost, or over WebSocket tunnels on an
// HTTP/2 connection (the host's, inside TLS, or one with prior knowledge), real TCP on the loopback, the counterpart of
// qmux_crate_peer. qmux_interop.py runs one of each and reads both reports.
//
// Usage: crd-qmux-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --alpn NAME --streams N
//                      --bytes N --closer client|server|none --code N --max-streams N [--uni N --uni-bytes N]
//                      [--resets N] [--stops N] [--datagrams N --datagram-bytes N [--datagram-limit N]
//                      [--datagram-probe 1]] [--connections 2 [--early 1]] [--idle-ms N] [--keepalive-ms N]
//                      [--hold-ms N] [--transport tls|ws|h2|h2c]
//
//   port, seed, group, cipher  as crd-quic-peer's; interop_echo.hpp describes the application
//   alpn, idle-ms, keepalive-ms  interop_qmux.hpp's: `alpn` is the one application protocol this end offers or accepts
//   closer        none: neither end closes, so the connection ends on its idle timeout
//   early         a server's tickets allow 0-RTT, and a resuming client would use it
//   hold-ms       this long with nothing to do before the closing end closes
//   transport     tls: QMux over TLS (the default); ws: WebSocket over HTTP/1.1 inside TLS, `alpn` its subprotocol; h2:
//                 `connections` WebSocket tunnels (RFC 8441) at once on the host's HTTP/2 connection inside TLS (ALPN
//                 h2), `alpn` their subprotocol, and no early data on a client; h2c: the same on one HTTP/2 connection
//                 with prior knowledge (interop_qmux.hpp). A ws or h2 server's one TLS context lists `alpn`, http/1.1
//                 and h2, and serves each.
//
// The report of each connection goes to standard output when it has ended; the closing end closes with the reason
// `cerid-interop done`. Over h2, the HTTP/2 connection's own end follows its last tunnel's report: `h2_alpn`,
// `h2_close` and `h2_close_code`.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/http2.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/qmux_host.hpp>
#include <crd/net/stream_poller.hpp>
#include <crd/net/stream_socket.hpp>
#include <crd/net/ticket_cache.hpp>
#include <crd/net/websocket.hpp>
#include <crd/net/websocket_tunnel.hpp>

#include "interop_echo.hpp"
#include "interop_qmux.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace
{
using namespace crd::crypto;
using namespace crd::net;
using namespace crd_net_interop;
using crd::containers::Span;

constexpr u64 kRunNs = 60000000000U;
constexpr u64 kWaitNs = 1000000U;
constexpr StringView kServerName = "cerid.test";
constexpr StringView kCloseReason = kQmuxCloseReason;
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr u8 kEarlyContext[4] = {'e', 'c', 't', 'x'};
constexpr StringView kHttp11 = "http/1.1";
constexpr StringView kHttp2 = "h2";
constexpr usize kHttp2InputBytes = 1U << 17U;
constexpr usize kHttp2OutputBytes = 1U << 18U;
constexpr u64 kHttp2LingerNs = 2000000000U; // a server waits this long for its client to hang up once all is done

enum class Transport : crd::u8
{
    Tls,
    WebSocket,
    Http2,              // the host's HTTP/2 inside TLS
    Http2PriorKnowledge, // HTTP/2 over TCP with prior knowledge
};

struct Options : QmuxOptions
{
    Transport transport = Transport::Tls;
};

class App final : public QmuxHostEvents
{
public:
    // `tunnels`: the echo runs in the WebSocket tunnels of HTTP/2 connections, not in the connections themselves.
    App(EchoPeer& peer, bool tunnels) noexcept : m_peer(peer), m_tunnels(tunnels) {}

    void on_accept(QuicHandle handle, QmuxConnection& connection, const Endpoint&) noexcept override
    {
        // A tunnel is accepted open; its bearer before TLS, when it cannot yet tell h2 from anything else.
        if (m_tunnels && !connection.websocket())
        {
            m_bearer = handle;
            return;
        }
        m_peer.started(handle, nullptr);
    }

    void on_service(QuicHandle handle, QmuxConnection& connection, u64 now_ns) noexcept override
    {
        Echo* const echo = m_peer.echo_of(handle);
        if (echo == nullptr)
        {
            return;
        }
        EchoTurn turn;
        turn.established = connection.peer_parameters_known();
        turn.can_open = connection.state() == QmuxState::Established && turn.established;
        turn.early_data = connection.handshake().early_data();
        turn.now_ns = now_ns;
        if (echo->service(connection.streams(), connection.datagrams(), turn))
        {
            connection.close(m_peer.options().code, bytes_of(kCloseReason), now_ns);
        }
    }

    void on_closed(QuicHandle handle, const QmuxConnection& connection, SocketError transport) noexcept override
    {
        const Echo* const echo = m_peer.echo_of(handle);
        if (echo != nullptr)
        {
            echo->report(connection.handshake().resumed(), connection.handshake().early_data(), connection.datagrams());
            // A tunnel has no TLS of its own: what carries it is HTTP/2.
            report_qmux(connection, m_tunnels ? kHttp2 : connection.handshake().application_protocol(), transport);
            m_peer.ended(handle);
        }
        else if (m_tunnels && handle == m_bearer)
        {
            const StringView alpn = connection.handshake().application_protocol();
            const StringView tls_error = crd::crypto::tls_error_name(connection.tls_error());
            std::printf("h2_alpn=%.*s\nh2_close=%s\nh2_close_code=%llu\nh2_tls_error=%.*s\n",
                        static_cast<int>(alpn.size()), alpn.data(), qmux_close_name(connection.close_info()),
                        static_cast<unsigned long long>(connection.close_info().error_code),
                        static_cast<int>(tls_error.size()), tls_error.data());
            std::fflush(stdout);
            m_bearer_ended = true;
        }
    }

    // A client's HTTP/2 connection, from connect().
    void bear(QuicHandle handle) noexcept { m_bearer = handle; }
    [[nodiscard]] QuicHandle bearer() const noexcept { return m_bearer; }
    [[nodiscard]] bool bearer_ended() const noexcept { return m_bearer_ended; }

private:
    EchoPeer& m_peer;
    QuicHandle m_bearer;
    bool m_tunnels;
    bool m_bearer_ended = false;
};

[[nodiscard]] bool parse_transport(const char* value, Transport& out) noexcept
{
    static constexpr struct
    {
        const char* name;
        Transport transport;
    } kTransports[] = {{"tls", Transport::Tls},
                       {"ws", Transport::WebSocket},
                       {"h2", Transport::Http2},
                       {"h2c", Transport::Http2PriorKnowledge}};
    for (const auto& entry : kTransports)
    {
        if (std::strcmp(entry.name, value) == 0)
        {
            out = entry.transport;
            return true;
        }
    }
    return false;
}

// A server's listening socket on the loopback; false, said on stderr, when it could not be bound. A client's stays
// closed.
[[nodiscard]] bool listen_on(const Options& options, TcpListener& listener) noexcept
{
    if (options.client)
    {
        return true;
    }
    TcpListenOptions listen;
    listen.bind = Endpoint{IpAddress::v4_loopback(), static_cast<crd::u16>(options.port)};
    SocketError error = SocketError::None;
    listener = TcpListener::open(listen, error);
    if (error != SocketError::None)
    {
        std::fputs("crd-qmux-peer: the socket could not be bound\n", stderr);
        return false;
    }
    return true;
}

// One HTTP/2 connection with prior knowledge whose WebSocket tunnels each run the echo.
class Http2Tunnels
{
public:
    explicit Http2Tunnels(const Options& options) noexcept : m_tunnels(options, kHttp2) {}

    [[nodiscard]] bool init(crd::memory::IAllocator& allocator, const Options& options,
                            const WebSocketConfig& websocket, u64 now_ns) noexcept
    {
        Http2Config config;
        config.side = options.client ? Http2Side::Client : Http2Side::Server;
        config.connect_protocol = !options.client;
        config.output_bytes = kHttp2OutputBytes;
        return m_h2.init(allocator, config, now_ns) &&
               m_tunnels.init(allocator, m_h2, tunnel_config(options, websocket));
    }

    // Reads, runs and writes until nothing moves; false once the socket has failed or the peer has hung up.
    [[nodiscard]] bool turn(StreamSocket& socket, u64 now_ns) noexcept
    {
        bool moved = true;
        while (moved && !m_failed)
        {
            moved = read(socket);
            moved = drain(now_ns) || moved;
            const usize before = m_h2.output().size();
            m_tunnels.service(now_ns);
            moved = m_h2.output().size() != before || moved;
            moved = write(socket) || moved;
        }
        return !m_failed && !m_ended;
    }

    // A client's GOAWAY once every tunnel has reported; true once it is queued.
    [[nodiscard]] bool goaway() noexcept
    {
        m_goaway = m_goaway || m_h2.goaway(true);
        return m_goaway;
    }

    [[nodiscard]] bool done() const noexcept { return m_tunnels.done(); }
    [[nodiscard]] u32 live() const noexcept { return m_tunnels.live(); }
    [[nodiscard]] bool wants_write() const noexcept { return !m_h2.output().empty(); }
    [[nodiscard]] u32 error() const noexcept { return m_h2.error(); }

private:
    [[nodiscard]] bool read(StreamSocket& socket) noexcept
    {
        if (m_used == sizeof(m_input) || m_ended)
        {
            return false;
        }
        const StreamResult result = socket.read(Span<u8>(m_input + m_used, sizeof(m_input) - m_used));
        m_used += result.bytes;
        m_ended = result.ended;
        m_failed = result.error != SocketError::None && result.error != SocketError::Busy;
        return result.bytes != 0U;
    }

    [[nodiscard]] bool drain(u64 now_ns) noexcept
    {
        usize at = 0U;
        bool moved = false;
        for (;;)
        {
            usize taken = 0U;
            Http2Event event;
            const bool more = m_h2.poll(ConstSpan<u8>(m_input + at, m_used - at), now_ns, taken, event);
            at += taken;
            moved = moved || taken != 0U;
            if (!more)
            {
                break;
            }
            moved = true;
            static_cast<void>(m_tunnels.on_event(event, now_ns));
        }
        std::memmove(m_input, m_input + at, m_used - at);
        m_used -= at;
        return moved;
    }

    [[nodiscard]] bool write(StreamSocket& socket) noexcept
    {
        const ConstSpan<u8> out = m_h2.output();
        if (out.empty())
        {
            return false;
        }
        const StreamResult result = socket.write(out);
        m_h2.output_taken(result.bytes);
        m_failed = result.error != SocketError::None && result.error != SocketError::Busy;
        return result.bytes != 0U;
    }

    Http2Connection m_h2;
    TunnelPeer m_tunnels;
    usize m_used = 0U;
    bool m_ended = false;
    bool m_failed = false;
    bool m_goaway = false;
    u8 m_input[kHttp2InputBytes];
};

// The tunnels' run: the reports go out as the tunnels end. A client hangs up once every tunnel has reported and its
// GOAWAY has gone; a server once its client has, or kHttp2LingerNs after its own tunnels have all reported.
int run_http2(crd::memory::IAllocator& allocator, const Options& options)
{
    TcpListener listener;
    if (!listen_on(options, listener))
    {
        return 1;
    }
    const HostClock clock;
    TcpPoller poller(allocator, 1U, options.client ? nullptr : &listener);
    const ConstSpan<u8> subprotocols[] = {bytes_of(options.alpn)};
    const WebSocketConfig websocket = websocket_config(subprotocols);
    static Http2Tunnels link(options);
    if (!poller.ready() || !link.init(allocator, options, websocket, clock.monotonic_ns()))
    {
        std::fputs("crd-qmux-peer: the HTTP/2 connection could not be made\n", stderr);
        return 1;
    }
    StreamSocket* socket = nullptr;
    bool connected = !options.client;
    if (options.client)
    {
        SocketError error = SocketError::None;
        socket = poller.connect(0U, Endpoint{IpAddress::v4_loopback(), static_cast<crd::u16>(options.port)}, error);
        if (socket == nullptr)
        {
            std::fputs("crd-qmux-peer: the connection could not be started\n", stderr);
            return 1;
        }
    }
    else
    {
        std::printf("port=%u\n", static_cast<unsigned>(listener.local_endpoint().port));
        std::fflush(stdout);
    }
    const u64 deadline = clock.monotonic_ns() + kRunNs;
    u64 done_ns = 0U;
    while (clock.monotonic_ns() < deadline)
    {
        StreamReady ready[2];
        const usize count = poller.wait(Span<StreamReady>(ready, 2U), kWaitNs);
        const u64 now_ns = clock.monotonic_ns();
        for (usize at = 0U; at < count; ++at)
        {
            if (ready[at].token == kStreamListener && socket == nullptr)
            {
                SocketError error = SocketError::None;
                socket = poller.accept(0U, error);
            }
        }
        if (socket == nullptr)
        {
            continue;
        }
        if (!connected)
        {
            const SocketError error = poller.connected(0U);
            if (error == SocketError::Busy)
            {
                continue;
            }
            if (error != SocketError::None)
            {
                std::fputs("crd-qmux-peer: the connection failed\n", stderr);
                return 1;
            }
            connected = true;
        }
        const bool alive = link.turn(*socket, now_ns);
        if (link.done() && done_ns == 0U)
        {
            done_ns = now_ns;
        }
        const bool hung_up = options.client ? link.done() && link.goaway() && !link.wants_write()
                                            : done_ns != 0U && now_ns - done_ns >= kHttp2LingerNs;
        if (!alive || hung_up)
        {
            poller.close(0U, false);
            if (link.error() != 0U || (!link.done() && link.live() != 0U))
            {
                std::fprintf(stderr, "crd-qmux-peer: the HTTP/2 connection ended with error %u and %u tunnels live\n",
                             static_cast<unsigned>(link.error()), static_cast<unsigned>(link.live()));
                return 1;
            }
            return 0;
        }
        poller.interest(0U, static_cast<crd::u8>(kStreamReadable | (link.wants_write() ? kStreamWritable : 0U)));
    }
    std::fputs("crd-qmux-peer: the run outlasted its deadline\n", stderr);
    return 1;
}

// A client's HTTP/2 connection inside TLS: opened first, its `connections` tunnels once it has agreed on h2 and the
// server allows extended CONNECT, and closed with NO_ERROR once every tunnel has reported. False, said on stderr, when
// the connection or a tunnel could not be opened.
[[nodiscard]] bool bear_tunnels(QmuxHost& host, App& app, EchoPeer& peer, const TlsContext& context, const Options& options,
                                u64 now_ns) noexcept
{
    SocketError socket = SocketError::None;
    if (!app.bearer().valid())
    {
        QmuxConnect request;
        request.server = Endpoint{IpAddress::v4_loopback(), static_cast<crd::u16>(options.port)};
        request.tls = &context;
        app.bear(host.connect(request, now_ns, socket));
        if (!app.bearer().valid())
        {
            std::fputs("crd-qmux-peer: the connection could not be started\n", stderr);
            return false;
        }
        return true;
    }
    QmuxConnection* const bearer = host.connection(app.bearer());
    if (bearer == nullptr || !bearer->http2() || bearer->state() != QmuxState::Established)
    {
        return true;
    }
    if (peer.done())
    {
        bearer->close(0U, ConstSpan<u8>(), now_ns);
        host.wake(app.bearer());
        return true;
    }
    while (peer.count() < options.connections)
    {
        WsTunnelError error = WsTunnelError::None;
        const QuicHandle tunnel = host.open_tunnel(app.bearer(), now_ns, error);
        if (!tunnel.valid())
        {
            if (error == WsTunnelError::NotReady)
            {
                return true;
            }
            std::fputs("crd-qmux-peer: a tunnel could not be opened\n", stderr);
            return false;
        }
        peer.started(tunnel, nullptr);
    }
    return true;
}

// The run over TLS: QMux connections one after another, or (h2) one HTTP/2 connection's tunnels at once, which ends with
// that connection. A server over h2 may report no tunnel: it answers a CONNECT it refuses itself.
int run(crd::memory::IAllocator& allocator, const Options& options, const TlsContext& context)
{
    TcpListener listener;
    if (!listen_on(options, listener))
    {
        return 1;
    }
    const HostClock clock;
    TcpPoller poller(allocator, kServerSlots + 1U, options.client ? nullptr : &listener);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    TicketCache tickets(allocator, TicketCacheConfig{});
    static EchoPeer peer(options);
    const bool tunnels = options.transport == Transport::Http2;
    App app(peer, tunnels);
    static u8 early_context[sizeof(kEarlyContext)];
    std::memcpy(early_context, kEarlyContext, sizeof(kEarlyContext));
    const ConstSpan<u8> subprotocols[] = {bytes_of(options.alpn)};
    const WebSocketConfig websocket = websocket_config(subprotocols);
    const bool carried = tunnels || options.transport == Transport::WebSocket;
    Http2Config http2;
    http2.connect_protocol = !options.client;
    QmuxHostConfig config;
    config.connection = qmux_config(options);
    config.connection.websocket = carried ? &websocket : nullptr;
    config.connection.http2 = carried ? &http2 : nullptr;
    config.connection.tls = options.client ? nullptr : &context;
    config.connection.tickets = options.client ? &tickets : nullptr;
    config.connection.early_context = options.early && !options.client ? ConstSpan<u8>(early_context) : ConstSpan<u8>();
    config.connections = kServerSlots;
    QmuxHost host(allocator, poller, clock, options.client ? nullptr : &buckets, app, config);
    if (!poller.ready() || !host.ready() || !tickets.ready())
    {
        std::fputs("crd-qmux-peer: the host could not be made\n", stderr);
        return 1;
    }
    if (!options.client)
    {
        std::printf("port=%u\n", static_cast<unsigned>(listener.local_endpoint().port));
        std::fflush(stdout);
    }
    const u64 deadline = clock.monotonic_ns() + kRunNs;
    while (!(tunnels ? app.bearer_ended() : peer.done()) && clock.monotonic_ns() < deadline)
    {
        const u64 now_ns = clock.monotonic_ns();
        tickets.advance(now_ns);
        if (tunnels && options.client && !bear_tunnels(host, app, peer, context, options, now_ns))
        {
            return 1;
        }
        if (!tunnels && options.client && peer.idle() && peer.count() < options.connections)
        {
            QmuxConnect request;
            request.server = Endpoint{IpAddress::v4_loopback(), static_cast<crd::u16>(options.port)};
            request.tls = &context;
            SocketError error = SocketError::None;
            const QuicHandle handle = host.connect(request, now_ns, error);
            if (!handle.valid())
            {
                std::fputs("crd-qmux-peer: the connection could not be started\n", stderr);
                return 1;
            }
            peer.started(handle, peer.count() + 1U < options.connections ? &tickets : nullptr);
        }
        static_cast<void>(host.poll(kWaitNs));
        peer.wake(host, clock.monotonic_ns());
    }
    if (tunnels ? !app.bearer_ended() || (options.client && !peer.done()) : !peer.done())
    {
        std::fputs(app.bearer_ended() ? "crd-qmux-peer: the HTTP/2 connection ended before its tunnels\n"
                                      : "crd-qmux-peer: the run outlasted its deadline\n",
                   stderr);
        return 1;
    }
    return 0;
}

int usage()
{
    std::fputs("usage: crd-qmux-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --alpn NAME "
               "--streams N --bytes N --closer client|server|none --code N --max-streams N [--uni N --uni-bytes N] "
               "[--resets N] [--stops N] [--datagrams N --datagram-bytes N [--datagram-limit N] [--datagram-probe 1]] "
               "[--connections 2 [--early 1]] [--idle-ms N] [--keepalive-ms N] [--hold-ms N] [--transport tls|ws|h2|h2c]\n",
               stderr);
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    Options options;
    u32 required = 0U;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* const name = argv[at];
        const char* const value = argv[at + 1];
        if (parse_echo_option(name, value, options, required) || parse_qmux_option(name, value, options))
        {
            continue;
        }
        if (std::strcmp(name, "--transport") == 0)
        {
            if (!parse_transport(value, options.transport))
            {
                return usage();
            }
        }
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || required != kRequiredEchoOptions || !echo_options_valid(options) || options.alpn.empty() ||
        (options.transport == Transport::Http2 && options.client && options.early))
    {
        return usage();
    }
    if (options.transport == Transport::Http2PriorKnowledge)
    {
        return run_http2(*crd::memory::default_allocator(), options);
    }

    SigningKey key;
    TlsCredential credential;
    if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(options.seed, sizeof(options.seed))) != CryptoError::None ||
        credential.init(key, kServerName) != TlsError::None)
    {
        std::fputs("crd-qmux-peer: the credential could not be made\n", stderr);
        return 1;
    }
    const KeyId pin = credential.key_id();
    const AlgorithmId groups[] = {options.group};
    const AlgorithmId aeads[] = {options.cipher};
    const StringView every[] = {options.alpn, kHttp11, kHttp2};
    const bool websocket = options.transport == Transport::WebSocket;
    const bool http2 = options.transport == Transport::Http2;
    const StringView protocols[] = {websocket ? kHttp11 : (http2 ? kHttp2 : options.alpn)};
    TlsConfig tls;
    tls.role = options.client ? TlsRole::Client : TlsRole::Server;
    tls.groups = ConstSpan<AlgorithmId>(groups, 1U);
    tls.aeads = ConstSpan<AlgorithmId>(aeads, 1U);
    tls.server_name = kServerName;
    tls.application_protocols = protocols;
    if ((websocket || http2) && !options.client)
    {
        tls.application_protocols = every;
    }
    if (options.client)
    {
        tls.signatures = kSignatures;
        tls.pinned = ConstSpan<KeyId>(&pin, 1U);
    }
    else
    {
        tls.credential = &credential;
    }
    tls.early_data = options.early;
    ReplayFilter replay;
    if (options.early && !options.client)
    {
        const u64 period = kTlsDefaultTicketLifetime + kTlsMaxTicketAgeSkew;
        if (replay.init(1024U, period, static_cast<u64>(std::time(nullptr))) != CryptoError::None)
        {
            return 1;
        }
        tls.early_data_filter = &replay;
    }
    TlsContext context;
    if (context.init(tls) != TlsError::None)
    {
        std::fputs("crd-qmux-peer: the TLS context could not be made\n", stderr);
        return 1;
    }
    return run(*crd::memory::default_allocator(), options, context);
}
