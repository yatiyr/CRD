// crd-net transport budget board (NET-PATH) -- crd-net's end of quic_board.py: QuicServer and QuicClient over real UDP
// on the loopback, each against itself, the workloads every board peer runs alike (msquic_quic_board.c is msquic's).
//
// Usage: crd-quic-board --role client|server --port N --seed HEX --group NAME --connections N [--parallel N]
//                       [--bytes N] [--window N] [--offload 0|1]
//
//   port         a server's to bind on 127.0.0.1 (0: any, printed as `port=N`), a client's to reach
//   seed         the 32-byte Ed25519 private key of the server's self-signed credential; a client pins its public key
//   group        the one key exchange group each end offers (interop_echo.hpp's names); the AEAD is AES-128-GCM
//   connections  a client's connections, `parallel` of them under way at once (closing ones not counted), each closed
//                with code 0 once it has done its work; a server ends once as many have closed
//   bytes        a connection's work: the client writes `bytes` bytes (0 for none) and a FIN on one bidirectional
//                stream from one prefilled buffer, the server counts and drops them and answers with its FIN alone,
//                and the client closes once that FIN arrives: no byte is generated or checked. The answer comes after
//                both ends finished the handshake, which a close right after the client's own completion would not
//                wait for (it drops the client's unsent Finished, RFC 9000 section 10.2)
//   window       each stream's receive window and send buffer; the connection's window is 16 of them
//   offload      1 (the default): the sockets hand the host segmented trains and take coalesced ones (GSO and GRO
//                where the host has them); 0: one datagram a call each way, as on a NIC without either
//
// The report, one `name=value` per line on standard output once the run has ended: `connections` that closed,
// `established` among them, `bytes_received` and `bytes_sent` on the streams, and what every established connection
// negotiated (`group`, `cipher`, `version`; `mixed` when two differed). The CPU time and memory are the driver's to
// measure, from outside.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_client.hpp>
#include <crd/net/quic_server.hpp>
#include <crd/net/udp.hpp>

#include "interop_echo.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
using namespace crd::crypto;
using namespace crd::net;
using namespace crd_net_interop;

constexpr u64 kRunNs = 300000000000U;
constexpr u64 kMaxWaitNs = 10000000U;
constexpr u32 kSlots = 256U; // live connections at most, closing ones included
constexpr u32 kRoutes = 8192U;
constexpr u8 kResetKey[kQuicResetKeyBytes] = {0x5E, 0x5F};
constexpr u8 kTokenKey[kQuicTokenKeyBytes] = {0x10, 0x11, 0x12};
constexpr StringView kProtocols[] = {"cerid-interop"};
constexpr StringView kServerName = "cerid.test";
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr u32 kVersions[] = {static_cast<u32>(QuicVersion::V1)};
constexpr usize kClientCidBytes = 8U;
constexpr usize kChunkBytes = 65536U;
constexpr u8 kFill[kChunkBytes] = {};

struct Options
{
    bool client = false;
    u64 port = 0U;
    u8 seed[32] = {};
    AlgorithmId group = AlgorithmId::X25519;
    u64 connections = 0U;
    u64 parallel = 1U;
    u64 bytes = 0U;
    u64 window = 1U << 20U;
    bool offload = true;
};

// One live connection's place in the work, by its slot.
struct Work
{
    bool opened = false; // a client's stream is open
    bool fin_read = false;
    bool fin_sent = false;
    bool closing = false; // done with its work
    u64 sent = 0U;
};

class Board final : public QuicServerEvents, public QuicClientEvents
{
public:
    explicit Board(const Options& options) noexcept : m_options(options) {}

    void on_accept(QuicHandle handle, QuicConnection&, const Endpoint&) noexcept override { m_work[handle.slot] = {}; }
    void on_restart(QuicHandle, QuicConnection&) noexcept override {}

    void on_service(QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept override
    {
        Work& work = m_work[handle.slot];
        if (work.closing || connection.state() != QuicConnectionState::Established)
        {
            return;
        }
        QuicStreamSet& set = connection.streams();
        if (m_options.client && !work.opened)
        {
            u64 id = 0U;
            work.opened = set.open(true, id) == QuicStreamError::None;
        }
        if (m_options.client || set.recv_state(0U) != QuicRecvState::None)
        {
            serve(work, set);
        }
        if (m_options.client && work.fin_read)
        {
            close(work, connection, now_ns);
        }
    }

    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        count(connection);
        m_finished += m_work[handle.slot].closing ? 0U : 1U;
        ++m_closed;
        m_work[handle.slot] = {};
    }

