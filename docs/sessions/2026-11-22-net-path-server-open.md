# 2026-11-22 — NET-PATH: a server opens a path ID in a slot of its own

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), "Server-side
open". Follows the [4-tuples note](2026-11-22-net-path-path-tuples.md)'s Next.

## CI first

Nothing to fix. Hosted CI has returned no result for this track since 0799aca4: the cancelled CI runs ran no job,
and the fuzz host-checks are queued. Fuzz host-check 44969571822 was dispatched on a7367245.

## What landed

- `refactor(net)` b2c36541: the open path slots take turns in `send()`. Each slot has its own amplification credit,
  owed ACK and timers, and acknowledges its own space: an ACK frame for path ID 0, a PATH_ACK for any other path ID.
- `feat(net)` 5cb7a389:
  - The limit can be up to kQuicMaxPaths - 1. The `PathSlot`s are allocated at construction.
  - ID spaces open at negotiation, and IDs are issued up to the peer's limit.
  - `begin_path`, `commit_path` and `undo_path` open a path ID's slot at its first authenticated packet; a forged
    first packet is undone. The slot validates its 4-tuple before any data goes there.
  - A packet is sealed in its slot's number space.
  - A PATH_ACK of a never-opened path ID is PROTOCOL_VIOLATION.
  - QuicHost sizes its peer tokens by `QuicHostLimits::path_ids`: before this, multipath could overflow the table.
- `fix(net)`: a slot sends nothing before it holds the client's ID for its path ID (`addressed`), and its PMTU search
  waits for validation.

## Evidence

- `crd-net-tests "[net],[cids]"` on linux-gcc-debug (WSL): 888 cases, 881 passed, 7 skipped. The new cases cover ID
  spaces up to each side's limit, the server open, an ID that arrives after the path's first packet, the further
  path's challenge timers, amplification limit and expiry, a never-opened PATH_ACK, every refused allocation,
  `top_up` within the peer's limit and `quic_path_ids`. Disabling `addressed` fails two assertions.
- `ctest -R "crd-check|crd-no-|quic|net-" -E "netns|router"` on final HEAD: 238 of 238 pass, all fuzz corpora
  included. The connection fuzzer runs at limit 2 (ID spaces and frames). It opens no path ID yet.
- win-debug: `[multipath],[quic-server],[cids]`, 52 cases pass. Pinned clang-tidy (LLVM 22.1.8): every touched source
  is clean.

## Next

- The client opens a path: the smallest unused path ID with IDs both ways, and its validation.
- PATH_ABANDON both ways, with three PTOs of retention and the slot and ID space freed. Today an abandon of any path
  ID closes the connection.
- A failed validation abandons its path ID, and MAX_PATH_ID rises as path IDs close.
- The fuzzers open path IDs. A hosted case routes a path-1 ID through `QuicServer`'s ingress, and `QuicHost::build`'s
  clamp gets a test.
