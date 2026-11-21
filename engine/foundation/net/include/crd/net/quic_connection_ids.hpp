#pragma once
// crd-net -- a QUIC connection's connection IDs (NET-PATH; contract: docs/design/network-foundation.md#multipath).
//
// QuicCids holds both ends' connection IDs (RFC 9000 section 5.1) in one sequence space per path ID (RFC 10069
// section 3.2): this side's, which the host routes and the peer sends to, and the peer's, which this side sends to.
//   spaces     path ID 0's space is held inline and open from the start. A connection that uses multipath opens one
//              for each further path ID (open()) on storage it holds (allocated at construction), at most
//              kQuicMaxPaths at once; close() gives it back. Each space numbers its IDs both ways from 0, keeps its own
//              Retire Prior To both ways, and holds up to its active_connection_id_limit (per path: RFC 10069
//              section 2). An ID is unique in the connection whatever its path ID (section 3.2.1).
//   peer's     a repeated frame changes nothing; one ID under two sequences or path IDs, or one stateless reset token
//              under two IDs, is PROTOCOL_VIOLATION (RFC 9000 sections 10.3.2 and 19.15); past the limit is
//              CONNECTION_ID_LIMIT_ERROR. A raised Retire Prior To retires every ID below it, and the one sent to is
//              replaced by the frame's. An ID retired here alone (a path forgotten) is not held again when its frame
//              repeats. Retirements owed and in flight stay within twice the limit (section 5.1.2), resent when lost.
//              A token counts for a stateless reset once its ID was sent to, until the ID is retired (section 10.3.1):
//              the host watches it (QuicConnectionIds::watch_reset) for that time.
//   this side's come from the host with their tokens, owed to the peer until acknowledged (section 13.3), topped up
//              to the peer's limit in every open space; a rotation retires a space's IDs with Retire Prior To; the
//              peer's retirement may not name an ID not issued or the one its packet was sent to (section 19.16).
//              A datagram's Destination Connection ID names its space (find()), so its packet number decodes in the
//              path ID's space and its protection slot is that path's (S78).
//   memory     nothing is allocated; a space holds its IDs inline (sizeof(QuicCidSpace)), and the 16 latest IDs this
//              side retired in it, so that a repeated frame is not held again.
//   threads    the connection's, on its loop's thread.

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/crypto/quic.hpp>
#include <crd/net/connection_table.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quic_transport_parameters.hpp>

