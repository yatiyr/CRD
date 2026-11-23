# 2026-11-23: NET-PATH, the hostile path and per-slot credit

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), Proof bullet.

## CI

Fuzz host-check 45153550994 (0949f05c) failed, net-quic-connection seed 8402. Its input 805dc37f fails the follow
check's final REQUIRE and replays on HEAD; it is find 0736d765's class (Next). CI 45126393085 passed.

## What landed

- Connection fuzzer, per-slot credit: each client datagram records its path ID and counts for the slot that reads it,
  the leading slot for a long header or an unrouted ID (8-byte IDs on the keyed raw path). Each unproven 4-tuple of
  each open slot holds `credit() + sent <= 3 * received` at every delivery and send. The per-address sum is gone.
- Follow check: a failed validation is required only once the server has opened a packet of the kept path ID from the
  client's current address, sent in the client's current epoch (since its last rebinding or move).
- `Net` fixture: loss, copies and jitter per client socket. A new `[multipath]` case (4 seeds) makes only path ID 1's
  link hostile and NAT-rebinds its socket mid-stream, to a new port or a new address. The server follows in slot 1
  before validating there, and keeps congestion state only after a port change. Path ID 0's 4-tuple, table, floor and
  controller stay untouched, and 192 KiB arrive whole.

## Evidence

- Mutants, each caught and then restored:
  - Cross-slot crediting: 5 committed inputs catch it. The old summed oracle missed it on the committed corpus.
  - `send()` spending the leading slot's credit: caught.
  - Port-only keep on slot 0 alone: the case's floor check fails.
  - Further slots never following: the case times out.
  - The waiting-path skip reverted: bb4b5a5c fails the relaxed REQUIRE itself. The committed corpus reaches that
    REQUIRE with the address heard 5 times (failures 1 to 4) and unheard 10 times.
- libFuzzer runs of 168, 362, 345, 410 and 436 s:
  - f370db2c: path ID 0 was dark with its 72-entry storage full, waiting for its probe timeout.
  - A harness null `Tuples` access in the leading-slot loop.
  - 9976136a: a stale packet counted as heard.
  - 3bbcc526: keyed IDs read as 16 bytes.
  - 0736d765.
  - The three oracle inputs are in the corpus.
- linux-gcc-debug: crd-net-tests 1155 cases (7 skipped), every net corpus and the `crd-no-|crd-check` guards.
  linux-gcc-release builds. LLVM 22 tidy is clean, and the validators pass.

## Next

- 805dc37f and 0736d765 (in WSL `~/hc8402`, `~/fz-slot5-art`), a hypothesis to measure. The client rebinds and moves to
  the preferred address. The server's move to socket 1 is deferred, so its active path stays on the dead old address,
  while socket 0 holds the client's new address validated. Measure:
  - the client's storage (count and eliciting) when its PATH_RESPONSEs are due;
  - the server's validation deadlines against the client's probe timeouts.
  If storage is the cause, revisit f370db2c's relaxation too. Then take picoquic interop.
