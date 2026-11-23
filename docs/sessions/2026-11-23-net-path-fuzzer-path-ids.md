# 2026-11-23: NET-PATH, the connection fuzzer opens path IDs

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), Proof
bullet. RFC 10069 sections 2.4, 3.1, 3.2.1 and 3.4 read for it.

## What landed

- `fuzz_quic_connection.cpp`, with multipath on (byte 0's bit 6): 0xF0 has either end abandon a path ID (0 to 7,
  four error codes, one too large) and, over paths, 0xF2 has the client `add_path()` on a socket of its choice, to
  either server address. Each answer is checked exactly against the state, both limits, the paths still sending and
  the earlier abandonments; nothing refused moves a counter; a server's `add_path()` is `NotClient`.
- New oracles after every step: no path ID opens in a slot of either end twice (its nonce, section 2.4); `add_path()`
  never names an abandoned or earlier ID; each end's limit is its initial one plus the path IDs it closed, and its
  peer's view of it is never above it; the client opened exactly the IDs it added, the server no more.
- The one-path oracles read the right slot: migrations, the preferred address and the follow check read path ID 0's
  slots; ECN "failed stays failed" holds per slot and path ID; the server's credit counts what goes to an address no
  slot has proven, against all it received there; the stateless reset uses the ID of the slot that sent. A confirmed
  pair keeps the client's lowest path ID after its stream (each other one abandoned, `Abandoned` each), and the
  follow check also waits while the client validates its own address there (a path ID whose data another took).
- The transfer moves time at most 5 s per round: paths that acknowledge at once left only the idle timeout between
  writes, which closed both ends of healthy pairs (four older seeds among them; all now carry their stream).
- Engine fix (libFuzzer's first find, 29 inputs): a `send()` held back moved the PATH_ACKs' carrier and counted it
  in `ack_path_moves`; it now restores both, so the next send chooses again.

## Evidence

- New `[multipath]` case: the client marks path ID 0 backup while the server's sockets are held; the held send
  writes nothing and moves nothing, the next one moves the PATH_ACKs once. Fails with the restore removed.
- Corpus: four seeds (path IDs opened and abandoned by both ends with a refused code; path ID 0 abandoned and its slot
  reclaimed by path ID 2, then path ID 3 to the preferred address; a path opened on a held socket; path ID 0 refused
  as the last path, IDs 1 and 2 abandoned unused, then path ID 3) and four finds (the carrier, three oracle edges).
- Mutations: no limit rise on close and no `LastPath` are caught by the committed corpus; no carrier restore by the
  unit case and all 29 finds. Dropping `add_path()`'s abandoned-ID skip survives: an abandonment retires the path
  ID's peer IDs, so that check hides it. With e9afa1df's fix reverted, bb4b5a5c still fails the follow check (an
  uncapped variant of the transfer change hid it, so the cap replaced it).
- libFuzzer (linux-clang-fuzz, `-fork=2`): 500 s, 40623 executions, 30 finds; then 528 s, 31756 executions, one
  find (the follow edge above); then 520 s, 28650 executions, two finds: a raw client's frames left the server
  Closed three PTOs after it closed, which the frames oracle now allows once it was closing (corpus f4da46a8), and
  one UBSan load through a null recovery that never replayed, alone or after the whole corpus in one process (the
  host's known misexecution; recorded, not chased).
- linux-gcc-debug: crd-net-tests (1154 cases), every net corpus and the `crd-no-|crd-check` guards pass;
  linux-gcc-release links the fuzz target and crd-net-tests; LLVM 22 tidy is clean on the three changed sources.
  Validators pass.

## Next

- S78's hostile-path proof of the connection's paths (loss, reordering, duplication and rebinding on one path ID).
