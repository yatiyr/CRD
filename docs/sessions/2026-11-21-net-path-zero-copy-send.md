# 2026-11-21: NET-PATH, io_uring's zero-copy send

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S33](../design/network-foundation.md#sockets).

CI first: run 44887080529 (70f94328) fails linux-gcc-release, -shipping and -relwithdebinfo on GCC LTO's
`-Warray-bounds` in `SendQueue::queued`, which 7ed26c44 already bounds; all three lanes build at this batch's head.

## What landed

- `DatagramSocket::send_stable`, `sends_taken`, `sends_released(wait)`: a caller that keeps its bytes still lets a
  socket lend them; the defaults copy, so simulated sockets are unchanged.
- `SendQueue` sends through `send_stable` and keeps lent slots (queued, lent, free in its order) until the socket
  releases them, counted in admission; its destructor waits for the host.
- `udp_ring.cpp`: a second io_uring per socket for `IORING_OP_SENDMSG_ZC` with copy reports, one linked chain a send
  with MSG_DONTWAIT returning what `sendmmsg` would, COOP_TASKRUN with TASKRUN_FLAG, 128 requests, the 17-fragment
  bound counted as the kernel joins pages, eight copy reports turning it off and a probe every 1024 sends, a close
  that waits. `udp.cpp` numbers what it takes and sends a message the host failed zero-copy again copied, once.
- `UdpOptions::zero_copy_bytes` defaults to 0: the board below.

## Evidence

- WSL linux-gcc-debug: all 1027 crd-net-tests cases; `[.netns]` on both paths; the `CRD_NET_UDP_RING=0` file
  filter; win-debug's touched files (zero-copy cases skip). linux-gcc-release, -shipping and -relwithdebinfo build
  warning-free; release runs the UDP, ring, zero-copy, loop and QUIC host files and `[.netns]`; linux-clang-tsan runs
  the zero-copy, ring and loop files and the namespace case clean; clang-tidy 22.1.8 clean on the changed sources
  (it also named the receive ring's `this_thread` constant, renamed).
- Found by the tests: loopback inside `unshare -n` is down (the loopback cases run outside it); netem orphans its
  buffers, so a socket behind it never fills (a token bucket does); a scattered 16-datagram train fails EMSGSIZE
  (MAX_SKB_FRAGS); a copied retry has to stick to its message, or a stale-report resend asks zero-copy again.
- Teeth seen failing: no destructor wait, lent slots outside admission, no fragment bound, no MSG_DONTWAIT.
- Board (`[.udp-board]`, dummy device, 3 rounds, sender CPU a KiB, zero-copy against copied): 1 datagram 928-1044 ns
  against 620-654; 8 in slots 186-195 against 127-134; 12 in slots 149 against 106-107; 54 contiguous (64800 B) 148
  against 66. Zero-copy loses 1.4 to 2.2 times at every size on this host.

## Next

Zero-copy measured on a real NIC with UDP segmentation offload (the cloud run) to set its default; Windows RIO; then
multipath and address discovery behind their NET-STD flags.
