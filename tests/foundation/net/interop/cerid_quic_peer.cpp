// crd-net interoperability test (4.2a) -- a QUIC endpoint over crd-net's QuicServer and QuicClient, real UDP on the
// loopback, the counterpart of ngtcp2_quic_peer.c. quic_interop.py runs one of each and reads both reports.
//
// Usage: crd-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --version 1|2 --streams N
//                      --bytes N --closer client|server --code N --max-streams N [--compat 1] [--retry 1]
//                      [--key-update N] [--datagrams N --datagram-bytes N [--datagram-limit N] [--datagram-probe 1]]
//                      [--connections 2 [--early 1] [--reject-early 1]] [--only-version 1] [--progress-at N]
//                      [--receive-bytes N] [--congestion newreno|cubic|bbr]
//                      [--http3 1 ...] [--webtransport legacy|h3 ...] [--transport h3 --alpn NAME ...]
//
//   port         a server's to bind (0: any, printed as `port=N`), a client's to reach
//   seed         the 32-byte Ed25519 private key of the server's self-signed credential; a client pins its public key
//   group        crd-tls-peer's names (x25519mlkem768, secp256r1mlkem768, secp384r1mlkem1024, x25519, ...)
//   cipher       aes128, aes256 or chacha20
//   streams      bidirectional streams a client opens, each carrying `bytes` bytes and a FIN; a server echoes as many
//                bytes as it reads and its FIN after the client's. Both ends check every byte (the pattern below)
//   closer       the end that closes once every stream is done, with the application error `code`
//   max-streams  the bidirectional streams this end lets its peer have open at once
//   compat       compatible version negotiation (RFC 9368): a client offers versions 1 and 2, a server prefers 2
//   retry        a server answers a client Initial without a token with a Retry
//   key-update   this end starts a key update every N 1-RTT packets it sends
//   datagrams    DATAGRAM frames a client sends, `datagram-bytes` each: the index in four bytes and a pattern; a server
//                sends each back in another pattern
//   datagram-limit  the max_datagram_frame_size this end advertises
//   datagram-probe  this end also offers one payload a byte over `datagram-bytes`, which its peer's limit must refuse
//   connections  connections run one after the other; a client resumes the second on a ticket of the first
//   early        0-RTT: a resuming client writes its streams and datagrams at once
//   reject-early a server's early data context changes after the first connection, so the second's 0-RTT is refused
//   only-version a server runs `version` alone and answers others with Version Negotiation
//   progress-at  a server prints `progress=1` once it has read this many bytes
//   receive-bytes  the largest datagram the loop reads whole (1500 by default), advertised as max_udp_payload_size
//   congestion   the connections' controller (congestion.hpp; CUBIC by default)
//   http3        HTTP/3 over Http3Connection instead of the echo: http_exchange.h has its options, ALPN `h3` instead
//   webtransport WebTransport sessions instead, `legacy` or `h3`: interop_webtransport.hpp has its options
//   transport    h3: WebSocket tunnels (RFC 9220) on one HTTP/3 connection instead, `connections` of them at once,
//                each a QMux connection running the echo with `alpn` its subprotocol (interop_qmux.hpp has the
//                options); crd-qmux-peer's report for each, and `h3_close`, `h3_close_code` once the connection ends.
//                The client closes the connection with H3_NO_ERROR once its tunnels are done and the server's GOAWAY
//                came; a server sends GOAWAY once its tunnels are done, and closes after kTunnelLingerNs if the client
//                has not
//
// The report of each connection, one `name=value` per line from `connection=N` on standard output when it has ended,
// is the same as ngtcp2_quic_peer.c's without what only OpenSSL reports, and with the path's ECN validation (`ecn`,
// `ecn_failure`), the CE count the peer reported over every space (`ecn_ce`), the congestion events that reduced
// the window (`reductions`), the path MTU search's datagram size (`pmtu`), probes lost (`pmtu_probes_lost`) and
// black holes (`black_holes`), the peer's max_udp_payload_size (`peer_max_udp_payload`), and the controller
// (`congestion`) with its model's bandwidth in bytes per second and minimum round trip (`bandwidth`, `min_rtt_ns`; 0
// for a window controller). The application is `cerid-interop`, a private ALPN.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/congestion.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_client.hpp>
#include <crd/net/quic_server.hpp>
#include <crd/net/ticket_cache.hpp>
#include <crd/net/udp.hpp>

