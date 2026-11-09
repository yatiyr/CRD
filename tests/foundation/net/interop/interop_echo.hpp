#pragma once

// crd-net interoperability tests (4.2a) -- the application crd-quic-peer and crd-qmux-peer both run over a connection's
// QuicStreamSet and QuicDatagrams, and what the two programs parse alike.
//
//   bidirectional  a client opens `streams` streams and writes `bytes` bytes of a pattern with a FIN; a server writes
//                  back as many bytes of another pattern as it reads, and its FIN after the client's.
//   unidirectional each end opens `uni` streams of `uni-bytes` bytes and a FIN, then `resets` streams it writes and
//                  resets (code kInteropResetCode), then `stops` streams it writes until its peer asks it to stop (code
//                  kInteropStopCode).
//   datagrams      a client sends `datagrams` of `datagram-bytes` each, the index in four bytes and a pattern; a server
//                  sends each back in another pattern.
//
// Both ends check every byte. The report of each connection, one `name=value` per line from `connection=N`, goes to
// standard output when it has ended.

#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/host_handle.hpp>
#include <crd/net/quic_datagram.hpp>
#include <crd/net/quic_stream_set.hpp>
#include <crd/net/ticket_cache.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace crd_net_interop
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::StringView;

inline constexpr usize kMaxStreams = 4096U;
inline constexpr usize kMaxUniStreams = 64U;
inline constexpr usize kMaxConnections = 2U;
inline constexpr usize kMaxDatagrams = 4096U;
inline constexpr usize kMaxDatagramBytes = 1400U;
inline constexpr usize kDatagramIndexBytes = 4U;
inline constexpr u64 kDatagramSettleNs = 1000000000U; // no datagram for this long: those still out are lost
inline constexpr u64 kWakeNs = 20000000U;
inline constexpr u32 kServerSlots = 4U;
inline constexpr u64 kInteropResetCode = 7U;
inline constexpr u64 kInteropStopCode = 9U;
inline constexpr u64 kResetStreamBytes = 4096U;

struct EchoOptions
{
    bool client = false;
    bool client_closes = true;
    bool never_closes = false; // neither end closes: the connection ends some other way
    u64 port = 0U;
    u64 streams = 0U;
    u64 bytes = 0U;
    u64 code = 0U;
    u64 max_streams = 16U;
    u64 uni = 0U;
    u64 uni_bytes = 0U;
    u64 resets = 0U;
    u64 stops = 0U;
    u64 datagrams = 0U;
    u64 datagram_bytes = 0U;
    u64 datagram_limit = kMaxDatagramBytes;
    bool datagram_probe = false;
    u64 connections = 1U;
    bool early = false;
    u64 progress_at = 0U;
    u64 hold_ns = 0U; // this long after everything is done before the closing end closes
    bool timed_service = false; // the application is served on the timer whatever else happens
    crd::crypto::AlgorithmId group{};
    crd::crypto::AlgorithmId cipher{};
    u8 seed[32] = {};
};

// The byte at `offset` of a stream's direction: tag 0 and 1 a bidirectional stream's client and server direction, 4 and
// 5 a client's and a server's unidirectional stream. `stream` is its ID.
inline u8 pattern(u64 tag, u64 stream, u64 offset)
{
    const u64 at = (offset + (stream * 37U)) & 0xFFFFU;
    return static_cast<u8>((at * 31U) + (at >> 8U) + (tag * 7U));
}

inline bool parse_group(const char* name, crd::crypto::AlgorithmId& out)
{
    using crd::crypto::AlgorithmId;
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

inline bool parse_cipher(const char* name, crd::crypto::AlgorithmId& out)
{
    using crd::crypto::AlgorithmId;
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

[[nodiscard]] inline int hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return digit - 'a' + 10;
    }
    return -1;
}