    [[nodiscard]] bool done() const noexcept { return m_closed >= m_options.connections; }
    // Whether a client starts another connection now: `parallel` are under way at most, closing ones (which hold their
    // slots for three PTOs, RFC 9000 section 10.2) not counted.
    [[nodiscard]] bool may_start() const noexcept
    {
        return m_options.client && m_started < m_options.connections && m_started - m_finished < m_options.parallel &&
               m_started - m_closed < kSlots;
    }
    void start(QuicHandle handle) noexcept
    {
        m_work[handle.slot] = {};
        ++m_started;
    }

    void report() const noexcept
    {
        std::printf("connections=%llu\nestablished=%llu\nbytes_received=%llu\nbytes_sent=%llu\n",
                    static_cast<unsigned long long>(m_closed), static_cast<unsigned long long>(m_established),
                    static_cast<unsigned long long>(m_received), static_cast<unsigned long long>(m_sent));
        std::printf("group=%s\ncipher=%s\nversion=%s\n", m_mixed ? "mixed" : group_name(m_group),
                    m_mixed ? "mixed" : cipher_name(m_cipher), m_mixed ? "mixed" : (m_version == 1U ? "1" : "other"));
        std::fflush(stdout);
    }

private:
    // A closed connection whose handshake finished joins the report: a server may read the client's Finished and its
    // close in one datagram, so it never serves the connection established.
    void count(const QuicConnection& connection) noexcept
    {
        const TlsState state = connection.handshake().state();
        if (state != TlsState::Established && state != TlsState::Closed)
        {
            return;
        }
        const AlgorithmId group = connection.handshake().group();
        const AlgorithmId cipher = connection.handshake().aead();
        const u32 version = static_cast<u32>(connection.version());
        if (m_established != 0U && (group != m_group || cipher != m_cipher || version != m_version))
        {
            m_mixed = true;
        }
        m_group = group;
        m_cipher = cipher;
        m_version = version;
        ++m_established;
    }

    void close(Work& work, QuicConnection& connection, u64 now_ns) noexcept
    {
        work.closing = true;
        ++m_finished;
        connection.close(0U, {}, now_ns);
    }

    // Reads stream 0 to its end, counting; a client writes its bytes and FIN, a server its FIN once the client's came.
    void serve(Work& work, QuicStreamSet& set) noexcept
    {
        while (!work.fin_read)
        {
            const QuicStreamRead piece = set.peek(0U);
            const usize size = piece.data.size();
            if ((size == 0U && !piece.fin) || set.consume(0U, size) != QuicStreamError::None)
            {
                break;
            }
            m_received += size;
            work.fin_read = piece.fin;
        }
        const bool may_end = m_options.client || work.fin_read;
        const u64 goal = m_options.client ? m_options.bytes : 0U;
        while (!work.fin_sent && may_end)
        {
            const u64 left = goal - work.sent;
            const usize want = left < kChunkBytes ? static_cast<usize>(left) : kChunkBytes;
            usize taken = 0U;
            if (set.write(0U, ConstSpan<u8>(kFill, want), want == left, taken) != QuicStreamError::None)
            {
                break;
            }
            work.sent += taken;
            m_sent += taken;
            work.fin_sent = taken == want && want == left;
            if (taken < want || want == 0U)
            {
                break;
            }
        }
    }

    Options m_options;
    Work m_work[kSlots] = {};
    u64 m_started = 0U;
    u64 m_finished = 0U;
    u64 m_closed = 0U;
    u64 m_established = 0U;
    u64 m_received = 0U;
    u64 m_sent = 0U;
    AlgorithmId m_group{};
    AlgorithmId m_cipher{};
    u32 m_version = 0U;
    bool m_mixed = false;
};

