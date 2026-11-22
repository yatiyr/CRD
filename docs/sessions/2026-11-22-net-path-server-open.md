# 2026-11-22 — NET-PATH: a server opens a path ID in a slot of its own

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), "Server-side
open". Follows the [4-tuples note](2026-11-22-net-path-path-tuples.md)'s Next.

## CI first

Nothing to fix. No run on this track's commits has finished: the cancelled CI runs ran no job, and fuzz host-checks
44963728694, 44960293954 and 44956266583 are still queued. Local runs are the only evidence.

## What landed

- `refactor(net)` b2c36541: the open path slots take turns in `send()`, each with its own amplification credit, owed
  ACK and timers. An ACK frame acknowledges path ID 0 and a PATH_ACK any other path ID (`QuicAckTracker::write`'s
  `path_id`), so a carried acknowledgement is settled in the slot that sent it.
- `feat(net)` 5cb7a389:
  - The limit can be up to kQuicMaxPaths - 1. `PathSlot`s are allocated at construction.
  - ID spaces open at negotiation, and IDs are issued up to the peer's limit (RFC 10069 section 3.2.1).
  - `begin_path`, `commit_path` and `undo_path` open a path ID's slot at its first authenticated packet. The slot
    gets its own validation, and its data waits until validation ends (section 3.1). Packets are sealed in their
    slot's number space.
  - A PATH_ACK of a path ID that never opened is PROTOCOL_VIOLATION.
  - The owed PING is the handshake slot's.
  - QuicHost now sizes its peer-token table by `QuicHostLimits::path_ids`: before this, a multipath connection could
    overflow it.

## Evidence

- `crd-net-tests "[net],[cids]"` on linux-gcc-debug (WSL): 886 cases, 879 passed, 7 skipped.
  New cases:
  - ID spaces up to each side's limit;
  - the server open, with a forged first packet undone, the PATH_RESPONSE, the PATH_ACK of packet 1, a challenge to
    path ID 1's ID on the new 4-tuple, at most 3600 bytes there before validation, and validation;
  - a PATH_ACK of a never-opened path ID;
  - every refused allocation;
  - `top_up` within the peer's limit;
  - `quic_path_ids`.
- win-debug: `[quic-connection],[multipath],[recovery],[cids],[quic-server],[quic-client]`, 220 cases pass.
- `ctest -R "crd-check|crd-no-|quic|net-" -E "netns|router"` on linux-gcc-debug: 238 of 238 pass. One construction
  was reordered for `crd-check-allocation-results`'s window.
- The connection fuzzer runs at limit 2, and its host oracle counts every path ID's IDs. All QUIC fuzz corpora
  replay.
- Pinned clang-tidy (LLVM 22.1.8): all 9 touched sources are clean.

## Next

The client opens a path: the smallest unused path ID with IDs both ways, its slot and PATH_CHALLENGE, and validation.
Then PATH_ABANDON both ways, with three PTOs of retention, and the slot and ID space freed together. A failed
validation abandons its path ID. MAX_PATH_ID rises as path IDs close. Last, the connection and cids fuzzers open path
IDs.