#include "interop_echo.hpp"
#include "interop_http3.hpp"
#include "interop_qmux.hpp"
#include "interop_webtransport.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

namespace
{
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::net;
using namespace crd_net_interop;

constexpr u64 kRunNs = 60000000000U;
constexpr u64 kWaitNs = 1000000U;
constexpr u8 kResetKey[kQuicResetKeyBytes] = {0x5E, 0x5F};
constexpr u8 kTokenKey[kQuicTokenKeyBytes] = {0x10, 0x11, 0x12};
constexpr StringView kProtocols[] = {"cerid-interop"};
constexpr StringView kHttp3Protocols[] = {HTTP3_ALPN};
constexpr u32 kHttp3UniStreams = 16U;
constexpr StringView kServerName = "cerid.test";
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr u32 kBothVersions[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};
constexpr u32 kPreferV2[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};
constexpr usize kClientCidBytes = 8U;
constexpr u32 kReceiveBytes = 65536U;
constexpr u32 kSendBytes = 65536U;
constexpr u64 kConnectionWindow = 1U << 22U;
constexpr u64 kConnectionMaxWindow = 1U << 24U;
constexpr u32 kDatagramRingBytes = 1U << 20U;
constexpr u32 kDatagramRingCount = 2048U;
constexpr u8 kEarlyContext[4] = {'e', 'c', 't', 'x'};
constexpr StringView kTunnelVersion = "h3";
constexpr u64 kTunnelLingerNs = 2000000000U; // how long an end waits for the other once its tunnels are done
constexpr u32 kTunnelStreamBytes = 1U << 21U;

struct Options : QmuxOptions
{
    bool tunnels = false; // --transport h3; `connections` is then the tunnels' count, on one QUIC connection
    bool compat = false;
    bool retry = false;
    u64 key_update = 0U;
    u32 version = 1U;
    bool reject_early = false;
    bool only_version = false;
    u32 receive_bytes = 1500U;
    CongestionAlgorithm congestion = CongestionAlgorithm::Cubic;
    httpx_options_t http3;
    WtOptions wt;
};

[[nodiscard]] bool runs_webtransport(const Options& options) noexcept
{
    return options.wt.revision != WtRevision::None;
}

// Whether the application speaks HTTP/3 at all, and so the ALPN and the streams HTTP/3 needs are in order.
[[nodiscard]] bool runs_http3(const Options& options) noexcept
{
    return options.http3.enabled != 0 || runs_webtransport(options) || options.tunnels;
}

// One QUIC connection's HTTP/3 whose WebSocket tunnels each run the echo (interop_qmux.hpp's TunnelPeer).
class Http3Tunnels
{
public:
    // `options` counts the tunnels; it and `websocket` outlive this.
    Http3Tunnels(const QmuxOptions& options, const WebSocketConfig& websocket) noexcept
        : m_options(options), m_websocket(websocket), m_peer(options, kTunnelVersion)
    {
    }
    Http3Tunnels(const Http3Tunnels&) = delete;
    Http3Tunnels& operator=(const Http3Tunnels&) = delete;
    Http3Tunnels(Http3Tunnels&&) = delete;
    Http3Tunnels& operator=(Http3Tunnels&&) = delete;
    ~Http3Tunnels() = default;

    // The error to close the connection with, 0 to go on; `done` when this end should close it with H3_NO_ERROR.
    [[nodiscard]] u64 service(QuicConnection& connection, bool established, u64 now_ns, bool& done) noexcept
    {
        if (!m_made && !make(connection))
        {
            return kH3InternalError;
        }
        if (!m_h3.started() && (!established || !m_h3.start()))
        {
            return m_h3.error();
        }
        Http3Event event;
        while (m_h3.poll(event))
        {
            static_cast<void>(m_peer.on_event(event, now_ns)); // the rest is no tunnel's, and this end opens no other
        }
        if (m_h3.error() != 0U)
        {
            return m_h3.error();
        }
        m_peer.service(now_ns);
        if (m_peer.done() && m_done_ns == 0U)
        {
            m_done_ns = now_ns;
            if (!m_options.client && !m_h3.goaway(true))
            {
                return m_h3.error();
            }
        }
        const bool lingered = m_done_ns != 0U && now_ns - m_done_ns >= kTunnelLingerNs;
        done = lingered || (m_done_ns != 0U && m_options.client && m_h3.goaway_received());
        return m_h3.error();
    }