QuicConnectionConfig connection_config(const Options& options, const TlsContext& context)
{
    QuicConnectionConfig config;
    config.side = options.client ? QuicSide::Client : QuicSide::Server;
    config.version = QuicVersion::V1;
    config.versions = ConstSpan<u32>(kVersions);
    config.tls = &context;
    config.parameters.max_idle_timeout = 30000U;
    config.streams.peer_bidi = 1U;
    config.streams.local_bidi = 1U;
    config.streams.receive_bytes = static_cast<u32>(options.window);
    config.streams.send_bytes = static_cast<u32>(options.window);
    config.streams.stream_window = static_cast<u32>(options.window);
    config.streams.connection_window = options.window * 16U;
    config.streams.connection_max_window = options.window * 16U;
    return config;
}

// Turns the loop until the board is done, starting a client's connections as earlier ones finish: a turn that freed a
// place starts the next connection before any wait.
template <typename Start>
int drive(const HostClock& clock, const SocketSet& sockets, IoLoop& loop, const Board& board, const Start& start)
{
    const u64 deadline = clock.monotonic_ns() + kRunNs;
    while (!board.done())
    {
        if (!start(clock.monotonic_ns()))
        {
            return 1;
        }
        const u64 next = loop.poll();
        const u64 now = clock.monotonic_ns();
        if (now >= deadline)
        {
            std::fputs("crd-quic-board: the run outlasted its deadline\n", stderr);
            return 1;
        }
        if (next > now && !board.may_start())
        {
            static_cast<void>(sockets.wait_readable(next - now < kMaxWaitNs ? next - now : kMaxWaitNs));
        }
    }
    return 0;
}

bool open_socket(const Options& options, u64 port, UdpSocket& socket) noexcept
{
    UdpOptions udp;
    udp.bind = Endpoint{IpAddress::v4(127, 0, 0, 1), static_cast<crd::u16>(port)};
    udp.segment_send = options.offload;
    udp.coalesce_receive = options.offload;
    SocketError error = SocketError::Failed;
    socket = UdpSocket::open(udp, error);
    return error == SocketError::None;
}

int run_server(crd::memory::IAllocator& allocator, const Options& options, const TlsContext& context)
{
    UdpSocket socket;
    if (!open_socket(options, options.port, socket))
    {
        std::fputs("crd-quic-board: the socket could not be bound\n", stderr);
        return 1;
    }
    const SocketSet sockets(socket);
    const HostClock clock;
    ConnectionIdTable routes(allocator, kRoutes);
    // One loopback client is one prefix: the gates stay far above the board's rate.
    HandshakeRules rules;
    rules.unvalidated = GateRule{1000000U, kNanosPerSecond};
    rules.per_prefix = GateRule{1000000U, kNanosPerSecond};
    HandshakeBuckets buckets(allocator, rules);
    QuicResetKey reset;
    QuicTokenKeys tokens;
    if (reset.init(ConstSpan<u8>(kResetKey)) != CryptoError::None ||
        tokens.init(ConstSpan<u8>(kTokenKey), {}) != CryptoError::None)
    {
        return 1;
    }
    Ingress ingress(IngressConfig{}, routes, buckets, &tokens, nullptr, &reset);
    static Board board(options);
    QuicServerConfig config;
    config.connection = connection_config(options, context);
    config.connections = kSlots;
    config.new_tokens = false; // as the other board peers: no token for a later connection
    QuicServer server(allocator, sockets, clock, ingress, routes, reset, &tokens, board, config);
    IoLoop loop(allocator, sockets, clock, ingress, server, IoLoopConfig{});
    if (!server.ready() || !loop.ready())
    {
        std::fputs("crd-quic-board: the server could not be made\n", stderr);
        return 1;
    }
    std::printf("port=%u\n", static_cast<unsigned>(socket.local_endpoint().port));
    std::fflush(stdout);
    const int result = drive(clock, sockets, loop, board, [](u64) { return true; });
    board.report();
    return result;
}

