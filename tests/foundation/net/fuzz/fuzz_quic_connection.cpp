// crd-fuzz-net-quic-connection: a QUIC connection of crd-net (4.2a; docs/design/network-foundation.md#quic-connection)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: byte 0 picks the mode (bits 0 and 1) and the version (bit 2); the rest drives it.
//   0  a network script between a client and a server: each byte an operation on the datagrams in flight (deliver,
//      drop, duplicate, reorder, corrupt a byte, advance time, 0xFD a Retry in place of the client Initials in flight
//      while the server has none); then a clean path until both confirm or one closes. With bit 3 the client offers
//      the other version too and the server prefers it: a compatible switch (RFC 9368 section 2.3). With bit 4 a
//      first clean connection leaves the client a ticket, and the scripted one resumes with a stream and a datagram
//      written before its first flight: 0-RTT (RFC 9001 section 4.6).
//   1  frames from a raw client after the handshake: each [length][bytes] sealed as one 1-RTT packet to the server.
//   2  frames in Initial packets anyone can seal (their keys come from the public Destination Connection ID): each
//      [length][bytes] to a fresh server.
//
// Oracles. Always: a server sends at most three times what it received before the address is proven; every client
// datagram carrying an Initial has 1200 bytes; nothing is allocated after construction; a close names a transport
// error of RFC 9000 section 20.1 (or RFC 9368's) or CRYPTO_ERROR when this side closes. Script: after the clean path,
// both ends are confirmed or one has closed (no handshake hangs), and a confirmed pair agrees on the application
// protocol, and then carries a stream larger than every window whole and unchanged and datagrams each at most once,
// byte-exact and each reported (or one side closes). Frames: the server, which grants streams and takes datagrams of
// up to 600 bytes, stays Established (Handshaking for Initial frames) or closes; a closed server sends a
// CONNECTION_CLOSE only. Both modes route each end's own IDs through a host (QuicRoutedIds): its table holds exactly
// the IDs its connection holds, after every step; a script byte 0xFE rotates the server's IDs; and a confirmed client
// that a stateless reset of the ID it sends to reaches drains, while a forged token changes nothing. A confirmed pair
// agrees on whether a Retry was taken; the server accepts it only for an Initial to the Retry's ID. A confirmed pair
// runs one version: the other one after a switch, the configured one otherwise. 0-RTT: an offer ends Accepted or
// Rejected on both ends alike; accepted, the server reads the early stream whole and unchanged, never marking more
// bytes replayable than came; rejected, the client's streams were reset and the server opened none of them.
#include <crd/core/platform.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/quic_header.hpp>

#include <cstring>
#include <ctime>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::StringView;
using crd::containers::WireReader;
using namespace crd::crypto;
using namespace crd::net;

constexpr u8 kClientCid[8] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7};
constexpr u8 kServerCid[16] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                               0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};
constexpr u8 kInitialDcid[8] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7};
constexpr u8 kRetryCid[10] = {0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3};
constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm};
constexpr StringView kProtocols[] = {"cerid-game/1"};
constexpr const char* kServerName = "game.cerid.test";
constexpr u8 kSeeds[2][kPrfKeyBytes] = {{0x01}, {0x02}};
constexpr u64 kMs = 1000000U;
constexpr usize kDatagram = 1500U;
constexpr usize kFlights = 64U;
constexpr u8 kTicketKey[kTlsTicketKeyBytes] = {0x71};
constexpr u8 kAppContext[] = {0x5C, 0x11, 0xE7, 0x02};
constexpr u64 kEarlyBytes = 800U; // within the stream and connection windows of config_for
constexpr u8 kEarlyPayload[] = {0xDA, 0x7A};

bool valid_close(u64 code)
{
    return code <= static_cast<u64>(QuicTransportError::VersionNegotiationError) ||
           (code >= static_cast<u64>(QuicTransportError::CryptoErrorFirst) &&
            code < static_cast<u64>(QuicTransportError::CryptoErrorFirst) + 256U);
}

void check_closed(const QuicConnection& end)
{
    // A peer's close carries whatever code it chose; this side's own closes name RFC 9000's.
    if (end.close_info().local && !end.close_info().application)
    {
        CRD_FUZZ_REQUIRE(valid_close(end.close_info().error_code));
    }
}

struct Identity
{
    SigningKey key;
    TlsCredential credential;
    TlsContext client;
    TlsContext server;
    ReplayFilter filter; // the 0-RTT server's, made fresh for each input
    TlsContext early_client;
    TlsContext early_server;
    bool ready = false;

    Identity()
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        if (key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) != CryptoError::None ||
            credential.init(key, kServerName) != TlsError::None)
        {
            return;
        }
        const KeyId pin = credential.key_id();
        TlsConfig client_config;
        client_config.role = TlsRole::Client;
        client_config.groups = kGroups;
        client_config.signatures = kSignatures;
        client_config.aeads = kAeads;
        client_config.pinned = ConstSpan<KeyId>(&pin, 1U);
        client_config.server_name = kServerName;
        client_config.application_protocols = kProtocols;
        TlsConfig server_config;
        server_config.role = TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = kServerName;
        server_config.application_protocols = kProtocols;
        ready = client.init(client_config) == TlsError::None && server.init(server_config) == TlsError::None;
        client_config.early_data = true;
        server_config.early_data = true;
        server_config.early_data_filter = &filter;
        server_config.ticket_key = ConstSpan<u8>(kTicketKey);
        ready = ready && fresh_filter() && early_client.init(client_config) == TlsError::None &&
                early_server.init(server_config) == TlsError::None;
    }

    bool fresh_filter()
    {
        filter.clear();
        const u64 period = kTlsDefaultTicketLifetime + kTlsMaxTicketAgeSkew;
        return filter.init(64U, period, static_cast<u64>(std::time(nullptr))) == CryptoError::None;
    }
};

