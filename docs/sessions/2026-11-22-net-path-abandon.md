# 2026-11-22 — NET-PATH: PATH_ABANDON both ways, retention and the freed slot

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows the [CI fixes note](2026-11-22-net-path-ci-fixes-and-blocked-wake.md);
design: S78's "Lifecycle" bullet in [network-foundation.md](../design/network-foundation.md).

## CI first

No run on this track's commits had finished: CI 45018369099 (8877f587) pending, fuzz host-check 45017650858
(9f652553) queued. The tree was clean at `origin/main` after the shutdown; nothing was lost.

## What landed

- `QuicConnection::abandon_path` and the peer's PATH_ABANDON (RFC 10069 section 3.4) for path IDs above 0: the peer's
  IDs retired at once, the slot stopped, PATH_ABANDON owed on another path and resent when lost, the receiver's
  answer sent once (copies extend nothing). `QuicPathAbandon` (`abandon_of`) keeps both frames' codes and the deadline.
- `QuicRecovery::stop_path`: the stopped slot's packets in flight lost off every estimate, then no send, ACK frame or
  timer there; it still records arrivals, and its PATH_ACK rides another slot's packet (section 3.4.3).
  `QuicPathControl::stop` ends the controller and its coupling membership.
- Retention: slot and ID space close three PTOs after this side's frame is acknowledged (or the peer's first frame,
  if later); the slot then serves the next path ID, numbered from 0. A peer that never answers holds nothing longer.
- A further slot's failed validation abandons its path ID with PATH_UNSTABLE_OR_POOR (section 3.1's MUST).
- PATH_STATUS and PATH_CIDS_BLOCKED of an abandoned or closed path ID are ignored (section 4); the latter was
  PROTOCOL_VIOLATION for a closed one.
- No frame names a path ID above the peer's limit (section 4): `abandon_path` refuses one, and the peer's
  PATH_ABANDON of one above its own limit is closed after three PTOs without an answer (follow-up commit).

## Evidence

- linux-gcc-debug (WSL): `crd-net-tests` 1107 cases, 1100 passed, 7 skipped; `[multipath],[cids]` 56 cases; the
  recovery, connection and cids corpora replay clean. linux-gcc-release (LTO, `-Warray-bounds`, `-Wnull-dereference`)
  links and passes `[multipath],[cids]` after two fixes it found (a bounds path in `on_validated`, a test pointer).
- New cases: either end abandoning path ID 1 over lossy links with data in flight, then path ID 2 in the freed slot
  (4 seeds); a lost PATH_ABANDON; an unused path ID; a raw peer's 65 copies, a straggler and the closed path ID's
  frames; a raw peer that acknowledges and never answers (closed at the bound, not 1 ns before); path IDs above the
  peer's limit, both ways; both failed-validation cases; the refusals. linux-gcc-asan `[multipath],[cids]`: 56 cases
  pass; win-debug `[multipath],[cids]` passes.
- linux-clang-fuzz: `crd-fuzz-net-quic-recovery-libfuzzer` with the new stop operation, seed 8601, 300 s. The first run
  found a null slot-0 read in `can_send` once slot 0 closed (fixed: `QuicRecovery::stopped`); its input joins the
  corpus, and the rerun was clean (456183 runs).
- Teeth: no PATH_ACK riding, repeats re-answered, and no abandon on failed validation each fail their cases.
- `crd-fuzz-net-quic-connection-libfuzzer`, seed 8403, 300 s from the corpus: one oracle event, "0-RTT offered" at the
  start of an early-data script (before any path frame). It does not replay alone (3 of 3 clean); the harness
  seeds its anti-replay filter from `std::time`. Kept in `~/.cerid/track-c/artifacts` for the next batch.

## Next

- The connection fuzzer's unreplayable early-data event above: find its process state and fix it.
- A path ID 0 abandoned while others live; MAX_PATH_ID raised as path IDs close; concurrent validations paced.
