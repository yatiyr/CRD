# 2026-11-21: NET-PATH, Windows' Registered I/O

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S33](../design/network-foundation.md#sockets).

CI first: run 44895376363 (cf3611f8) fails linux-gcc-shipping on GCC LTO's `-Warray-bounds` in
`SendQueue::queued(socket)`, the bound 7ed26c44 already adds; nothing else in the finished runs is this track's.

## What landed

- `udp_rio.cpp`: a `WSA_FLAG_REGISTERED_IO` socket with posted `RIOReceiveEx` receives, one registration per buffer,
  lent or copied by `receive()`; 64 registered send slots and 8 train slots, `RIO_MSG_DEFER` and one commit a call.
- `UdpOptions::registered_io` (off by default), `ring_datagram_bytes` (both platforms' buffer size),
  `SocketFeatures::send_bytes`, `OffloadCounters::sends_failed`.
- Late send failures feed back as the call's errors do on the plain path (a train counts in `trains_refused`,
  WSAEOPNOTSUPP or unmarked WSAEINVAL ends segmenting, marked WSAEINVAL ends marking).
- Every Windows socket waits on an event (`RIONotify`, or `WSAEventSelect(FD_READ)` for a plain one), so `SocketSet`
  waits on any mix in one `WaitForMultipleObjects`.

## Found by probing the host (scratch programs, not kept)

- A Registered I/O socket refuses `WSAPoll`, `WSARecvMsg`, `WSASendMsg` (WSAENOTSOCK), FIONBIO and SIO_UDP_NETRESET.
- A datagram that arrives while no receive is posted is dropped, whatever SO_RCVBUF says.
- Send control messages are ignored without a `RIO_CMSG_BUFFER` header (no ECN mark, a train sent as one datagram);
  with it, ECN marks and USO work. CE fails late with WSAEINVAL; a too-large send fails late with WSAEMSGSIZE.
- `WSARecvMsg` re-enables FD_READ, so the plain event wait is sound.

## Evidence

- win-debug: all 1033 crd-net-tests cases; win-release: the UDP, Registered I/O, loop and QUIC client files;
  linux-gcc-debug: every crd-net case and the socket files again with `CRD_NET_UDP_RING=0`; linux-gcc-release
  builds warning-free and runs the socket files; win-asan and win-clang-cl run the socket files clean; the
  `crd-check` and `crd-no-` CTest guards pass; clang-tidy 22.1.8 clean on the changed sources (it found an enum,
  the `CRD_NET_RIO` macro and `kInvalidSocket`'s unused-in-header constant, all fixed).
- Board (`[.udp-board]`, win-release, loopback, 400000 datagrams of 1200 B, 3 rounds, cycles a datagram): receiver
  523-573 against 1842-2347, sender 5489-6881 against 5976-7533.

## Next

Registered I/O and URO on a real NIC with the zero-copy send default (one Windows-to-Linux cloud run), then multipath
and address discovery behind their NET-STD flags, and the budget boards against msquic and quiche.
