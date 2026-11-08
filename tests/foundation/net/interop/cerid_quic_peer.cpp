// crd-net interoperability test (4.2a) -- a QUIC endpoint over crd-net's QuicServer and QuicClient, real UDP on the
// loopback, the counterpart of ngtcp2_quic_peer.c. quic_interop.py runs one of each and reads both reports.
//
// Usage: crd-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME --version 1|2 --streams N
//                      --bytes N --closer client|server --code N --max-streams N [--compat 1] [--retry 1]
//                      [--key-update N]
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
//
// The report, one `name=value` per line on standard output when the connection has ended, is the same as
// ngtcp2_quic_peer.c's without what only OpenSSL reports. The application is `cerid-interop`, a private ALPN.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_client.hpp>
#include <crd/net/quic_server.hpp>
#include <crd/net/udp.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::StringView;
using namespace crd::crypto;
using namespace crd::net;

constexpr usize kMaxStreams = 4096U;
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

struct Options
{
    bool client = false;
    bool client_closes = true;
    bool compat = false;
    bool retry = false;
    u64 key_update = 0U;
    u32 version = 1U;
    u64 port = 0U;
    u64 streams = 0U;
    u64 bytes = 0U;
    u64 code = 0U;
    u64 max_streams = 16U;
    AlgorithmId group{};
    AlgorithmId cipher{};
    u8 seed[32] = {};
};

// The byte at `offset` of a stream's direction (tag 0: client to server, 1: server to client).
u8 pattern(u64 tag, u64 stream, u64 offset)
{
    const u64 at = (offset + (stream * 37U)) & 0xFFFFU;
    return static_cast<u8>((at * 31U) + (at >> 8U) + (tag * 7U));
}

struct Stream
{
    u64 received = 0U;
    u64 sent = 0U;
    bool in_end = false;
    bool out_end = false;
    bool finished = false; // every byte and the FIN acknowledged
};

// The one connection's application, either end's.
class Echo
{
public:
    explicit Echo(const Options& options) noexcept : m_options(options) {}

    void service(QuicConnection& connection, u64 now_ns) noexcept
    {
        if (m_closing)
        {
            return;
        }
        m_established = m_established || connection.state() == QuicConnectionState::Established;
        QuicStreamSet& set = connection.streams();
        QuicStreamEvent event;
        while (set.poll(event))
        {
            if ((event.events & kQuicStreamFinished) != 0U && event.id / 4U < kMaxStreams)
            {
                if (!m_streams[event.id / 4U].finished)
                {
                    m_streams[event.id / 4U].finished = true;
                    ++m_finished;
                }
            }
        }
        if (m_options.client && connection.state() == QuicConnectionState::Established)
        {
            open_streams(set);
        }
        const u64 range = m_options.client ? m_opened : kMaxStreams;
        for (u64 index = 0U; index < range; ++index)
        {
            const u64 id = index * 4U;
            if (!m_options.client && set.recv_state(id) == QuicRecvState::None)
            {
                continue;
            }
            Stream& stream = m_streams[index];
            read(set, id, stream);
            const u64 goal = m_options.client ? m_options.bytes : stream.received;
            if (!stream.out_end)
            {
                write(set, id, stream, goal, m_options.client || stream.in_end);
            }
        }
        if (m_options.client_closes == m_options.client && complete())
        {
            m_closing = true;
            connection.close(m_options.code, {}, now_ns);
        }
    }

    void closed(const QuicConnection& connection) noexcept
    {
        const QuicCloseInfo& info = connection.close_info();
        const char* const kind = info.local             ? "local"
                                 : info.idle            ? "idle"
                                 : info.stateless_reset ? "reset"
                                 : info.application     ? "application"
                                                        : "transport";
        std::printf("established=%d\nversion=%u\nstreams=%llu\nbytes_received=%llu\nbytes_sent=%llu\nverified=%d\n",
                    m_established ? 1 : 0, connection.version() == QuicVersion::V2 ? 2U : 1U,
                    static_cast<unsigned long long>(m_options.client ? m_opened : m_seen),
                    static_cast<unsigned long long>(m_received), static_cast<unsigned long long>(m_sent),
                    m_verified ? 1 : 0);
        std::printf("close=%s\nclose_code=%llu\n", kind, static_cast<unsigned long long>(info.error_code));
        std::fflush(stdout);
        m_done = true;
    }

    [[nodiscard]] bool done() const noexcept { return m_done; }
    [[nodiscard]] bool established() const noexcept { return m_established; }

private:
    void open_streams(QuicStreamSet& set) noexcept
    {
        while (m_opened < m_options.streams)
        {
            u64 id = 0U;
            if (set.open(true, id) != QuicStreamError::None)
            {
                return;
            }
            ++m_opened;
        }
    }