int run_client(crd::memory::IAllocator& allocator, const Options& options, const TlsContext& context)
{
    UdpSocket socket;
    if (!open_socket(options, 0U, socket))
    {
        return 1;
    }
    const SocketSet sockets(socket);
    const HostClock clock;
    ConnectionIdTable routes(allocator, kRoutes);
    QuicResetKey reset;
    if (reset.init(ConstSpan<u8>(kResetKey)) != CryptoError::None)
    {
        return 1;
    }
    IngressConfig ingress_config;
    ingress_config.local_cid_bytes = kClientCidBytes;
    Ingress ingress(ingress_config, routes);
    static Board board(options);
    QuicClientConfig config;
    config.connection = connection_config(options, context);
    config.connections = kSlots;
    QuicClient client(allocator, sockets, ingress, routes, reset, board, config);
    IoLoop loop(allocator, sockets, clock, ingress, client, IoLoopConfig{});
    if (!client.ready() || !loop.ready())
    {
        std::fputs("crd-quic-board: the client could not be made\n", stderr);
        return 1;
    }
    QuicClientConnect request;
    request.server = Endpoint{IpAddress::v4(127, 0, 0, 1), static_cast<crd::u16>(options.port)};
    const auto start = [&](u64 now_ns)
    {
        while (board.may_start())
        {
            const QuicHandle handle = client.connect(request, now_ns);
            if (!handle.valid())
            {
                std::fputs("crd-quic-board: a connection could not be started\n", stderr);
                return false;
            }
            board.start(handle);
        }
        return true;
    };
    const int result = drive(clock, sockets, loop, board, start);
    board.report();
    return result;
}

int usage()
{
    std::fputs("usage: crd-quic-board --role client|server --port N --seed HEX --group NAME --connections N "
               "[--parallel N] [--bytes N] [--window N] [--offload 0|1]\n",
               stderr);
    return 2;
}

bool parse(int argc, char** argv, Options& options)
{
    bool role = false;
    bool seed = false;
    for (int at = 1; at + 1 < argc; at += 2)
    {
        const char* const name = argv[at];
        const char* const value = argv[at + 1];
        const u64 number = std::strtoull(value, nullptr, 10);
        if (std::strcmp(name, "--role") == 0)
        {
            options.client = std::strcmp(value, "client") == 0;
            role = options.client || std::strcmp(value, "server") == 0;
        }
        else if (std::strcmp(name, "--seed") == 0)
        {
            seed = parse_seed(value, options.seed);
        }
        else if (std::strcmp(name, "--group") == 0)
        {
            if (!parse_group(value, options.group))
            {
                return false;
            }
        }
        else if (std::strcmp(name, "--port") == 0)
        {
            options.port = number;
        }
        else if (std::strcmp(name, "--connections") == 0)
        {
            options.connections = number;
        }
        else if (std::strcmp(name, "--parallel") == 0)
        {
            options.parallel = number;
        }
        else if (std::strcmp(name, "--bytes") == 0)
        {
            options.bytes = number;
        }
        else if (std::strcmp(name, "--window") == 0)
        {
            options.window = number;
        }
        else if (std::strcmp(name, "--offload") == 0)
        {
            options.offload = number != 0U;
        }
        else
        {
            return false;
        }
    }
    return argc % 2 == 1 && role && seed && options.port <= 65535U && options.connections != 0U &&
           options.parallel != 0U && options.parallel <= kSlots && options.window >= 4096U &&
           options.window <= (u64{1} << 26U) && (!options.client || options.port != 0U);
}
} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse(argc, argv, options))
    {
        return usage();
    }
    SigningKey key;
    TlsCredential credential;
    if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(options.seed, sizeof(options.seed))) != CryptoError::None ||
        credential.init(key, kServerName) != TlsError::None)
    {
        std::fputs("crd-quic-board: the credential could not be made\n", stderr);
        return 1;
    }
    const KeyId pin = credential.key_id();
    const AlgorithmId groups[] = {options.group};
    const AlgorithmId aeads[] = {AlgorithmId::Aes128Gcm};
    TlsConfig tls;
    tls.role = options.client ? TlsRole::Client : TlsRole::Server;
    tls.groups = ConstSpan<AlgorithmId>(groups, 1U);
    tls.aeads = ConstSpan<AlgorithmId>(aeads, 1U);
    tls.server_name = kServerName;
    tls.application_protocols = ConstSpan<StringView>(kProtocols);
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
        std::fputs("crd-quic-board: the TLS context could not be made\n", stderr);
        return 1;
    }
    crd::memory::IAllocator& allocator = *crd::memory::default_allocator();
    return options.client ? run_client(allocator, options, context) : run_server(allocator, options, context);
}
