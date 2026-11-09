// crd-net interoperability test (4.2a) -- a QMux endpoint over crd-net's QmuxHost, real TCP on the loopback, the
// counterpart of qmux_crate_peer. qmux_interop.py runs one of each and reads both reports.
//
// Usage: crd-qmux-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --alpn NAME --streams N
//                      --bytes N --closer client|server|none --code N --max-streams N [--uni N --uni-bytes N]
//                      [--resets N] [--stops N] [--datagrams N --datagram-bytes N [--datagram-limit N]
//                      [--datagram-probe 1]] [--connections 2 [--early 1]] [--idle-ms N] [--keepalive-ms N]
//                      [--hold-ms N] [--websocket 1]
//
//   port, seed, group, cipher  as crd-quic-peer's; interop_echo.hpp describes the application
//   alpn          the one application protocol this end offers or accepts
//   closer        none: neither end closes, so the connection ends on its idle timeout
//   early         a server's tickets allow 0-RTT, and a resuming client would use it
//   idle-ms       the idle timeout this end offers (default 30000)
//   keepalive-ms  a QX_PING after this long with no record
//   hold-ms       this long with nothing to do before the closing end closes
//   websocket     WebSocket over HTTP/1.1, `alpn` its subprotocol; a server's TLS lists `alpn` as well
//
// The report of each connection goes to standard output when it has ended; the closing end closes with the reason
// `cerid-interop done`.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/qmux.hpp>
#include <crd/net/qmux_host.hpp>
#include <crd/net/stream_poller.hpp>
#include <crd/net/stream_socket.hpp>
#include <crd/net/ticket_cache.hpp>

#include "interop_echo.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace
{
using namespace crd::crypto;
using namespace crd::net;
using namespace crd_net_interop;

constexpr u64 kRunNs = 60000000000U;
constexpr u64 kWaitNs = 1000000U;
constexpr StringView kServerName = "cerid.test";
constexpr StringView kCloseReason = "cerid-interop done";
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr u32 kStreamBytes = 65536U;
constexpr u64 kConnectionWindow = 1U << 22U;
constexpr u64 kConnectionMaxWindow = 1U << 24U;
constexpr u32 kDatagramRingBytes = 1U << 20U;
constexpr u32 kDatagramRingCount = 2048U;
constexpr u8 kEarlyContext[4] = {'e', 'c', 't', 'x'};
constexpr StringView kHttp11 = "http/1.1";
constexpr StringView kWebSocketPath = "/interop";

struct Options : EchoOptions
{
    StringView alpn;
    u64 idle_ms = 30000U;
    u64 keepalive_ms = 0U;
    bool websocket = false;
};

[[nodiscard]] ConstSpan<u8> bytes_of(StringView text) noexcept
{
    return ConstSpan<u8>(reinterpret_cast<const u8*>(text.data()), text.size());
}

[[nodiscard]] const char* close_name(const QmuxCloseInfo& info) noexcept
{
    switch (info.reason)
    {
    case QmuxCloseReason::None:
        return "none";
    case QmuxCloseReason::Local:
        return "local";
    case QmuxCloseReason::Error:
        return "transport";
    case QmuxCloseReason::Peer:
        return info.application ? "application" : "peer-transport";
    case QmuxCloseReason::PeerShutdown:
        return "shutdown";
    case QmuxCloseReason::IdleTimeout:
        return "idle";
    case QmuxCloseReason::HandshakeTimeout:
        return "handshake-timeout";
    case QmuxCloseReason::TlsFailed:
        return "tls";
    case QmuxCloseReason::Truncated:
        return "truncated";
    case QmuxCloseReason::Reset:
        return "reset";
    case QmuxCloseReason::UpgradeRefused:
        return "upgrade-refused";
    case QmuxCloseReason::WebSocketFailed:
        return "websocket-failed";
    }
    return "unknown";
}

class App final : public QmuxHostEvents
{
public:
    explicit App(EchoPeer& peer) noexcept : m_peer(peer) {}

    void on_accept(QuicHandle handle, QmuxConnection&, const Endpoint&) noexcept override
    {
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
            connection.close(m_peer.options().code,
                             ConstSpan<u8>(reinterpret_cast<const u8*>(kCloseReason.data()), kCloseReason.size()),
                             now_ns);
        }
    }

    void on_closed(QuicHandle handle, const QmuxConnection& connection, SocketError transport) noexcept override
    {
        const Echo* const echo = m_peer.echo_of(handle);
        if (echo == nullptr)
        {
            return;
        }
        echo->report(connection.handshake().resumed(), connection.handshake().early_data(), connection.datagrams());
        const QmuxCloseInfo& info = connection.close_info();
        const StringView alpn = connection.handshake().application_protocol();
        const StringView tls_error = tls_error_name(connection.tls_error());
        const ConstSpan<u8> reason = connection.close_reason();
        const ConstSpan<u8> subprotocol = connection.websocket_protocol();
        std::printf("websocket_protocol=%.*s\n", static_cast<int>(subprotocol.size()),
                    reinterpret_cast<const char*>(subprotocol.data()));
        std::printf("alpn=%.*s\nclose=%s\nclose_code=%llu\nclose_reason=%.*s\ntls_error=%.*s\nsocket_error=%d\n",
                    static_cast<int>(alpn.size()), alpn.data(), close_name(info),
                    static_cast<unsigned long long>(info.error_code), static_cast<int>(reason.size()),
                    reinterpret_cast<const char*>(reason.data()), static_cast<int>(tls_error.size()), tls_error.data(),
                    static_cast<int>(transport));
        const QmuxCounters& counters = connection.counters();
        std::printf("records_sent=%llu\nrecords_received=%llu\npings_sent=%llu\npongs_received=%llu\nrtt=%d\n",
                    static_cast<unsigned long long>(counters.records_sent),
                    static_cast<unsigned long long>(counters.records_received),
                    static_cast<unsigned long long>(counters.pings_sent),
                    static_cast<unsigned long long>(counters.pongs_received), connection.rtt_ns() != 0U ? 1 : 0);
        std::fflush(stdout);
        m_peer.ended(handle);
    }

private:
    EchoPeer& m_peer;
};