Identity& identity()
{
    static Identity held;
    CRD_FUZZ_REQUIRE(held.ready);
    return held;
}

// An end's host: a table routing its IDs under a reset key, sequence 0 routed first.
struct Host
{
    ConnectionIdTable routes;
    QuicResetKey key;
    QuicRoutedIds ids;

    Host(crd::memory::IAllocator& allocator, u32 index, ConstSpan<u8> sequence0)
        : routes(allocator, 32U, ConstSpan<u8>(kSeeds[index], kPrfKeyBytes)), ids(routes, key, index)
    {
        u8 secret[kQuicResetKeyBytes] = {static_cast<u8>(0xA0U + index)};
        CRD_FUZZ_REQUIRE(key.init(ConstSpan<u8>(secret)) == CryptoError::None);
        CRD_FUZZ_REQUIRE(routes.insert(sequence0, index) == RouteError::None);
    }

    // The host routes exactly the IDs its connection holds (sequence 0 is routed until the connection is accepted).
    void check(const QuicConnection& end) const
    {
        CRD_FUZZ_REQUIRE(end.state() == QuicConnectionState::Idle || routes.size() == end.local_cid_count());
    }
};

constexpr u32 kV1First[] = {static_cast<u32>(QuicVersion::V1), static_cast<u32>(QuicVersion::V2)};
constexpr u32 kV2First[] = {static_cast<u32>(QuicVersion::V2), static_cast<u32>(QuicVersion::V1)};

QuicVersion other_version(QuicVersion version)
{
    return (version == QuicVersion::V1) ? QuicVersion::V2 : QuicVersion::V1;
}

// A client's tickets: the newest one, handed out once.
class Tickets final : public TlsTicketStore
{
public:
    void keep(const TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        if (ticket.size() <= sizeof(m_ticket))
        {
            m_binding = binding;
            std::memcpy(m_ticket, ticket.data(), ticket.size());
            m_bytes = ticket.size();
        }
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        if (m_bytes == 0U || !(m_binding == binding))
        {
            return {};
        }
        std::memcpy(m_handed, m_ticket, m_bytes);
        const usize bytes = m_bytes;
        m_bytes = 0U;
        return ConstSpan<u8>(m_handed, bytes);
    }

    [[nodiscard]] bool holds() const { return m_bytes != 0U; }

private:
    TlsBinding m_binding;
    u8 m_ticket[4096] = {};
    u8 m_handed[4096] = {};
    usize m_bytes = 0U;
};

// With `switching` the client lists its version first and the server the other one; with `tickets` both ends run
// the contexts that take 0-RTT, the client keeping and offering its tickets there.
QuicConnectionConfig config_for(QuicSide side, QuicVersion version, QuicConnectionIds* ids, bool switching = false,
                                Tickets* tickets = nullptr)
{
    QuicConnectionConfig config;
    config.side = side;
    if (switching)
    {
        const bool first = (side == QuicSide::Client) == (version == QuicVersion::V1);
        config.versions = first ? ConstSpan<u32>(kV1First, 2U) : ConstSpan<u32>(kV2First, 2U);
    }
    config.ids = ids;
    config.parameters.active_connection_id_limit = 4U;
    config.version = version;
    config.tls = (side == QuicSide::Client) ? &identity().client : &identity().server;
    if (tickets != nullptr)
    {
        config.tls = (side == QuicSide::Client) ? &identity().early_client : &identity().early_server;
        config.tickets = (side == QuicSide::Client) ? tickets : nullptr;
        config.early_context = (side == QuicSide::Server) ? ConstSpan<u8>(kAppContext) : ConstSpan<u8>();
    }
    config.parameters.max_idle_timeout = 30000U;
    config.sent_packets = 64U;
    config.sent_frames = 128U;
    config.crypto_bytes = 8192U;
    config.streams.peer_bidi = 2U;
    config.streams.peer_uni = 1U;
    config.streams.local_bidi = 2U;
    config.streams.local_uni = 1U;
    config.streams.receive_bytes = 2048U;
    config.streams.send_bytes = 2048U;
    config.streams.stream_window = 1024U;
    config.streams.connection_window = 2048U;
    config.streams.connection_max_window = 8192U;
    config.datagrams.max_frame_bytes = 600U;
    config.datagrams.receive_bytes = 2400U; // send_count of the largest: nothing is dropped between reads
    config.datagrams.receive_count = 4U;
    config.datagrams.send_bytes = 1024U;
    config.datagrams.send_count = 4U;
    config.datagrams.outcomes = true;
    config.entropy_seed = ConstSpan<u8>(kSeeds[side == QuicSide::Client ? 0U : 1U], kPrfKeyBytes);
    return config;
}

QuicAccept accept_of(ConstSpan<u8> dcid, ConstSpan<u8> scid)
{
    QuicAccept accept;
    accept.local_cid = ConstSpan<u8>(kServerCid, sizeof(kServerCid));
    accept.client_dcid = dcid;
    accept.client_scid = scid;
    accept.original_dcid = dcid;
    return accept;
}

// ---- Mode 0: a network script ----

struct Flight
{
    u8 bytes[kDatagram] = {};
    usize size = 0U;
    usize to = 0U;
};