namespace crd::net
{
inline constexpr usize kQuicMaxPeerConnectionIds = 8U;  // the peer's IDs a space holds at most
inline constexpr usize kQuicMaxLocalConnectionIds = 8U; // this side's active IDs a space holds at most
inline constexpr u64 kQuicNoSequence = ~u64{0};         // no connection ID

// The host's side of a connection's own connection IDs: it routes each one to the connection and derives its
// stateless reset token from its QuicResetKey (RFC 9000 section 10.3.2), so that it can still reset a connection
// whose state it lost. Called on the connection's thread; it outlives the connection.
class QuicConnectionIds
{
public:
    virtual ~QuicConnectionIds() = default;
    // A fresh ID of `id.size()` bytes (sequence 0's length) from at least 128 random bits, now routed to the
    // connection, and its token into `token` (kQuicResetTokenBytes). False when none can be routed.
    [[nodiscard]] virtual bool issue(cont::Span<u8> id, cont::Span<u8> token) noexcept = 0;
    // The token of an ID the host routes already: a server's sequence 0, sent as stateless_reset_token.
    [[nodiscard]] virtual bool token(cont::ConstSpan<u8> id, cont::Span<u8> token) noexcept = 0;
    // Stops routing `id`: the peer retired it, or the connection closed (every ID it holds, sequence 0 included).
    virtual void retire(cont::ConstSpan<u8> id) noexcept = 0;
    // The peer's stateless reset `token` is live: the connection has sent to its ID, which is not retired (RFC 9000
    // section 10.3.1). A host that matches datagrams for no connection against tokens keeps it until unwatch_reset().
    virtual void watch_reset(cont::ConstSpan<u8> token) noexcept;
    virtual void unwatch_reset(cont::ConstSpan<u8> token) noexcept;

protected:
    QuicConnectionIds() = default;
    QuicConnectionIds(const QuicConnectionIds&) = default;
    QuicConnectionIds(QuicConnectionIds&&) noexcept = default;
    QuicConnectionIds& operator=(const QuicConnectionIds&) = default;
    QuicConnectionIds& operator=(QuicConnectionIds&&) noexcept = default;
};

// The host side over a loop's ConnectionIdTable and its endpoint's QuicResetKey: IDs drawn from the provider's
// randomness and routed to `connection`, and, in `peer_tokens` when given, the connection's live peer tokens.
class QuicRoutedIds final : public QuicConnectionIds
{
public:
    QuicRoutedIds(ConnectionIdTable& routes, const crypto::QuicResetKey& reset, u32 connection,
                  ConnectionIdTable* peer_tokens = nullptr) noexcept
        : m_routes(routes), m_reset(reset), m_peer_tokens(peer_tokens), m_connection(connection)
    {
    }
    [[nodiscard]] bool issue(cont::Span<u8> id, cont::Span<u8> token) noexcept override;
    [[nodiscard]] bool token(cont::ConstSpan<u8> id, cont::Span<u8> token) noexcept override;
    void retire(cont::ConstSpan<u8> id) noexcept override;
    void watch_reset(cont::ConstSpan<u8> token) noexcept override;
    void unwatch_reset(cont::ConstSpan<u8> token) noexcept override;

private:
    ConnectionIdTable& m_routes;
    const crypto::QuicResetKey& m_reset;
    ConnectionIdTable* m_peer_tokens = nullptr;
    u32 m_connection = kNoConnection;
};

// One of the peer's connection IDs.
struct QuicPeerCid
{
    u64 sequence = 0U;
    QuicConnectionId id;
    u8 reset_token[crypto::kQuicResetTokenBytes] = {};
    bool has_token = false;
    bool used = false; // a packet was sent to it: its token is checked (section 10.3.1)
};

// One of this side's connection IDs.
struct QuicLocalCid
{
    u64 sequence = 0U;
    QuicConnectionId id;
    u8 reset_token[crypto::kQuicResetTokenBytes] = {};
    bool owed = false;  // its NEW_CONNECTION_ID (path 0's) or PATH_NEW_CONNECTION_ID frame waits to be sent
    bool acked = false; // the peer acknowledged it (path 0's sequence 0: the handshake carried it)
};

// This side's ID of `sequence` in path ID `path_id`'s space; kQuicNoSequence for none.
struct QuicCidRef
{
    u64 path_id = 0U;
    u64 sequence = kQuicNoSequence;

    [[nodiscard]] bool found() const noexcept { return sequence != kQuicNoSequence; }
};

// One path ID's IDs both ways: storage that QuicCids alone reads and writes.
class QuicCidSpace
{
private:
    friend class QuicCids;

    enum class Retirement : u8
    {
        Done,     // acknowledged: kept only so that a repeated frame is not held again
        Owed,     // its frame waits to be sent
        InFlight, // sent, not yet acknowledged
    };
    struct Retired
    {
        u64 sequence = kQuicNoSequence;
        Retirement state = Retirement::Done;
    };

    u64 m_path_id = 0U;
    bool m_open = false;
    QuicPeerCid m_peer[kQuicMaxPeerConnectionIds];
    usize m_peer_count = 0U;
    u64 m_current = kQuicNoSequence; // the peer ID this path sends to
    u64 m_retire_prior_to = 0U;      // the peer's
    Retired m_retired[2U * kQuicMaxPeerConnectionIds];
    QuicLocalCid m_local[2U * kQuicMaxLocalConnectionIds]; // the active ones and those a rotation retires
    usize m_local_count = 0U;
    u64 m_local_next = 0U;
    u64 m_local_retire_prior_to = 0U; // this side's
};

class QuicCids
{
public:
    // `host` routes this side's further IDs (null: path 0's sequence 0 alone, and no tokens watched).
    explicit QuicCids(QuicConnectionIds* host = nullptr) noexcept;
    QuicCids(const QuicCids&) = delete;
    QuicCids& operator=(const QuicCids&) = delete;
    QuicCids(QuicCids&&) = delete;
    QuicCids& operator=(QuicCids&&) = delete;
    ~QuicCids() = default;

