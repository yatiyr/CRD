# 2026-11-21: NET-PATH, the Linux io_uring receive

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S33](../design/network-foundation.md#sockets).

CI first: nothing of this track failed (44887080529 running clean; later pushes cancelled or not started).

## What landed

- `udp_ring.cpp`, raw io_uring: one multishot `recvmsg` over a registered buffer ring, a one-shot POLLERR watch on
  the error queue, teardown that reads every request's last completion before its `crd::vm` pages go.
- `UdpSocket` (Linux, not Android) opens it before bind and otherwise keeps `recvmmsg`; `features()` reports
  `lent_receive` and `receive_bytes`. Empty slots are lent the buffers until the next receive, slots with storage
  take a copy; `IoLoop` reads lending sockets into storage-less slots.
- Found by the tests and fixed in the design: buffers sized for any datagram; no re-arm while every buffer is lent
  (the wait watches the socket meanwhile); the end posted behind the last datagram read at once; the error-queue
  watch (`sendmmsg` drops a socket error met past its first message); one shot, not multishot (a report flood
  overflowed the completion ring); the ring follows the receiving thread, which may wake one extra turn, so the
  two-socket loop test now stops on purpose instead of after exactly two turns.
- linux-gcc-release's LTO found a false `-Warray-bounds` in `SendQueue::queued` (this track's 69f6db02): bounded.

## Evidence

- WSL linux-gcc-debug: all 1024 crd-net-tests cases on the ring; the real-socket files again under
  `crd-net-udp-recvmmsg` (71 cases, CTest); `[.netns]` on both paths three times each; the ring file 20 times; the
  `crd-no-*` and `crd-check-*` guards. Ring cases: the independent probe, refused counts, lent and copied slots, the
  largest IPv4 datagram, a 4-buffer ring at 1 to 64 slots a call, reads on another thread, after the opener ends and
  by 16 threads in turn, waits alone and in a set, a move, a close from another thread, a lent Packet Too Big.
- linux-clang-tsan clean (ring, udp, io_loop, quic_client files); linux-gcc-release passes; win-debug passes (ring
  cases skip); clang-tidy 22.1.8 clean on the changed sources.
- Teeth seen failing on the way: no watch (namespace train test), multishot watch (80-report flood), arming with
  every buffer lent or leaving the end unread (4-buffer empty wait), 1500-byte buffers (`test_udp` large datagrams).
- Board (`[.udp-board]`, release, 3 interleaved rounds, 400000 datagrams of 1200 bytes, 32 slots a receive): 325 to
  349 ns receiver CPU a datagram on the ring, 323 to 325 on `recvmmsg`; 2 to 10% of the blast lost against 18 to 20%.

## Next

io_uring's zero-copy send (`SENDMSG_ZC`, the send queue holding slots until the notification), Windows RIO, then
multipath and address discovery. `RECV_ZC` needs Linux 6.15 and a header-splitting NIC: a physical item.
