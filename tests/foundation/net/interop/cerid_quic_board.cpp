// crd-net transport budget board (NET-PATH) -- crd-net's end of quic_board.py: QuicServer and QuicClient over real UDP
// on the loopback, each against itself, the workloads every board peer runs alike (msquic_quic_board.c is msquic's)
// and crd-net's own bursts apart.
//
// Usage: crd-quic-board --role client|server --port N --seed HEX --group NAME --connections N [--parallel N]
//                       [--bytes N] [--window N] [--offload 0|1] [--hold 0|1] [--bursts N --gap MS]
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
//   hold         1: a client keeps each connection open, idle, once its work is done, and closes them all once every
//                one's is, so all `connections` are live at once (the memory workload); a server needs no switch
//   bursts, gap  a client's one connection does its work `bursts` times, each on a new stream opened `gap`
//                milliseconds after the last one's answer came (the idle trim's workload; a server answers each
//                stream as it comes and needs no switch)
//
// The report, one `name=value` per line on standard output once the run has ended: `connections` that closed,
// `established` among them, `bytes_received` and `bytes_sent` on the streams, the `exchanges` done (a client's answers
// read, a server's streams answered), the `allocations` the closed connections took once their second exchange had
// ended (QuicConnection::buffer_allocations: a buffer or table retaken after an idle trim counts; the first exchange's
// turn may still write the server's session ticket, which takes its CRYPTO buffer once), the exchanges a client began
// `trimmed` (its streams' buffers given back by the idle trim, so its stream takes new ones), what every established
// connection negotiated (`group`, `cipher`, `version`; `mixed` when two differed), `peak_live`, the most connections
// live at once (a client's started, a server's accepted, and not yet closed), a server's Initials it `refused`, and
// what the socket handed the host and took from it: `datagrams_sent` and `send_messages`, `datagrams_received` and
// `receive_messages`, a message being one datagram or one segmented or coalesced train (one buffer the host's stack
// carries whole on Linux), and the loop's `waits` for its next event, the `wait_timeouts` among them that no datagram
// ended, and the time they asked for (`wait_asked_ns`) and took (`wait_ns`): an end idle while the other works. Why a
// connection ended unestablished goes to standard error. The CPU time and memory are the driver's to measure.

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
constexpr u64 kMaxGapMs = 10000U; // a third of the idle timeout
// Live connections at most, closing ones included: a full server refuses a new one (CONNECTION_REFUSED), and each
// closing one holds its slot for three PTOs (RFC 9000 section 10.2), about a tenth of a second at the board's rates.
constexpr u32 kSlots = 4096U;
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
    bool hold = false;
    u64 bursts = 1U;
    u64 gap_ns = 0U;
};

// One live connection's place in the work, by its slot.
struct Work
{
    QuicHandle handle;
    u64 stream = 0U;     // the stream of the exchange under way: client-initiated bidirectional, 0 first
    bool opened = false; // a client opened it, a server saw it
    bool fin_read = false;
    bool fin_sent = false;
    bool done = false; // its work is done: closing, or held open
    bool closing = false;
    u64 sent = 0U;
    u64 exchanges = 0U;
    u64 due_ns = 0U;      // a client opens the next exchange's stream from then on
    u32 allocations = 0U; // the connection's buffer_allocations() when its second exchange ended
};

class Board final : public QuicServerEvents, public QuicClientEvents
{
public:
    explicit Board(const Options& options) noexcept : m_options(options) {}

    void on_accept(QuicHandle handle, QuicConnection&, const Endpoint&) noexcept override { open(handle); }
    void on_restart(QuicHandle, QuicConnection&) noexcept override {}

