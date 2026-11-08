# 2026-11-08 — 4.2a: QUIC byte streams and flow control

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S38](../design/network-foundation.md#quic-streams).

## CI first

Nothing of this track failed. Complete-tier dispatch 42430818018 (378eed04) is running: its five failed jobs
(linux-gcc-shipping, win-arm64-debug and three ARM64 Linux lanes) each fail only `crd-no-untagged-physical-numeric`
on bessel.hpp, fixed by 40ad3b23. 42435554329 is still queued, so NET.3's hosted audit still waits. The relayed user words about the GPL rewrite belong to the other agent's work; this batch did not touch it.

## What landed (capability `network.transport`)

- `quic_stream.hpp`: `QuicReceiveBuffer` (reassembly by offset in a window, one bit per byte, the final size's three
  refusals, changed bytes refused, nothing changed by a refusal), `QuicSendBuffer` (held until acknowledged, lost
  data first and lowest first, fresh data under the flow-control limit, chunks never wrapping, the FIN with the last
  byte or alone), `QuicSendCredit` and `QuicReceiveWindow` (updates at half the window, on a blocked peer and after a
  lost update, auto-tuned by doubling within two RTTs up to the buffer).
- The connection's CRYPTO data goes through the same buffers: `QuicHandshake::take` consumes its copy, so the bytes
  sent stay in a `QuicSendBuffer` until acknowledged; received CRYPTO data is reassembled here before `provide`.
- `crd-fuzz-net-quic-stream` with 3 seeds, in `scripts/fuzz.py`.

## Evidence

- win-debug (MSVC): crd-net-tests 224 cases pass, 13 new; linux-gcc-debug: 225 cases pass; both replay the corpus.
- 24 source mutations of quic_stream.cpp each caught (the final size's three rules, the window, changed bytes, the
  contiguous end, consumed bits, lowest lost, the wrap, the FIN at the final size, acknowledged copies kept, lost
  bits cleared on acknowledgement and on sending, room, writes after FIN, blocked once per limit, half window, the
  tuning's RTT and maximum, the latest lost update, raise only, consumption bound, bitmaps zeroed). Three survivors
  of the first runs became tests; one redundant guard (`!m_fin_acked` on a lost FIN) was removed.
- linux-clang-fuzz: 1.22 M executions in 405 s, corpus 3 to 711, no artifacts.
- LLVM 20 strict tidy clean on the four new files.

## Next

- 4.2a: the connection core on S11 and S34 to S38: per-level CRYPTO buffers into `QuicHandshake`, transport
  parameters set and checked, recovery wired to a bounded sent-frame ledger with skipped numbers from entropy, key
  discards, amplification accounting, Initial padding, Retry, HANDSHAKE_DONE, idle timeout and close; then stream
  states and limits, DATAGRAM, connection IDs, stateless reset, the 0-RTT buffer and interop with a pinned peer.
