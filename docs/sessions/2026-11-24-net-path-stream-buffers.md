# 2026-11-24: NET-PATH, stream buffers grown on demand

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S38](../design/network-foundation.md#quic-streams)'s Storage
and [S40](../design/network-foundation.md#quic-stream-set)'s Slots. Board:
[2026-11-24](../bench/2026-11-24-net-path-stream-buffers.md).

## CI first

Nothing of this track failed: CI 45238977369 (f4da960a) had no failed job, CI 45265525125 (c19dbe8f) had no job yet.

## What landed

- The last note's open question: a client closing as its handshake completes, before its Finished leaves, closes its
  server in the Handshake space (APPLICATION_ERROR, no reason), which drains three of its longest PTOs at the initial
  RTT, 3.07 s: the board's 3 s, RFC 9000 section 10.2.3 and RFC 9002, no defect (a new `test_quic_connection.cpp` case).
- `QuicReceiveBuffer::grow` and `QuicSendBuffer::grow` move a buffer's contents into larger storage; a FIN or final
  size alone needs no room. `QuicStreamSet` gives a slot 4 KiB buffers when it first holds a stream and doubles them
  up to the limits as data needs; the slot keeps them. In-window data refused room closes with INTERNAL_ERROR, a send
  buffer refused growth takes less, `open()` refused buffers is NoMemory (ChannelLink: Closed). `whole_buffers` keeps
  fixed memory; `buffer_allocations()` and `buffer_bytes()` report the buffers.
- The suites that asserted no allocation after init (QUIC connection, keyed, client, server, congestion, QMux, QMux
  host, WebSocket, tunnels, WebTransport, channels) now count the streams' buffers apart and keep `==`; the fuzz
  oracles on live or peak bytes subtract `buffer_bytes()` or compare `BudgetAllocator::allocations()` with
  `buffer_allocations()`.

## Evidence

- linux-gcc-debug and linux-gcc-asan: all 1,169 `crd-net-tests` cases (7 Windows-only skips), the 30 net fuzz corpus
  CTests (seven under ASan). New: `grow` at all 128 read offsets into 128, 192 and 256 bytes; the send buffer grown at
  every offset chunk for chunk against a never-grown one; the set's growth, every refusal and a zero leak count; three
  slot reuses allocating nothing; `crd-fuzz-net-quic-stream` growing both buffers into scribbled-over other storage
  (seed `grow`). clang-tidy 22.1.8 clean on every changed source, clang-format on the added lines, braces pass.
- Board: crd-net 3,693 handshakes per CPU-second at 1 MiB windows (915 before), 3,626 at 64 KiB; bytes unchanged.

## Next

The msquic arm's connection that never ends at 1 MiB (both runs); the bulk gap (ksoftirqd's share, then perf);
quiche's end of the board; p99 latency under 1% loss and reordering; memory per connection from `buffer_bytes()`.