    void on_service(QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept override
    {
        Work& work = m_work[handle.slot];
        if (work.closing || connection.state() != QuicConnectionState::Established)
        {
            return;
        }
        QuicStreamSet& set = connection.streams();
        if (m_options.client && !work.opened && !work.done && now_ns >= work.due_ns)
        {
            // Refused while the server's MAX_STREAMS has not come back; its arrival serves the connection again. With
            // one stream at a time, a closed stream's slot holds no buffer only once the idle trim gave it back.
            const bool kept = set.buffer_bytes() != 0U;
            work.opened = set.open(true, work.stream) == QuicStreamError::None;
            m_trimmed += work.opened && work.exchanges != 0U && !kept ? 1U : 0U;
        }
        else if (!m_options.client)
        {
            work.opened = work.opened || set.recv_state(work.stream) != QuicRecvState::None;
        }
        if (work.opened)
        {
            serve(work, set);
        }
        // serve() reads and writes by state; the events are taken so an ended stream's slot closes.
        QuicStreamEvent event;
        while (set.poll(event))
        {
        }
        if (!work.done && (m_options.client ? work.fin_read : work.fin_sent))
        {
            next_exchange(handle, connection, work, now_ns);
        }
        if (m_options.client && work.done && (!m_options.hold || m_released) && !work.closing)
        {
            work.closing = true;
            connection.close(0U, {}, now_ns);
        }
    }

    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        count(connection);
        Work& work = m_work[handle.slot];
        m_allocations += work.exchanges >= 2U ? connection.buffer_allocations() - work.allocations : 0U;
        finish(work);
        ++m_closed;
        m_work[handle.slot] = {};
    }

    [[nodiscard]] bool done() const noexcept { return m_closed >= m_options.connections; }
    // Whether the loop has work before its next event: a connection to start, or a holding client's to close.
    [[nodiscard]] bool pending() const noexcept { return may_start() || release_due(); }
    // A holding client's connections have all done their work and stay open.
    [[nodiscard]] bool release_due() const noexcept
    {
        return m_options.hold && !m_released && m_finished >= m_options.connections;
    }
    // Whether a client starts another connection now: `parallel` are under way at most, closing ones (which hold their
    // slots for three PTOs, RFC 9000 section 10.2) not counted.
    [[nodiscard]] bool may_start() const noexcept
    {
        return m_options.client && m_started < m_options.connections && m_started - m_finished < m_options.parallel &&
               m_started - m_closed < kSlots;
    }
    void start(QuicHandle handle) noexcept
    {
        ++m_started;
        open(handle);
    }
    // When a client's next exchange is due (kNeverNs for none); take_due() hands its connection to wake once.
    [[nodiscard]] u64 due_ns() const noexcept { return m_due_ns; }
    [[nodiscard]] bool take_due(u64 now_ns, QuicHandle& handle) noexcept
    {
        if (now_ns < m_due_ns)
        {
            return false;
        }
        handle = m_due;
        m_due_ns = kNeverNs;
        return true;
    }
    // A holding client closes its connections: each is served in the host's next turn, and closes there.
    void release(QuicHost& host) noexcept
    {
        m_released = true;
        for (const Work& work : m_work)
        {
            if (work.done && !work.closing)
            {
                host.wake(work.handle);
            }
        }
    }