// MSVC C4324 as for crd::crypto::AeadKey: the connections are 16-byte aligned; ARM64 MSVC reports the padding.
#if CRD_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct Pair
{
    crd::fuzz::BudgetAllocator allocator;
    Host client_host;
    Host server_host;
    QuicConnection client;
    QuicConnection server;
    Flight flights[kFlights];
    usize count = 0U;
    u64 now = 1000U * kMs;
    QuicVersion version;
    bool retry_sent = false;

    explicit Pair(QuicVersion run, bool switching = false, Tickets* tickets = nullptr)
        : client_host(allocator, 0U, ConstSpan<u8>(kClientCid, sizeof(kClientCid))),
          server_host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid))),
          client(allocator, config_for(QuicSide::Client, run, &client_host.ids, switching, tickets)),
          server(allocator, config_for(QuicSide::Server, run, &server_host.ids, switching, tickets)), version(run)
    {
    }

    // The server answers the client Initials in flight with one Retry, as a stateless ingress does: they are gone.
    void retry()
    {
        if (retry_sent || server.state() != QuicConnectionState::Idle || count == kFlights)
        {
            return;
        }
        for (usize index = 0U; index < count;)
        {
            if (flights[index].to == 1U)
            {
                remove(index);
                continue;
            }
            ++index;
        }
        u8 token[24];
        std::memset(token, 0x6B, sizeof(token));
        Flight& flight = flights[count];
        CRD_FUZZ_REQUIRE(write_retry(version, ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                                     ConstSpan<u8>(kRetryCid, sizeof(kRetryCid)),
                                     ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), ConstSpan<u8>(token), 0U,
                                     Span<u8>(flight.bytes, sizeof(flight.bytes)), flight.size) == CryptoError::None);
        flight.to = 0U;
        ++count;
        retry_sent = true;
    }

    void check_hosts() const
    {
        client_host.check(client);
        server_host.check(server);
    }

    void check_sent(usize from, ConstSpan<u8> datagram)
    {
        QuicHeader header;
        if (from == 0U && parse_quic_header(datagram, sizeof(kServerCid), header) == QuicHeaderError::None &&
            header.type == QuicPacketType::Initial)
        {
            CRD_FUZZ_REQUIRE(datagram.size() >= kQuicMinInitialDatagram);
        }
        if (from == 1U && !server.address_validated())
        {
            CRD_FUZZ_REQUIRE(server.counters().bytes_sent <= 3U * server.counters().bytes_received);
        }
    }

    void flush()
    {
        QuicConnection* const ends[2] = {&client, &server};
        for (usize from = 0U; from < 2U; ++from)
        {
            for (usize guard = 0U; guard < 64U; ++guard)
            {
                u8 out[kDatagram];
                const usize size = ends[from]->send(Span<u8>(out, sizeof(out)), now);
                if (size == 0U)
                {
                    break;
                }
                check_sent(from, ConstSpan<u8>(out, size));
                if (count < kFlights)
                {
                    std::memcpy(flights[count].bytes, out, size);
                    flights[count].size = size;
                    flights[count].to = 1U - from;
                    ++count;
                }
            }
        }
    }

    void deliver(usize index)
    {
        Flight& flight = flights[index];
        QuicConnection& to = (flight.to == 0U) ? client : server;
        if (flight.to == 1U && to.state() == QuicConnectionState::Idle)
        {
            QuicHeader header;
            if (parse_quic_header(ConstSpan<u8>(flight.bytes, flight.size), sizeof(kServerCid), header) !=
                    QuicHeaderError::None ||
                header.type != QuicPacketType::Initial)
            {
                return;
            }
            QuicAccept accept = accept_of(header.dcid, header.scid);
            const ConstSpan<u8> retried(kRetryCid, sizeof(kRetryCid));
            accept.retried = retry_sent && header.dcid.size() == retried.size() &&
                             std::memcmp(header.dcid.data(), retried.data(), retried.size()) == 0;
            accept.original_dcid = accept.retried ? ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)) : header.dcid;
            accept.validated = accept.retried;
            if (!to.accept(accept, now))
            {
                return;
            }
        }
        to.receive(Span<u8>(flight.bytes, flight.size), QuicEcn::NotEct, now);
        check_closed(to);
        check_hosts();
    }

    void remove(usize index) { flights[index] = flights[--count]; }

    void timers()
    {
        for (QuicConnection* end : {&client, &server})
        {
            if (end->next_timer_ns() <= now)
            {
                end->on_timeout(now);
                check_closed(*end);
            }
        }
        check_hosts();
    }

    // A clean path: everything delivered in order, time moving to each timer, for at most `events` timers. The
    // script may have taught both ends an RTT of minutes, so the bound counts events, not time: the idle timeout
    // ends every connection that neither confirms nor closes.
    void settle(usize events)
    {
        usize stalled = 0U;
        for (usize event = 0U; event < events; ++event)
        {
            flush();
            while (count > 0U)
            {
                deliver(0U);
                remove(0U);
                flush();
            }
            const u64 next =
                (client.next_timer_ns() < server.next_timer_ns()) ? client.next_timer_ns() : server.next_timer_ns();
            if (next == kNeverNs || (client.confirmed() && server.confirmed()))
            {
                return;
            }
            stalled = (next <= now) ? stalled + 1U : 0U;
            CRD_FUZZ_REQUIRE(stalled < 64U); // a timer due that nothing serves would spin the caller's loop
            now = (next > now) ? next : now;
            timers();
        }
    }
};
#if CRD_COMPILER_MSVC
#pragma warning(pop)
#endif

