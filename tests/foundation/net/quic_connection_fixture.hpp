#pragma once

// What the QUIC connection and server tests share (4.2a; docs/design/network-foundation.md#quic-connection): the
// server identity and the TLS contexts that trust it, connection configurations with deterministic entropy, and the
// seeded generator of the hostile paths.

#include <crd/containers/string_view.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/crypto/replay.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/quic_connection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <ctime>

namespace crd::net::testing
{
namespace cont = crd::containers;

inline constexpr crypto::AlgorithmId kGroups[] = {crypto::AlgorithmId::X25519Mlkem768, crypto::AlgorithmId::X25519};
inline constexpr crypto::AlgorithmId kSignatures[] = {crypto::AlgorithmId::Ed25519};
inline constexpr crypto::AlgorithmId kAeads[] = {crypto::AlgorithmId::Aes128Gcm, crypto::AlgorithmId::Chacha20Poly1305};
inline constexpr cont::StringView kProtocols[] = {"cerid-game/1"};
inline constexpr const char* kServerName = "game.cerid.test";
inline constexpr crypto::QuicVersion kVersions[] = {crypto::QuicVersion::V1, crypto::QuicVersion::V2};
inline constexpr u64 kStartNs = 1000000000U;
inline constexpr u64 kMs = 1000000U;
inline constexpr usize kDatagram = 1500U;
inline constexpr u64 kUnix = 1793577600U; // 2026-11-02T00:00:00Z: the fleet clock tokens carry
inline constexpr u8 kTokenKey[crypto::kQuicTokenKeyBytes] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
inline const Endpoint kClientPeer{IpAddress::v4(192, 0, 2, 9), 4433U};

inline constexpr u8 kTicketKey[crypto::kTlsTicketKeyBytes] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF};

// A server key and the client and server contexts that trust it; `wrong_pin` makes the client pin another key. With
// an `early` replay filter both ends configure early data (0-RTT) under one ticket key, and `plain` is a server of
// the same fleet that takes none.
struct Identity
{
    crypto::SigningKey key;
    crypto::SigningKey other;
    crypto::TlsCredential credential;
    crypto::TlsContext client;
    crypto::TlsContext server;
    crypto::TlsContext plain;

    explicit Identity(bool wrong_pin = false, crypto::ReplayFilter* early = nullptr)
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        REQUIRE(key.from_seed(crypto::AlgorithmId::Ed25519, cont::ConstSpan<u8>(seed, sizeof(seed))) ==
                crypto::CryptoError::None);
        seed[0] = 0xEEU;
        REQUIRE(other.from_seed(crypto::AlgorithmId::Ed25519, cont::ConstSpan<u8>(seed, sizeof(seed))) ==
                crypto::CryptoError::None);
        REQUIRE(credential.init(key, kServerName) == crypto::TlsError::None);
        crypto::TlsCredential stranger;
        REQUIRE(stranger.init(other, kServerName) == crypto::TlsError::None);
        const crypto::KeyId pin = wrong_pin ? stranger.key_id() : credential.key_id();

        crypto::TlsConfig client_config;
        client_config.role = crypto::TlsRole::Client;
        client_config.groups = kGroups;
        client_config.signatures = kSignatures;
        client_config.aeads = kAeads;
        client_config.pinned = cont::ConstSpan<crypto::KeyId>(&pin, 1U);
        client_config.server_name = kServerName;
        client_config.application_protocols = kProtocols;
        client_config.early_data = early != nullptr;
        REQUIRE(client.init(client_config) == crypto::TlsError::None);

        crypto::TlsConfig server_config;
        server_config.role = crypto::TlsRole::Server;
        server_config.groups = kGroups;
        server_config.aeads = kAeads;
        server_config.credential = &credential;
        server_config.server_name = kServerName;
        server_config.application_protocols = kProtocols;
        if (early != nullptr)
        {
            server_config.ticket_key = cont::ConstSpan<u8>(kTicketKey);
            REQUIRE(plain.init(server_config) == crypto::TlsError::None);
            server_config.early_data = true;
            server_config.early_data_filter = early;
        }
        REQUIRE(server.init(server_config) == crypto::TlsError::None);
    }
};

inline constexpr u8 kSeeds[4][crypto::kPrfKeyBytes] = {{0x01}, {0x02}, {0x03}, {0x04}};

