# 2026-11-08 — 4.2a: QUIC acknowledgements and loss recovery

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S37](../design/network-foundation.md#quic-recovery).

## CI first

Nothing of this track failed. In 42428822989 (20a1cfcd), every lane's `crd-no-untagged-physical-numeric` is the
rewrite agent's bessel.hpp (fixed by 40ad3b23) and the mikktspace UBSan shift is TP-5. win-clang-cl-shipping also
failed `starvation watchdog: a Low lane flooded by Normal work` (backlog read as 0). Cause: the test's handler
published the fire count before storing the report's backlog, both relaxed. It now stores the fields first and
publishes the count with release (read with acquire). NET.3's complete-tier dispatches are still queued. The relayed
user words about the GPL rewrite belong to the other agent's work; this batch did not touch it.

## What landed (capability `network.transport`)

- `quic_recovery.hpp`: `QuicAckTracker` (32 merged ranges, every RFC 9000 section 13.2 trigger, ACK frames trimmed
  to the room with delay and ECN counts, ranges retired by an acknowledged ACK), `QuicRtt` (RFC 9002 section 5) and
  `QuicRecovery` (appendix A: the thresholds, the loss timer, the probe timeout with backoff, the Application space
  held until confirmation, the client's anti-deadlock probe, the amplification-blocked server, key discards,
  persistent congestion), sent packets in caller storage, acknowledgements of numbers never sent refused.
- `QuicRecoveryEvents`: per-packet acknowledged, lost and discarded events, losses first: the seam NET-PATH's
  controllers and the connection's frame retransmission read.
- `crd-fuzz-net-quic-recovery` with 3 seeds, in `scripts/fuzz.py`.

## Evidence

- win-debug (MSVC): crd-net-tests 211 cases pass, 19 new (429 assertions); crd-jobs-tests `[starvation]` 8 runs pass.
  linux-gcc-debug: 212 cases pass, the recovery corpus replays clean.
- 23 source mutations of quic_recovery.cpp each caught (thresholds, delay cap and adjustment, Initial delay, the
  Application wait, Handshake ACK validation, unsent gaps, persistent congestion's run break and sample rule, the
  gap trigger, CE, the second packet, retired ranges and the kept largest, arming only in flight, discard reset,
  amplification, the RTT sample rule, the backoff and its reset, compaction, loss-before-ack order).
- linux-clang-fuzz: 1.04 M executions in 408 s, corpus 3 to 1006, no artifacts.
- LLVM 20 strict tidy clean on the four new files and the starvation test.

## Next

- Audit NET.3's first completed complete-tier run (9c7a6717 onwards) and close it.
- 4.2a: the connection: CRYPTO reassembly and the TLS handshake carrying the parameters, the recovery wired in with
  skipped numbers from entropy, streams and flow control, DATAGRAM, connection IDs, stateless reset, INVALID_TOKEN,
  a bounded 0-RTT reorder buffer; then interop against a pinned peer (ngtcp2 or picoquic).