bool finished(const QuicConnection& end)
{
    return end.state() == QuicConnectionState::Closing || end.state() == QuicConnectionState::Draining ||
           end.state() == QuicConnectionState::Closed;
}

constexpr u64 kTransferBytes = 5000U; // more than every stream and connection window of config_for

// After the script, a confirmed pair carries a stream of more than every window over the clean path: each byte the
// server reads is the byte written, and all of it arrives with the FIN, or one side has closed.
void transfer(Pair& pair)
{
    QuicStreamSet& client = pair.client.streams();
    QuicStreamSet& server = pair.server.streams();
    u64 id = 0U;
    CRD_FUZZ_REQUIRE(client.open(true, id) == QuicStreamError::None);
    u64 written = 0U;
    u64 read = 0U;
    bool fin = false;
    for (usize round = 0U; round < 512U && !fin && !finished(pair.client) && !finished(pair.server); ++round)
    {
        u8 chunk[512];
        for (usize at = 0U; at < sizeof(chunk); ++at)
        {
            chunk[at] = static_cast<u8>((written + at) * 7U);
        }
        const u64 left = kTransferBytes - written;
        const usize want = (left < sizeof(chunk)) ? static_cast<usize>(left) : sizeof(chunk);
        usize taken = 0U;
        const bool fin_here = written + want == kTransferBytes;
        if (client.write(id, ConstSpan<u8>(chunk, want), fin_here, taken) == QuicStreamError::None)
        {
            written += taken;
        }
        pair.settle(8U);
        for (QuicStreamRead data = server.peek(id); !data.data.empty() || data.fin; data = server.peek(id))
        {
            for (usize at = 0U; at < data.data.size(); ++at)
            {
                CRD_FUZZ_REQUIRE(data.data[at] == static_cast<u8>((read + at) * 7U));
            }
            read += data.data.size();
            CRD_FUZZ_REQUIRE(server.consume(id, data.data.size()) == QuicStreamError::None);
            if (data.fin)
            {
                fin = true;
                break;
            }
        }
        // Time moves to the next timer (a delayed ACK frees the send buffer).
        const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns()) ? pair.client.next_timer_ns()
                                                                                     : pair.server.next_timer_ns();
        pair.now = (next != kNeverNs && next > pair.now) ? next : pair.now;
        pair.timers();
    }
    CRD_FUZZ_REQUIRE((fin && read == kTransferBytes) || finished(pair.client) || finished(pair.server));
}

constexpr u64 kDatagrams = 12U;

// Then datagrams of rising sizes from the client, their ids from `base`: each the server reads arrives once and
// byte-exact, and every one is reported acknowledged or lost (or one side closes).
void exchange_datagrams(Pair& pair, u64 base)
{
    QuicDatagrams& out = pair.client.datagrams();
    QuicDatagrams& in = pair.server.datagrams();
    bool arrived[kDatagrams] = {};
    u64 queued = 0U;
    u64 reported = 0U;
    for (usize round = 0U; round < 256U && reported < kDatagrams && !finished(pair.client) && !finished(pair.server);
         ++round)
    {
        u8 payload[600];
        u64 id = 0U;
        while (queued < kDatagrams)
        {
            const usize bytes = static_cast<usize>(1U + (queued * 47U) % out.max_payload());
            payload[0] = static_cast<u8>(queued);
            for (usize at = 1U; at < bytes; ++at)
            {
                payload[at] = static_cast<u8>((queued * 31U) + at);
            }
            if (out.queue(ConstSpan<u8>(payload, bytes), pair.now, id) != QuicDatagramError::None)
            {
                break;
            }
            CRD_FUZZ_REQUIRE(id == base + queued);
            ++queued;
        }
        pair.settle(8U);
        ConstSpan<u8> held;
        for (; in.peek(held); in.pop())
        {
            const u64 which = held.empty() ? kDatagrams : held[0];
            CRD_FUZZ_REQUIRE(which < queued && !arrived[which]);
            CRD_FUZZ_REQUIRE(held.size() == static_cast<usize>(1U + (which * 47U) % out.max_payload()));
            for (usize at = 1U; at < held.size(); ++at)
            {
                CRD_FUZZ_REQUIRE(held[at] == static_cast<u8>((which * 31U) + at));
            }
            arrived[which] = true;
        }
        QuicDatagramReport report;
        while (out.poll(report))
        {
            const u64 index = report.id - base;
            CRD_FUZZ_REQUIRE(report.id >= base && index < queued && report.outcome != QuicDatagramOutcome::Expired);
            CRD_FUZZ_REQUIRE(report.outcome == QuicDatagramOutcome::Lost || arrived[index]);
            ++reported;
        }
        const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns()) ? pair.client.next_timer_ns()
                                                                                     : pair.server.next_timer_ns();
        pair.now = (next != kNeverNs && next > pair.now) ? next : pair.now;
        pair.timers();
    }
    CRD_FUZZ_REQUIRE(in.counters().dropped == 0U);
    CRD_FUZZ_REQUIRE(reported == kDatagrams || finished(pair.client) || finished(pair.server));
}

