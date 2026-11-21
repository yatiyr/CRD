#pragma once

// crd-net -- Linux's io_uring under a UdpSocket's receive (NET-PATH; docs/design/network-foundation.md#sockets): one
// multishot recvmsg the host keeps armed fills a ring of buffers it selects itself, and receive() reads the
// completions back without a call while they wait. The ring, its buffers and everything the host reads or writes live
// in pages of the ring's own, never in the movable socket. What a datagram means (its peer, codepoint and train) stays
// udp.cpp's; this is the host interface alone. Private to crd-net's sources.

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

// Android's application sandbox kills a process that makes io_uring's calls, so the ring is a desktop and server
// Linux path; headers before Linux 6.0 lack the multishot recvmsg it is built on.
#if CRD_OS_LINUX && !CRD_OS_ANDROID && __has_include(<linux/io_uring.h>)
#include <linux/io_uring.h>
#if defined(IORING_RECV_MULTISHOT)
#define CRD_NET_RING 1
#endif
#endif
#ifndef CRD_NET_RING
#define CRD_NET_RING 0
#endif

#if CRD_NET_RING
#include <poll.h>
#include <sys/socket.h>

namespace crd::net
{
namespace cont = crd::containers;

struct UdpRing;

// The room a received datagram's peer address takes in a buffer: a sockaddr_in6 rounded up so the control messages
// behind it start aligned for cmsghdr.
inline constexpr u32 kRingNameBytes = 32U;
// The most buffers a ring holds (io_uring's buffer ring limit) and the fewest it is worth opening with.
inline constexpr u32 kRingMaxBuffers = 32768U;
inline constexpr u32 kRingMinBuffers = 4U;
// The ICMP reports one receive() reads at most, each into a report buffer of the ring's own (udp.cpp's budget), which
// holds more than any ICMP or ICMPv6 error quotes of a datagram (an IPv6 minimum MTU's worth).
inline constexpr u32 kRingReports = 64U;
inline constexpr u32 kRingReportBytes = 1280U;

// One received datagram as the host wrote it into its buffer.
struct RingMessage
{
    msghdr header{};        // name and control as recvmsg reports them, for udp.cpp's readers
    cont::Span<u8> payload; // the bytes the buffer holds
    bool truncated = false; // the datagram was longer and its rest is lost
};

// What one completion carried: a datagram, the receive's end (ring_armed() is then false), or both; or that the
// socket's error queue holds a report.
struct RingCompletion
{
    bool datagram = false;
    bool report = false;
    RingMessage message;
    int error = 0; // the errno the receive ended with, 0 for none
};

// A ring of `buffers` buffers (a power of two from kRingMinBuffers to kRingMaxBuffers), each holding a datagram of
// up to `datagram_bytes` and `control_bytes` of control messages, armed on `socket`, unbound yet (what
// is waiting at once is then a refusal); nullptr where the host refuses any part of it (no io_uring, a kernel before
// 6.0, a sandbox, no memory), and the socket then keeps its plain path.
[[nodiscard]] UdpRing* ring_open(int socket, u32 buffers, u32 datagram_bytes, u32 control_bytes) noexcept;
// Ends the receive, waits until the host is done with every buffer, and gives the pages back. Null is a no-op.
void ring_close(UdpRing* ring) noexcept;

// The ring's descriptor: readable while a completion waits, so a SocketSet waits on it in place of the socket.
[[nodiscard]] int ring_handle(const UdpRing& ring) noexcept;
[[nodiscard]] bool ring_armed(const UdpRing& ring) noexcept;
// A completion waits to be read.
[[nodiscard]] bool ring_pending(const UdpRing& ring) noexcept;
// Arms the receive again after its end, and with `watch` the watch on the socket's error queue after its
// completion; false when the receive is not armed: the host could not take it now (tried again on the next call) or
// every buffer is lent, which the host would end it on at once.
[[nodiscard]] bool ring_arm(UdpRing& ring, bool watch) noexcept;
// Moves the receive to the calling thread where another armed it: the host runs a receive's completions as the task
// that armed it, so a loop on a thread of its own arms its own and never wakes the thread that opened the socket. The
// old receive is cancelled and a datagram it still completes is read as any other; false when the host could not take
// the move now (tried again on the next call).
[[nodiscard]] bool ring_follow(UdpRing& ring) noexcept;

// The oldest completion, false when none waits (or, `ends_only`, when it carries a datagram, which stays). A
// datagram's buffer is lent until ring_recycle().
[[nodiscard]] bool ring_next(UdpRing& ring, RingCompletion& completion, bool ends_only = false) noexcept;
// Hands every buffer lent since the last call back to the host.
void ring_recycle(UdpRing& ring) noexcept;
// The kRingReports report buffers of kRingReportBytes each, end to end, for the ICMP reports lent slots hold.
[[nodiscard]] cont::Span<u8> ring_reports(UdpRing& ring) noexcept;

} // namespace crd::net
#endif
