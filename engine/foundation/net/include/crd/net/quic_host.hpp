#pragma once

// crd-net -- what the two QUIC hosts share, the server (quic_server.hpp) and the client (quic_client.hpp): the
// connections behind an I/O loop (4.2a; contract: docs/design/network-foundation.md#quic-client). Sources: RFC 9000
// sections 9 and 10.3.
//
//   pool      a fixed pool of connections with a free list. A handle carries its slot's generation, so a stale one
//             finds nothing. The connections' storage holds one more than the pool: the spare, where a connection is
//             built before it goes live, and where a client's successor is built while its predecessor still stands
//             (after Version Negotiation), so a refused successor leaves the predecessor to be reported.
//   route     a connection reads datagrams from its peer's address only: another address would be a migration or a
//             NAT rebinding, which NET-PATH validates (section 9); until then they are dropped and counted. A keyed
//             server's slot opened with no peer takes the address of the first packet that opens (S55).
//   service   a connection that received, whose timer is due or that the application woke is served in on_timers,
//             once per turn: its timeout, the host's service (the application's turn), then its datagrams into the
//             host's SendQueue until it has none or the host is busy (served again within `send_retry_ns`). The
//             deadlines are a binary min-heap, so a turn costs O(log n) per connection served, never a scan.
//   reset     a short header for no connection is looked up by its last 16 bytes among every connection's live peer
//             tokens, a table under a secret SipHash key whose walk says nothing of a token's value, and checked as a
//             stateless reset by the connection the token belongs to, in constant time and only from that
//             connection's peer address (sections 10.3.1 and 21.11).
//   close     a Closed connection is reported (QuicHostEvents::on_closed), destroyed (its IDs leave the table) and
//             its slot reused under a new generation.
//   memory    the pool, the heap and the send queue come from the injected allocator at construction; a connection's
//             own buffers come from it when the connection is built, so a host bounds them with its allocator.
//             Routing and serving allocate nothing.
//   threads   the loop's thread: the events, wake() and connection() too.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/deadline_heap.hpp>
#include <crd/net/host_handle.hpp>
#include <crd/net/io_loop.hpp>
#include <crd/net/quic_connection.hpp>

namespace crd::memory
{
class IAllocator;
}

namespace crd::net
{
// The peer tokens a connection can have live: one per peer ID it holds.
inline constexpr u32 kQuicHostTokensPerConnection = static_cast<u32>(kQuicMaxPeerConnectionIds);

// The application's side of either host, called on the loop's thread.
class QuicHostEvents
{
public:
    virtual ~QuicHostEvents() = default;
    // The connection received, timed out or was woken: the application reads and writes its streams and datagrams;
    // what it writes is sent right after.
    virtual void on_service(QuicHandle handle, QuicConnection& connection, u64 now_ns) noexcept = 0;
    // The connection is Closed and is destroyed after this call.
    virtual void on_closed(QuicHandle handle, const QuicConnection& connection) noexcept = 0;

protected:
    QuicHostEvents() = default;
    QuicHostEvents(const QuicHostEvents&) = default;
    QuicHostEvents(QuicHostEvents&&) noexcept = default;
    QuicHostEvents& operator=(const QuicHostEvents&) = default;
    QuicHostEvents& operator=(QuicHostEvents&&) noexcept = default;
};

// What a host's configuration gives the pool.
struct QuicHostLimits
{
    u32 connections = 0U;      // live connections at most (1 to kQuicMaxHostConnections)
    u32 send_slots = 0U;       // datagrams waiting for the host at most
    usize datagram_bytes = 0U; // the largest datagram a connection sends
    u64 send_retry_ns = 0U;    // a connection the busy host held back is served again within this
};

struct QuicHostCounters
{
    u64 closed = 0U;
    u64 foreign_path = 0U; // datagrams routed to a connection from another address (NET-PATH's migration)
    u64 peer_resets = 0U;  // a peer's stateless reset ended a connection
    u64 datagrams_sent = 0U;
    u64 send_dropped = 0U; // datagrams the host refused for good
};

class QuicHost : public IngressHandler
{
public:
    ~QuicHost() override;
    QuicHost(const QuicHost&) = delete;
    QuicHost& operator=(const QuicHost&) = delete;
    QuicHost(QuicHost&&) = delete;
    QuicHost& operator=(QuicHost&&) = delete;

    void on_route(const IngressDecision& decision, const ReceivedDatagram& datagram, u64 now_ns) noexcept final;
    [[nodiscard]] bool on_unrouted(const IngressDecision& decision, const ReceivedDatagram& datagram,
                                   u64 now_ns) noexcept final;
    [[nodiscard]] u64 on_timers(u64 now_ns) noexcept final;

    // The live connection `handle` names; null for a stale or unknown handle.
    [[nodiscard]] QuicConnection* connection(QuicHandle handle) noexcept;
    // Serves the connection in the next turn (the application wrote to it outside on_service).
    void wake(QuicHandle handle) noexcept;

    [[nodiscard]] u32 live() const noexcept { return m_live; }
    [[nodiscard]] u32 queued() const noexcept { return m_queue.queued(); }
    // The peer tokens the host matches stateless resets against (diagnostics and tests).
    [[nodiscard]] u32 live_tokens() const noexcept { return m_peer_tokens.size(); }

protected:
    // `routes` is the loop's table, `reset` derives the stateless reset tokens of the IDs the host issues; both and
    // `events` outlive the host. Nothing is allocated before open_pool().
    QuicHost(memory::IAllocator& allocator, DatagramSocket& socket, ConnectionIdTable& routes,
             const crypto::QuicResetKey& reset, QuicHostEvents& events, const QuicHostLimits& limits) noexcept;