// A stateless reset reaching a confirmed client: a forged token changes nothing, the token of the ID it sends to
// drains it.
void stateless_reset(Pair& pair)
{
    QuicConnection& client = pair.client;
    if (client.state() != QuicConnectionState::Established)
    {
        return;
    }
    client.ping(); // the ID it sends to is used from its next packet on
    pair.flush();
    u8 datagram[48];
    std::memset(datagram, 0x3C, sizeof(datagram));
    datagram[0] = 0x4BU;
    const Span<u8> token(datagram + sizeof(datagram) - kQuicResetTokenBytes, kQuicResetTokenBytes);
    CRD_FUZZ_REQUIRE(pair.server_host.key.token(client.peer_cid(), token) == CryptoError::None);
    token[0] ^= 0x01U;
    CRD_FUZZ_REQUIRE(!client.stateless_reset(ConstSpan<u8>(datagram), pair.now));
    token[0] ^= 0x01U;
    client.receive(Span<u8>(datagram), QuicEcn::NotEct, pair.now);
    CRD_FUZZ_REQUIRE(client.state() == QuicConnectionState::Draining && client.close_info().stateless_reset);
}

// Time moves to the next timer of either end (the script may have taught both an RTT of minutes) and the clean path
// runs.
void next_timer(Pair& pair)
{
    const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns()) ? pair.client.next_timer_ns()
                                                                                 : pair.server.next_timer_ns();
    pair.now = (next != kNeverNs && next > pair.now) ? next : pair.now;
    pair.timers();
    pair.settle(8U);
}

// A first clean connection that leaves `tickets` a ticket for the next one (after the handshake, at the Application
// level).
void take_ticket(QuicVersion version, bool switching, Tickets& tickets)
{
    Pair first(version, switching, &tickets);
    CRD_FUZZ_REQUIRE(first.client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                                          ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), first.now));
    first.settle(4096U);
    CRD_FUZZ_REQUIRE(first.client.confirmed() && first.server.confirmed());
    for (usize round = 0U; round < 64U && !tickets.holds(); ++round)
    {
        next_timer(first);
    }
    CRD_FUZZ_REQUIRE(tickets.holds());
}

// The early datagram (id 0): reported once, Acked only when the server holds it, and marked replayable there when
// 0-RTT was accepted (it can only have come in a 0-RTT packet: it was queued before the first flight).
void settle_early_datagram(Pair& pair, bool accepted)
{
    QuicDatagramReport report;
    bool reported = false;
    for (usize round = 0U; round < 256U && !reported && !finished(pair.client) && !finished(pair.server); ++round)
    {
        reported = pair.client.datagrams().poll(report);
        if (!reported)
        {
            next_timer(pair);
        }
    }
    if (!reported)
    {
        CRD_FUZZ_REQUIRE(finished(pair.client) || finished(pair.server));
        return;
    }
    CRD_FUZZ_REQUIRE(report.id == 0U && report.outcome != QuicDatagramOutcome::Expired);
    CRD_FUZZ_REQUIRE(accepted || report.outcome == QuicDatagramOutcome::Lost);
    ConstSpan<u8> held;
    const bool arrived = pair.server.datagrams().peek(held);
    CRD_FUZZ_REQUIRE(report.outcome == QuicDatagramOutcome::Lost || arrived);
    if (arrived)
    {
        CRD_FUZZ_REQUIRE(accepted && held.size() == sizeof(kEarlyPayload) &&
                         std::memcmp(held.data(), kEarlyPayload, held.size()) == 0 &&
                         pair.server.datagrams().oldest_early());
        pair.server.datagrams().pop();
    }
}

// The server's reads of the early stream after the script: whole and unchanged, its replayable marks never past
// what came early.
void read_early(Pair& pair)
{
    QuicStreamSet& server = pair.server.streams();
    u64 read = 0U;
    bool fin = false;
    for (usize round = 0U; round < 512U && !fin && !finished(pair.client) && !finished(pair.server); ++round)
    {
        for (QuicStreamRead data = server.peek(0U); !data.data.empty() || data.fin; data = server.peek(0U))
        {
            CRD_FUZZ_REQUIRE(data.early_bytes <= data.data.size() && read + data.early_bytes <= kEarlyBytes);
            for (usize at = 0U; at < data.data.size(); ++at)
            {
                CRD_FUZZ_REQUIRE(data.data[at] == static_cast<u8>((read + at) * 13U));
            }
            read += data.data.size();
            CRD_FUZZ_REQUIRE(server.consume(0U, data.data.size()) == QuicStreamError::None);
            fin = data.fin;
            if (fin)
            {
                break;
            }
        }
        if (!fin)
        {
            next_timer(pair);
        }
    }
    CRD_FUZZ_REQUIRE((fin && read == kEarlyBytes) || finished(pair.client) || finished(pair.server));
}

