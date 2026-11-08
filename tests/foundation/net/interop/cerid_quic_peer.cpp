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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
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
constexpr usize kMaxConnections = 2U;
constexpr usize kMaxDatagrams = 4096U;
constexpr usize kMaxDatagramBytes = 1400U;
constexpr usize kDatagramIndexBytes = 4U;
constexpr u32 kDatagramRingBytes = 1U << 20U;
constexpr u32 kDatagramRingCount = 2048U;
constexpr u64 kDatagramSettleNs = 1000000000U; // no datagram for this long: those still out are lost
constexpr u64 kWakeNs = 20000000U;
constexpr u32 kServerSlots = 4U;
constexpr u8 kEarlyContext[4] = {'e', 'c', 't', 'x'};

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
    u64 datagrams = 0U;
    u64 datagram_bytes = 0U;
    u64 datagram_limit = kMaxDatagramBytes;
    bool datagram_probe = false;
    u64 connections = 1U;
    bool early = false;
    bool reject_early = false;
    bool only_version = false;
    u64 progress_at = 0U;
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

// A connection's application, either end's.
class Echo
{
public:
    // `tickets`: the client's store when a later connection resumes on this one's ticket, which it waits for.
    void start(const Options& options, u64 number, const TicketCache* tickets) noexcept
    {
        m_options = options;
        m_number = number;
        m_tickets = tickets;
    }

    void service(QuicConnection& connection, u64 now_ns) noexcept
    {
        if (m_closing)
        {
            return;
        }
        m_established = m_established || connection.state() == QuicConnectionState::Established;
        if (m_first_service_ns == 0U)
        {
            m_first_service_ns = now_ns;
        }
        forget_rejected_early(connection);
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
        if (m_options.client && (connection.state() == QuicConnectionState::Established ||
                                 connection.handshake().early_data() == TlsEarlyData::Offered))
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
        service_datagrams(connection.datagrams(), now_ns);
        if (m_options.progress_at != 0U && !m_progressed && m_received >= m_options.progress_at)
        {
            m_progressed = true;
            std::puts("progress=1");
            std::fflush(stdout);
        }
        if (m_options.client_closes == m_options.client && complete(connection.datagrams(), now_ns))
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
        const StringView early = tls_early_data_name(connection.handshake().early_data());
        std::printf("connection=%llu\n", static_cast<unsigned long long>(m_number));
        std::printf("established=%d\nversion=%u\nstreams=%llu\nbytes_received=%llu\nbytes_sent=%llu\nverified=%d\n",
                    m_established ? 1 : 0, connection.version() == QuicVersion::V2 ? 2U : 1U,
                    static_cast<unsigned long long>(m_options.client ? m_opened : m_seen),
                    static_cast<unsigned long long>(m_received), static_cast<unsigned long long>(m_sent),
                    m_verified ? 1 : 0);
        std::printf("close=%s\nclose_code=%llu\n", kind, static_cast<unsigned long long>(info.error_code));
        std::printf("resumed=%d\nearly=%.*s\nearly_bytes=%llu\nearly_datagrams=%llu\n",
                    connection.handshake().resumed() ? 1 : 0, static_cast<int>(early.size()), early.data(),
                    static_cast<unsigned long long>(m_early_bytes), static_cast<unsigned long long>(m_early_datagrams));
        std::printf("datagrams_sent=%llu\ndatagrams_received=%llu\ndatagrams_acked=%llu\ndatagrams_verified=%d\n"
                    "datagrams_refused=%d\n",
                    static_cast<unsigned long long>(connection.datagrams().counters().sent),
                    static_cast<unsigned long long>(m_datagrams_received),
                    static_cast<unsigned long long>(m_datagrams_acked), m_datagrams_verified ? 1 : 0,
                    m_datagram_refused ? 1 : 0);
        std::fflush(stdout);
    }

private:
    // RFC 9001 section 4.6.2: a server's refusal of 0-RTT took the client's streams and datagrams, which start again.
    void forget_rejected_early(const QuicConnection& connection) noexcept
    {
        if (!m_options.client || m_forgot_early || connection.handshake().early_data() != TlsEarlyData::Rejected)
        {
            return;
        }
        m_forgot_early = true;
        for (u64 index = 0U; index < m_opened; ++index)
        {
            m_streams[index] = Stream{};
        }
        m_opened = 0U;
        m_received = 0U;
        m_sent = 0U;
        m_datagrams_sent = 0U;
        m_datagrams_received = 0U;
        m_datagrams_acked = 0U;
        std::memset(m_datagrams_seen, 0, sizeof(m_datagrams_seen));
    }

