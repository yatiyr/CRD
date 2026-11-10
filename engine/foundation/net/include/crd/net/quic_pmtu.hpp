#pragma once

// crd-net -- QUIC's datagram packetization layer PMTU discovery (NET-PATH; contract:
// docs/design/network-foundation.md#pmtu). Sources: RFC 8899 sections 4 and 5, RFC 9000 sections 14.2 to 14.4.
//
//   sizes      a path starts at kQuicMinInitialDatagram, QUIC's floor, which its handshake proved. `base` is what the
//              path is known to carry: an IPv6 path's kQuicIpv6BaseDatagram (its 1280-byte MTU floor), 1200 bytes
//              otherwise; `ceiling` is the most this side sends there (its configuration and the peer's
//              max_udp_payload_size). size() is the PLPMTU every datagram but a probe takes.
//   probing    a probe is one datagram of exactly the size probed, sent alone (QuicConnection: PING and PADDING), one
//              at a time. Base probes `base` above the floor (RFC 8899 section 5.2); Searching probes the ceiling
//              first, then halves the gap between the largest size acknowledged and the smallest that failed, to the
//              byte. A size fails after kQuicPmtuMaxProbes consecutive losses (MAX_PROBES), so random loss alone
//              almost never fails one; an acknowledgement proves a size, even one declared lost before (a spurious
//              loss). A base that fails is Error: 1200 bytes until the raise timer probes it again.
//   raising    a finished search (or Error) searches again kQuicPmtuRaiseNs after it ended (PMTU_RAISE_TIMER), and
//              only while the connection has data to send, so probing never keeps an idle connection alive.
//   black hole after a raise, a path that stops carrying large datagrams (a route change, a tunnel) loses them while
//              small ones arrive. A loss detection whose lost packets all exceed 1200 bytes and were sent after the
//              newest acknowledged packet of that size is suspect; kQuicPmtuBlackHoleDetections suspect detections
//              with no larger packet acknowledged after them fall back to 1200 bytes and search again below the size
//              that failed. Congestion loses large packets too, but its acknowledgements of later large packets
//              clear the suspicion; the connection sends 1200-byte datagrams while a probe timeout is pending, so a
//              path that carries nothing large still gets its acknowledgements through.
//   congestion probe losses are no congestion signal (RFC 9000 section 14.4); the connection keeps them from its
//              controller and from persistent congestion, while probes stay inside the window (RFC 8899 section 3).
//   time       integer state and the caller's clock alone: a seeded run probes the same sizes (NET-SIM's replays).

#include <crd/core/types.hpp>
#include <crd/net/quic_header.hpp>
#include <crd/net/quota.hpp>

namespace crd::net
{
// QUIC's floor, which every path carries (RFC 9000 section 14), as the search counts sizes.
inline constexpr u32 kQuicPmtuFloor = static_cast<u32>(kQuicMinInitialDatagram);
inline constexpr u32 kQuicPmtuMaxProbes = 3U;                   // RFC 8899 section 5.1.2's MAX_PROBES
inline constexpr u64 kQuicPmtuRaiseNs = 600U * kNanosPerSecond; // section 5.1.1's PMTU_RAISE_TIMER
inline constexpr u32 kQuicPmtuBlackHoleDetections = 3U;
// An IPv6 path's 1280-byte link MTU floor (RFC 8200 section 5) less the IPv6 and UDP headers.
inline constexpr u32 kQuicIpv6BaseDatagram = 1232U;
// The search's default ceiling: Ethernet's 1500-byte MTU less the IPv4 and UDP headers, the most the common path
// carries (an IPv6 one finds its 1452 bytes below it).
inline constexpr u32 kQuicDefaultMaxDatagram = 1472U;

enum class QuicPmtuState : u8
{
    Disabled,       // no probes: datagrams of min(base, ceiling) (no discovery, or a ceiling at the floor)
    Base,           // probing the base above 1200 bytes
    Searching,      // probing between the largest size acknowledged and the smallest that failed
    SearchComplete, // found; a black hole or the raise timer searches again
    Error,          // the base failed: 1200 bytes until the raise timer
};

struct QuicPmtuCounters
{
    u64 probes_sent = 0U;
    u64 probes_acked = 0U;
    u64 probes_lost = 0U;
    u64 black_holes = 0U;
};

class QuicPathMtu
{
public:
    // A new path, as the sizes note says; without `discovery` (a socket that cannot forbid fragmentation) datagrams
    // take min(base, ceiling) and nothing is probed. `base` and `ceiling` below the floor count as the floor.
    void start(u32 base, u32 ceiling, bool discovery) noexcept;
    // Lowers the ceiling (the peer's max_udp_payload_size); a size or bound above it falls to it.
    void limit(u32 ceiling) noexcept;

    [[nodiscard]] u32 size() const noexcept { return m_size; }
    [[nodiscard]] QuicPmtuState state() const noexcept { return m_state; }
    [[nodiscard]] u32 ceiling() const noexcept { return m_ceiling; }
    // The size of the probe due at `now_ns`, 0 for none (one is in flight, or nothing is searched). `busy`: the
    // connection has data to send now, which a search the raise timer restarts waits for.
    [[nodiscard]] u32 probe(u64 now_ns, bool busy) noexcept;
    // The probe probe() named left.
    void on_probe_sent(u32 bytes) noexcept;
    // A probe of `bytes` was acknowledged (spuriously lost ones included), or declared lost.
    void on_probe_acked(u32 bytes, u64 now_ns) noexcept;
    void on_probe_lost(u32 bytes, u64 now_ns) noexcept;
    // A packet that was no probe, numbered `number` in the Application space, was acknowledged or declared lost.
    void on_acked(u64 number, u32 bytes) noexcept;
    void on_lost(u64 number, u32 bytes) noexcept;
    // A loss detection ended, after its ACK frame's acknowledgements: true when it found a black hole (size() fell).
    bool end_detection(u64 now_ns) noexcept;

    [[nodiscard]] const QuicPmtuCounters& counters() const noexcept { return m_counters; }

private:
    void search(u64 now_ns) noexcept; // Searching from m_low, or SearchComplete when nothing is left between
    void complete(u64 now_ns) noexcept;

    QuicPmtuState m_state = QuicPmtuState::Disabled;
    u32 m_size = kQuicPmtuFloor;
    u32 m_base = kQuicPmtuFloor;
    u32 m_ceiling = kQuicPmtuFloor;
    u32 m_low = kQuicPmtuFloor;  // the largest size acknowledged
    u32 m_high = kQuicPmtuFloor; // the largest not known to fail
    u32 m_trying = 0U;           // the size probed (0: the next probe picks one)
    u32 m_failures = 0U;         // consecutive losses of probes of m_trying
    u32 m_flight = 0U;           // the size of the probe in flight, 0 for none
    bool m_optimistic = false;   // the next probe of the search is its ceiling
    u64 m_raise_ns = kNeverNs;

    // Black hole detection.
    u64 m_large_acked = 0U; // the newest acknowledged packet over 1200 bytes, plus one (0: none)
    u64 m_suspects[kQuicPmtuBlackHoleDetections] = {}; // each suspect detection's oldest lost packet
    u32 m_suspect_count = 0U;
    u64 m_burst_oldest = kNeverNs; // this detection's oldest large lost packet
    bool m_burst_small = false;    // this detection lost a packet of 1200 bytes or less
    QuicPmtuCounters m_counters;
};

} // namespace crd::net