inline QuicConnectionConfig config_for(crypto::QuicSide side, const Identity& identity, crypto::QuicVersion version,
                                       usize seed = 0U)
{
    QuicConnectionConfig config;
    config.side = side;
    config.version = version;
    config.tls = (side == crypto::QuicSide::Client) ? &identity.client : &identity.server;
    config.parameters.max_idle_timeout = 30000U;
    config.entropy_seed = cont::ConstSpan<u8>(kSeeds[seed % 4U], crypto::kPrfKeyBytes);
    return config;
}

// splitmix64: the seeded paths' impairments.
struct Rng
{
    u64 state = 0U;

    u64 next()
    {
        u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    bool chance(u32 permille) { return (next() % 1000U) < permille; }
};

// What a path does to a datagram's ECN codepoint (RFC 9000 section 13.4.2's faulty paths).
enum class EcnPath : u8
{
    Carry,      // as sent
    Bleach,     // cleared to Not-ECT
    Remark,     // each ECT re-marked as the other: ECT(0) as ECT(1), ECT(1) as ECT(0)
    DropMarked, // a datagram with any codepoint is dropped
};

// A path's impairments, each datagram decided by the seeded generator.
struct Path
{
    u32 loss = 0U;      // permille
    u32 duplicate = 0U; // permille
    u64 delay_ns = 10U * kMs;
    u64 jitter_ns = 0U; // a uniform extra delay, which reorders
    EcnPath ecn = EcnPath::Carry;
    u32 ce = 0U; // permille of datagrams with a codepoint marked CE, as a congested router's queue does
    usize mtu = 0U; // the largest datagram the path carries: a link that drops larger ones unreported (0: any)
    // Nonzero: the hop dropping a datagram over `mtu` answers with a Packet Too Big message quoting this many of its
    // first bytes and naming `too_big_limit` (0: `mtu`), handed to the sender at once.
    usize too_big_quote = 0U;
    u32 too_big_limit = 0U;
    // A bottleneck: datagrams leave one after another at `rate` bytes per second (0: at once), and one that finds
    // `buffer` bytes already queued is dropped (0: an unbounded queue), as a drop-tail router does.
    u64 rate = 0U;
    usize buffer = 0U;
    // With a rate: an ECT(1) datagram that finds more than `ce_queue` bytes queued is marked CE, the L4S queue's step
    // of a DualQ AQM (RFC 9332 section 2.4, 1 ms); classic traffic keeps the drop-tail queue. 0 marks none.
    usize ce_queue = 0U;
};

// A client's ticket store holding one ticket, handed out once.
class Tickets final : public crypto::TlsTicketStore
{
public:
    void keep(const crypto::TlsBinding& binding, cont::ConstSpan<u8> ticket) noexcept override
    {
        if (ticket.size() <= sizeof(m_ticket))
        {
            m_binding = binding;
            std::memcpy(m_ticket, ticket.data(), ticket.size());
            m_bytes = ticket.size();
        }
    }

    cont::ConstSpan<u8> take(const crypto::TlsBinding& binding) noexcept override
    {
        if (m_bytes == 0U || !(m_binding == binding))
        {
            return {};
        }
        std::memcpy(m_handed, m_ticket, m_bytes);
        const usize bytes = m_bytes;
        m_bytes = 0U;
        return cont::ConstSpan<u8>(m_handed, bytes);
    }

    [[nodiscard]] bool holds() const { return m_bytes != 0U; }

private:
    crypto::TlsBinding m_binding;
    u8 m_ticket[4096] = {};
    u8 m_handed[4096] = {};
    usize m_bytes = 0U;
};

// The window a ticket can be presented in: what the early data replay filter covers.
inline constexpr u64 kTicketPeriod = crypto::kTlsDefaultTicketLifetime + crypto::kTlsMaxTicketAgeSkew;

// A replay filter ready before the contexts that hold it are built.
struct ReadyFilter
{
    crypto::ReplayFilter filter;

    ReadyFilter()
    {
        REQUIRE(filter.init(1024U, kTicketPeriod, static_cast<u64>(std::time(nullptr))) == crypto::CryptoError::None);
    }
};

using cont::ConstSpan;
using cont::Span;
using crypto::CryptoError;
using crypto::QuicTokenKeys;
using crypto::QuicTokenPeer;

// The IDs a pair of test connections runs the handshake with: the server's, and the client's length.
inline constexpr u8 kNetServerCid[16] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
                                         0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F};
