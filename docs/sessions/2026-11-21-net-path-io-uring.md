# 2026-11-21: NET-PATH, the Linux io_uring receive

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S33](../design/network-foundation.md#sockets).

## CI first

Nothing of this track failed. Run 44887080529 (70f94328) still had jobs running and none had failed; 44889979894 and
44891021691 were cancelled by newer pushes, and 44891884134 (cf3611f8) had not started.

## What landed

- `udp_ring.cpp`: raw io_uring (no liburing): one multishot `recvmsg` over a registered buffer ring, a one-shot
  POLLERR watch on the error queue, cancellation and teardown that reads every request's last completion before the
  pages go. Everything the host reads or writes is in `crd::vm` pages of the ring's own.
- `UdpSocket` on Linux (not Android) opens it before bind, so a refusal shows at once and the socket keeps
  `recvmmsg`; `features()` reports `lent_receive` and `receive_bytes`. Empty slots are lent the ring's buffers until
  the socket's next receive; slots with storage take a copy. `IoLoop` reads lending sockets into storage-less slots.
- Found by the tests and fixed in the design: buffers sized for any datagram (a 1500-byte ring cut loopback datagrams
  the plain path carries whole); no re-arm while every buffer is lent (the host ends the receive at once; the wait
  watches the socket meanwhile); the end the host posts behind the last datagram read at once (it woke an empty
  wait); the error-queue watch (`sendmmsg` drops a socket error met past its first message, so a report reached no
  receive: the namespace train test caught it); one shot, not multishot (a flood of reports overflowed the
  completion ring); the ring follows the receiving thread (completions run as the arming thread).

## Evidence

- WSL linux-gcc-debug: the whole crd-net-tests (1024 cases) on the ring; the real-socket files again with
  `CRD_NET_UDP_RING=0` (71 cases, 5 ring-only skipped); `[.netns]` under `unshare -Urn` on both paths, three times
  each; the ring file 20 times in a row. The ring cases: exactly where the host runs it (an independent probe),
  counts that refuse it, lent and copied slots in one receive, the largest IPv4 datagram whole, a 4-buffer ring
  whole and in order at 1 to 64 slots a call, opened on one thread and read on another, after the opener ends, a
  job's 16 threads in turn, waits woken alone and in a set of two, a move and a close from another thread, and a
  router's Packet Too Big in a lent slot.
- win-debug: the same real-socket files pass (71 cases; the ring cases skip there).
- linux-clang-tsan: the ring file three times and the udp, io_loop and quic_client files, no report.
  linux-gcc-release: the real-socket files; its LTO link found a false `-Warray-bounds` in `SendQueue::queued`
  (69f6db02's), now bounded by `kMaxSetSockets` too.
- Teeth, each seen failing while the design settled: without the error-queue watch the namespace train test gets
  no report; a multishot watch fails the 80-report flood test; arming while every buffer is lent, or leaving the
  trailing end unread, fails the 4-buffer test's empty wait; 1500-byte buffers fail `test_udp`'s large datagrams.
- Board (`[.udp-board]`, release, one process, 3 interleaved rounds, a sender thread blasting 400000 datagrams of
  1200 bytes, 32 slots a receive): the receiver spends 325 to 349 ns of CPU a datagram on the ring against 323 to
  325 on `recvmmsg` (the kernel's copy dominates both), and loses 2 to 10% of the blast against 18 to 20%: the host
  drains the socket into the ring's buffers ahead of the receiver.

## Next

io_uring's zero-copy send (`SENDMSG_ZC`, with the send queue holding slots until the host's notification), Windows
RIO behind the same seam, then multipath and address discovery. `RECV_ZC` needs Linux 6.15 and a NIC that splits
headers: a physical item, named once here.