    void report() const noexcept
    {
        std::printf("connections=%llu\nestablished=%llu\nbytes_received=%llu\nbytes_sent=%llu\n",
                    static_cast<unsigned long long>(m_closed), static_cast<unsigned long long>(m_established),
                    static_cast<unsigned long long>(m_received), static_cast<unsigned long long>(m_sent));
        const char* const version = m_version == 1U ? "1" : "other";
        std::printf("group=%s\ncipher=%s\nversion=%s\npeak_live=%llu\n", m_mixed ? "mixed" : group_name(m_group),
                    m_mixed ? "mixed" : cipher_name(m_cipher), m_mixed ? "mixed" : version,
                    static_cast<unsigned long long>(m_peak_live));
        std::printf("exchanges=%llu\nallocations=%llu\ntrimmed=%llu\n", static_cast<unsigned long long>(m_exchanges),
                    static_cast<unsigned long long>(m_allocations), static_cast<unsigned long long>(m_trimmed));
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
            // Why a connection ended unestablished, on standard error for the driver's failure message.
            const QuicCloseInfo& close = connection.close_info();
            std::fprintf(stderr,
                         "unestablished: tls=%u local=%d application=%d idle=%d reset=%d code=%llu frame=%llu\n",
                         static_cast<unsigned>(state),close.local ? 1 : 0, close.application ? 1 : 0,
                         close.idle ? 1 : 0, close.stateless_reset ? 1 : 0,
                         static_cast<unsigned long long>(close.error_code),
                         static_cast<unsigned long long>(close.frame_type));
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

    // A connection a client started or a server accepted, live until on_closed.
    void open(QuicHandle handle) noexcept
    {
        m_work[handle.slot] = {};
        m_work[handle.slot].handle = handle;
        ++m_opened;
        const u64 live = m_opened - m_closed;
        m_peak_live = live > m_peak_live ? live : m_peak_live;
    }

    // Its place among the `parallel` under way is free.
    void finish(Work& work) noexcept
    {
        m_finished += work.done ? 0U : 1U;
        work.done = true;
    }

    // An exchange ended (a client's answer read, a server's stream answered): a client is done after `bursts` of them
    // or opens the next one's stream `gap` later, which the loop wakes it for; a server waits for the next stream.
    void next_exchange(QuicHandle handle, const QuicConnection& connection, Work& work, u64 now_ns) noexcept
    {
        work.allocations = work.exchanges == 1U ? connection.buffer_allocations() : work.allocations;
        ++work.exchanges;
        ++m_exchanges;
        if (m_options.client && work.exchanges >= m_options.bursts)
        {
            finish(work);
            return;
        }
        work.stream += 4U; // the next client-initiated bidirectional stream (RFC 9000 section 2.1)
        work.opened = false;
        work.fin_read = false;
        work.fin_sent = false;
        work.sent = 0U;
        if (m_options.client)
        {
            work.due_ns = now_ns + m_options.gap_ns;
            m_due_ns = work.due_ns;
            m_due = handle;
        }
    }

    // Reads the exchange's stream to its end, counting; a client writes its bytes and FIN, a server its FIN once the
    // client's came.
    void serve(Work& work, QuicStreamSet& set) noexcept
    {
        while (!work.fin_read)
        {
            const QuicStreamRead piece = set.peek(work.stream);
            const usize size = piece.data.size();
            if ((size == 0U && !piece.fin) || set.consume(work.stream, size) != QuicStreamError::None)
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
            if (set.write(work.stream, ConstSpan<u8>(kFill, want), want == left, taken) != QuicStreamError::None)
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
    u64 m_opened = 0U;
    u64 m_peak_live = 0U;
    u64 m_finished = 0U;
    u64 m_closed = 0U;
    u64 m_established = 0U;
    u64 m_received = 0U;
    u64 m_sent = 0U;
    u64 m_exchanges = 0U;
    u64 m_allocations = 0U;
    u64 m_trimmed = 0U;
    u64 m_due_ns = kNeverNs;
    QuicHandle m_due;
    AlgorithmId m_group{};
    AlgorithmId m_cipher{};
    u32 m_version = 0U;
    bool m_mixed = false;
    bool m_released = false; // a holding client closed its connections
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

// The loop's waits for its next event: how many, how many its timeout ended, the time they asked for and took.
struct Waits
{
    u64 count = 0U;
    u64 timeouts = 0U;
    u64 asked_ns = 0U;
    u64 taken_ns = 0U;
};

// Turns the loop until the board is done, starting a client's connections as earlier ones finish (and closing a holding
// client's once all have): a turn that freed a place starts the next connection before any wait.
template <typename Start>
int drive(const HostClock& clock, const SocketSet& sockets, IoLoop& loop, const Board& board, const Start& start,
          Waits& waits)
{
    const u64 deadline = clock.monotonic_ns() + kRunNs;
    while (!board.done())
    {
        if (!start(clock.monotonic_ns()))
        {
            return 1;
        }
        const u64 polled = loop.poll();
        const u64 next = polled < board.due_ns() ? polled : board.due_ns();
        const u64 now = clock.monotonic_ns();
        if (now >= deadline)
        {
            std::fputs("crd-quic-board: the run outlasted its deadline\n", stderr);
            return 1;
        }
        if (next > now && !board.pending())
        {
            const u64 asked = next - now < kMaxWaitNs ? next - now : kMaxWaitNs;
            const bool readable = sockets.wait_readable(asked);
            ++waits.count;
            waits.timeouts += readable ? 0U : 1U;
            waits.asked_ns += asked;
            waits.taken_ns += clock.monotonic_ns() - now;
        }
    }
    return 0;
}

// The loop's waits, then the socket's datagrams and messages each way: a train counts as one message of its datagrams.
void report_loop(const UdpSocket& socket, const IoLoop& loop, const Waits& waits) noexcept
{
    std::printf("waits=%llu\nwait_timeouts=%llu\nwait_asked_ns=%llu\nwait_ns=%llu\n",
                static_cast<unsigned long long>(waits.count), static_cast<unsigned long long>(waits.timeouts),
                static_cast<unsigned long long>(waits.asked_ns), static_cast<unsigned long long>(waits.taken_ns));
    const OffloadCounters& offloads = socket.offload_counters();
    const u64 sent = socket.sends_taken();
    const u64 received = loop.counters().received;
    const u64 sent_messages = sent - offloads.train_datagrams_sent + offloads.trains_sent;
    const u64 received_messages = received - offloads.train_datagrams_received + offloads.trains_received;
    std::printf("datagrams_sent=%llu\nsend_messages=%llu\ndatagrams_received=%llu\nreceive_messages=%llu\n",
                static_cast<unsigned long long>(sent), static_cast<unsigned long long>(sent_messages),
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(received_messages));
    std::fflush(stdout);
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
    Waits waits;
    const int result = drive(clock, sockets, loop, board, [](u64) { return true; }, waits);
    board.report();
    std::printf("refused=%llu\n", static_cast<unsigned long long>(server.counters().refused));
    report_loop(socket, loop, waits);
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
        if (board.release_due())
        {
            board.release(client);
        }
        QuicHandle due;
        if (board.take_due(now_ns, due))
        {
            client.wake(due);
        }
        return true;
    };
    Waits waits;
    const int result = drive(clock, sockets, loop, board, start, waits);
    board.report();
    report_loop(socket, loop, waits);
    return result;
}

int usage()
{
    std::fputs("usage: crd-quic-board --role client|server --port N --seed HEX --group NAME --connections N "
               "[--parallel N] [--bytes N] [--window N] [--offload 0|1] [--hold 0|1] [--bursts N --gap MS]\n",
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
        else if (std::strcmp(name, "--hold") == 0)
        {
            options.hold = number != 0U;
        }
        else if (std::strcmp(name, "--bursts") == 0)
        {
            options.bursts = number;
        }
        else if (std::strcmp(name, "--gap") == 0)
        {
            options.gap_ns = number <= kMaxGapMs ? number * 1000000U : kRunNs;
        }
        else
        {
            return false;
        }
    }
    return argc % 2 == 1 && role && seed && options.port <= 65535U && options.connections != 0U &&
           options.parallel != 0U && options.parallel <= kSlots && (!options.hold || options.connections <= kSlots) &&
           options.window >= 4096U && options.window <= (u64{1} << 26U) && (!options.client || options.port != 0U) &&
           options.bursts != 0U && (options.bursts == 1U || options.connections == 1U) && options.gap_ns < kRunNs;
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