inline constexpr usize kNetClientCidBytes = 8U;

// Decides per datagram whether the path drops it beyond the random loss: `from` 0 is the client.
class Filter
{
public:
    virtual ~Filter() = default;
    virtual bool drop(usize from, ConstSpan<u8> datagram) = 0;

protected:
    Filter() = default;
    Filter(const Filter&) = default;
    Filter(Filter&&) noexcept = default;
    Filter& operator=(const Filter&) = default;
    Filter& operator=(Filter&&) noexcept = default;
};

// A client and a server joined by two impaired paths on a virtual clock. The server accepts the first client datagram.
class Net
{
public:
    static constexpr usize kFlights = 512U;

    Net(QuicConnection& client, QuicConnection& server)
    {
        m_ends[0] = &client;
        m_ends[1] = &server;
    }

    Path path[2];
    Rng rng;
    Filter* filter = nullptr;
    u64 now = kStartNs;
    usize sent[2] = {};
    usize dropped[2] = {};
    usize overflowed[2] = {}; // of those, dropped by a full bottleneck queue
    usize too_big[2] = {}; // Packet Too Big messages each end was handed, and those it matched
    usize too_big_matched[2] = {};
    usize marked[2] = {};                       // datagrams each end sent with a codepoint
    usize congested[2] = {};                    // of those, marked CE on the path
    u64 max_queued[2] = {};                     // the most bytes a bottleneck held when a datagram arrived
    bool client_initials_padded = true;         // every client datagram whose first packet is an Initial had 1200 bytes
    usize smallest[2] = {~usize{0}, ~usize{0}}; // each end's smallest datagram
    usize largest[2] = {};                      // and its largest
    bool amplification_kept = true; // the server never sent more than three times what it received unvalidated
    // A server behind an ingress: the first client datagrams are classified there, Retry and Version Negotiation
    // answered, and an admitted Initial accepted with what the ingress proved (a Retry token's original ID). `tokens`
    // mints the NEW_TOKEN token the server sends, for the version it runs once it has read the client's parameters.
    Ingress* ingress = nullptr;
    const QuicTokenKeys* tokens = nullptr;
    usize answers = 0U; // the ingress's answers sent toward the client
    u8 minted[kQuicMaxCarriedTokenBytes] = {};
    usize minted_bytes = 0U;

    QuicConnection& end(usize index) { return *m_ends[index]; }

    // Every datagram both ends have to send now goes on its path.
    void flush()
    {
        for (usize from = 0U; from < 2U; ++from)
        {
            for (usize guard = 0U; guard < 256U; ++guard)
            {
                u8 out[kDatagram];
                const usize size = m_ends[from]->send(Span<u8>(out, sizeof(out)), now);
                if (size == 0U)
                {
                    break;
                }
                ++sent[from];
                check(from, ConstSpan<u8>(out, size));
                Ecn ecn = m_ends[from]->marking();
                marked[from] += (ecn != Ecn::NotEct) ? 1U : 0U;
                const bool oversize = path[from].mtu != 0U && size > path[from].mtu;
                if (oversize && path[from].too_big_quote != 0U)
                {
                    const usize quoted = (size < path[from].too_big_quote) ? size : path[from].too_big_quote;
                    const u32 limit =
                        (path[from].too_big_limit != 0U) ? path[from].too_big_limit : static_cast<u32>(path[from].mtu);
                    ++too_big[from];
                    const bool matched = m_ends[from]->path_too_big(ConstSpan<u8>(out, quoted), limit, now);
                    too_big_matched[from] += matched ? 1U : 0U;
                }
                const bool lost = oversize || rng.chance(path[from].loss) ||
                                  (filter != nullptr && filter->drop(from, ConstSpan<u8>(out, size))) ||
                                  (path[from].ecn == EcnPath::DropMarked && ecn != Ecn::NotEct);
                if (lost)
                {
                    ++dropped[from];
                    continue;
                }
                ecn = on_path(from, ecn);
                enqueue(from, ConstSpan<u8>(out, size), ecn);
                if (rng.chance(path[from].duplicate))
                {
                    enqueue(from, ConstSpan<u8>(out, size), ecn);
                }
            }
        }
    }