inline bool parse_seed(const char* text, u8* out)
{
    if (std::strlen(text) != 64U)
    {
        return false;
    }
    for (usize at = 0U; at < 32U; ++at)
    {
        const int high = hex_digit(text[at * 2U]);
        const int low = hex_digit(text[(at * 2U) + 1U]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        out[at] = static_cast<u8>((high * 16) + low);
    }
    return true;
}

// The options both programs share. `known` tells whether `name` was one of them, and the required ones are counted in
// `required` (the role, the seed, the group and the cipher).
inline bool parse_echo_option(const char* name, const char* value, EchoOptions& options, u32& required)
{
    const u64 number = std::strtoull(value, nullptr, 10);
    const bool flag = std::strcmp(value, "0") != 0;
    const struct
    {
        const char* name;
        u64* field;
    } kNumbers[] = {{"--port", &options.port},
                    {"--streams", &options.streams},
                    {"--bytes", &options.bytes},
                    {"--code", &options.code},
                    {"--max-streams", &options.max_streams},
                    {"--uni", &options.uni},
                    {"--uni-bytes", &options.uni_bytes},
                    {"--resets", &options.resets},
                    {"--stops", &options.stops},
                    {"--datagrams", &options.datagrams},
                    {"--datagram-bytes", &options.datagram_bytes},
                    {"--datagram-limit", &options.datagram_limit},
                    {"--connections", &options.connections},
                    {"--progress-at", &options.progress_at}};
    for (const auto& entry : kNumbers)
    {
        if (std::strcmp(name, entry.name) == 0)
        {
            *entry.field = number;
            return true;
        }
    }
    if (std::strcmp(name, "--role") == 0)
    {
        options.client = std::strcmp(value, "client") == 0;
        ++required;
    }
    else if (std::strcmp(name, "--seed") == 0)
    {
        required += parse_seed(value, options.seed) ? 1U : 0U;
    }
    else if (std::strcmp(name, "--group") == 0)
    {
        required += parse_group(value, options.group) ? 1U : 0U;
    }
    else if (std::strcmp(name, "--cipher") == 0)
    {
        required += parse_cipher(value, options.cipher) ? 1U : 0U;
    }
    else if (std::strcmp(name, "--closer") == 0)
    {
        options.client_closes = std::strcmp(value, "client") == 0;
        options.never_closes = std::strcmp(value, "none") == 0;
    }
    else if (std::strcmp(name, "--datagram-probe") == 0)
    {
        options.datagram_probe = flag;
    }
    else if (std::strcmp(name, "--early") == 0)
    {
        options.early = flag;
    }
    else if (std::strcmp(name, "--hold-ms") == 0)
    {
        options.hold_ns = number * 1000000U;
    }
    else
    {
        return false;
    }
    return true;
}

inline constexpr u32 kRequiredEchoOptions = 4U;

// Whether the shared options hold together.
[[nodiscard]] inline bool echo_options_valid(const EchoOptions& options)
{
    return options.streams <= kMaxStreams && options.uni + options.resets + options.stops <= kMaxUniStreams &&
           options.connections != 0U && options.connections <= kMaxConnections && options.datagrams <= kMaxDatagrams &&
           options.datagram_bytes + 1U <= kMaxDatagramBytes &&
           (options.datagrams == 0U || options.datagram_bytes >= kDatagramIndexBytes);
}

struct EchoStream
{
    u64 received = 0U;
    u64 sent = 0U;
    bool in_end = false;
    bool out_end = false;
    bool finished = false; // every byte and the FIN acknowledged
    bool stop_sent = false;
    bool wrote_all = false; // of a stream to reset: a whole turn has passed since its last byte was written
};

// What one turn of a connection's service knows of the connection beyond its streams and datagrams.
struct EchoTurn
{
    bool established = false; // the handshake has finished
    bool can_open = false;    // a client's streams may be written now (established, or under 0-RTT)
    crd::crypto::TlsEarlyData early_data{};
    u64 now_ns = 0U;
};

// A connection's application, either end's.
class Echo
{
public:
    // `tickets`: the client's store when a later connection resumes on this one's ticket, which it waits for.
    void start(const EchoOptions& options, u64 number, const crd::net::TicketCache* tickets) noexcept
    {
        m_options = options;
        m_number = number;
        m_tickets = tickets;
    }

    // Serves the connection's streams and datagrams. True once, when this end should close it now.
    [[nodiscard]] bool service(crd::net::QuicStreamSet& set, crd::net::QuicDatagrams& datagrams,
                               const EchoTurn& turn) noexcept
    {
        if (m_closing)
        {
            return false;
        }
        m_established = m_established || turn.established;
        if (m_first_service_ns == 0U)
        {
            m_first_service_ns = turn.now_ns;
        }
        forget_rejected_early(turn.early_data);
        poll_events(set);
        if (m_options.client && turn.can_open)
        {
            open_streams(set);
        }
        const u64 range = m_options.client ? m_opened : kMaxStreams;
        for (u64 index = 0U; index < range; ++index)
        {
            const u64 id = index * 4U;
            if (!m_options.client && set.recv_state(id) == crd::net::QuicRecvState::None)
            {
                continue;
            }
            EchoStream& stream = m_streams[index];
            read(set, id, stream);
            const u64 goal = m_options.client ? m_options.bytes : stream.received;
            if (!stream.out_end)
            {
                write(set, id, stream, m_options.client ? 0U : 1U, goal, m_options.client || stream.in_end);
            }
        }
        if (turn.established)
        {
            service_unidirectional(set);
        }
        service_datagrams(datagrams, turn.now_ns);
        if (m_options.progress_at != 0U && !m_progressed && m_received >= m_options.progress_at)
        {
            m_progressed = true;
            std::puts("progress=1");
            std::fflush(stdout);
        }
        if (!m_established || m_options.never_closes || m_options.client_closes != m_options.client ||
            !complete(datagrams, turn.now_ns))
        {
            return false;
        }
        if (m_complete_ns == 0U)
        {
            m_complete_ns = turn.now_ns;
        }
        if (turn.now_ns - m_complete_ns < m_options.hold_ns)
        {
            return false;
        }
        m_closing = true;
        return true;
    }

    [[nodiscard]] const crd::net::TicketCache* tickets() const noexcept { return m_tickets; }

    // For another application over the connection, which reports through this one.
    void note_established(bool established) noexcept { m_established = m_established || established; }

    // The shared part of the report: `connection=N` and what the application counted.
    void report(bool resumed, crd::crypto::TlsEarlyData early_data,
                const crd::net::QuicDatagrams& datagrams) const noexcept
    {
        const StringView early = crd::crypto::tls_early_data_name(early_data);
        std::printf("connection=%llu\n", ull(m_number));
        std::printf("established=%d\nstreams=%llu\nbytes_received=%llu\nbytes_sent=%llu\nverified=%d\n",
                    m_established ? 1 : 0, ull(m_options.client ? m_opened : m_seen), ull(m_received), ull(m_sent),
                    m_verified ? 1 : 0);
        std::printf("uni_received=%llu\nuni_finished=%llu\nresets_seen=%llu\nreset_code=%llu\nstops_seen=%llu\n"
                    "stop_code=%llu\n",
                    ull(m_uni_received), ull(m_uni_finished), ull(m_resets_seen), ull(m_reset_code), ull(m_stops_seen),
                    ull(m_stop_code));
        std::printf("resumed=%d\nearly=%.*s\nearly_bytes=%llu\nearly_datagrams=%llu\n", resumed ? 1 : 0,
                    static_cast<int>(early.size()), early.data(), ull(m_early_bytes), ull(m_early_datagrams));
        std::printf("datagrams_sent=%llu\ndatagrams_received=%llu\ndatagrams_acked=%llu\ndatagrams_verified=%d\n"
                    "datagrams_refused=%d\n",
                    ull(datagrams.counters().sent), ull(m_datagrams_received), ull(m_datagrams_acked),
                    m_datagrams_verified ? 1 : 0, m_datagram_refused ? 1 : 0);
    }

private:
    [[nodiscard]] static unsigned long long ull(u64 value) noexcept { return value; }

    [[nodiscard]] u64 unidirectional_total() const noexcept
    {
        return m_options.uni + m_options.resets + m_options.stops;
    }

    void poll_events(crd::net::QuicStreamSet& set) noexcept
    {
        crd::net::QuicStreamEvent event;
        while (set.poll(event))
        {
            const u64 index = event.id / 4U;
            if ((event.id & 2U) != 0U)
            {
                unidirectional_event(event, index);
            }
            else if ((event.events & crd::net::kQuicStreamFinished) != 0U && index < kMaxStreams &&
                     !m_streams[index].finished)
            {
                m_streams[index].finished = true;
                ++m_finished;
            }
        }
    }

    void unidirectional_event(const crd::net::QuicStreamEvent& event, u64 index) noexcept
    {
        if (index >= kMaxUniStreams)
        {
            return;
        }
        const bool ours = ((event.id & 1U) != 0U) != m_options.client;
        if (ours && (event.events & crd::net::kQuicStreamFinished) != 0U && index < m_options.uni)
        {
            ++m_uni_finished;
        }
        if (ours && (event.events & crd::net::kQuicStreamStopSending) != 0U &&
            index >= m_options.uni + m_options.resets && !m_uni_out[index].out_end)
        {
            m_uni_out[index].out_end = true;
            ++m_stops_seen;
            m_stop_code = event.stop_error;
        }
        if (!ours && (event.events & crd::net::kQuicStreamReset) != 0U && index >= m_options.uni &&
            index < m_options.uni + m_options.resets && !m_uni_in[index].in_end)
        {
            m_uni_in[index].in_end = true;
            ++m_resets_seen;
            m_reset_code = event.reset_error;
        }
    }

    // Each end's own streams in index order, then its peer's, whose index says what becomes of them.
    void service_unidirectional(crd::net::QuicStreamSet& set) noexcept
    {
        const u64 total = unidirectional_total();
        while (m_uni_opened < total)
        {
            u64 id = 0U;
            if (set.open(false, id) != crd::net::QuicStreamError::None)
            {
                break;
            }
            ++m_uni_opened;
        }
        const u64 own = m_options.client ? 2U : 3U;
        const u64 own_tag = m_options.client ? 4U : 5U;
        for (u64 index = 0U; index < m_uni_opened; ++index)
        {
            write_unidirectional(set, (index * 4U) + own, own_tag, index);
        }
        const u64 peer = m_options.client ? 3U : 2U;
        const u64 peer_tag = m_options.client ? 5U : 4U;
        for (u64 index = 0U; index < total; ++index)
        {
            read_unidirectional(set, (index * 4U) + peer, peer_tag, index);
        }
    }

    void write_unidirectional(crd::net::QuicStreamSet& set, u64 id, u64 tag, u64 index) noexcept
    {
        EchoStream& stream = m_uni_out[index];
        if (stream.out_end)
        {
            return;
        }
        if (index < m_options.uni)
        {
            write(set, id, stream, tag, m_options.uni_bytes, true);
        }
        else if (index < m_options.uni + m_options.resets)
        {
            // The reset waits a turn so the bytes go first: a stream whose first frame is its RESET_STREAM may never
            // reach the peer's application.
            if (stream.wrote_all && set.reset(id, kInteropResetCode) == crd::net::QuicStreamError::None)
            {
                stream.out_end = true;
            }
            write(set, id, stream, tag, kResetStreamBytes, false);
            stream.wrote_all = stream.sent == kResetStreamBytes;
        }
        else
        {
            write(set, id, stream, tag, stream.sent + kResetStreamBytes, false);
        }
    }

    void read_unidirectional(crd::net::QuicStreamSet& set, u64 id, u64 tag, u64 index) noexcept
    {
        EchoStream& stream = m_uni_in[index];
        if (stream.in_end || set.recv_state(id) == crd::net::QuicRecvState::None)
        {
            return;
        }
        for (;;)
        {
            const crd::net::QuicStreamRead piece = set.peek(id);
            if (piece.reset)
            {
                return;
            }
            for (usize at = 0U; at < piece.data.size(); ++at)
            {
                m_verified = m_verified && piece.data[at] == pattern(tag, id, stream.received + at);
            }
            const usize size = piece.data.size();
            const bool fin = piece.fin;
            if (set.consume(id, size) != crd::net::QuicStreamError::None)
            {
                m_verified = false;
                return;
            }
            stream.received += size;
            m_received += size;
            if (fin)
            {
                stream.in_end = true;
                m_uni_received += index < m_options.uni ? 1U : 0U;
                return;
            }
            if (index >= m_options.uni + m_options.resets && stream.received != 0U && !stream.stop_sent)
            {
                stream.stop_sent = set.stop_sending(id, kInteropStopCode) == crd::net::QuicStreamError::None;
                stream.in_end = stream.stop_sent;
                m_stops_sent += stream.stop_sent ? 1U : 0U;
                return;
            }
            if (size == 0U)
            {
                return;
            }
        }
    }

    // RFC 9001 section 4.6.2: a server's refusal of 0-RTT took the client's streams and datagrams, which start again.
    void forget_rejected_early(crd::crypto::TlsEarlyData early_data) noexcept
    {
        if (!m_options.client || m_forgot_early || early_data != crd::crypto::TlsEarlyData::Rejected)
        {
            return;
        }
        m_forgot_early = true;
        for (u64 index = 0U; index < m_opened; ++index)
        {
            m_streams[index] = EchoStream{};
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
    [[nodiscard]] bool queue_datagram(crd::net::QuicDatagrams& set, u64 index, u64 tag, u64 now_ns) noexcept
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
        const crd::net::QuicDatagramError error = set.queue(ConstSpan<u8>(payload, size), now_ns, id);
        if (error != crd::net::QuicDatagramError::None && error != crd::net::QuicDatagramError::Full)
        {
            m_datagrams_verified = false;
        }
        return error != crd::net::QuicDatagramError::Full;
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

    void service_datagrams(crd::net::QuicDatagrams& set, u64 now_ns) noexcept
    {
        if (m_options.datagrams == 0U)
        {
            return;
        }
        crd::net::QuicDatagramReport report;
        while (set.poll(report))
        {
            m_datagrams_acked += report.outcome == crd::net::QuicDatagramOutcome::Acked ? 1U : 0U;
        }
        if (m_options.datagram_probe && !m_probed && set.sendable())
        {
            m_probed = true;
            static const u8 kOver[kMaxDatagramBytes] = {};
            u64 id = 0U;
            m_datagram_refused = set.queue(ConstSpan<u8>(kOver, static_cast<usize>(m_options.datagram_bytes) + 1U),
                                           now_ns, id) == crd::net::QuicDatagramError::TooLarge;
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

    [[nodiscard]] bool datagrams_settled(const crd::net::QuicDatagrams& set, u64 now_ns) const noexcept
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

    void open_streams(crd::net::QuicStreamSet& set) noexcept
    {
        while (m_opened < m_options.streams)
        {
            u64 id = 0U;
            if (set.open(true, id) != crd::net::QuicStreamError::None)
            {
                return;
            }
            ++m_opened;
        }
    }

    void read(crd::net::QuicStreamSet& set, u64 id, EchoStream& stream) noexcept
    {
        if (stream.in_end)
        {
            return;
        }
        const u64 tag = m_options.client ? 1U : 0U;
        for (;;)
        {
            const crd::net::QuicStreamRead piece = set.peek(id);
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
            if (set.consume(id, size) != crd::net::QuicStreamError::None)
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

    void write(crd::net::QuicStreamSet& set, u64 id, EchoStream& stream, u64 tag, u64 goal, bool fin) noexcept
    {
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
            const crd::net::QuicStreamError error = set.write(id, ConstSpan<u8>(chunk, want), last, taken);
            if (error != crd::net::QuicStreamError::None)
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

    [[nodiscard]] bool unidirectional_complete() const noexcept
    {
        return m_uni_opened == unidirectional_total() && m_uni_received == m_options.uni &&
               m_uni_finished == m_options.uni && m_resets_seen == m_options.resets &&
               m_stops_seen == m_options.stops && m_stops_sent == m_options.stops;
    }

    [[nodiscard]] bool complete(const crd::net::QuicDatagrams& datagrams, u64 now_ns) const noexcept
    {
        if (!datagrams_settled(datagrams, now_ns) || !unidirectional_complete() ||
            (m_tickets != nullptr && m_tickets->size() == 0U))
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
            const EchoStream& stream = m_streams[index];
            if (!stream.in_end || !stream.out_end || stream.received != m_options.bytes)
            {
                return false;
            }
        }
        return true;
    }

    EchoOptions m_options;
    const crd::net::TicketCache* m_tickets = nullptr;
    u64 m_number = 0U;
    EchoStream m_streams[kMaxStreams];
    EchoStream m_uni_out[kMaxUniStreams];
    EchoStream m_uni_in[kMaxUniStreams];
    u64 m_opened = 0U;
    u64 m_seen = 0U;
    u64 m_finished = 0U;
    u64 m_received = 0U;
    u64 m_sent = 0U;
    u64 m_uni_opened = 0U;
    u64 m_uni_received = 0U;
    u64 m_uni_finished = 0U;
    u64 m_resets_seen = 0U;
    u64 m_reset_code = 0U;
    u64 m_stops_seen = 0U;
    u64 m_stops_sent = 0U;
    u64 m_stop_code = 0U;
    u64 m_early_bytes = 0U;
    u64 m_early_datagrams = 0U;
    u64 m_datagrams_sent = 0U;
    u64 m_datagrams_received = 0U;
    u64 m_datagrams_acked = 0U;
    u64 m_first_service_ns = 0U;
    u64 m_last_datagram_ns = 0U;
    u64 m_complete_ns = 0U;
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
class EchoPeer
{
public:
    explicit EchoPeer(const EchoOptions& options) noexcept : m_options(options) {}

    void started(crd::net::QuicHandle handle, const crd::net::TicketCache* tickets) noexcept
    {
        if (m_started < m_options.connections && handle.slot < kServerSlots)
        {
            m_echo[m_started].start(m_options, m_started + 1U, tickets);
            m_handles[m_started] = handle;
            m_order[handle.slot] = m_started;
            ++m_started;
        }
    }

    // The connection's place in the order they started, for a live connection.
    [[nodiscard]] u64 index_of(crd::net::QuicHandle handle) const noexcept { return m_order[handle.slot]; }

    // The application of a live connection; null for one this run did not start.
    [[nodiscard]] Echo* echo_of(crd::net::QuicHandle handle) noexcept
    {
        return handle.slot < kServerSlots && m_handles[m_order[handle.slot]] == handle ? &m_echo[m_order[handle.slot]]
                                                                                       : nullptr;
    }

    void ended(crd::net::QuicHandle handle) noexcept
    {
        ++m_closed;
        m_handles[m_order[handle.slot]] = crd::net::QuicHandle{};
    }

    // Datagrams, a hold and a reset wait for nothing the connection knows of, so its application is served on a timer.
    template <typename Host>
    void wake(Host& host, u64 now_ns) noexcept
    {
        if ((m_options.datagrams == 0U && m_options.hold_ns == 0U && m_options.resets == 0U &&
             !m_options.timed_service) ||
            now_ns < m_next_wake_ns)
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

    [[nodiscard]] const EchoOptions& options() const noexcept { return m_options; }
    [[nodiscard]] u64 count() const noexcept { return m_started; }
    [[nodiscard]] bool idle() const noexcept { return m_closed == m_started; }
    [[nodiscard]] bool done() const noexcept { return m_closed >= m_options.connections; }

private:
    EchoOptions m_options;
    Echo m_echo[kMaxConnections];
    crd::net::QuicHandle m_handles[kMaxConnections];
    u64 m_order[kServerSlots] = {};
    u64 m_started = 0U;
    u64 m_closed = 0U;
    u64 m_next_wake_ns = 0U;
};
} // namespace crd_net_interop