    // Allocates the pool (a derived host's constructor calls it once its own checks passed); false, holding nothing,
    // when the limits are out of range, the table or queue is not ready, or the allocator refused.
    [[nodiscard]] bool open_pool() noexcept;
    [[nodiscard]] bool pool_open() const noexcept { return m_slots != nullptr; }
    [[nodiscard]] memory::IAllocator& allocator() noexcept { return m_allocator; }

    // The slot a new connection takes; kNoConnection when every one is in use.
    [[nodiscard]] u32 free_slot() const noexcept { return m_free; }
    // The routed IDs of slot `index`: a new connection issues its sequence 0 through them before it starts.
    [[nodiscard]] QuicRoutedIds& ids(u32 index) noexcept { return m_ids[index]; }
    // Opens a keyed connection (QuicConnection::open_keyed, the handshake of `config` dropped) to `peer` in a free
    // slot, its sequence 0 ID routed first; an invalid handle when the pool is full, that ID cannot be routed or the
    // connection refuses it. A `peer` of port 0 stays unbound until a packet that opens names it. `start.secret` is
    // zeroed in every case.
    [[nodiscard]] QuicHandle start_keyed(QuicConnectionConfig config, const Endpoint& peer, const QuicKeyedStart& start,
                                         u64 now_ns) noexcept;
    // Builds a connection of `config` for slot `index` in the spare storage, its further IDs routed to the slot and a
    // non-empty entropy seed varied by the count of connections built (a deterministic run stays deterministic
    // without two connections drawing alike); null, destroying it, when it is not ready.
    [[nodiscard]] QuicConnection* build(u32 index, QuicConnectionConfig config) noexcept;
    // Destroys a built connection that went live nowhere.
    void discard(QuicConnection& connection) noexcept;
    // A built connection of the free slot `index` goes live for `peer` and is served now.
    void enlist(u32 index, QuicConnection& connection, const Endpoint& peer, u64 now_ns) noexcept;
    // From replace(): slot `index`'s Closed connection is destroyed and `successor`, built for it, takes its place
    // under the same handle and peer, served now.
    void succeed(u32 index, QuicConnection& successor, u64 now_ns) noexcept;

    [[nodiscard]] QuicHandle handle_of(u32 index) const noexcept;
    [[nodiscard]] const Endpoint& peer_of(u32 index) const noexcept;
    // The live connection in `index`; null for none.
    [[nodiscard]] QuicConnection* live_connection(u32 index) noexcept;
    void schedule(u32 index, u64 due_ns) noexcept;
    // Queues one datagram the host writes itself (a server's refusal): room() flushes once when the queue is full.
    [[nodiscard]] cont::Span<u8> room() noexcept;
    void push(usize bytes, const Endpoint& peer) noexcept;
    [[nodiscard]] const QuicHostCounters& host_counters() const noexcept { return m_counters; }
    [[nodiscard]] QuicHostEvents& events() noexcept { return m_events; }

private:
    struct Slot
    {
        QuicConnection* connection = nullptr; // live: in `cell`
        QuicConnection* cell = nullptr;       // the storage this slot holds
        Endpoint peer;
        u32 generation = 0U;
        u32 next_free = kNoConnection;
    };

    // Serves the connection of slot `index` before its datagrams are sent: the application's turn and what the host
    // adds to it.
    virtual void service(u32 index, QuicConnection& connection, u64 now_ns) noexcept = 0;
    // Slot `index`'s connection is Closed: true when the host replaced it through succeed(), so it is not reported.
    [[nodiscard]] virtual bool replace(u32 index, QuicConnection& connection, u64 now_ns) noexcept;
    // Slot `index`'s connection was reported closed and is destroyed next: the host lets go of what it keeps for it.
    virtual void release(u32 index, const QuicConnection& connection) noexcept;
    // Every turn, after the due connections were served and the queue flushed.
    virtual void after_turn(u64 now_ns) noexcept;

    void serve(u32 index, u64 now_ns) noexcept;
    // Whether the connection's datagrams are all queued (false: the host is busy and the queue full).
    [[nodiscard]] bool drain(Slot& slot, u64 now_ns) noexcept;
    void flush() noexcept;
    void reclaim(u32 index, u64 now_ns) noexcept;

    memory::IAllocator& m_allocator;
    DatagramSocket& m_socket;
    ConnectionIdTable& m_routes;
    const crypto::QuicResetKey& m_reset;
    QuicHostEvents& m_events;
    QuicHostLimits m_limits;
    ConnectionIdTable m_peer_tokens; // every connection's live stateless reset tokens to its slot
    SendQueue m_queue;
    DeadlineHeap m_heap; // the live connections by their deadlines
    Slot* m_slots = nullptr;
    QuicConnection* m_storage = nullptr; // uninitialised storage for `connections` + 1
    QuicConnection* m_spare = nullptr;   // the cell no slot holds
    QuicRoutedIds* m_ids = nullptr;      // one per slot
    u32* m_due = nullptr; // the slots one turn serves
    u32 m_free = kNoConnection;
    u32 m_live = 0U;
    u64 m_built = 0U;
    QuicHostCounters m_counters;
};

} // namespace crd::net
