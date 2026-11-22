# 2026-11-22 — NET-PATH: MAX_PATH_ID raised as path IDs close

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Lifecycle bullet in
[network-foundation.md](../design/network-foundation.md). The tree was clean at 8417333c, level with origin/main.

## CI first

CI 45013880738 (e446680a): its new red, win-asan's `[cids]` stack-use-after-scope in "the peer's IDs are held per
path ID", is the case 9e56a205 already fixed; its other reds were fixed at HEAD before. Fuzz host-checks 45021997098
and 45017650858 are still queued.

## What landed

- RFC 10069 sections 3.2.1 and 4.6: each path ID that closes raises this side's limit by one
  (`local_max_path_id()`); the storage its ID space gives back opens the next path ID at once, so 1 +
  initial_max_path_id path IDs stay open for good and a closed one is never reused. MAX_PATH_ID is owed as the limit
  rises and resent when lost only while it carries the latest limit; the peer's frames are read under the raised
  limit. Path ID 0's close raises it only while a further slot is spare (the handshake slot is not reclaimed yet).
- `add_path()` walks the open ID spaces (`QuicCids::open_after`), bounded by kQuicMaxPaths however far limits rise.
- A PATHS_BLOCKED owed at the server's old limit is dropped when MAX_PATH_ID raises it (section 4.7).
- Defect found on the way: `send()`'s turn loop counted each turn from the slot the previous turn had just set, so
  with three slots open a slot could be skipped every call; a path whose challenge was due then spun its timer at one
  instant. Turns now count from the last writer.

## Evidence

- linux-gcc-debug (WSL): `crd-net-tests` 1116 cases (1109 passed, 7 skipped). New cases: twenty open-abandon
  cycles from alternating ends under 20% loss, 20% duplication and jitter, both limits rising to 22 past kQuicMaxPaths,
  three path IDs open throughout, the host routing exactly their IDs, path ID 21 then carrying a stream; the raw
  server's MAX_PATH_ID after each close, resent once when lost, not resent when the lost one is stale, IDs issued up to
  the raised limit, PATH_STATUS read under it and PROTOCOL_VIOLATION above it; a PATHS_BLOCKED owed at the old limit
  not sent once MAX_PATH_ID lands; the lost-PATH_ABANDON case now opening path ID 3 beside path ID 1 (three slots,
  which hung before the turn fix); `QuicCids::open_after` in `[cids]`.
- Teeth: resending every lost MAX_PATH_ID sends a fifth frame (stale case); keeping the stale PATHS_BLOCKED reaches the
  server; no raise stops the cycles at the first close.
- linux-clang-fuzz: corpora replay clean; quic-cids seed 9001, 180 s, 414551 runs (its model now checks the open
  path IDs in rising order); quic-connection seed 9002, 300 s, 29333 runs; clean.
- linux-gcc-release (LTO) links clean, `[multipath],[cids]` 65 cases pass there; after the teeth were restored the
  debug lane's `[multipath],[cids],[recovery],[quic-connection]` 213 cases pass. Pinned clang-tidy 22.1.8: the changed
  sources and tests clean.

## Next

NET-PATH: the handshake slot reclaimed once path ID 0 closes; concurrent validations paced (section 7.2).