    void read(QuicStreamSet& set, u64 id, Stream& stream) noexcept
    {
        if (stream.in_end)
        {
            return;
        }
        const u64 tag = m_options.client ? 1U : 0U;
        for (;;)
        {
            const QuicStreamRead piece = set.peek(id);
            if (piece.reset)
            {
                m_verified = false;
                stream.in_end = true;
                return;
            }
            for (usize at = 0U; at < piece.data.size(); ++at)
            {
                m_verified = m_verified && piece.data[at] == pattern(tag, id, stream.received + at);
            }
            const usize size = piece.data.size();
            const bool fin = piece.fin;
            if (set.consume(id, size) != QuicStreamError::None)
            {
                m_verified = false;
                return;
            }
            stream.received += size;
            m_received += size;
            if (fin)
            {
                stream.in_end = true;
                ++m_seen;
                return;
            }
            if (size == 0U)
            {
                return;
            }
        }
    }

    void write(QuicStreamSet& set, u64 id, Stream& stream, u64 goal, bool fin) noexcept
    {
        const u64 tag = m_options.client ? 0U : 1U;
        u8 chunk[1024];
        for (;;)
        {
            const u64 left = goal - stream.sent;
            const usize want = left < sizeof(chunk) ? static_cast<usize>(left) : sizeof(chunk);
            for (usize at = 0U; at < want; ++at)
            {
                chunk[at] = pattern(tag, id, stream.sent + at);
            }
            const bool last = fin && want == left;
            usize taken = 0U;
            const QuicStreamError error = set.write(id, ConstSpan<u8>(chunk, want), last, taken);
            if (error != QuicStreamError::None)
            {
                return;
            }
            stream.sent += taken;
            m_sent += taken;
            if (last && taken == want)
            {
                stream.out_end = true;
                return;
            }
            if (taken < want || want == 0U)
            {
                return;
            }
        }
    }

    [[nodiscard]] bool complete() const noexcept
    {
        if (!m_options.client)
        {
            return m_finished >= m_options.streams && m_seen >= m_options.streams;
        }
        if (m_opened < m_options.streams)
        {
            return false;
        }
        for (u64 index = 0U; index < m_options.streams; ++index)
        {
            const Stream& stream = m_streams[index];
            if (!stream.in_end || !stream.out_end || stream.received != m_options.bytes)
            {
                return false;
            }
        }
        return true;
    }

    Options m_options;
    Stream m_streams[kMaxStreams];
    u64 m_opened = 0U;
    u64 m_seen = 0U;
    u64 m_finished = 0U;
    u64 m_received = 0U;
    u64 m_sent = 0U;
    bool m_established = false;
    bool m_verified = true;
    bool m_closing = false;
    bool m_done = false;
};

class ServerApp final : public QuicServerEvents
{
public:
    explicit ServerApp(Echo& echo) noexcept : m_echo(echo) {}
    void on_accept(QuicHandle, QuicConnection&, const Endpoint&) noexcept override {}
    void on_service(QuicHandle, QuicConnection& connection, u64 now_ns) noexcept override
    {
        m_echo.service(connection, now_ns);
    }
    void on_closed(QuicHandle, const QuicConnection& connection) noexcept override { m_echo.closed(connection); }

private:
    Echo& m_echo;
};

