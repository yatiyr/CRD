#pragma once

// crd-net -- a QUIC client: its connections behind an I/O loop over one socket (4.2a; contract:
// docs/design/network-foundation.md#quic-client). Sources: RFC 9000 sections 5.2.1, 6.2, 7.2, 8.1.3 and 10.3, RFC
// 9368 sections 2.1 and 4, RFC 9369 section 5.
//
//   loop      the client's IoLoop runs a client's Ingress (S1 and S2 alone): every datagram goes to the connection its
//             Destination Connection ID routes to, so a client's connections, to one server or many, share one
//             socket. Each issues IDs of the ingress's length (8 to 20 bytes) through the loop's table.
//   connect   connect() takes a slot of a fixed pool and starts a connection to its server: a fresh sequence 0 ID
//             routed to it, a first Destination Connection ID of kQuicClientInitialDcidBytes from the provider's
//             randomness (section 7.2), and the token the client remembers for that server and version, if any.
//   tokens    a NEW_TOKEN token a connection receives is remembered for its server's address and port and the version
//             the connection ran (RFC 9369 section 5) in a fixed table, the oldest replaced when full, and carried by
//             the next connection there once: a client does not reuse a token (section 8.1.3).
//   tickets   unless the configuration names its own store, the client keeps the TLS session tickets its connections
//             receive in a TicketCache (ticket_cache.hpp), its connections' one correlation domain: a client meant to
//             be unlinkable from another shares neither its tickets nor its socket.
//   versions  a connection whose server answers with Version Negotiation listing a version of the client's
//             preference is replaced by one in that version under the same handle (RFC 9368 section 4), with
//             after_version_negotiation set so the downgrade check runs and a second Version Negotiation ends it;
//             QuicClientEvents::on_restart tells the application, whose writes to the first connection are gone.
//   host      routing by peer address, serving under a deadline heap, the server's stateless reset and closing are
//             QuicHost's (quic_host.hpp).
//   memory    the pool, the token table, the heap and the send queue come from the injected allocator at
//             construction; a connection's own buffers when connect() builds it. Routing and serving allocate
//             nothing.
//   threads   the loop's thread: connect(), the events, wake() and connection() too.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/ingress.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/quic_host.hpp>
#include <crd/net/ticket_cache.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
inline constexpr usize kQuicClientInitialDcidBytes = 16U; // 128 random bits (RFC 9000 section 7.2: at least 8)
inline constexpr u32 kQuicMaxClientTokens = 4096U;

struct QuicClientConfig
{
    // Every connection's configuration: side Client, with its TLS context (connect() may name another), its version
    // and, in `versions`, the client's preference. The client sets `ids`, `token` and after_version_negotiation; a
    // non-empty `entropy_seed` is varied per connection (QuicHost::build). What its spans view must outlive the client.
    // `tickets` left null is the client's own TicketCache.
    QuicConnectionConfig connection;
    u32 connections = 16U;        // live connections at most (1 to kQuicMaxHostConnections)
    u32 tokens = 16U;             // servers whose NEW_TOKEN token is remembered (0 to kQuicMaxClientTokens)
    u32 send_slots = 64U;         // datagrams waiting for the host at most
    u32 receive_bytes = 1500U;    // the loop's slot_bytes: what each connection advertises (QuicHostLimits)
    u64 send_retry_ns = 1000000U; // a connection the busy host held back is served again within 1 ms
    TicketCacheConfig tickets;    // the client's own store (entries 0: no resumption); unused with connection.tickets
};

// One connection to start.
struct QuicClientConnect
{
    Endpoint server;
    const crypto::TlsContext* tls = nullptr; // null: the configuration's (its server name and pins)
    // A token from elsewhere (an earlier process's NEW_TOKEN token for this server and version); empty: the one the
    // client remembers, if any.
    cont::ConstSpan<u8> token;
};

// The application's side, called on the loop's thread.
class QuicClientEvents : public QuicHostEvents
{
public:
    // Version Negotiation replaced the connection of `handle` by `connection`, in another version: what the
    // application wrote to the first one is gone and is written again (RFC 9368 section 4).
    virtual void on_restart(QuicHandle handle, QuicConnection& connection) noexcept = 0;
};

