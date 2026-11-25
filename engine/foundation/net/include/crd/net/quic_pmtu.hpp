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
//              small ones arrive. A loss detection whose lost packets all exceed 1200 bytes is suspect unless the
//              newest packet acknowledged over 1200 bytes left after them and is at least as large as the smallest;
//              kQuicPmtuBlackHoleDetections suspect detections that no such acknowledgement cleared, whose lost
//              packets left at least a round trip apart, fall back to 1200 bytes and search again below the size
//              that failed. Congestion loses large packets too, but its acknowledgements of later ones as large clear
//              the suspicion; a smaller one (a path fallen to 1300 bytes still carries 1250) proves nothing about
//              them. A reordering lets a small packet overtake large ones (a delay spike's end, a LEO handover's),
//              and RFC 9002's time threshold may declare them lost a detection each: those left within the delay the
//              path shed, which the round trip they had includes, so the span keeps them from passing for a black
//              hole. While a suspicion is held, a datagram of 1200 bytes follows each kQuicPmtuCanaryAfter larger
//              ones (canary()): its acknowledgement declares them lost a round trip later, so a path that carries
//              only small datagrams shows it without waiting for a probe timeout, and the size in use stays. The
//              connection also sends 1200-byte datagrams while a probe timeout is pending, so a path that carries
//              nothing large still gets its acknowledgements through.
//   hints      an ICMP Packet Too Big message (RFC 8899 section 4.6) only lowers: once the connection matched its quote
//              to a packet it sent (section 4.6.1), a probe above the hop's limit fails at once and the next probe
//              tries the limit itself; a datagram of the size in use above it means the path shrank, and the size
//              falls to the floor to search again up to the limit, as a black hole would. A limit below the base, or
//              at or above the datagram quoted, is inconsistent and ignored. Only an acknowledged probe raises.
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
// The large datagrams before each canary while a suspicion is held: RFC 9002's packet threshold, so the canary's
// acknowledgement declares the oldest of them lost at once and the time threshold the rest.
inline constexpr u32 kQuicPmtuCanaryAfter = 3U;
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
    u64 hints = 0U;         // Packet Too Big messages used
    u64 hints_ignored = 0U; // and those inconsistent with the search, or without discovery
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
    // Whether probe() reads `busy` at `now_ns`: a settled or failed search whose raise timer has passed.
    [[nodiscard]] bool raise_due(u64 now_ns) const noexcept;
    // The probe probe() named left.
    void on_probe_sent(u32 bytes) noexcept;
    // A probe of `bytes` was acknowledged (spuriously lost ones included), or declared lost.
    void on_probe_acked(u32 bytes, u64 now_ns) noexcept;
    void on_probe_lost(u32 bytes, u64 now_ns) noexcept;
    // A packet of the Application space that was no probe, sent at `sent_ns`, was acknowledged or declared lost.
    void on_acked(u64 sent_ns, u32 bytes) noexcept;
    // An ack-eliciting packet of `bytes` that was no probe left on the path, in the Application space.
    void on_sent(u32 bytes) noexcept;
    // The next ack-eliciting datagram keeps to the floor, as the black hole note says.
    [[nodiscard]] bool canary() const noexcept
    {
        return m_suspect_count != 0U && m_large_run >= kQuicPmtuCanaryAfter;
    }
    void on_lost(u64 sent_ns, u32 bytes) noexcept;
    // A loss detection ended, after its ACK frame's acknowledgements, on a path whose round trip is `rtt_ns`
    // (RFC 9002's max(smoothed_rtt, latest_rtt)): true when it found a black hole (size() fell).
    bool end_detection(u64 now_ns, u64 rtt_ns) noexcept;
    // A Packet Too Big message, matched to a sent datagram of `quoted` bytes, for a hop carrying `limit` bytes of UDP
    // payload, as the hints note says: true when it was used.
    bool on_too_big(u32 quoted, u32 limit, u64 now_ns) noexcept;

    [[nodiscard]] const QuicPmtuCounters& counters() const noexcept { return m_counters; }

private:
    void search(u64 now_ns) noexcept; // Searching from m_low, or SearchComplete when nothing is left between
    void complete(u64 now_ns) noexcept;

    // The search; the narrow fields first, so the layout pads only before the times.
    QuicPmtuState m_state = QuicPmtuState::Disabled;
    bool m_optimistic = false;  // the next probe of the search is its ceiling
    bool m_burst_small = false; // black hole detection: this detection lost a packet of 1200 bytes or less
    u8 m_failures = 0U;         // consecutive losses of probes of m_trying
    u8 m_suspect_count = 0U;    // black hole detection: the suspect detections held
    u8 m_large_run = 0U;        // ack-eliciting packets over 1200 bytes sent since the last one of 1200 or less
    // Black hole detection, by size (a datagram's bytes, which fit 16 bits): the newest acknowledged packet over 1200
    // bytes; each suspect detection's smallest lost packet; this detection's smallest large lost one (0xFFFF: none).
    u16 m_large_after_bytes = 0U;
    u16 m_suspect_bytes[kQuicPmtuBlackHoleDetections] = {};
    u16 m_burst_bytes = 0xFFFFU;
    u32 m_size = kQuicPmtuFloor;
    u32 m_base = kQuicPmtuFloor;
    u32 m_ceiling = kQuicPmtuFloor;
    u32 m_low = kQuicPmtuFloor;  // the largest size acknowledged
    u32 m_high = kQuicPmtuFloor; // the largest not known to fail
    u32 m_trying = 0U;           // the size probed (0: the next probe picks one)
    u32 m_flight = 0U;           // the size of the probe in flight, 0 for none
    u64 m_raise_ns = kNeverNs;

    // Black hole detection, by when each packet left: the newest acknowledged packet over 1200 bytes, plus one (0:
    // none); each suspect detection's oldest lost packet; this detection's oldest large lost packet.
    u64 m_large_after_ns = 0U;
    u64 m_suspect_sent_ns[kQuicPmtuBlackHoleDetections] = {};
    u64 m_burst_sent_ns = kNeverNs;
    QuicPmtuCounters m_counters;
};

} // namespace crd::net