class ClientApp final : public QuicClientEvents
{
public:
    explicit ClientApp(Echo& echo) noexcept : m_echo(echo) {}
    void on_restart(QuicHandle, QuicConnection&) noexcept override {}
    void on_service(QuicHandle, QuicConnection& connection, u64 now_ns) noexcept override
    {
        m_echo.service(connection, now_ns);
    }
    void on_closed(QuicHandle, const QuicConnection& connection) noexcept override { m_echo.closed(connection); }

private:
    Echo& m_echo;
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

QuicConnectionConfig connection_config(const Options& options, const TlsContext& context)
{
    QuicConnectionConfig config;
    config.side = options.client ? QuicSide::Client : QuicSide::Server;
    config.version = options.version == 2U ? QuicVersion::V2 : QuicVersion::V1;
    config.tls = &context;
    config.parameters.max_idle_timeout = 30000U;
    config.streams = stream_limits(options);
    config.key_update_interval = options.key_update;
    return config;
}

template <typename Loop>
int drive(const HostClock& clock, DatagramSocket& socket, Loop& loop, const Echo& echo)
{
    const u64 deadline = clock.monotonic_ns() + kRunNs;
    while (!echo.done() && clock.monotonic_ns() < deadline)
    {
        static_cast<void>(loop.poll());
        static_cast<void>(socket.wait_readable(kWaitNs));
    }
    if (!echo.done())
    {
        std::fputs("crd-quic-peer: the run outlasted its deadline\n", stderr);
        return 1;
    }
    return echo.established() ? 0 : 1;
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
    Ingress ingress(IngressConfig{}, routes, buckets, &tokens, nullptr, &reset);
    static Echo echo(options);
    ServerApp app(echo);
    QuicServerConfig config;
    config.connection = connection_config(options, context);
    config.connection.versions = options.compat ? ConstSpan<u32>(kPreferV2) : ConstSpan<u32>(kBothVersions);
    config.connections = 4U;
    QuicServer server(allocator, socket, clock, ingress, routes, reset, &tokens, app, config);
    IoLoop loop(allocator, socket, clock, ingress, server, IoLoopConfig{});
    if (!server.ready() || !loop.ready())
    {
        std::fputs("crd-quic-peer: the server could not be made\n", stderr);
        return 1;
    }
    std::printf("port=%u\n", static_cast<unsigned>(socket.local_endpoint().port));
    std::fflush(stdout);
    return drive(clock, socket, loop, echo);
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
    static Echo echo(options);
    ClientApp app(echo);
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
    if (!client.connect(request, clock.monotonic_ns()).valid())
    {
        std::fputs("crd-quic-peer: the connection could not be started\n", stderr);
        return 1;
    }
    return drive(clock, socket, loop, echo);
}

bool parse_group(const char* name, AlgorithmId& out)
{
    static constexpr struct
    {
        const char* name;
        AlgorithmId id;
    } kGroups[] = {{"x25519mlkem768", AlgorithmId::X25519Mlkem768},
                   {"secp256r1mlkem768", AlgorithmId::Secp256r1Mlkem768},
                   {"secp384r1mlkem1024", AlgorithmId::Secp384r1Mlkem1024},
                   {"x25519", AlgorithmId::X25519}};
    for (const auto& group : kGroups)
    {
        if (std::strcmp(group.name, name) == 0)
        {
            out = group.id;
            return true;
        }
    }
    return false;
}

bool parse_cipher(const char* name, AlgorithmId& out)
{
    if (std::strcmp(name, "aes128") == 0)
    {
        out = AlgorithmId::Aes128Gcm;
        return true;
    }
    if (std::strcmp(name, "aes256") == 0)
    {
        out = AlgorithmId::Aes256Gcm;
        return true;
    }
    if (std::strcmp(name, "chacha20") == 0)
    {
        out = AlgorithmId::Chacha20Poly1305;
        return true;
    }
    return false;
}

bool parse_seed(const char* text, u8* out)
{
    if (std::strlen(text) != 64U)
    {
        return false;
    }
    for (usize at = 0U; at < 32U; ++at)
    {
        unsigned value = 0U;
        if (std::sscanf(text + (at * 2U), "%2x", &value) != 1)
        {
            return false;
        }
        out[at] = static_cast<u8>(value);
    }
    return true;
}

int usage()
{
    std::fputs("usage: crd-quic-peer --role client|server --port N --seed HEX --group NAME --cipher NAME "
               "--version 1|2 --streams N --bytes N --closer client|server --code N --max-streams N [--compat 1] "
               "[--retry 1] [--key-update N]\n",
               stderr);
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    Options options;
    bool role = false;
    bool seed = false;
    bool group = false;
    bool cipher = false;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* const name = argv[at];
        const char* const value = argv[at + 1];
        if (std::strcmp(name, "--role") == 0)
        {
            options.client = std::strcmp(value, "client") == 0;
            role = true;
        }
        else if (std::strcmp(name, "--port") == 0)
        {
            options.port = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--seed") == 0)
        {
            seed = parse_seed(value, options.seed);
        }
        else if (std::strcmp(name, "--group") == 0)
        {
            group = parse_group(value, options.group);
        }
        else if (std::strcmp(name, "--cipher") == 0)
        {
            cipher = parse_cipher(value, options.cipher);
        }
        else if (std::strcmp(name, "--version") == 0)
        {
            options.version = static_cast<u32>(std::strtoul(value, nullptr, 10));
        }
        else if (std::strcmp(name, "--streams") == 0)
        {
            options.streams = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--bytes") == 0)
        {
            options.bytes = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--closer") == 0)
        {
            options.client_closes = std::strcmp(value, "client") == 0;
        }
        else if (std::strcmp(name, "--code") == 0)
        {
            options.code = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--max-streams") == 0)
        {
            options.max_streams = std::strtoull(value, nullptr, 10);
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
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || !role || !seed || !group || !cipher || (options.version != 1U && options.version != 2U) ||
        options.streams > kMaxStreams)
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
    TlsContext context;
    if (context.init(tls) != TlsError::None)
    {
        std::fputs("crd-quic-peer: the TLS context could not be made\n", stderr);
        return 1;
    }
    crd::memory::IAllocator& allocator = *crd::memory::default_allocator();
    return options.client ? run_client(allocator, options, context) : run_server(allocator, options, context);
}