QmuxConfig connection_config(const Options& options)
{
    QmuxConfig config;
    const u64 uni = options.uni + options.resets + options.stops;
    config.streams.peer_bidi = static_cast<u32>(options.max_streams);
    config.streams.local_bidi = static_cast<u32>(options.max_streams);
    config.streams.peer_uni = static_cast<u32>(uni != 0U ? uni : 1U);
    config.streams.local_uni = static_cast<u32>(uni != 0U ? uni : 1U);
    config.streams.receive_bytes = kStreamBytes;
    config.streams.send_bytes = kStreamBytes;
    config.streams.stream_window = kStreamBytes;
    config.streams.connection_window = kConnectionWindow;
    config.streams.connection_max_window = kConnectionMaxWindow;
    if (options.datagrams != 0U)
    {
        config.datagrams.max_frame_bytes = static_cast<u32>(options.datagram_limit);
        config.datagrams.receive_bytes = kDatagramRingBytes;
        config.datagrams.receive_count = kDatagramRingCount;
        config.datagrams.send_bytes = kDatagramRingBytes;
        config.datagrams.send_count = kDatagramRingCount;
    }
    config.idle_timeout_ns = options.idle_ms * 1000000U;
    config.keepalive_ns = options.keepalive_ms * 1000000U;
    return config;
}

int run(crd::memory::IAllocator& allocator, const Options& options, const TlsContext& context)
{
    TcpListener listener;
    if (!options.client)
    {
        TcpListenOptions listen;
        listen.bind = Endpoint{IpAddress::v4_loopback(), static_cast<crd::u16>(options.port)};
        SocketError error = SocketError::None;
        listener = TcpListener::open(listen, error);
        if (error != SocketError::None)
        {
            std::fputs("crd-qmux-peer: the socket could not be bound\n", stderr);
            return 1;
        }
    }
    const HostClock clock;
    TcpPoller poller(allocator, kServerSlots + 1U, options.client ? nullptr : &listener);
    HandshakeBuckets buckets(allocator, HandshakeRules{});
    TicketCache tickets(allocator, TicketCacheConfig{});
    static EchoPeer peer(options);
    App app(peer);
    static u8 early_context[sizeof(kEarlyContext)];
    std::memcpy(early_context, kEarlyContext, sizeof(kEarlyContext));
    const ConstSpan<u8> subprotocols[] = {bytes_of(options.alpn)};
    WebSocketConfig websocket;
    websocket.protocols = subprotocols;
    websocket.path = bytes_of(kWebSocketPath);
    websocket.host = bytes_of(kServerName);
    QmuxHostConfig config;
    config.connection = connection_config(options);
    config.connection.websocket = options.websocket ? &websocket : nullptr;
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
    while (!peer.done() && clock.monotonic_ns() < deadline)
    {
        const u64 now_ns = clock.monotonic_ns();
        tickets.advance(now_ns);
        if (options.client && peer.idle() && peer.count() < options.connections)
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
    if (!peer.done())
    {
        std::fputs("crd-qmux-peer: the run outlasted its deadline\n", stderr);
        return 1;
    }
    return 0;
}

int usage()
{
    std::fputs("usage: crd-qmux-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --alpn NAME "
               "--streams N --bytes N --closer client|server|none --code N --max-streams N [--uni N --uni-bytes N] "
               "[--resets N] [--stops N] [--datagrams N --datagram-bytes N [--datagram-limit N] [--datagram-probe 1]] "
               "[--connections 2 [--early 1]] [--idle-ms N] [--keepalive-ms N] [--hold-ms N] [--websocket 1]\n",
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
        if (parse_echo_option(name, value, options, required))
        {
            continue;
        }
        if (std::strcmp(name, "--alpn") == 0)
        {
            options.alpn = StringView(value, std::strlen(value));
        }
        else if (std::strcmp(name, "--idle-ms") == 0)
        {
            options.idle_ms = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--keepalive-ms") == 0)
        {
            options.keepalive_ms = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--websocket") == 0)
        {
            options.websocket = std::strcmp(value, "0") != 0;
        }
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || required != kRequiredEchoOptions || !echo_options_valid(options) || options.alpn.empty())
    {
        return usage();
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
    const StringView both[] = {options.alpn, kHttp11};
    const StringView protocols[] = {options.websocket ? kHttp11 : options.alpn};
    TlsConfig tls;
    tls.role = options.client ? TlsRole::Client : TlsRole::Server;
    tls.groups = ConstSpan<AlgorithmId>(groups, 1U);
    tls.aeads = ConstSpan<AlgorithmId>(aeads, 1U);
    tls.server_name = kServerName;
    tls.application_protocols = protocols;
    if (options.websocket && !options.client)
    {
        tls.application_protocols = both;
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