void run_script(QuicVersion version, bool switching, bool early, ConstSpan<u8> script)
{
    Tickets tickets;
    if (early)
    {
        CRD_FUZZ_REQUIRE(identity().fresh_filter());
        take_ticket(version, switching, tickets);
    }
    Pair pair(version, switching, early ? &tickets : nullptr);
    CRD_FUZZ_REQUIRE(pair.client.ready() && pair.server.ready());
    const usize allocations = pair.allocator.peak_bytes();
    CRD_FUZZ_REQUIRE(pair.client.connect(ConstSpan<u8>(kClientCid, sizeof(kClientCid)),
                                         ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), pair.now));
    // A switch runs the first connection in the other version: its ticket is offered by none (RFC 9369 section 5).
    const bool offered = pair.client.handshake().early_data() == TlsEarlyData::Offered;
    CRD_FUZZ_REQUIRE(offered == (early && !switching));
    if (offered)
    {
        u64 id = 0U;
        CRD_FUZZ_REQUIRE(pair.client.streams().open(true, id) == QuicStreamError::None && id == 0U);
        u8 bytes[kEarlyBytes];
        for (usize at = 0U; at < sizeof(bytes); ++at)
        {
            bytes[at] = static_cast<u8>(at * 13U);
        }
        usize taken = 0U;
        CRD_FUZZ_REQUIRE(pair.client.streams().write(0U, ConstSpan<u8>(bytes), true, taken) == QuicStreamError::None &&
                         taken == kEarlyBytes);
        u64 datagram = 0U;
        CRD_FUZZ_REQUIRE(pair.client.datagrams().queue(ConstSpan<u8>(kEarlyPayload), pair.now, datagram) ==
                         QuicDatagramError::None);
    }
    for (usize at = 0U; at < script.size(); ++at)
    {
        pair.flush();
        const u8 op = script[at];
        const usize pick = (pair.count == 0U) ? 0U : (op >> 3U) % pair.count;
        if (op == 0xFEU)
        {
            static_cast<void>(pair.server.rotate_connection_ids());
            continue;
        }
        if (op == 0xFDU)
        {
            pair.retry();
            continue;
        }
        switch (op & 7U)
        {
            case 0U:
            case 1U:
                if (pair.count > 0U)
                {
                    pair.deliver(pick);
                    pair.remove(pick);
                }
                break;
            case 2U:
                if (pair.count > 0U)
                {
                    pair.remove(pick); // lost
                }
                break;
            case 3U:
                if (pair.count > 0U && pair.count < kFlights)
                {
                    pair.flights[pair.count++] = pair.flights[pick]; // duplicated
                }
                break;
            case 4U:
                if (pair.count > 1U)
                {
                    const Flight held = pair.flights[0];
                    pair.flights[0] = pair.flights[pick];
                    pair.flights[pick] = held; // reordered
                }
                break;
            case 5U:
                if (pair.count > 0U && at + 2U < script.size())
                {
                    Flight& flight = pair.flights[pick];
                    flight.bytes[script[at + 1U] % flight.size] ^= static_cast<u8>(script[at + 2U] | 1U);
                    at += 2U;
                }
                break;
            case 6U:
                pair.now += static_cast<u64>(op >> 3U) * 10U * kMs;
                pair.timers();
                break;
            default:
            {
                const u64 next = (pair.client.next_timer_ns() < pair.server.next_timer_ns())
                                     ? pair.client.next_timer_ns()
                                     : pair.server.next_timer_ns();
                pair.now = (next != kNeverNs && next > pair.now) ? next : pair.now;
                pair.timers();
            }
            break;
        }
    }
    pair.settle(4096U);
    CRD_FUZZ_REQUIRE((pair.client.confirmed() && pair.server.confirmed()) || finished(pair.client) ||
                     finished(pair.server));
    if (pair.client.confirmed() && pair.server.confirmed())
    {
        CRD_FUZZ_REQUIRE(pair.client.handshake().application_protocol() ==
                         pair.server.handshake().application_protocol());
        CRD_FUZZ_REQUIRE(pair.client.retried() == pair.server.retried());
        const QuicVersion running = switching ? other_version(version) : version;
        CRD_FUZZ_REQUIRE(pair.client.version() == running && pair.server.version() == running);
        if (offered)
        {
            const TlsEarlyData decided = pair.client.handshake().early_data();
            CRD_FUZZ_REQUIRE(decided == TlsEarlyData::Accepted || decided == TlsEarlyData::Rejected);
            CRD_FUZZ_REQUIRE((decided == TlsEarlyData::Accepted) ==
                             (pair.server.handshake().early_data() == TlsEarlyData::Accepted));
            if (decided == TlsEarlyData::Accepted)
            {
                read_early(pair);
            }
            else
            {
                CRD_FUZZ_REQUIRE(pair.client.streams().open_streams() == 0U);
                CRD_FUZZ_REQUIRE(pair.server.streams().recv_state(0U) == QuicRecvState::None);
                CRD_FUZZ_REQUIRE(pair.server.datagrams().held() == 0U);
            }
            settle_early_datagram(pair, decided == TlsEarlyData::Accepted);
        }
        transfer(pair);
        exchange_datagrams(pair, offered ? 1U : 0U);
        stateless_reset(pair);
    }
    CRD_FUZZ_REQUIRE(pair.allocator.peak_bytes() == allocations);
}

// ---- Modes 1 and 2: frames from a raw peer ----

QuicLevel level_of(QuicPacketType type)
{
    if (type == QuicPacketType::Initial)
    {
        return QuicLevel::Initial;
    }
    return (type == QuicPacketType::Handshake) ? QuicLevel::Handshake : QuicLevel::Application;
}

QuicPacketType type_of(QuicLevel level)
{
    if (level == QuicLevel::Initial)
    {
        return QuicPacketType::Initial;
    }
    return (level == QuicLevel::Handshake) ? QuicPacketType::Handshake : QuicPacketType::OneRtt;
}

struct Raw
{
    crd::fuzz::BudgetAllocator allocator;
    Host host;
    QuicConnection server;
    QuicHandshake handshake;
    QuicPacketProtection protection;
    u64 next[3] = {};
    u64 sent[3] = {};
    u64 received[3] = {};
    u8 dcid[20] = {};
    usize dcid_bytes = sizeof(kInitialDcid);
    u64 now = 1000U * kMs;
    bool failed = false;
    u32 version;