    // Offers datagram `index` in `tag`'s pattern: false when the queue is full and it should be offered again.
    [[nodiscard]] bool queue_datagram(QuicDatagrams& set, u64 index, u64 tag, u64 now_ns) noexcept
    {
        u8 payload[kMaxDatagramBytes];
        const usize size = static_cast<usize>(m_options.datagram_bytes);
        for (usize at = 0U; at < kDatagramIndexBytes; ++at)
        {
            payload[at] = static_cast<u8>(index >> (at * 8U));
        }
        for (usize at = kDatagramIndexBytes; at < size; ++at)
        {
            payload[at] = pattern(tag, index, at);
        }
        u64 id = 0U;
        const QuicDatagramError error = set.queue(ConstSpan<u8>(payload, size), now_ns, id);
        if (error != QuicDatagramError::None && error != QuicDatagramError::Full)
        {
            m_datagrams_verified = false;
        }
        return error != QuicDatagramError::Full;
    }

    [[nodiscard]] static u64 datagram_index(ConstSpan<u8> payload) noexcept
    {
        u64 index = 0U;
        for (usize at = 0U; at < kDatagramIndexBytes && at < payload.size(); ++at)
        {
            index |= static_cast<u64>(payload[at]) << (at * 8U);
        }
        return index;
    }

    // Holds a received payload to its pattern, its size and its one delivery.
    void check_datagram(ConstSpan<u8> payload, u64 index, u64 tag) noexcept
    {
        if (payload.size() != m_options.datagram_bytes || index >= m_options.datagrams ||
            (m_datagrams_seen[index / 8U] & (1U << (index % 8U))) != 0U)
        {
            m_datagrams_verified = false;
            return;
        }
        m_datagrams_seen[index / 8U] = static_cast<u8>(m_datagrams_seen[index / 8U] | (1U << (index % 8U)));
        for (usize at = kDatagramIndexBytes; at < payload.size(); ++at)
        {
            m_datagrams_verified = m_datagrams_verified && payload[at] == pattern(tag, index, at);
        }
    }

    void service_datagrams(QuicDatagrams& set, u64 now_ns) noexcept
    {
        if (m_options.datagrams == 0U)
        {
            return;
        }
        QuicDatagramReport report;
        while (set.poll(report))
        {
            m_datagrams_acked += report.outcome == QuicDatagramOutcome::Acked ? 1U : 0U;
        }
        if (m_options.datagram_probe && !m_probed && set.sendable())
        {
            m_probed = true;
            static const u8 kOver[kMaxDatagramBytes] = {};
            u64 id = 0U;
            m_datagram_refused = set.queue(ConstSpan<u8>(kOver, static_cast<usize>(m_options.datagram_bytes) + 1U),
                                           now_ns, id) == QuicDatagramError::TooLarge;
        }
        const u64 tag = m_options.client ? 3U : 2U;
        ConstSpan<u8> payload;
        while (set.peek(payload))
        {
            const u64 index = datagram_index(payload);
            if (!m_options.client && !queue_datagram(set, index, 3U, now_ns))
            {
                break;
            }
            check_datagram(payload, index, tag);
            ++m_datagrams_received;
            m_early_datagrams += set.oldest_early() ? 1U : 0U;
            m_last_datagram_ns = now_ns;
            set.pop();
        }
        while (m_options.client && m_datagrams_sent < m_options.datagrams && set.sendable() &&
               queue_datagram(set, m_datagrams_sent, 2U, now_ns))
        {
            ++m_datagrams_sent;
            m_last_datagram_ns = now_ns;
        }
    }

    [[nodiscard]] bool datagrams_settled(const QuicDatagrams& set, u64 now_ns) const noexcept
    {
        if (m_options.datagrams == 0U)
        {
            return true;
        }
        if (set.queued() != 0U || (m_options.client && m_datagrams_sent < m_options.datagrams))
        {
            return false;
        }
        const u64 since = m_last_datagram_ns != 0U ? m_last_datagram_ns : m_first_service_ns;
        return m_datagrams_received >= m_options.datagrams || now_ns - since >= kDatagramSettleNs;
    }

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
            m_early_bytes += piece.early_bytes;
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

    [[nodiscard]] bool complete(const QuicDatagrams& datagrams, u64 now_ns) const noexcept
    {
        if (!datagrams_settled(datagrams, now_ns) || (m_tickets != nullptr && m_tickets->size() == 0U))
        {
            return false;
        }
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
    const TicketCache* m_tickets = nullptr;
    u64 m_number = 0U;
    Stream m_streams[kMaxStreams];
    u64 m_opened = 0U;
    u64 m_seen = 0U;
    u64 m_finished = 0U;
    u64 m_received = 0U;
    u64 m_sent = 0U;
    u64 m_early_bytes = 0U;
    u64 m_early_datagrams = 0U;
    u64 m_datagrams_sent = 0U;
    u64 m_datagrams_received = 0U;
    u64 m_datagrams_acked = 0U;
    u64 m_first_service_ns = 0U;
    u64 m_last_datagram_ns = 0U;
    u8 m_datagrams_seen[kMaxDatagrams / 8U] = {};
    bool m_established = false;
    bool m_verified = true;
    bool m_datagrams_verified = true;
    bool m_datagram_refused = false;
    bool m_probed = false;
    bool m_forgot_early = false;
    bool m_progressed = false;
    bool m_closing = false;
};

// A run's connections, in the order they started.
class Peer
{
public:
    explicit Peer(const Options& options) noexcept : m_options(options) {}

