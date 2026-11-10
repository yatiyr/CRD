#pragma once

// crd-net -- Explicit Congestion Notification's codepoints and counts (NET-PATH; contract:
// docs/design/network-foundation.md#ecn). Sources: RFC 3168 section 5, RFC 9000 section 13.4.1. The sockets carry a
// codepoint with each datagram (udp.hpp); QUIC counts what arrived and validates what it sent (quic_ecn.hpp).

#include <crd/core/types.hpp>

namespace crd::net
{

// The ECN field of an IP header: its two low bits of the IPv4 TOS or IPv6 Traffic Class byte.
enum class Ecn : u8
{
    NotEct = 0U,
    Ect1 = 1U, // L4S's (RFC 9331)
    Ect0 = 2U,
    Ce = 3U, // Congestion Experienced: set by a router instead of dropping
};

// Packets received with each codepoint but Not-ECT (RFC 9000 section 13.4.1): what an ACK frame of type 0x03
// reports, per packet number space.
struct EcnCounts
{
    u64 ect0 = 0U;
    u64 ect1 = 0U;
    u64 ce = 0U;
};

} // namespace crd::net