    explicit Raw(QuicVersion run)
        : host(allocator, 1U, ConstSpan<u8>(kServerCid, sizeof(kServerCid))),
          server(allocator, config_for(QuicSide::Server, run, &host.ids)), version(static_cast<u32>(run))
    {
        std::memcpy(dcid, kInitialDcid, sizeof(kInitialDcid));
    }

    // One packet of `type` holding `frames` as its payload (PADDING when empty), then the server's answers read.
    void send(QuicPacketType type, ConstSpan<u8> frames)
    {
        u8 datagram[kDatagram] = {};
        QuicPacketWriter writer;
        QuicPacketSpec spec;
        spec.type = type;
        spec.version = version;
        spec.dcid = ConstSpan<u8>(dcid, dcid_bytes);
        spec.scid = ConstSpan<u8>(kClientCid, sizeof(kClientCid));
        spec.number = next[static_cast<usize>(quic_space_of(type))]++;
        if (!writer.begin(spec, Span<u8>(datagram, kQuicMinInitialDatagram)))
        {
            return;
        }
        const usize used = (frames.size() < writer.frames().size()) ? frames.size() : writer.frames().size();
        if (used != 0U)
        {
            std::memcpy(writer.frames().data(), frames.data(), used);
        }
        const usize floor = (type == QuicPacketType::Initial) ? kQuicMinInitialDatagram : 0U;
        usize size = 0U;
        if (protection.seal(writer, used, {floor, 0U}, size) != QuicPacketError::None)
        {
            return;
        }
        server.receive(Span<u8>(datagram, size), QuicEcn::NotEct, now);
        check_closed(server);
        drain();
    }

    void drain()
    {
        for (usize guard = 0U; guard < 64U; ++guard)
        {
            u8 datagram[kDatagram];
            const usize size = server.send(Span<u8>(datagram, sizeof(datagram)), now);
            if (size == 0U)
            {
                return;
            }
            CRD_FUZZ_REQUIRE(size <= kQuicMinInitialDatagram);
            if (!server.address_validated())
            {
                CRD_FUZZ_REQUIRE(server.counters().bytes_sent <= 3U * server.counters().bytes_received);
            }
            read(Span<u8>(datagram, size));
        }
    }

    void read(Span<u8> datagram)
    {
        QuicDatagramPackets packets(datagram, sizeof(kClientCid));
        QuicHeader header;
        Span<u8> packet;
        while (packets.next(header, packet))
        {
            if (header.type != QuicPacketType::OneRtt && dcid_bytes == sizeof(kInitialDcid))
            {
                std::memcpy(dcid, header.scid.data(), header.scid.size());
                dcid_bytes = header.scid.size();
            }
            QuicOpened opened;
            if (protection.open(header, packet, opened) != QuicPacketError::None)
            {
                continue;
            }
            WireReader in(opened.payload);
            while (!in.done())
            {
                QuicFrame frame;
                CRD_FUZZ_REQUIRE(read_quic_frame(in, frame) == QuicTransportError::NoError);
                CRD_FUZZ_REQUIRE(quic_frame_allowed(frame.kind, frame.application, opened.type, true));
                if (server.state() == QuicConnectionState::Closing)
                {
                    CRD_FUZZ_REQUIRE(frame.kind == QuicFrameKind::ConnectionClose ||
                                     frame.kind == QuicFrameKind::Padding);
                }
                const QuicLevel level = level_of(opened.type);
                const usize index = static_cast<usize>(level);
                if (frame.kind == QuicFrameKind::Crypto && frame.offset == received[index])
                {
                    received[index] += frame.data.size();
                    failed = failed || handshake.provide(level, frame.data) != TlsError::None;
                }
            }
            failed = failed || protection.install(handshake) != CryptoError::None;
        }
    }

    void send_crypto(QuicLevel level)
    {
        failed = failed || protection.install(handshake) != CryptoError::None;
        const usize index = static_cast<usize>(level);
        while (!failed && handshake.pending(level) > 0U)
        {
            u8 chunk[900];
            usize taken = 0U;
            failed = handshake.take(level, Span<u8>(chunk, sizeof(chunk)), taken) != TlsError::None;
            QuicFrame frame;
            frame.kind = QuicFrameKind::Crypto;
            frame.offset = sent[index];
            frame.data = ConstSpan<u8>(chunk, taken);
            sent[index] += taken;
            u8 frames[1000];
            const usize bytes = write_quic_frame(frame, Span<u8>(frames, sizeof(frames)));
            send(type_of(level), ConstSpan<u8>(frames, bytes));
        }
    }
};

