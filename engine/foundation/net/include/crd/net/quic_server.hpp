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
//   route     a connection reads datagrams from its peer's address only: another address would be a migration or a
//             NAT rebinding, which NET-PATH validates (section 9); until then they are dropped and counted, and every
//             connection's parameters carry disable_active_migration.
//   service   a connection that received, whose timer is due or that the application woke is served in on_timers,
//             once per turn: its timeout, the application's QuicServerEvents::on_service, its NEW_TOKEN token once its
//             version is final (QuicTokenKeys::mint_new_token for its address, section 8.1.3), then its datagrams
//             into the server's SendQueue until it has none or the host is busy. The connections' deadlines are a
//             binary min-heap, so a turn costs O(log n) per connection served, never a scan of the pool.
//   reset     a short header for no connection from the address of a connection is checked as that peer's stateless
//             reset (RFC 9000 section 10.3.1) before the ingress answers it with its own; an address is indexed by
//             the first of its connections.
//   close     a Closed connection is reported (on_closed), destroyed (its IDs leave the table) and its slot reused;
//             a handle carries its slot's generation, so a stale one finds nothing.
//   memory    the pool, the held ring, the heap and the send queue come from the injected allocator at construction;
//             a connection's own buffers come from it when the connection is admitted (QuicConnection's
//             constructor), so a host bounds them with its allocator. Routing and serving allocate nothing.
//   threads   the loop's thread: on_service, wake() and connection() too.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_connection.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
inline constexpr u32 kQuicMaxServerConnections = u32{1} << 20U;
inline constexpr u32 kQuicMaxEarlyDatagrams = 4096U;

struct QuicServerConfig
{
    // Every connection's configuration: side Server, with its TLS context and, in `versions`, every version the
    // ingress runs. The server sets `version` (the client Initial's), `ids` (its routed IDs) and
    // disable_active_migration; a non-empty `entropy_seed` is varied per connection by its admission count, so a
    // deterministic run (tests, NET-SIM) stays deterministic without two connections drawing alike. What its spans
    // view (versions, early_context) must outlive the server.
    QuicConnectionConfig connection;
    u32 connections = 256U;             // live connections at most (1 to kQuicMaxServerConnections)
    u32 early_datagrams = 16U;          // 0-RTT datagrams held before their Initial (0: none held)
    u32 early_per_client = 4U;          // of them per Destination Connection ID and address
    u32 early_bytes = 1500U;            // the largest held (the loop's slot_bytes); a longer one is dropped
    u64 early_lifetime_ns = 100000000U; // how long one is held (100 ms)
    u32 send_slots = 64U;               // datagrams waiting for the host at most
    u64 send_retry_ns = 1000000U;       // a connection the busy host held back is served again within 1 ms
    bool new_tokens = true;             // send each client a NEW_TOKEN token (with the server's token keys)
};

// A connection as the application holds it.
struct QuicServerHandle
{
    u32 slot = kNoConnection;
    u32 generation = 0U;

    friend bool operator==(const QuicServerHandle&, const QuicServerHandle&) noexcept = default;
};

// The application's side, called on the loop's thread.
class QuicServerEvents
{
public:
    virtual ~QuicServerEvents() = default;
    // A connection was admitted from `peer`, before it reads its first datagram.
    virtual void on_accept(QuicServerHandle handle, QuicConnection& connection, const Endpoint& peer) noexcept = 0;
    // The connection received, timed out or was woken: the application reads and writes its streams and datagrams;
    // what it writes is sent right after.
    virtual void on_service(QuicServerHandle handle, QuicConnection& connection, u64 now_ns) noexcept = 0;
    // The connection is Closed and is destroyed after this call.
    virtual void on_closed(QuicServerHandle handle, const QuicConnection& connection) noexcept = 0;

protected:
    QuicServerEvents() = default;
    QuicServerEvents(const QuicServerEvents&) = default;
    QuicServerEvents(QuicServerEvents&&) noexcept = default;
    QuicServerEvents& operator=(const QuicServerEvents&) = default;
    QuicServerEvents& operator=(QuicServerEvents&&) noexcept = default;
};

struct QuicServerCounters
{
    u64 admitted = 0U;
    u64 refused = 0U; // admitted Initials answered with CONNECTION_REFUSED (or dropped when its rate was spent)
    u64 closed = 0U;
    u64 foreign_path = 0U; // datagrams routed to a connection from another address (NET-PATH's migration)
    u64 early_held = 0U;
    u64 early_read = 0U;    // held 0-RTT datagrams read by their connection
    u64 early_dropped = 0U; // not held: no ring, the client's share spent or longer than early_bytes
    u64 early_evicted = 0U; // the oldest, when the ring was full
    u64 early_expired = 0U;
    u64 peer_resets = 0U; // a peer's stateless reset ended a connection
    u64 new_tokens = 0U;
    u64 datagrams_sent = 0U;
    u64 send_dropped = 0U; // datagrams the host refused for good
};