    // The connection has ended: after every tunnel's report, how.
    static void report(const QuicConnection& connection, const char* kind) noexcept
    {
        std::printf("h3_close=%s\nh3_close_code=%llu\n", kind,
                    static_cast<unsigned long long>(connection.close_info().error_code));
        std::fflush(stdout);
    }

private:
    [[nodiscard]] bool make(QuicConnection& connection) noexcept
    {
        m_made = true;
        httpx_options_t http3;
        httpx_default_options(&http3);
        crd::memory::IAllocator& allocator = *crd::memory::default_allocator();
        return m_h3.init(allocator, connection.streams(), nullptr, http3_config(http3, m_options.client, 0U)) &&
               m_peer.init(allocator, m_h3, tunnel_config(m_options, m_websocket));
    }

    const QmuxOptions& m_options;
    const WebSocketConfig& m_websocket;
    Http3Connection m_h3;
    TunnelPeer m_peer;
    u64 m_done_ns = 0U;
    bool m_made = false;
};

// The HTTP/3 or WebTransport application of each connection, in the order they started.
Http3Exchange g_exchanges[kMaxConnections];
WtExchange g_wt_exchanges[kMaxConnections];
// The one connection's tunnels with --transport h3, ended with it: their HTTP/3 state reads its stream set.
Http3Tunnels* g_tunnels = nullptr;
const Options* g_options = nullptr;

// One turn of WebTransport on a connection.
void serve_webtransport(EchoPeer& peer, QuicHandle handle, QuicConnection& connection, const EchoTurn& turn) noexcept
{
    WtRun run;
    run.echo = g_options;
    run.wt = &g_options->wt;
    run.http3 = &g_options->http3;
    bool done = false;
    const u64 error = g_wt_exchanges[peer.index_of(handle)].service(connection, run, turn, done);
    if (error != 0U)
    {
        connection.close(error, {}, turn.now_ns);
    }
    else if (done)
    {
        connection.close(g_options->code, {}, turn.now_ns);
    }
}

// One turn of HTTP/3 on a connection.
void serve_http3(EchoPeer& peer, QuicHandle handle, QuicConnection& connection, const EchoTurn& turn) noexcept
{
    const u64 index = peer.index_of(handle);
    Http3Run run;
    run.options = &g_options->http3;
    run.client = g_options->client;
    run.datagrams = g_options->datagrams;
    run.datagram_bytes = g_options->datagram_bytes;
    run.remembered = index != 0U && g_options->early ? g_exchanges[index - 1U].peer_settings() : nullptr;
    run.tickets = peer.echo_of(handle)->tickets();
    bool done = false;
    const u64 error = g_exchanges[index].service(connection, run, turn, done);
    if (error != 0U)
    {
        connection.close(error, {}, turn.now_ns);
    }
    else if (done)
    {
        connection.close(g_options->code, {}, turn.now_ns);
    }
}

void serve(EchoPeer& peer, QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept
{
    Echo* const echo = peer.echo_of(handle);
    if (echo == nullptr)
    {
        return;
    }
    EchoTurn turn;
    turn.established = connection.state() == QuicConnectionState::Established;
    turn.early_data = connection.handshake().early_data();
    turn.can_open = turn.established || turn.early_data == TlsEarlyData::Offered;
    turn.now_ns = now_ns;
    if (g_tunnels != nullptr)
    {
        bool done = false;
        const u64 error = g_tunnels->service(connection, turn.established, now_ns, done);
        if (error != 0U || done)
        {
            connection.close(error != 0U ? error : kH3NoError, {}, now_ns);
        }
        return;
    }
    if (runs_webtransport(*g_options))
    {
        echo->note_established(turn.established);
        serve_webtransport(peer, handle, connection, turn);
        return;
    }
    if (g_options->http3.enabled != 0)
    {
        echo->note_established(turn.established);
        serve_http3(peer, handle, connection, turn);
        return;
    }
    if (echo->service(connection.streams(), connection.datagrams(), turn))
    {
        connection.close(peer.options().code, {}, now_ns);
    }
}

// The path's ECN validation (quic_ecn.hpp) as the report names it, in the enumerations' order.
constexpr const char* kEcnStates[] = {"off", "testing", "unknown", "capable", "failed"};
constexpr const char* kEcnFailures[] = {"none", "missing", "decreased", "exceeded", "short", "lost"};

const char* ecn_state(QuicEcnState state) noexcept
{
    return kEcnStates[static_cast<unsigned>(state)];
}

const char* ecn_failure(QuicEcnFailure failure) noexcept
{
    return kEcnFailures[static_cast<unsigned>(failure)];
}

// The controllers (congestion.hpp) by --congestion's names, in CongestionAlgorithm's order.
constexpr const char* kCongestionNames[] = {"newreno", "cubic", "bbr"};

const char* congestion_name(CongestionAlgorithm algorithm) noexcept
{
    return kCongestionNames[static_cast<unsigned>(algorithm)];
}

bool congestion_named(const char* name, CongestionAlgorithm& algorithm) noexcept
{
    for (unsigned at = 0U; at < sizeof(kCongestionNames) / sizeof(kCongestionNames[0]); ++at)
    {
        if (std::strcmp(name, kCongestionNames[at]) == 0)
        {
            algorithm = static_cast<CongestionAlgorithm>(at);
            return true;
        }
    }
    return false;
}

const char* close_kind(const QuicCloseInfo& info) noexcept
{
    if (info.local)
    {
        return "local";
    }
    if (info.idle)
    {
        return "idle";
    }
    if (info.stateless_reset)
    {
        return "reset";
    }
    return info.application ? "application" : "transport";
}

void report(EchoPeer& peer, QuicHandle handle, const QuicConnection& connection) noexcept
{
    const Echo* const echo = peer.echo_of(handle);
    if (echo == nullptr)
    {
        return;
    }
    const QuicCloseInfo& info = connection.close_info();
    if (g_tunnels != nullptr)
    {
        Http3Tunnels::report(connection, close_kind(info));
        g_tunnels->~Http3Tunnels();
        g_tunnels = nullptr;
        peer.ended(handle);
        return;
    }
    echo->report(connection.handshake().resumed(), connection.handshake().early_data(), connection.datagrams());
    if (g_options->http3.enabled != 0)
    {
        g_exchanges[peer.index_of(handle)].report();
    }
    if (runs_webtransport(*g_options))
    {
        g_wt_exchanges[peer.index_of(handle)].report();
    }
    const QuicEcnValidator& ecn = connection.ecn();
    const u64 ce = ecn.reported(QuicSpace::Initial).ce + ecn.reported(QuicSpace::Handshake).ce +
                   ecn.reported(QuicSpace::Application).ce;
    std::printf("version=%u\nclose=%s\nclose_code=%llu\necn=%s\necn_failure=%s\necn_ce=%llu\nreductions=%llu\n",
                connection.version() == QuicVersion::V2 ? 2U : 1U, close_kind(info),
                static_cast<unsigned long long>(info.error_code), ecn_state(ecn.state()), ecn_failure(ecn.failure()),
                static_cast<unsigned long long>(ce),
                static_cast<unsigned long long>(connection.congestion().state().reductions));
    const CongestionController& congestion = connection.congestion();
    const CongestionState model = congestion.state();
    std::printf("congestion=%s\nbandwidth=%llu\nmin_rtt_ns=%llu\n", congestion_name(congestion.algorithm()),
                static_cast<unsigned long long>(model.bandwidth), static_cast<unsigned long long>(model.min_rtt_ns));
    const QuicPathMtu& path = connection.path_mtu();
    std::printf("pmtu=%u\npmtu_probes_lost=%llu\nblack_holes=%llu\npeer_max_udp_payload=%llu\n", path.size(),
                static_cast<unsigned long long>(path.counters().probes_lost),
                static_cast<unsigned long long>(path.counters().black_holes),
                static_cast<unsigned long long>(
                    connection.peer_parameters_known() ? connection.peer_parameters().max_udp_payload_size : 0U));
    std::fflush(stdout);
    peer.ended(handle);
}

class ServerApp final : public QuicServerEvents
{
public:
    // `context`: the application's part of the early data context, changed by every connection when it is set.
    ServerApp(EchoPeer& peer, Span<u8> context) noexcept : m_peer(peer), m_context(context) {}
    void on_accept(QuicHandle handle, QuicConnection&, const Endpoint&) noexcept override
    {
        m_peer.started(handle, nullptr);
        if (!m_context.empty())
        {
            // This connection holds its copy, which its tickets name; the next connection's differs.
            m_context[0] = static_cast<u8>(m_context[0] ^ 1U);
        }
    }
    void on_service(QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept override
    {
        serve(m_peer, handle, connection, now_ns);
    }
    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        report(m_peer, handle, connection);
    }

private:
    EchoPeer& m_peer;
    Span<u8> m_context;
};

class ClientApp final : public QuicClientEvents
{
public:
    explicit ClientApp(EchoPeer& peer) noexcept : m_peer(peer) {}
    void on_restart(QuicHandle, QuicConnection&) noexcept override {}
    void on_service(QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept override
    {
        serve(m_peer, handle, connection, now_ns);
    }
    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        report(m_peer, handle, connection);
    }

private:
    EchoPeer& m_peer;
};

QuicStreamLimits stream_limits(const Options& options)
{
    QuicStreamLimits limits;
    limits.peer_bidi = static_cast<u32>(options.max_streams);
    limits.local_bidi = static_cast<u32>(options.max_streams);
    limits.receive_bytes = kReceiveBytes;
    limits.send_bytes = kSendBytes;
    limits.stream_window = kReceiveBytes;
    limits.connection_window = kConnectionWindow;
    limits.connection_max_window = kConnectionMaxWindow;
    if (runs_http3(options))
    {
        limits.peer_uni = kHttp3UniStreams + static_cast<u32>(options.uni);
        limits.local_uni = kHttp3UniStreams + static_cast<u32>(options.uni);
    }
    if (options.tunnels)
    {
        // A tunnel's stream carries a whole QMux connection: it gets a connection's room, as the HTTP/2 path's peer
        // windows give it. With less, a QMux stream reset while its bytes wait behind others' goes as RESET_STREAM
        // alone (final size 0, RFC 9000 section 19.4), which the qmux crate never hands to accept_uni()
        // (docs/third-party-defects.md).
        limits.receive_bytes = kTunnelStreamBytes;
        limits.send_bytes = kTunnelStreamBytes;
        limits.stream_window = kTunnelStreamBytes;
    }
    limits.reset_stream_at = runs_webtransport(options);
    return limits;
}

QuicDatagramLimits datagram_limits(const Options& options)
{
    QuicDatagramLimits limits;
    if ((options.datagrams != 0U && !options.tunnels) || runs_webtransport(options))
    {
        limits.max_frame_bytes = static_cast<u32>(options.datagram_limit);
        limits.receive_bytes = kDatagramRingBytes;
        limits.receive_count = kDatagramRingCount;
        limits.send_bytes = kDatagramRingBytes;
        limits.send_count = kDatagramRingCount;
        limits.outcomes = true;
    }
    return limits;
}

QuicConnectionConfig connection_config(const Options& options, const TlsContext& context)
{
    QuicConnectionConfig config;
    config.side = options.client ? QuicSide::Client : QuicSide::Server;
    config.version = options.version == 2U ? QuicVersion::V2 : QuicVersion::V1;
    config.tls = &context;
    config.parameters.max_idle_timeout = 30000U;
    config.streams = stream_limits(options);
    config.datagrams = datagram_limits(options);
    config.key_update_interval = options.key_update;
    config.congestion.algorithm = options.congestion;
    return config;
}

// `turn` runs before each pass and returns false to end the run.
template <typename Turn>
int drive(const HostClock& clock, DatagramSocket& socket, IoLoop& loop, QuicHost& host, EchoPeer& peer,
          const Turn& turn)
{
    const u64 deadline = clock.monotonic_ns() + kRunNs;
    while (!peer.done() && clock.monotonic_ns() < deadline)
    {
        if (!turn(clock.monotonic_ns()))
        {
            return 1;
        }
        static_cast<void>(loop.poll());
        peer.wake(host, clock.monotonic_ns());
        static_cast<void>(socket.wait_readable(kWaitNs));
    }
    if (!peer.done())
    {
        std::fputs("crd-quic-peer: the run outlasted its deadline\n", stderr);
        return 1;
    }
    return 0;
}

int run_server(crd::memory::IAllocator& allocator, const Options& options, const TlsContext& context)
{
    UdpOptions udp;
    udp.bind = Endpoint{IpAddress::v4(127, 0, 0, 1), static_cast<crd::u16>(options.port)};
    SocketError error = SocketError::Failed;
    UdpSocket socket = UdpSocket::open(udp, error);
    if (error != SocketError::None)
    {
        std::fputs("crd-quic-peer: the socket could not be bound\n", stderr);
        return 1;
    }
    const HostClock clock;
    ConnectionIdTable routes(allocator, 64U);
    HandshakeRules rules;
    rules.always_retry = options.retry;
    HandshakeBuckets buckets(allocator, rules);
    QuicResetKey reset;
    QuicTokenKeys tokens;
    if (reset.init(ConstSpan<u8>(kResetKey)) != CryptoError::None ||
        tokens.init(ConstSpan<u8>(kTokenKey), {}) != CryptoError::None)
    {
        return 1;
    }
    const u32 only[] = {options.version == 2U ? static_cast<u32>(QuicVersion::V2) : static_cast<u32>(QuicVersion::V1)};
    IngressConfig ingress_config;
    if (options.only_version)
    {
        ingress_config.versions[0] = only[0];
        ingress_config.version_count = 1U;
    }
    Ingress ingress(ingress_config, routes, buckets, &tokens, nullptr, &reset);
    static EchoPeer peer(options);
    static u8 early_context[kHttp3EarlyContextBytes];
    const usize early_bytes =
        options.http3.enabled != 0
            ? Http3Connection::early_context(http3_config(options.http3, false, options.datagrams),
                                             Span<u8>(early_context))
            : sizeof(kEarlyContext);
    if (options.http3.enabled == 0)
    {
        std::memcpy(early_context, kEarlyContext, sizeof(kEarlyContext));
    }
    ServerApp app(peer, options.reject_early ? Span<u8>(early_context, early_bytes) : Span<u8>());
    QuicServerConfig config;
    config.connection = connection_config(options, context);
    config.connection.versions = ConstSpan<u32>(kBothVersions);
    if (options.only_version)
    {
        config.connection.versions = ConstSpan<u32>(only);
    }
    else if (options.compat)
    {
        config.connection.versions = ConstSpan<u32>(kPreferV2);
    }
    if (options.early)
    {
        config.connection.early_context = ConstSpan<u8>(early_context, early_bytes);
    }
    config.connections = kServerSlots;
    config.receive_bytes = options.receive_bytes;
    QuicServer server(allocator, socket, clock, ingress, routes, reset, &tokens, app, config);
    IoLoop loop(allocator, socket, clock, ingress, server, IoLoopConfig{.slot_bytes = options.receive_bytes});
    if (!server.ready() || !loop.ready())
    {
        std::fputs("crd-quic-peer: the server could not be made\n", stderr);
        return 1;
    }
    std::printf("port=%u\n", static_cast<unsigned>(socket.local_endpoint().port));
    std::fflush(stdout);
    return drive(clock, socket, loop, server, peer, [](u64) { return true; });
}

int run_client(crd::memory::IAllocator& allocator, const Options& options, const TlsContext& context)
{
    UdpOptions udp;
    udp.bind = Endpoint{IpAddress::v4(127, 0, 0, 1), 0U};
    SocketError error = SocketError::Failed;
    UdpSocket socket = UdpSocket::open(udp, error);
    if (error != SocketError::None)
    {
        return 1;
    }
    const HostClock clock;
    ConnectionIdTable routes(allocator, 64U);
    QuicResetKey reset;
    if (reset.init(ConstSpan<u8>(kResetKey)) != CryptoError::None)
    {
        return 1;
    }
    IngressConfig ingress_config;
    ingress_config.local_cid_bytes = kClientCidBytes;
    Ingress ingress(ingress_config, routes);
    static EchoPeer peer(options);
    ClientApp app(peer);
    QuicClientConfig config;
    config.connection = connection_config(options, context);
    if (options.compat)
    {
        config.connection.versions = ConstSpan<u32>(kBothVersions);
    }
    config.connections = 1U;
    config.receive_bytes = options.receive_bytes;
    QuicClient client(allocator, socket, ingress, routes, reset, app, config);
    IoLoop loop(allocator, socket, clock, ingress, client, IoLoopConfig{.slot_bytes = options.receive_bytes});
    if (!client.ready() || !loop.ready())
    {
        std::fputs("crd-quic-peer: the client could not be made\n", stderr);
        return 1;
    }
    QuicClientConnect request;
    request.server = Endpoint{IpAddress::v4(127, 0, 0, 1), static_cast<crd::u16>(options.port)};
    const auto next_connection = [&](u64 now_ns)
    {
        if (!peer.idle() || peer.count() >= options.connections)
        {
            return true;
        }
        const QuicHandle handle = client.connect(request, now_ns);
        if (!handle.valid())
        {
            std::fputs("crd-quic-peer: the connection could not be started\n", stderr);
            return false;
        }
        peer.started(handle, peer.count() + 1U < options.connections ? &client.tickets() : nullptr);
        return true;
    };
    return drive(clock, socket, loop, client, peer, next_connection);
}

int usage()
{
    std::fputs("usage: crd-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME "
               "--version 1|2 --streams N --bytes N --closer client|server --code N --max-streams N [--compat 1] "
               "[--retry 1] [--key-update N] [--datagrams N --datagram-bytes N [--datagram-limit N] "
               "[--datagram-probe 1]] [--connections 2 [--early 1] [--reject-early 1]] [--only-version 1] "
               "[--progress-at N] [--receive-bytes N] [--congestion newreno|cubic|bbr] [--http3 1 ...] "
               "[--webtransport legacy|h3 ...] "
               "[--transport h3 --alpn NAME [--idle-ms N] [--keepalive-ms N]]\n",
               stderr);
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    Options options;
    httpx_default_options(&options.http3);
    u32 required = 0U;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* const name = argv[at];
        const char* const value = argv[at + 1];
        if (parse_echo_option(name, value, options, required) || httpx_parse_option(&options.http3, name, value) != 0 ||
            wt_parse_option(options.wt, name, value) || parse_qmux_option(name, value, options))
        {
            continue;
        }
        if (std::strcmp(name, "--transport") == 0 && std::strcmp(value, "h3") == 0)
        {
            options.tunnels = true;
        }
        else if (std::strcmp(name, "--version") == 0)
        {
            options.version = static_cast<u32>(std::strtoul(value, nullptr, 10));
        }
        else if (std::strcmp(name, "--compat") == 0)
        {
            options.compat = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--retry") == 0)
        {
            options.retry = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--key-update") == 0)
        {
            options.key_update = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--reject-early") == 0)
        {
            options.reject_early = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--only-version") == 0)
        {
            options.only_version = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--receive-bytes") == 0)
        {
            options.receive_bytes = static_cast<u32>(std::strtoul(value, nullptr, 10));
        }
        else if (std::strcmp(name, "--congestion") == 0)
        {
            if (!congestion_named(value, options.congestion))
            {
                return usage();
            }
        }
        else
        {
            return usage();
        }
    }
    const bool other_http3 = options.http3.enabled != 0 || runs_webtransport(options);
    if (argc % 2 == 0 || required != kRequiredEchoOptions || !echo_options_valid(options) ||
        (options.version != 1U && options.version != 2U) || httpx_options_valid(&options.http3) == 0 ||
        options.tunnels == options.alpn.empty() || (options.tunnels && other_http3))
    {
        return usage();
    }
    options.timed_service = runs_http3(options);
    // The tunnels count `connections` and run their own echo; the QUIC run is one connection with none.
    static const QmuxOptions tunnel_options = options;
    static const ConstSpan<u8> subprotocols[] = {bytes_of(options.alpn)};
    static const WebSocketConfig websocket = websocket_config(subprotocols);
    alignas(Http3Tunnels) static unsigned char tunnel_storage[sizeof(Http3Tunnels)];
    if (options.tunnels)
    {
        options.connections = 1U;
        g_tunnels = new (tunnel_storage) Http3Tunnels(tunnel_options, websocket);
    }
    g_options = &options;

    SigningKey key;
    TlsCredential credential;
    if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(options.seed, sizeof(options.seed))) != CryptoError::None ||
        credential.init(key, kServerName) != TlsError::None)
    {
        std::fputs("crd-quic-peer: the credential could not be made\n", stderr);
        return 1;
    }
    const KeyId pin = credential.key_id();
    const AlgorithmId groups[] = {options.group};
    const AlgorithmId aeads[] = {options.cipher};
    TlsConfig tls;
    tls.role = options.client ? TlsRole::Client : TlsRole::Server;
    tls.groups = ConstSpan<AlgorithmId>(groups, 1U);
    tls.aeads = ConstSpan<AlgorithmId>(aeads, 1U);
    tls.server_name = kServerName;
    tls.application_protocols =
        runs_http3(options) ? ConstSpan<StringView>(kHttp3Protocols) : ConstSpan<StringView>(kProtocols);
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
        std::fputs("crd-quic-peer: the TLS context could not be made\n", stderr);
        return 1;
    }
    crd::memory::IAllocator& allocator = *crd::memory::default_allocator();
    return options.client ? run_client(allocator, options, context) : run_server(allocator, options, context);
}
