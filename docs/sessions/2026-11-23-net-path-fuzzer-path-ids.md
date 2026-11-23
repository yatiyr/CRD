# 2026-11-23: NET-PATH, the connection fuzzer opens path IDs

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), Proof
bullet. RFC 10069 sections 2.4, 3.1, 3.2.1 and 3.4 read for it.

## What landed

- `fuzz_quic_connection.cpp` with multipath: 0xF0 has either end abandon a path ID (0 to 7, four error codes, one
  too large); over paths 0xF2 has the client `add_path()` on any socket to either server address. Every answer is
  checked exactly (state, both limits, paths still sending, earlier abandonments); nothing refused moves a counter.
- New oracles: no path ID opens in a slot of either end twice (section 2.4); `add_path()` never names an abandoned or
  earlier ID; each end's limit is its initial one plus the IDs it closed, its peer's view never above it.
- The one-path oracles read the right slot: path ID 0's for migrations, the preferred address and the follow check;
  ECN per slot; the server's credit per address over every slot; the stateless reset by the slot that sent. A
  confirmed pair keeps the client's lowest path ID after its stream, and the follow check waits while the client
  validates its own address. The transfer moves time 5 s at most per round (healthy pairs idled out before).
- Engine fix (libFuzzer's first find, 29 inputs): a `send()` held back moved the PATH_ACKs' carrier and counted it;
  it now restores both. The frames mode allows Closed once the server was closing (its third find).

## Evidence

- New `[multipath]` case: with the server's sockets held, the client marks path ID 0 backup; the held send moves
  nothing, the next one moves the PATH_ACKs once. Fails with the restore removed.
- Corpus: four path-ID seeds (both ends abandoning, a refused code, an ID opened after closes; path ID 0's slot
  reclaimed, a path to the preferred address; a held socket; `LastPath` from both ends and unused IDs abandoned)
  and four finds. Each seed reaches the datagram exchange and the stateless reset.
- Mutations caught by the committed corpus: no limit rise on close, no `LastPath`, closed spaces reopened under their
  own IDs (the `add_path()` check; the per-slot check alone does not see it, as no seed reopens an ID that held a
  slot). The carrier mutant fails the unit case and all 29 finds. Dropping `add_path()`'s abandoned-ID skip survives:
  an abandonment retires the ID's peer IDs first. With e9afa1df reverted, bb4b5a5c still fails.
- libFuzzer (linux-clang-fuzz, `-fork=2`): 500 s (40623 runs, 30 finds), 528 s (31756, one) and 520 s (28650, two:
  the frames edge and one UBSan null load at `quic_recovery.hpp:401` that never replayed, `~/fz-mp3-art/crash-bd8fbb72`
  in WSL; the host's known misexecution, recorded).
- linux-gcc-debug: crd-net-tests (1154 cases), the net corpora and the `crd-no-|crd-check` guards; linux-gcc-release
  and linux-clang-fuzz build clean; LLVM 22 tidy clean on the changed sources; validators pass.

## Next

- A per-slot credit oracle (receives attributed to their slot), then S78's hostile-path proof on one path ID.
