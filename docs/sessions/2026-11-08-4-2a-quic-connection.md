# 2026-11-08 — 4.2a: the QUIC connection core

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S39](../design/network-foundation.md#quic-connection).

## CI first

Nothing of this track failed. Complete-tier dispatch 42430818018 (378eed04) still runs; its five failed jobs fail only
`crd-no-untagged-physical-numeric` on bessel.hpp (fixed by 40ad3b23), read from each job's log. 42435554329 is still
queued, so NET.3's hosted audit still waits. The relayed user words about the GPL rewrite belong to the other agent's
work (operator note, 2026-11-07); this batch did not touch it.

## What landed (capability `network.transport`)

- `quic_connection.hpp`: `QuicConnection`, sans-I/O, carrying `QuicHandshake` in S36's packets to confirmation in both
  versions: per-level CRYPTO data through S38's buffers, the peer's parameters checked on arrival (section 7.3, RFC
  9368 both ways), S37's recovery settling a bounded frame ledger (CRYPTO, HANDSHAKE_DONE, RETIRE_CONNECTION_ID),
  probes resending unacknowledged CRYPTO data, the anti-amplification limit with recovery's blocked state, Initial
  padding and coalescing, the QUIC bit greased toward a greasing peer, 1-RTT numbers skipped from a SipHash entropy
  stream, key discards, HANDSHAKE_DONE resent, every frame type dispatched (stream frames and DATAGRAM refused with
  RFC 9000's and RFC 9221's errors while no credit is advertised), the peer's connection IDs with retirement,
  PATH_CHALLENGE answered, a PING after 16 ACK-only packets, client Initials under 1200 bytes discarded, NEW_TOKEN
  kept, Version Negotiation read, closing, draining, idle timeout, `ping()`; no timer is reported due that nothing
  serves (an owed ACK in a space without keys or storage).
- `crd-fuzz-net-quic-connection` with 4 seeds, in `scripts/fuzz.py`.

## Evidence

- win-debug (MSVC): crd-net-tests 246 cases pass, 22 new; linux-gcc-debug: 247 cases pass; both replay the corpus.
- 27 source mutations of quic_connection.cpp each caught. Of the first run, five survivors became tests (probes
  carrying data, the Source Connection ID check, the ID switched to on retirement, idle restart on sending, the
  retirement bound) and two hung the tests: timers reported due that nothing served, now excluded (`can_acknowledge`)
  and caught by a stall oracle in the test network and the fuzz target; a peer that acknowledges only ack-eliciting
  packets would have filled storage with ACK-only packets, now ended by the PING.
- 3,000 generated inputs replayed through the fuzz target found two oracle errors of its own (a peer's close code
  held to this side's codes; a clean-path bound in time where a script had taught an RTT of minutes), both fixed in
  the target; linux-clang-fuzz: 113,031 executions in 387 s, corpus 4 to 998, no artifacts.
- LLVM 20 strict tidy clean on the four new files.

## Next

- 4.2a: streams (states, limits, data through S38 with both credits), DATAGRAM, this side's connection IDs and
  stateless reset, Retry and NEW_TOKEN, compatible version negotiation's switch, the 0-RTT buffer, the connection
  behind the IngressHandler, interop with a pinned peer; then the TCP and TLS path and the browser endpoints.
