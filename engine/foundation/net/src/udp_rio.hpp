#pragma once

// crd-net -- Windows' Registered I/O under a UdpSocket (NET-PATH; docs/design/network-foundation.md#sockets): the
// socket's receives wait posted in buffers registered with the host once, which fills them as datagrams arrive, and
// receive() reads the completions back without a call; its sends are copied into registered slots and handed over
// deferred, one commit a batch. A socket made for it takes no other Winsock call (WSAPoll, WSARecvMsg and WSASendMsg
// fail with WSAENOTSOCK), so it is made here and waits on the event its completion queue signals. What a datagram
// means (its peer, codepoint and train) stays udp.cpp's; this is the host interface alone. Private to crd-net's
// sources.

#include "native_socket.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

// Every Windows crd-net supports (8 and later) has Registered I/O.
#if CRD_OS_WINDOWS
#include <crd/net/udp.hpp>

namespace crd::net
{
namespace cont = crd::containers;

struct UdpRio;

// The fewest and most receive buffers a socket posts (ring_buffers); the datagrams it sends at once, each in a slot of
// its own, and the trains beside them in slots that hold kMaxTrainBytes.
inline constexpr u32 kRioMinBuffers = 4U;
inline constexpr u32 kRioMaxBuffers = 32768U;
inline constexpr u32 kRioSends = 64U;
inline constexpr u32 kRioTrains = 8U;
// Room in a slot for its control messages behind their RIO_CMSG_BUFFER header, which the host reads and writes on
// both sends and receives: a codepoint and a train's datagram size.
inline constexpr u32 kRioControlBytes = 64U;

// A socket of `family` made for Registered I/O, not inherited; kInvalidSocket where the host has no Registered I/O (or
// refuses it now), and the caller then makes a plain one.
[[nodiscard]] NativeSocket rio_socket(int family) noexcept;
// The receives and sends of `socket` (from rio_socket(), configured and not yet bound): `buffers` receive buffers (a
// power of two from kRioMinBuffers to kRioMaxBuffers) of `datagram_bytes`, kRioSends send slots of as many, and with
// `trains` kRioTrains train slots, every one registered on its own as the host's contract for a buffer in use asks,
// so their pages stay locked while the socket is open. Every receive is posted. nullptr where the host refuses any
// part of it (memory, the locked-page quota).
[[nodiscard]] UdpRio* rio_open(NativeSocket socket, u32 buffers, u32 datagram_bytes, bool trains) noexcept;
// After the socket is closed, which ends every request: deregisters the buffers and gives the pages back. Null is a
// no-op.
void rio_close(UdpRio* rio) noexcept;

// The event the receive completion queue signals, for waits: armed by rio_watch(), set at once if a completion waits.
[[nodiscard]] u64 rio_event(const UdpRio& rio) noexcept;
// Arms the event for the next completion (a no-op while it is armed); false when the host refused.
[[nodiscard]] bool rio_watch(UdpRio& rio) noexcept;

// One completed receive: its datagram in a registered buffer, the peer and control messages the host wrote beside it.
struct RioReceived
{
    cont::Span<u8> payload;
    sockaddr_storage peer{};
    WSAMSG control{};       // only Control is set: the messages, for udp.cpp's reader
    bool truncated = false; // the datagram was longer and its rest is lost
    int error = 0;          // the host's error for this receive (no datagram), 0 for none
    u32 buffer = 0U;
};

// Posts again, deferred, every buffer the last receive() lent.
void rio_recycle(UdpRio& rio) noexcept;
// The oldest completed receive, false when none waits. Its buffer is out until rio_release().
[[nodiscard]] bool rio_next(UdpRio& rio, RioReceived& received) noexcept;
// Gives a completed receive's buffer back: `lent` keeps it for the slot it fills until the next rio_recycle(),
// otherwise it is posted again at once, deferred. A datagram that arrives while no receive is posted is lost, so
// every buffer goes back as soon as its bytes are read.
void rio_release(UdpRio& rio, u32 buffer, bool lent) noexcept;
// Starts every deferred receive.
void rio_flush(UdpRio& rio) noexcept;

// A free send slot: the bytes to copy a datagram (or a train, end to end) into, its peer's address and the room for
// its control messages.
struct RioSendSlot
{
    cont::Span<u8> bytes;
    SOCKADDR_INET* peer = nullptr;
    cont::Span<u8> control; // the messages alone; the header before them is rio_send()'s
    u32 index = 0U;
};
// What the send completions read so far reported: the datagrams and trains the host failed after the call took
// them, and whether a failure says the host segments no train or marks no codepoint, as the WSASendMsg path learns
// from the same errors in the call.
struct RioFailures
{
    u64 datagrams = 0U;
    u64 trains = 0U;
    bool unsegmented = false; // a train failed WSAEOPNOTSUPP, or WSAEINVAL unmarked
    bool unmarked = false;    // a marked send failed WSAEINVAL
};
// Reads the send completions that arrived: each slot is free again, and each failure added to `failures`.
void rio_reap(UdpRio& rio, RioFailures& failures) noexcept;
// A free slot, a train's with `train`, after starting the deferred sends and reading their completions where none is
// free; false while every slot of that kind is in flight, or for a train on a socket without train slots.
[[nodiscard]] bool rio_send_slot(UdpRio& rio, bool train, RioSendSlot& slot, RioFailures& failures) noexcept;
// Hands the host `slot` with `bytes` of `datagrams` datagrams, `marked` with a codepoint, and `control_bytes` of
// control messages, deferred; false and the host's error where it refused (the slot is then free again).
[[nodiscard]] bool rio_send(UdpRio& rio, const RioSendSlot& slot, usize bytes, usize control_bytes, u32 datagrams,
                            bool marked, int& error) noexcept;
// Starts every deferred send.
void rio_commit(UdpRio& rio) noexcept;

} // namespace crd::net
#endif