struct QuicClientCounters : QuicHostCounters
{
    u64 connected = 0U;
    u64 refused = 0U;     // connect() calls that started nothing
    u64 restarts = 0U;    // connections replaced after Version Negotiation
    u64 tokens_kept = 0U; // NEW_TOKEN tokens remembered
    u64 tokens_used = 0U; // remembered tokens a connection carried
    u64 unexpected = 0U;  // admissions and early datagrams a client's ingress cannot produce, dropped
};

class QuicClient final : public QuicHost
{
public:
    // `ingress` is a client's (!Ingress::server()) over the loop's table `routes`; `reset` derives the stateless reset
    // tokens of the IDs the client issues. All must outlive the client, and the loop that calls it must be destroyed
    // first.
    QuicClient(memory::IAllocator& allocator, DatagramSocket& socket, const Ingress& ingress, ConnectionIdTable& routes,
               const crypto::QuicResetKey& reset, QuicClientEvents& events, const QuicClientConfig& config) noexcept;
    ~QuicClient() override;
    QuicClient(const QuicClient&) = delete;
    QuicClient& operator=(const QuicClient&) = delete;
    QuicClient(QuicClient&&) = delete;
    QuicClient& operator=(QuicClient&&) = delete;

    // False when the configuration is out of range (a server's connection configuration or one QuicConnection
    // refuses, a version the ingress does not run, counts out of bounds, a datagram size under 1200 bytes,
    // receive_bytes over 65527), the ingress is a server's or not ready, the table is not ready, or the allocator
    // refused.
    [[nodiscard]] bool ready() const noexcept { return m_started != nullptr; }

    // Starts a connection to `request.server`, served in the loop's next turn (the application writes its first
    // streams in on_service). An invalid handle when every connection is in use, the server's port is 0, no ID can
    // be routed, the token is longer than kQuicMaxCarriedTokenBytes, or the connection refuses its configuration.
    [[nodiscard]] QuicHandle connect(const QuicClientConnect& request, u64 now_ns) noexcept;
    // Opens the raw UDP path to `server` on `start` (S55; its `local_id_bytes` is the ingress's length), from the
    // configuration's limits; `start.secret` is zeroed in every case. An invalid handle when every connection is in
    // use, the server's port is 0, or QuicConnection::open_keyed refuses.
    [[nodiscard]] QuicHandle open_keyed(const Endpoint& server, QuicKeyedStart start, u64 now_ns) noexcept;

    // The token remembered for `server` and `version`; empty for none (tests and diagnostics: it stays remembered).
    [[nodiscard]] cont::ConstSpan<u8> token_for(const Endpoint& server, crypto::QuicVersion version) const noexcept;
    [[nodiscard]] QuicClientCounters counters() const noexcept;
    // The client's own store (empty when the configuration names another).
    [[nodiscard]] const TicketCache& tickets() const noexcept { return m_tickets; }

    void on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept override;
    void on_early(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept override;

private:
    // What the client keeps per slot beside the host's.
    struct Started
    {
        const crypto::TlsContext* tls = nullptr;
        bool after_version_negotiation = false;
        bool token_kept = false; // its NEW_TOKEN token is remembered
    };

    // A remembered NEW_TOKEN token.
    struct Kept
    {
        Endpoint server;
        u64 stamp = 0U; // 0: a free entry; else the order it was kept in
        u32 version = 0U;
        u16 bytes = 0U;
        u8 token[kQuicMaxCarriedTokenBytes] = {};
    };

    void service(u32 index, QuicConnection& connection, u64 now_ns) noexcept override;
    [[nodiscard]] bool replace(u32 index, QuicConnection& connection, u64 now_ns) noexcept override;
    void release(u32 index, const QuicConnection& connection) noexcept override;
    void after_turn(u64 now_ns) noexcept override;

    // Builds and starts a connection for slot `index`; null when it refuses.
    [[nodiscard]] QuicConnection* start(u32 index, const Endpoint& server, const QuicConnectionConfig& config,
                                        cont::ConstSpan<u8> token, u64 now_ns) noexcept;
    void keep_token(const Endpoint& server, const QuicConnection& connection) noexcept;
    // The entry of `server` and `version`; `tokens` for none.
    [[nodiscard]] u32 find_token(const Endpoint& server, u32 version) const noexcept;

    const Ingress& m_ingress;
    QuicClientEvents& m_events;
    QuicClientConfig m_config;
    TicketCache m_tickets;
    Started* m_started = nullptr; // one per slot
    Kept* m_kept = nullptr;       // `tokens` entries
    u64 m_stamp = 0U;
    QuicClientCounters m_counters;
};

} // namespace crd::net