    // ---- Spaces ----
    // Opens path ID `path_id`'s space on `storage`, empty both ways. False, changing nothing, when it is open already,
    // kQuicMaxPaths are open, or `path_id` is past 2^32 - 1 (RFC 10069 section 2.1).
    [[nodiscard]] bool open(u64 path_id, QuicCidSpace& storage) noexcept;
    // Closes `path_id`'s space: the host stops routing its own IDs and the peer's tokens there count no more (RFC
    // 10069 section 3.4: abandoning a path retires its IDs both ways). Its storage, for a later open(); null when it
    // is not open.
    [[nodiscard]] QuicCidSpace* close(u64 path_id) noexcept;
    [[nodiscard]] bool is_open(u64 path_id) const noexcept { return space(path_id) != nullptr; }

    // ---- The peer's IDs ----
    // Path ID 0's sequence 0, the Source Connection ID of the peer's long headers, with no token yet: the one sent to.
    void first_peer(cont::ConstSpan<u8> id) noexcept;
    // A NEW_CONNECTION_ID frame (path ID 0) or PATH_NEW_CONNECTION_ID frame for an open space, under `limit` (this
    // side's active_connection_id_limit, at most kQuicMaxPeerConnectionIds). `added`: an ID is held that was not. A
    // refusal changes nothing, so the connection's close still goes to an ID the peer routes.
    [[nodiscard]] QuicTransportError add_peer(const QuicFrame& frame, u64 limit, bool& added) noexcept;
    // Path ID 0's sequence 0's token (the server's stateless_reset_token), when that ID is still held.
    void peer_reset_token(cont::ConstSpan<u8> token) noexcept;
    [[nodiscard]] usize peer_count(u64 path_id = 0U) const noexcept;
    // The `index`th held (in no order), or the one of `sequence`; null for none.
    [[nodiscard]] const QuicPeerCid* peer_at(u64 path_id, usize index) const noexcept;
    [[nodiscard]] const QuicPeerCid* peer(u64 path_id, u64 sequence) const noexcept;
    // The ID a path ID's path sends to (kQuicNoSequence for none), and a move to another one held.
    [[nodiscard]] u64 current(u64 path_id = 0U) const noexcept;
    void set_current(u64 path_id, u64 sequence) noexcept;
    // A packet goes to the ID: its token counts from now on.
    void use(u64 path_id, u64 sequence) noexcept;
    // This side retires a held ID that is not the current one (its path forgotten, RFC 9000 section 9.5). False,
    // changing nothing, past the bound on retirements (CONNECTION_ID_LIMIT_ERROR); true for one not held.
    [[nodiscard]] bool retire_peer(u64 path_id, u64 sequence, u64 limit) noexcept;
    // Whether `tail` (a datagram's last kQuicResetTokenBytes) is the token of a used ID still held, every one compared
    // in constant time.
    [[nodiscard]] bool reset_by(cont::ConstSpan<u8> tail) const noexcept;

    // ---- Retirements this side owes ----
    [[nodiscard]] bool retirement_owed() const noexcept;
    // The next one owed; false for none.
    [[nodiscard]] bool next_retirement(u64& path_id, u64& sequence) const noexcept;
    // Its frame went, and how that settled: a lost one is owed again while its space is open.
    void retirement_sent(u64 path_id, u64 sequence) noexcept;
    void retirement_settled(u64 path_id, u64 sequence, bool acked) noexcept;

