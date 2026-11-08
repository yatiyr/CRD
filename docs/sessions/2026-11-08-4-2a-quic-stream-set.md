# 2026-11-08 — 4.2a: the connection's streams

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S40](../design/network-foundation.md#quic-stream-set).

## CI first

Nothing of this track failed. All eleven failed jobs of dispatch 42430818018 (378eed04) fail only
`crd-no-untagged-physical-numeric` on bessel.hpp (fixed by 40ad3b23); linux-gcc-asan also fails the mikktspace
oracle (TP-5). 42435554329 is still queued. The relayed user words about the GPL rewrite belong to the other agent's
work (operator note, 2026-11-07); this batch did not touch it.

## What landed (capability `network.transport`)

- `quic_stream_set.hpp`: `QuicStreamSet`, every stream of a connection: four slot pools found by binary search,
  implicit opening, STREAM_LIMIT_ERROR and STREAM_STATE_ERROR, MAX_STREAMS as slots free (after the application
  polled), one STREAMS_BLOCKED per limit, both credits as running totals, sections 3.1 to 3.5's states with
  STOP_SENDING answered by RESET_STREAM, section 13.3's retransmissions, urgencies with round robin, an exact
  `has_frame()`, zero-copy `peek`/`consume`, and a per-slot event queue that cannot fill.
- `QuicConnection` carries it: `QuicConnectionConfig::streams` fills the transport parameters, stream frames go to
  the set and its frames ride in 1-RTT packets as ledger records (eight per packet by default).
- S38 fix: a lost-update resend no longer doubles the window (it is not evidence of a fast reader).
- `memory::try_construct_array` replaces the copies of the same helper in io_loop.cpp and quic_connection.cpp.
- `crd-fuzz-net-quic-stream-set` with 3 seeds; `crd-fuzz-net-quic-connection` now carries a stream after its scripts.

## Evidence

- win-debug (MSVC): crd-net-tests 262 cases pass (16 new); linux-gcc-debug: 263 cases pass; both lanes' construct
  cases, the 20 `crd-no-*`/`crd-check-*` guards and both corpus replays pass.
- 37 source mutations; the first pass left 12 survivors: 9 became tests (the STOP_SENDING code in the reset, a
  blocked peer's grant, readable and writable events, DATA_BLOCKED's cause, each kind's peer grant, a repeated reset,
  data after STOP_SENDING), 2 were redundant conditions now removed, and 1 is the unreachable recovery of a frame
  `next_frame` sized that fails to fit: 34 of the 35 on the final code are caught.
- linux-clang-fuzz: `net-quic-stream-set` 1,062,587 executions in 233 s (corpus 3 to 1614), `net-quic-connection`
  54,292 in 233 s (4 to 900); no artifacts. LLVM 20 strict tidy clean on all 13 changed C++ files.

## Next

- 4.2a: DATAGRAM (RFC 9221), this side's connection IDs and stateless reset, Retry and NEW_TOKEN on the client,
  compatible version negotiation's switch, the 0-RTT buffer and 0-RTT streams, the connection behind the
  IngressHandler, interop with a pinned peer; then the TCP and TLS path and the browser endpoints.