class QuicServer final : public IngressHandler
{
public:
    // `routes` is the loop's table (the one `ingress` reads), `reset` derives the stateless reset tokens of the IDs
    // the server issues, `tokens` mints NEW_TOKEN tokens (null: none sent). All must outlive the server, and the loop
    // that calls it must be destroyed first.
    QuicServer(memory::IAllocator& allocator, DatagramSocket& socket, const TransportClock& clock, Ingress& ingress,
               ConnectionIdTable& routes, const crypto::QuicResetKey& reset, const crypto::QuicTokenKeys* tokens,
               QuicServerEvents& events, const QuicServerConfig& config) noexcept;
    ~QuicServer() override;
    QuicServer(const QuicServer&) = delete;
    QuicServer& operator=(const QuicServer&) = delete;
    QuicServer(QuicServer&&) = delete;
    QuicServer& operator=(QuicServer&&) = delete;

    // False when the configuration is out of range (a client's connection configuration or one QuicConnection
    // refuses, counts out of bounds, a datagram size under 1200 bytes), the ingress, table or key is not ready, or
    // the allocator refused.
    [[nodiscard]] bool ready() const noexcept { return m_slots != nullptr; }

    void on_route(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept override;
    void on_admit(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept override;
    void on_early(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept override;
    [[nodiscard]] bool on_unrouted(const IngressDecision& decision, const ReceivedDatagram& datagram,
                                   u64 now_ns) noexcept override;
    [[nodiscard]] u64 on_timers(u64 now_ns) noexcept override;

    // The live connection `handle` names; null for a stale or unknown handle.
    [[nodiscard]] QuicConnection* connection(QuicServerHandle handle) noexcept;
    // Serves the connection in the next turn (the application wrote to it outside on_service).
    void wake(QuicServerHandle handle) noexcept;

    [[nodiscard]] u32 live() const noexcept { return m_live; }
    [[nodiscard]] u32 early_held() const noexcept;
    [[nodiscard]] u32 queued() const noexcept { return m_queue.queued(); }
    [[nodiscard]] const QuicServerCounters& counters() const noexcept { return m_counters; }

private:
    static constexpr u32 kNotQueued = ~u32{0};

    struct Slot
    {
        QuicConnection* connection = nullptr; // in the pool's storage while live
        Endpoint peer;
        u64 due_ns = kNeverNs;
        u32 generation = 0U;
        u32 heap = kNotQueued; // its place in the heap
        u32 next_free = kNoConnection;
        QuicConnectionId client_dcid; // routed while `client_routed` (until reclaimed)
        bool client_routed = false;
        bool peer_indexed = false;
        bool token_given = false;
        u8 seed[crypto::kPrfKeyBytes] = {};
    };

    struct Early
    {
        u64 arrived_ns = 0U;
        Endpoint peer;
        QuicConnectionId dcid;
        u32 bytes = 0U; // 0: a free or read entry
    };

    [[nodiscard]] Slot* live_slot(u32 index) noexcept;
    [[nodiscard]] QuicServerHandle handle_of(u32 index) const noexcept;
    void refuse(const IngressDecision& decision, const Endpoint& peer, u64 now_ns) noexcept;
    // The held 0-RTT datagrams of `dcid` from `peer`, read by the connection in `index` in arrival order.
    void read_early(u32 index, cont::ConstSpan<u8> dcid, u64 now_ns) noexcept;
    void expire_early(u64 now_ns) noexcept;
    void serve(u32 index, u64 now_ns) noexcept;
    // Whether the connection's datagrams are all queued (false: the host is busy and the queue full).
    [[nodiscard]] bool drain(Slot& slot, u64 now_ns) noexcept;
    void flush() noexcept;
    void reclaim(u32 index) noexcept;

    // The heap of live connections by due_ns.
    void schedule(u32 index, u64 due_ns) noexcept;
    void unschedule(u32 index) noexcept;
    void sift_up(u32 place) noexcept;
    void sift_down(u32 place) noexcept;
    void put(u32 place, u32 index) noexcept;

    memory::IAllocator& m_allocator;
    DatagramSocket& m_socket;
    const TransportClock& m_clock;
    Ingress& m_ingress;
    ConnectionIdTable& m_routes;
    const crypto::QuicTokenKeys* m_tokens = nullptr;
    QuicServerEvents& m_events;
    QuicServerConfig m_config;
    ConnectionIdTable m_by_peer; // a peer address and port to the first of its connections
    SendQueue m_queue;
    Slot* m_slots = nullptr;
    QuicConnection* m_pool = nullptr; // uninitialised storage for `connections`
    QuicRoutedIds* m_ids = nullptr;   // one per slot
    u32* m_heap = nullptr;
    u32* m_due = nullptr;     // the slots one turn serves
    Early* m_early = nullptr; // a ring in arrival order
    u8* m_early_bytes = nullptr;
    u32 m_early_head = 0U;
    u32 m_early_count = 0U; // entries from the head, read ones among them
    u32 m_heap_size = 0U;
    u32 m_free = kNoConnection;
    u32 m_live = 0U;
    u64 m_admissions = 0U;
    QuicServerCounters m_counters;
};

} // namespace crd::net
