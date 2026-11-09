// crd-net interoperability test (4.2a) -- a QUIC endpoint over crd-net's QuicServer and QuicClient, real UDP on the
// loopback, the counterpart of ngtcp2_quic_peer.c. quic_interop.py runs one of each and reads both reports.
//
// Usage: crd-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --version 1|2 --streams N
//                      --bytes N --closer client|server --code N --max-streams N [--compat 1] [--retry 1]
//                      [--key-update N] [--datagrams N --datagram-bytes N [--datagram-limit N] [--datagram-probe 1]]
//                      [--connections 2 [--early 1] [--reject-early 1]] [--only-version 1] [--progress-at N]
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
//
// The report of each connection, one `name=value` per line from `connection=N` on standard output when it has ended,
// is the same as ngtcp2_quic_peer.c's without what only OpenSSL reports. The application is `cerid-interop`, a private
// ALPN.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_client.hpp>
#include <crd/net/quic_server.hpp>
#include <crd/net/ticket_cache.hpp>
#include <crd/net/udp.hpp>

#include "interop_echo.hpp"

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

struct Options : EchoOptions
{
    bool compat = false;
    bool retry = false;
    u64 key_update = 0U;
    u32 version = 1U;
    bool reject_early = false;
    bool only_version = false;
};

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
    if (echo->service(connection.streams(), connection.datagrams(), turn))
    {
        connection.close(peer.options().code, {}, now_ns);
    }
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
    echo->report(connection.handshake().resumed(), connection.handshake().early_data(), connection.datagrams());
    std::printf("version=%u\nclose=%s\nclose_code=%llu\n", connection.version() == QuicVersion::V2 ? 2U : 1U,
                close_kind(info),
                static_cast<unsigned long long>(info.error_code));
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
    return limits;
}

QuicDatagramLimits datagram_limits(const Options& options)
{
    QuicDatagramLimits limits;
    if (options.datagrams != 0U)
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
    static u8 early_context[sizeof(kEarlyContext)];
    std::memcpy(early_context, kEarlyContext, sizeof(kEarlyContext));
    ServerApp app(peer, options.reject_early ? Span<u8>(early_context) : Span<u8>());
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
        config.connection.early_context = ConstSpan<u8>(early_context);
    }
    config.connections = kServerSlots;
    QuicServer server(allocator, socket, clock, ingress, routes, reset, &tokens, app, config);
    IoLoop loop(allocator, socket, clock, ingress, server, IoLoopConfig{});
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
    QuicClient client(allocator, socket, ingress, routes, reset, app, config);
    IoLoop loop(allocator, socket, clock, ingress, client, IoLoopConfig{});
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
               "[--progress-at N]\n",
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
        if (std::strcmp(name, "--version") == 0)
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
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || required != kRequiredEchoOptions || !echo_options_valid(options) ||
        (options.version != 1U && options.version != 2U))
    {
        return usage();
    }

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
    tls.application_protocols = kProtocols;
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
