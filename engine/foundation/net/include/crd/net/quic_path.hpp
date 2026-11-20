#pragma once

// crd-net -- a QUIC connection's paths and their validation (NET-PATH; contract:
// docs/design/network-foundation.md#migration). Sources: RFC 9000 sections 8.1, 8.2, 9 and 9.3 to 9.5.
//
//   paths       a connection knows at most kQuicMaxPaths paths, each a host socket and the peer's address there. One is
//               active: datagrams go there. A path joins once a packet from it opened, never on bytes alone; when the
//               table is full the connection gives up its least recently used path that is neither active nor the
//               fallback (the last validated path before the active one, which a failed validation reverts to:
//               section 9.3.2). A validated path left behind stays known, so the peer may return without another
//               validation (section 9.3), until it is given up.
//   amplification  until a path is validated, what goes on it stays within three times what arrived on it (sections
//               8.1 and 9.3.1); a validated path is unlimited.
//   validation  a PATH_CHALLENGE of fresh unpredictable data goes on the path, again after the probe period and then
//               twice as long each time (no more often than an Initial, section 8.2.1), until a PATH_RESPONSE echoes
//               one of the path's last kQuicPathChallengesKept, on whatever path it arrives (section 8.2.2), or the
//               timeout the caller gave passes (section 8.2.4: three times the larger of the current probe period and
//               a new path's). An answered challenge the amplification limit kept under 1200 bytes validates the
//               address only; validation goes on with an expanded one (section 8.2.3), and a path whose expanded
//               challenge is never answered fails as any.
//   responses   each path holds the latest challenge received on it, answered once and only there (section 8.2.2).
//   time        integer state and the caller's clock alone.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/net/endpoint.hpp>
#include <crd/net/quic_frame.hpp>
#include <crd/net/quota.hpp>

namespace crd::net
{
inline constexpr usize kQuicMaxPaths = 4U;
inline constexpr usize kQuicNoPath = kQuicMaxPaths;
inline constexpr usize kQuicPathChallengesKept = 4U; // per path: a response to an older one is late and ignored

// A path as the host names it: the socket it sends through (0 for the one the connection opened on) and the peer's
// address there. Port 0 is a path not yet bound (a keyed server's slot before its first packet).
struct QuicPath
{
    u32 socket = 0U;
    Endpoint peer;

    friend bool operator==(const QuicPath&, const QuicPath&) noexcept = default;
};

// Whether `to` differs from `from` in the peer's port alone: a NAT rebinding, whose congestion and RTT state may be
// kept (RFC 9000 section 9.4).
[[nodiscard]] bool quic_port_only_change(const QuicPath& from, const QuicPath& to) noexcept;

enum class QuicPathResponse : u8
{
    Ignored,   // echoes no challenge of a path under validation: late, or never sent
    Validated, // the path's address is proven
};

class QuicPaths
{
public:
    // The connection's first path, active, validated or not (a server before its client's address is proven).
    void open(const QuicPath& path, bool validated, u64 now_ns) noexcept;

    // The slot holding `path`; kQuicNoPath for none.
    [[nodiscard]] usize find(const QuicPath& path) const noexcept;
    // `path`'s slot, a free one taken when it has none; kQuicNoPath when the table is full.
    [[nodiscard]] usize admit(const QuicPath& path, u64 now_ns) noexcept;
    // The least recently used path neither active nor the fallback, the one to give up for a new one.
    [[nodiscard]] usize least_recent() const noexcept;
    // Forgets the path (not the active one): its challenges answer nothing from now on.
    void drop(usize index) noexcept;
    // The unbound active path takes the address of the first packet that opens.
    void bind(const QuicPath& path) noexcept;
    [[nodiscard]] bool used(usize index) const noexcept { return index < kQuicMaxPaths && m_slots[index].used; }
    [[nodiscard]] const QuicPath& path(usize index) const noexcept { return m_slots[index].path; }

    [[nodiscard]] usize active() const noexcept { return m_active; }
    [[nodiscard]] usize fallback() const noexcept { return m_fallback; }
    // Datagrams go on `index` from now on; the active path before it becomes the fallback when validated.
    void activate(usize index) noexcept;
    void clear_fallback() noexcept { m_fallback = kQuicNoPath; }

    // ---- Amplification ----