void run_frames(QuicVersion version, ConstSpan<u8> input, bool initial)
{
    Raw raw(version);
    CRD_FUZZ_REQUIRE(raw.server.ready());
    const usize allocations = raw.allocator.peak_bytes();
    CRD_FUZZ_REQUIRE(raw.protection.start(QuicSide::Client, version,
                                          ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid))) == CryptoError::None);
    CRD_FUZZ_REQUIRE(raw.server.accept(
        accept_of(ConstSpan<u8>(kInitialDcid, sizeof(kInitialDcid)), ConstSpan<u8>(kClientCid, sizeof(kClientCid))),
        raw.now));
    if (!initial)
    {
        QuicTransportParameters parameters;
        parameters.has_initial_source_connection_id = true;
        static_cast<void>(
            parameters.initial_source_connection_id.assign(ConstSpan<u8>(kClientCid, sizeof(kClientCid))));
        u8 encoded[256];
        const usize bytes =
            write_quic_transport_parameters(parameters, QuicSide::Client, {3U}, Span<u8>(encoded, sizeof(encoded)));
        CRD_FUZZ_REQUIRE(raw.handshake.init(identity().client, ConstSpan<u8>(encoded, bytes)) == TlsError::None);
        raw.send_crypto(QuicLevel::Initial);
        raw.send_crypto(QuicLevel::Handshake);
        CRD_FUZZ_REQUIRE(!raw.failed && raw.server.state() == QuicConnectionState::Established);
        CRD_FUZZ_REQUIRE(raw.protection.can_send(QuicPacketType::OneRtt));
        raw.protection.confirm();
    }
    const QuicPacketType type = initial ? QuicPacketType::Initial : QuicPacketType::OneRtt;
    for (usize at = 0U; at < input.size();)
    {
        const usize length = input[at++];
        const usize take = (length < input.size() - at) ? length : input.size() - at;
        raw.send(type, input.subspan(at, take));
        at += take;
        raw.host.check(raw.server);
        const QuicConnectionState state = raw.server.state();
        const QuicConnectionState open = initial ? QuicConnectionState::Handshaking : QuicConnectionState::Established;
        CRD_FUZZ_REQUIRE(state == open || state == QuicConnectionState::Closing ||
                         state == QuicConnectionState::Draining);
        raw.now += kMs;
        if (raw.server.next_timer_ns() <= raw.now)
        {
            raw.server.on_timeout(raw.now);
            check_closed(raw.server);
            raw.drain();
        }
    }
    CRD_FUZZ_REQUIRE(raw.allocator.peak_bytes() == allocations);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 1U || size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    const QuicVersion version = ((data[0] & 4U) != 0U) ? QuicVersion::V2 : QuicVersion::V1;
    const ConstSpan<u8> rest(data + 1U, size - 1U);
    switch (data[0] & 3U)
    {
        case 0U:
            run_script(version, (data[0] & 8U) != 0U, (data[0] & 16U) != 0U, rest);
            break;
        case 1U:
            run_frames(version, rest, false);
            break;
        default:
            run_frames(version, rest, true);
            break;
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // A script: deliver everything in order with a timer step between flights.
    const u8 clean[] = {0x00U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-clean", clean, sizeof(clean));
    // A Retry in place of the first flight: the client goes again to its ID with its token.
    const u8 retried[] = {0x00U, 0xFDU, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-retry", retried, sizeof(retried));
    // A compatible switch from each version, one after a Retry.
    const u8 switched[] = {0x08U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-switch", switched, sizeof(switched));
    const u8 switched_retry[] = {0x0CU, 0xFDU, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U,
                                 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-switch-retry", switched_retry, sizeof(switched_retry));
    // 0-RTT on a resumed connection, clean, after a Retry and on a hostile path in version 2.
    const u8 early[] = {0x10U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-early", early, sizeof(early));
    const u8 early_retry[] = {0x10U, 0xFDU, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U,
                              0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U};
    sink.add("script-early-retry", early_retry, sizeof(early_retry));
    const u8 early_hostile[] = {0x14U, 0x04U, 0x02U, 0x03U, 0x00U, 0x0CU, 0x05U, 0x40U, 0x08U, 0x07U,
                                0x00U, 0x02U, 0x06U, 0x86U, 0x07U, 0x00U, 0x00U, 0x0BU, 0x00U};
    sink.add("script-early-hostile", early_hostile, sizeof(early_hostile));
    // Losses, duplicates, a reorder and a corrupted byte in version 2.
    const u8 hostile[] = {0x04U, 0x02U, 0x03U, 0x00U, 0x0CU, 0x05U, 0x40U, 0x08U, 0x07U, 0x00U,
                          0x02U, 0x06U, 0x86U, 0x07U, 0x00U, 0x00U, 0x0BU, 0x00U, 0x00U};
    sink.add("script-hostile", hostile, sizeof(hostile));
    // 1-RTT frames after the handshake: a PING, a STREAM frame, a NEW_CONNECTION_ID, a PATH_CHALLENGE.
    const u8 frames[] = {0x01U, 0x01U, 0x01U, 0x04U, 0x0AU, 0x00U, 0x01U, 0x41U, 0x1CU, 0x18U, 0x01U, 0x00U,
                         0x08U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U, 0xA0U, 0xA1U, 0xA2U,
                         0xA3U, 0xA4U, 0xA5U, 0xA6U, 0xA7U, 0xA8U, 0xA9U, 0xAAU, 0xABU, 0xACU, 0xADU, 0xAEU,
                         0xAFU, 0x09U, 0x1AU, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U};
    sink.add("one-rtt-frames", frames, sizeof(frames));
    // A clean script that rotates the server's IDs once the handshake has run.
    const u8 rotate[] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                         0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFEU, 0x00U, 0x00U, 0x00U,
                         0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
    sink.add("script-rotate", rotate, sizeof(rotate));
    // RETIRE_CONNECTION_ID of the server's sequence 1, then of a sequence never issued.
    const u8 retire[] = {0x01U, 0x02U, 0x19U, 0x01U, 0x02U, 0x19U, 0x05U};
    sink.add("one-rtt-retire", retire, sizeof(retire));
    // Initial frames anyone can seal: a PING, an ACK of a packet never sent, a CRYPTO frame of garbage.
    const u8 initial[] = {0x02U, 0x01U, 0x01U, 0x05U, 0x02U, 0x05U, 0x00U, 0x00U, 0x00U,
                          0x07U, 0x06U, 0x00U, 0x04U, 0x01U, 0x00U, 0x00U, 0x00U};
    sink.add("initial-frames", initial, sizeof(initial));
}