    // Sends, then names the next arrival or timer.
    u64 next_event()
    {
        flush();
        u64 next = kNeverNs;
        for (usize index = 0U; index < m_count; ++index)
        {
            next = (m_flights[index].at < next) ? m_flights[index].at : next;
        }
        for (QuicConnection* const end : m_ends)
        {
            const u64 timer = end->next_timer_ns();
            next = (timer < next) ? timer : next;
        }
        return next;
    }

    // Moves time to `at` and runs every arrival and timer due. A timer that stays due while nothing arrives is a
    // connection that would spin its loop: the run fails.
    void run_at(u64 at)
    {
        m_stalled = (at <= now && m_count == 0U) ? m_stalled + 1U : 0U;
        REQUIRE(m_stalled < 64U);
        now = (at > now) ? at : now;
        for (usize index = 0U; index < m_count;)
        {
            if (m_flights[index].at <= now)
            {
                Flight flight = m_flights[index];
                m_flights[index] = m_flights[--m_count];
                deliver(flight);
                continue;
            }
            ++index;
        }
        for (QuicConnection* const end : m_ends)
        {
            if (end->next_timer_ns() <= now)
            {
                end->on_timeout(now);
            }
        }
    }

    // One event; false when nothing is left to happen.
    bool step()
    {
        const u64 next = next_event();
        if (next == kNeverNs)
        {
            return false;
        }
        run_at(next);
        return true;
    }

    // Runs every event up to `until` and stops the clock there.
    void advance(u64 until)
    {
        for (u64 next = next_event(); next <= until; next = next_event())
        {
            run_at(next);
        }
        now = (until > now) ? until : now;
    }

    template <typename Done> bool run_until(Done done, u64 limit_ns = 120000U * kMs)
    {
        const u64 until = now + limit_ns;
        flush();
        while (!done())
        {
            if (now > until || !step())
            {
                return done();
            }
        }
        return true;
    }

    [[nodiscard]] usize in_flight() const { return m_count; }

private:
    struct Flight
    {
        u8 bytes[kDatagram] = {};
        usize size = 0U;
        u64 at = 0U;
        usize to = 0U;
        Ecn ecn = Ecn::NotEct;
    };

    // The codepoint a datagram arrives with. The generator is drawn only on a CE-marking path, so other paths' runs
    // stay what they were.
    Ecn on_path(usize from, Ecn ecn)
    {
        if (ecn == Ecn::NotEct)
        {
            return ecn;
        }
        if (path[from].ecn == EcnPath::Bleach)
        {
            return Ecn::NotEct;
        }
        if (path[from].ecn == EcnPath::Remark)
        {
            ecn = (ecn == Ecn::Ect0) ? Ecn::Ect1 : ((ecn == Ecn::Ect1) ? Ecn::Ect0 : ecn);
        }
        if (path[from].ce != 0U && rng.chance(path[from].ce))
        {
            ++congested[from];
            return Ecn::Ce;
        }
        return ecn;
    }

    void check(usize from, ConstSpan<u8> datagram)
    {
        smallest[from] = (datagram.size() < smallest[from]) ? datagram.size() : smallest[from];
        largest[from] = (datagram.size() > largest[from]) ? datagram.size() : largest[from];
        QuicHeader header;
        const usize short_bytes = (from == 0U) ? sizeof(kNetServerCid) : kNetClientCidBytes;
        if (from == 0U && parse_quic_header(datagram, short_bytes, header) == QuicHeaderError::None &&
            header.type == QuicPacketType::Initial && datagram.size() < kQuicMinInitialDatagram)
        {
            client_initials_padded = false;
        }
        const QuicConnection& server = *m_ends[1];
        if (from == 1U && !server.address_validated() &&
            server.counters().bytes_sent > 3U * server.counters().bytes_received)
        {
            amplification_kept = false;
        }
    }

