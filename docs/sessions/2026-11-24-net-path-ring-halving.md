# 2026-11-24: NET-PATH, a refused ring is asked again with half its buffers

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follow-up named by
[the in-order note](2026-11-24-net-path-in-order-bits.md): a resource refusal retries with half the buffers.
CI first: nothing to fix (no run finished since 04440292; host-check 45296064704 and CI 45284160004 still queued).

## What landed

- `UdpSocket::open`: a ring the host refuses for memory or kernel resources at Memory, Setup or Register (Linux
  ENOMEM, ENOBUFS, EAGAIN; Windows' memory and quota codes and WSAENOBUFS) is asked again with half the buffers, down
  to 4. io_uring retries on the same socket. A Registered I/O socket takes no second request queue, so the next one
  reopens with half the buffers in its place. A descriptor limit or a policy (EPERM) ends the ring at once.
- `SocketFeatures::ring_buffers` reports the count held (0 with no ring). `ring_refusal` and `ring_error` keep the
  first refusal while a smaller ring lends, so the Azure refusal that CI 45238977369 hit stays readable.
- The `udp-ring` host-check tally keeps `, granted N` in its key; the test prints it beside the step and errno.

## Evidence

- Linux, `RLIMIT_AS` lowered to the process's size plus 256 MiB: a 32768-buffer ring is refused at Memory, ENOMEM
  and lends 2048 buffers; with 192 KiB, not even 4 fit and the socket receives through `recvmmsg`, still naming
  Memory, ENOMEM. A sandbox's Setup, EPERM asks for no smaller ring.
- Windows, a nested job limiting the process's commit: 256 MiB gives 2048 buffers after Memory, error 1455
  (`ERROR_COMMITMENT_LIMIT`); 1 MiB (less than the 72 send slots) gives the plain path, Memory, 1455. Unlimited, 4,
  1024 and 32768 buffers all lend whole.
- Teeth: the Linux predicate forced false fails the 256 MiB section; the Windows reopen forced plain fails it (3
  assertions).
- linux-gcc-debug and linux-gcc-asan `[udp]` (34 cases), win-debug `[udp]` (33), the 19 `crd-no-*`/`crd-check*`
  guards; pinned clang-tidy 22.1.8 clean on the three changed sources; clang-format on added lines; braces pass.

## Next

- The bulk gap's kernel half: count datagrams per send and per receive call and the receiver's ACK rate.
- The Azure refusal's step and errno, from the next hosted linux-gcc-asan run that refuses a ring.