    void started(QuicHandle handle, const TicketCache* tickets) noexcept
    {
        if (m_started < m_options.connections && handle.slot < kServerSlots)
        {
            m_echo[m_started].start(m_options, m_started + 1U, tickets);
            m_handles[m_started] = handle;
            m_order[handle.slot] = m_started;
            ++m_started;
        }
    }

    void service(QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept
    {
        Echo* const echo = echo_of(handle);
        if (echo != nullptr)
        {
            echo->service(connection, now_ns);
        }
    }

    void closed(QuicHandle handle, const QuicConnection& connection) noexcept
    {
        Echo* const echo = echo_of(handle);
        if (echo != nullptr)
        {
            echo->closed(connection);
            ++m_closed;
            m_handles[m_order[handle.slot]] = QuicHandle{};
        }
    }

    // Datagrams wait for nothing the connection knows of, so its application is served on a timer.
    void wake(QuicHost& host, u64 now_ns) noexcept
    {
        if (m_options.datagrams == 0U || now_ns < m_next_wake_ns)
        {
            return;
        }
        m_next_wake_ns = now_ns + kWakeNs;
        for (u64 index = 0U; index < m_started; ++index)
        {
            if (m_handles[index].valid())
            {
                host.wake(m_handles[index]);
            }
        }
    }

    [[nodiscard]] u64 count() const noexcept { return m_started; }
    [[nodiscard]] bool idle() const noexcept { return m_closed == m_started; }
    [[nodiscard]] bool done() const noexcept { return m_closed >= m_options.connections; }

private:
    [[nodiscard]] Echo* echo_of(QuicHandle handle) noexcept
    {
        return handle.slot < kServerSlots && m_handles[m_order[handle.slot]] == handle ? &m_echo[m_order[handle.slot]]
                                                                                       : nullptr;
    }

    Options m_options;
    Echo m_echo[kMaxConnections];
    QuicHandle m_handles[kMaxConnections];
    u64 m_order[kServerSlots] = {};
    u64 m_started = 0U;
    u64 m_closed = 0U;
    u64 m_next_wake_ns = 0U;
};

class ServerApp final : public QuicServerEvents
{
public:
    // `context`: the application's part of the early data context, changed by every connection when it is set.
    ServerApp(Peer& peer, Span<u8> context) noexcept : m_peer(peer), m_context(context) {}
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
        m_peer.service(handle, connection, now_ns);
    }
    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        m_peer.closed(handle, connection);
    }

private:
    Peer& m_peer;
    Span<u8> m_context;
};

class ClientApp final : public QuicClientEvents
{
public:
    explicit ClientApp(Peer& peer) noexcept : m_peer(peer) {}
    void on_restart(QuicHandle, QuicConnection&) noexcept override {}
    void on_service(QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept override
    {
        m_peer.service(handle, connection, now_ns);
    }
    void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept override
    {
        m_peer.closed(handle, connection);
    }

private:
    Peer& m_peer;
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
int drive(const HostClock& clock, DatagramSocket& socket, IoLoop& loop, QuicHost& host, Peer& peer, Turn&& turn)
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
    static Peer peer(options);
    static u8 early_context[sizeof(kEarlyContext)];
    std::memcpy(early_context, kEarlyContext, sizeof(kEarlyContext));
    ServerApp app(peer, options.reject_early ? Span<u8>(early_context) : Span<u8>());
    QuicServerConfig config;
    config.connection = connection_config(options, context);
    config.connection.versions = options.only_version ? ConstSpan<u32>(only)
                                 : options.compat     ? ConstSpan<u32>(kPreferV2)
                                                      : ConstSpan<u32>(kBothVersions);
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
    static Peer peer(options);
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
        else if (std::strcmp(name, "--datagrams") == 0)
        {
            options.datagrams = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--datagram-bytes") == 0)
        {
            options.datagram_bytes = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--datagram-limit") == 0)
        {
            options.datagram_limit = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--datagram-probe") == 0)
        {
            options.datagram_probe = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--connections") == 0)
        {
            options.connections = std::strtoull(value, nullptr, 10);
        }
        else if (std::strcmp(name, "--early") == 0)
        {
            options.early = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--reject-early") == 0)
        {
            options.reject_early = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--only-version") == 0)
        {
            options.only_version = std::strcmp(value, "0") != 0;
        }
        else if (std::strcmp(name, "--progress-at") == 0)
        {
            options.progress_at = std::strtoull(value, nullptr, 10);
        }
        else
        {
            return usage();
        }
    }
    if (argc % 2 == 0 || !role || !seed || !group || !cipher || (options.version != 1U && options.version != 2U) ||
        options.streams > kMaxStreams || options.connections == 0U || options.connections > kMaxConnections ||
        options.datagrams > kMaxDatagrams || options.datagram_bytes + 1U > kMaxDatagramBytes ||
        (options.datagrams != 0U && options.datagram_bytes < kDatagramIndexBytes))
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