    void enqueue(usize from, ConstSpan<u8> datagram, Ecn ecn = Ecn::NotEct)
    {
        u64 departs = now;
        if (path[from].rate != 0U)
        {
            const u64 free = (m_link_free[from] > now) ? m_link_free[from] : now;
            const u64 queued = (free - now) * path[from].rate / (1000U * kMs);
            if (path[from].buffer != 0U && queued + datagram.size() > path[from].buffer)
            {
                ++dropped[from];
                ++overflowed[from];
                return;
            }
            m_link_free[from] = free + datagram.size() * 1000U * kMs / path[from].rate;
            departs = m_link_free[from];
            max_queued[from] = (queued > max_queued[from]) ? queued : max_queued[from];
            if (path[from].ce_queue != 0U && queued > path[from].ce_queue && ecn == Ecn::Ect1)
            {
                ecn = Ecn::Ce;
                ++congested[from];
            }
        }
        REQUIRE(m_count < kFlights);
        Flight& flight = m_flights[m_count++];
        std::memcpy(flight.bytes, datagram.data(), datagram.size());
        flight.size = datagram.size();
        const u64 jitter = (path[from].jitter_ns == 0U) ? 0U : rng.next() % path[from].jitter_ns;
        flight.at = departs + path[from].delay_ns + jitter;
        flight.to = 1U - from;
        flight.ecn = ecn;
    }

    // The ingress's verdict on a datagram for the idle server: true when the server accepted it.
    bool admit(const Flight& flight)
    {
        const IngressDecision decision =
            ingress->classify(ConstSpan<u8>(flight.bytes, flight.size), false, kClientPeer, now, kUnix);
        if (decision.action == IngressAction::Retry || decision.action == IngressAction::NegotiateVersion ||
            decision.action == IngressAction::InvalidToken)
        {
            u8 answer[kQuicMaxStatelessAnswerBytes];
            const usize size = ingress->answer(decision, kClientPeer, kUnix, Span<u8>(answer));
            REQUIRE(size != 0U);
            ++answers;
            enqueue(1U, ConstSpan<u8>(answer, size));
            return false;
        }
        if (decision.action != IngressAction::Admit)
        {
            return false;
        }
        QuicAccept accept;
        accept.local_cid = ConstSpan<u8>(kNetServerCid, sizeof(kNetServerCid));
        accept.client_dcid = decision.header.dcid;
        accept.client_scid = decision.header.scid;
        accept.retried = decision.claims.original_dcid_bytes != 0U;
        accept.original_dcid = accept.retried ? decision.claims.original_dcid_view() : decision.header.dcid;
        accept.validated = decision.validated;
        REQUIRE(m_ends[1]->accept(accept, now));
        return true;
    }

    void deliver(Flight& flight)
    {
        QuicConnection& to = *m_ends[flight.to];
        if (flight.to == 1U && to.state() == QuicConnectionState::Idle && ingress != nullptr && !admit(flight))
        {
            return;
        }
        if (flight.to == 1U && to.state() == QuicConnectionState::Idle)
        {
            QuicHeader header;
            REQUIRE(parse_quic_header(ConstSpan<u8>(flight.bytes, flight.size), sizeof(kNetServerCid), header) ==
                    QuicHeaderError::None);
            QuicAccept accept;
            accept.local_cid = ConstSpan<u8>(kNetServerCid, sizeof(kNetServerCid));
            accept.client_dcid = header.dcid;
            accept.client_scid = header.scid;
            accept.original_dcid = header.dcid;
            REQUIRE(to.accept(accept, now));
        }
        to.receive(Span<u8>(flight.bytes, flight.size), flight.ecn, now);
        if (flight.to == 1U && tokens != nullptr && minted_bytes == 0U && to.peer_parameters_known())
        {
            CHECK_FALSE(to.give_new_token({}));
            REQUIRE(tokens->mint_new_token(to.version(), QuicTokenPeer{kClientPeer.address.view(), 0U}, kUnix,
                                           Span<u8>(minted, sizeof(minted)), minted_bytes) == CryptoError::None);
            REQUIRE(to.give_new_token(ConstSpan<u8>(minted, minted_bytes)));
            CHECK_FALSE(to.give_new_token(ConstSpan<u8>(minted, minted_bytes)));
        }
    }

    QuicConnection* m_ends[2] = {};
    u64 m_link_free[2] = {}; // when each direction's bottleneck finishes what it holds
    Flight m_flights[kFlights];
    usize m_count = 0U;
    usize m_stalled = 0U;
};

} // namespace crd::net::testing
