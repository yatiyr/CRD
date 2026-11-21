# 2026-11-22 — NET-PATH: routing by path ID and single-use path IDs

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), "Routing by
path ID". The first slices of the path ID lifecycle; this side's limit stays 0 until the server-side open lands.

## CI first

Run 44941356765 (0799aca4, dispatched) failed six lanes, all already fixed at HEAD: the four Windows lanes (win-debug,
win-debug-scalar, win-asan, win-arm64-debug) and win-tidy by 48bc7127 and the batch before, linux-gcc-public-checks'
two `[.netns]` CTests by 6ce80283. No later run finished.

## What landed

- `refactor(net)` 50b161ba: a 1-RTT packet opens, decodes and is recorded in the slot holding its connection ID's
  path ID (`slot_of`), dropped when none does; only path ID 0's packets reach S75's migration. ACK frames reach
  path ID 0's slot and PATH_ACK its own, through the receiving slot: recovery, control, RTT, bytes in flight and
  key phases per slot; a path ID whose state is gone is ignored (RFC 10069 section 4). PATH_STATUS keeps the
  newest sequence per path ID (section 4.3). No behaviour changes while the limit is 0; the new branches are proven
  with the lifecycle.
- `feat(net)` d4988bb5: `QuicCids::open` takes path IDs once and in rising order; `consumed()` names every path ID
  that can no longer open (sections 2.4 and 3.4). The fuzzer's `spaces-refused-and-reused` seed now opens upward
  and tries a consumed path ID (seed-d7a2ea8c8aea6dab replaces seed-d211fbfb2d749207).

## Evidence

- `crd-net-tests "[net]"` on linux-gcc-debug (WSL): 867 cases, 860 passed, 7 skipped as before; `[cids]` 12
  cases; the `crd-check*`, `crd-no-*`, cids and connection corpus CTests pass after the last sync. The pinned
  clang-tidy (LLVM 22.1.8, win-tidy database) reports the four changed sources clean.
- `crd-fuzz-net-quic-cids-libfuzzer` on linux-clang-fuzz, 2 jobs for 240 s: 1.03 M runs, no fault; the model now
  checks `consumed()` for every path ID after each operation.
- Teeth: opening by `is_open` alone fails three `[cids]` assertions and the corpus replay.

## Next

The server-side open, in one slice with the raised limit: path ID spaces opened in rising order on issue or on the
peer's frame, extra slots' recovery, control and storage allocated at construction, a tentative protection slot
rolled back when the first packet fails, each path ID's own 4-tuple, PATH_RESPONSE and PATH_ACK in its own space,
PATH_ABANDON with three PTOs of retention and MAX_PATH_ID rising as slots free, with the connection fuzzer's oracles.
First decision: `QuicPaths` (S75, 115 uses) knows no path ID, so how a path ID owns its 4-tuples, two path IDs
sharing one, comes first. Latent at limit 0, fixed then: `ack_frame` must refuse a PATH_ACK of a path ID never opened
under the limit (only a consumed one is ignored), the PATH_STATUS table must free a closed path ID's entry, and
`admit_path` must not run for a packet of another path ID. RFC 10069's text: `~/.cerid/downloads/drafts-2026-11-21/`
`rfc10069.txt`.
