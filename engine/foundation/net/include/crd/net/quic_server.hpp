#pragma once

// crd-net -- a QUIC server: the connections behind an I/O loop's ingress (4.2a; contract:
// docs/design/network-foundation.md#quic-server). Sources: RFC 9000 sections 5.2.2, 7.2, 8.1.3, 9, 10.2.3, 10.3 and
// 21.4, RFC 9001 section 4.1.4.
//
//   admit     an admitted client Initial takes a connection from a fixed pool: a fresh connection ID (sequence 0)
//             routed to it, the client's Destination Connection ID routed to it while it lives (the client's later
//             Initials and 0-RTT carry it, RFC 9000 section 7.2, and a late copy of its first Initial then reaches the
//             connection, which drops it once its Initial keys are gone, instead of opening a second one), accept()
//             and the datagram. When every connection is in use, or the connection refuses to start, the client is
//             answered with CONNECTION_REFUSED (Ingress::refuse, section 10.2.3).
//   0-RTT     0-RTT for no connection (IngressAction::Early) is held in a bounded ring, at most `early_per_client`
//             datagrams per Destination Connection ID and peer address, each for `early_lifetime_ns`, the oldest
//             evicted when the ring is full; the Initial that creates the connection reads them after itself
//             (section 5.2.2). A peer can make the server hold at most the ring (threat T10).
//   host      routing by peer address, serving under a deadline heap, a peer's stateless reset and closing are
//             QuicHost's (quic_host.hpp); every connection's parameters carry disable_active_migration until NET-PATH
//             validates migrations. A connection's service adds, after the application's on_service, its NEW_TOKEN
//             token once its version is final (QuicTokenKeys::mint_new_token for its address, section 8.1.3).
//   memory    the pool, the held ring, the heap and the send queue come from the injected allocator at construction;
//             a connection's own buffers come from it when the connection is admitted, so a host bounds them with its
//             allocator. Routing and serving allocate nothing.
//   threads   the loop's thread: on_service, wake() and connection() too.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_connection.hpp>
#include <crd/net/quic_host.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
inline constexpr u32 kQuicMaxEarlyDatagrams = 4096U;

struct QuicServerConfig
{
    // Every connection's configuration: side Server, with its TLS context and, in `versions`, every version the
    // ingress runs. The server sets `version` (the client Initial's), `ids` (its routed IDs) and
    // disable_active_migration; a non-empty `entropy_seed` is varied per connection (QuicHost::build). What its spans
    // view (versions, early_context) must outlive the server.
    QuicConnectionConfig connection;
    u32 connections = 256U;             // live connections at most (1 to kQuicMaxHostConnections)
    u32 early_datagrams = 16U;          // 0-RTT datagrams held before their Initial (0: none held)
    u32 early_per_client = 4U;          // of them per Destination Connection ID and address
    u32 early_bytes = 1500U;            // the largest held (the loop's slot_bytes); a longer one is dropped
    u64 early_lifetime_ns = 100000000U; // how long one is held (100 ms)
    u32 send_slots = 64U;               // datagrams waiting for the host at most
    u64 send_retry_ns = 1000000U;       // a connection the busy host held back is served again within 1 ms
    bool new_tokens = true;             // send each client a NEW_TOKEN token (with the server's token keys)
};

// The application's side, called on the loop's thread.
class QuicServerEvents : public QuicHostEvents
{
public:
    // A connection was admitted from `peer`, before it reads its first datagram.
    virtual void on_accept(QuicHandle handle, QuicConnection& connection, const Endpoint& peer) noexcept = 0;
};

struct QuicServerCounters : QuicHostCounters
{
    u64 admitted = 0U;
    u64 refused = 0U; // admitted Initials answered with CONNECTION_REFUSED (or dropped when its rate was spent)
    u64 early_held = 0U;
    u64 early_read = 0U;    // held 0-RTT datagrams read by their connection
    u64 early_dropped = 0U; // not held: no ring, the client's share spent or longer than early_bytes
    u64 early_evicted = 0U; // the oldest, when the ring was full
    u64 early_expired = 0U;
    u64 new_tokens = 0U;
};

class QuicServer final : public QuicHost
{
public:
    // `ingress` is a server's (Ingress::server()) over the loop's table `routes`, `reset` derives the stateless reset
    // tokens of the IDs the server issues, `tokens` mints NEW_TOKEN tokens (null: none sent). All must outlive the
    // server, and the loop that calls it must be destroyed first.
    QuicServer(memory::IAllocator& allocator, DatagramSocket& socket, const TransportClock& clock, Ingress& ingress,
               ConnectionIdTable& routes, const crypto::QuicResetKey& reset, const crypto::QuicTokenKeys* tokens,
               QuicServerEvents& events, const QuicServerConfig& config) noexcept;
    ~QuicServer() override;
    QuicServer(const QuicServer&) = delete;
    QuicServer& operator=(const QuicServer&) = delete;
    QuicServer(QuicServer&&) = delete;
    QuicServer& operator=(QuicServer&&) = delete;

    // False when the configuration is out of range (a client's connection configuration or one QuicConnection
    // refuses, counts out of bounds, a datagram size under 1200 bytes), the ingress is a client's or not ready, the
    // table or key is not ready, or the allocator refused.
    [[nodiscard]] bool ready() const noexcept { return m_admitted != nullptr; }

    void on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept override;
    void on_early(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept override;

    // Opens the raw UDP path for a client that holds the same secret (S55; its `local_id_bytes` is the ingress's
    // length): `client`'s address, or port 0 to take the address of the first of its packets that opens. No on_accept
    // follows: the caller holds the handle. `start.secret` is zeroed in every case. An invalid handle when every
    // connection is in use or QuicConnection::open_keyed refuses.
    [[nodiscard]] QuicHandle open_keyed(const Endpoint& client, QuicKeyedStart start, u64 now_ns) noexcept;

    [[nodiscard]] u32 early_held() const noexcept;
    [[nodiscard]] QuicServerCounters counters() const noexcept;

private:
    // What the server keeps per slot beside the host's.
    struct Admitted
    {
        QuicConnectionId client_dcid; // routed while `client_routed`
        bool client_routed = false;
        bool token_given = false;
    };

    struct Early
    {
        u64 arrived_ns = 0U;
        Endpoint peer;
        QuicConnectionId dcid;
        u32 bytes = 0U; // 0: a free or read entry
        Ecn ecn = Ecn::NotEct;
    };

    void service(u32 index, QuicConnection& connection, u64 now_ns) noexcept override;
    void release(u32 index, const QuicConnection& connection) noexcept override;
    void after_turn(u64 now_ns) noexcept override;

    void refuse(const IngressDecision& decision, const Endpoint& peer, u64 now_ns) noexcept;
    // The held 0-RTT datagrams of `dcid` from `peer`, read by the connection in `index` in arrival order.
    void read_early(u32 index, QuicConnection& connection, cont::ConstSpan<u8> dcid, u64 now_ns) noexcept;
    void expire_early(u64 now_ns) noexcept;

    const TransportClock& m_clock;
    Ingress& m_ingress;
    ConnectionIdTable& m_routes;
    const crypto::QuicTokenKeys* m_tokens = nullptr;
    QuicServerEvents& m_events;
    QuicServerConfig m_config;
    Admitted* m_admitted = nullptr; // one per slot
    Early* m_early = nullptr;       // a ring in arrival order
    u8* m_early_bytes = nullptr;
    u32 m_early_head = 0U;
    u32 m_early_count = 0U; // entries from the head, read ones among them
    QuicServerCounters m_counters;
};

} // namespace crd::net
