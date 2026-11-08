# 2026-11-08 — NET.4 after the rows it tests; 4.2a: addresses and UDP sockets

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET.4](../ROADMAP.md#slice-net.4), [4.2a](../ROADMAP.md#slice-4.2a). Design: [S33](../design/network-foundation.md#sockets).

## CI first

Nothing new from this track. Dispatches 42430818018 and 42435554329 are still queued (only Preflight exists);
42416903705's three Build failures (linux-gcc-release, -shipping, -relwithdebinfo: link-time `-Warray-bounds` in
crd-containers-tests) are older than their fix 8d5c9e63. NET.3 still waits on the audit of the first complete-tier
run containing 9c7a6717.

## Order

4.2a listed NET.4 as its prerequisite, but NET.4 attacks and measures the transport, its paths, reachability and
NET-DEBUG's emulation lane. NET.4 now follows NET-DEBUG and precedes NET.5 (still in the NETCRASH window); 4.2a
depends on NET.3 and NET-STD; NET.5 gains NET.4. Delivery plan P02 updated.

## What landed (4.2a, capability `network.transport`)

- `endpoint.hpp`: one representation per address (IPv4 tail zero, zone only on IPv6, dual-stack peers unmapped),
  RFC 4291 reading and RFC 5952 printing without a host call, `prefix()` for S3's buckets.
- `udp.hpp`: `DatagramSocket` (the seam NET-SIM and NET-DEBUG implement) and `UdpSocket`: non-blocking, batched into
  caller storage (Linux `recvmmsg`/`sendmmsg` per 32), truncation marked, `Busy` back-pressure, family and size
  refusals before the host, Windows `SO_EXCLUSIVEADDRUSE` and ICMP report suppression, no allocation.
- `crd-fuzz-net-endpoint` with a committed corpus; host-check.yml gains two net-endpoint legs.

## Evidence

- win-debug (MSVC): crd-net-tests 144 cases pass; corpus replay 12 inputs. linux-gcc-debug: 145 cases pass,
  including 200,000 random texts whose verdict and bytes agree with glibc's `inet_pton` and 20,000 addresses printed
  as `inet_ntop` prints them. LLVM 20 strict tidy clean on every changed file.
- linux-clang-fuzz: the first run found a real defect: eight groups followed by `::` were accepted, because the gap
  position 8 was also the "no gap" sentinel. Fixed (a flag), with a unit case and a named seed.
- On the fixed code, 770 M executions in two runs ended with four events (an oracle verdict, a stack overflow at a
  stack pc, a stack-use-after-return in `std::min`, a SEGV in `parse_v4` on `z:3`). None replays (2000 runs each);
  the functions are pure over the input. Recorded per the host lesson, and sent to the hosted host check.

## Next

- The audit of the first complete-tier run with 9c7a6717 (closes NET.3) and of the first run with this batch.
- 4.2a: the I/O loop and executor seam with an injected clock, stages S1 to S3 (sanity, routing by a keyed PRF,
  admission buckets with Retry), then QUIC v1/v2 packets, streams, datagrams and loss recovery.