    // ---- This side's IDs ----
    // Path ID 0's sequence 0, acknowledged (the handshake carries it), with `token` (empty: none).
    void first_local(cont::ConstSpan<u8> id, cont::ConstSpan<u8> token) noexcept;
    // One more ID of `path_id`'s from the host, owed unless `announced` (the transport parameters carry it: a
    // server's preferred address, sequence 1). False when none could be routed, the space is not open, holds no
    // room, or this side's IDs are zero-length.
    [[nodiscard]] bool issue(u64 path_id, bool announced = false) noexcept;
    // Issues IDs until every open space holds `limit` active ones (the peer's active_connection_id_limit, at most
    // kQuicMaxLocalConnectionIds).
    void top_up(u64 limit) noexcept;
    // A rotation of `path_id`'s: a fresh set whose frames retire every earlier one (Retire Prior To). False, changing
    // nothing, while an earlier rotation's IDs are not all retired or none could be issued.
    [[nodiscard]] bool rotate(u64 path_id, u64 limit) noexcept;
    // The peer's RETIRE_CONNECTION_ID (path ID 0) or PATH_RETIRE_CONNECTION_ID for an open space, read from a packet
    // sent to `packet`: PROTOCOL_VIOLATION for one never issued, toward zero-length IDs, or that packet's own.
    [[nodiscard]] QuicTransportError retire_local(u64 path_id, u64 sequence, QuicCidRef packet) noexcept;
    // Which ID of this side's `id` is, in which space.
    [[nodiscard]] QuicCidRef find(cont::ConstSpan<u8> id) const noexcept;
    [[nodiscard]] const QuicLocalCid* local(u64 path_id, u64 sequence) const noexcept;
    [[nodiscard]] usize local_count(u64 path_id = 0U) const noexcept;
    [[nodiscard]] u64 local_retire_prior_to(u64 path_id) const noexcept;
    // The sequence the next ID issued in `path_id`'s space takes (0 for a space not open): a PATH_CIDS_BLOCKED frame's
    // Next Sequence Number above it is PROTOCOL_VIOLATION (RFC 10069 section 4.7).
    [[nodiscard]] u64 local_next(u64 path_id) const noexcept;
    // The next ID whose frame is owed (its space in `path_id`), and how its frame went.
    [[nodiscard]] bool local_owed() const noexcept;
    [[nodiscard]] const QuicLocalCid* next_owed(u64& path_id) const noexcept;
    void local_sent(u64 path_id, u64 sequence) noexcept;
    void local_settled(u64 path_id, u64 sequence, bool acked) noexcept;
    // Every ID the peer has not acknowledged is owed again (a new path: the frame sent on the old one may be lost).
    void reannounce() noexcept;

    // Hands every ID of this side's back to the host, holding none after, and stops the peer's tokens counting, in
    // every space.
    void finish() noexcept;

private:
    [[nodiscard]] QuicCidSpace* space(u64 path_id) noexcept;
    [[nodiscard]] const QuicCidSpace* space(u64 path_id) const noexcept;
    [[nodiscard]] static QuicPeerCid* held(QuicCidSpace& space, u64 sequence) noexcept;
    [[nodiscard]] static QuicLocalCid* own(QuicCidSpace& space, u64 sequence) noexcept;
    [[nodiscard]] static QuicCidSpace::Retired* retired(QuicCidSpace& space, u64 sequence) noexcept;
    // Whether the retirement of `sequence` is owed or in flight, how many are, and owing it: false, owing nothing, past
    // twice `limit` outstanding.
    [[nodiscard]] static bool retiring(const QuicCidSpace& space, u64 sequence) noexcept;
    [[nodiscard]] static usize outstanding(const QuicCidSpace& space) noexcept;
    [[nodiscard]] static bool owe(QuicCidSpace& space, u64 sequence, u64 limit) noexcept;
    void watch(const QuicPeerCid& id) noexcept;
    void unwatch(const QuicPeerCid& id) noexcept;
    // Forgets the peer's held ID at `index` (its token unwatched), and this side's IDs of a space (unrouted).
    void forget_peer(QuicCidSpace& space, usize index) noexcept;
    void unroute(QuicCidSpace& space) noexcept;
    [[nodiscard]] static usize active(const QuicCidSpace& space) noexcept;

    QuicConnectionIds* m_host = nullptr;
    u8 m_local_bytes = 0U; // this side's IDs' length: sequence 0's
    QuicCidSpace m_first;
    QuicCidSpace* m_spaces[kQuicMaxPaths] = {&m_first};
};

} // namespace crd::net