    void on_received(usize index, usize bytes, u64 now_ns) noexcept;
    void on_sent(usize index, usize bytes) noexcept;
    // What `index` may still carry: unlimited once validated.
    [[nodiscard]] usize credit(usize index) const noexcept;
    void set_validated(usize index) noexcept;
    // The peer's address is already proven (a client's own new path to its server's address): no amplification
    // limit, while the path itself still owes its validation (sections 8.1 and 9.2).
    void trust(usize index) noexcept { m_slots[index].validated = true; }
    [[nodiscard]] bool validated(usize index) const noexcept { return m_slots[index].validated; }

    // ---- Validation ----

    // Starts validating `index`, abandoned at `now_ns` + `timeout_ns`; nothing when it is underway.
    void validate(usize index, u64 now_ns, u64 timeout_ns) noexcept;
    [[nodiscard]] bool validating(usize index) const noexcept { return m_slots[index].deadline_ns != kNeverNs; }
    // A PATH_CHALLENGE is due on `index` at `now_ns`.
    [[nodiscard]] bool challenge_due(usize index, u64 now_ns) const noexcept;
    // One went with `data`, in a datagram `expanded` to 1200 bytes or not; the next is due `resend_ns` later, twice
    // that for each one sent before it.
    void challenge_sent(usize index, cont::ConstSpan<u8> data, bool expanded, u64 now_ns, u64 resend_ns) noexcept;
    // A PATH_RESPONSE's data: `index` names the path it validated.
    [[nodiscard]] QuicPathResponse on_response(cont::ConstSpan<u8> data, usize& index) noexcept;
    // Ends `index`'s validation without a verdict.
    void abandon(usize index) noexcept;
    // Whether `index` is known to carry 1200-byte datagrams: validated by an expanded challenge, or by other means
    // (set_validated: the handshake, a token).
    [[nodiscard]] bool proven(usize index) const noexcept { return m_slots[index].proven; }
    // A path whose validation timed out at `now_ns`, abandoned (it stays in the table); kQuicNoPath for none.
    [[nodiscard]] usize expired(u64 now_ns) noexcept;
    // When the next PATH_CHALLENGE is due and when validation is abandoned; kNeverNs while none is underway.
    [[nodiscard]] u64 due_ns(usize index) const noexcept { return m_slots[index].due_ns; }
    [[nodiscard]] u64 deadline_ns(usize index) const noexcept { return m_slots[index].deadline_ns; }
    // When the last datagram from `index` arrived (or it joined).
    [[nodiscard]] u64 last_ns(usize index) const noexcept { return m_slots[index].last_ns; }

    // ---- Responses ----

    void on_challenge(usize index, cont::ConstSpan<u8> data) noexcept;
    [[nodiscard]] bool response_owed(usize index) const noexcept { return m_slots[index].response_owed; }
    [[nodiscard]] cont::ConstSpan<u8> response(usize index) const noexcept
    {
        return cont::ConstSpan<u8>(m_slots[index].response, kQuicPathDataBytes);
    }
    void response_sent(usize index) noexcept { m_slots[index].response_owed = false; }

    // ---- The connection's own bookkeeping, kept on the path ----

    // The peer's connection ID (its sequence) this side sends to on the path, and this side's the peer last sent to.
    void set_peer_sequence(usize index, u64 sequence) noexcept { m_slots[index].peer_sequence = sequence; }
    [[nodiscard]] u64 peer_sequence(usize index) const noexcept { return m_slots[index].peer_sequence; }
    void set_local_sequence(usize index, u64 sequence) noexcept { m_slots[index].local_sequence = sequence; }
    [[nodiscard]] u64 local_sequence(usize index) const noexcept { return m_slots[index].local_sequence; }

private:
    struct Challenge
    {
        u8 data[kQuicPathDataBytes] = {};
        bool expanded = false;
    };

    struct Slot
    {
        QuicPath path;
        u64 received = 0U; // bytes, for the amplification limit
        u64 sent = 0U;
        u64 last_ns = 0U;           // the last datagram from it
        u64 deadline_ns = kNeverNs; // validation is abandoned then (kNeverNs: none underway)
        u64 due_ns = kNeverNs;      // the next PATH_CHALLENGE
        u64 resend_ns = 0U;         // the period before the next one
        Challenge challenges[kQuicPathChallengesKept];
        usize challenge_count = 0U; // sent in this validation, the latest at (count - 1) % kept
        u8 response[kQuicPathDataBytes] = {};
        u64 peer_sequence = 0U;
        u64 local_sequence = 0U;
        bool used = false;
        bool validated = false;
        bool proven = false; // carries 1200-byte datagrams
        bool response_owed = false;
    };

    Slot m_slots[kQuicMaxPaths];
    usize m_active = 0U;
    usize m_fallback = kQuicNoPath;
};

} // namespace crd::net
