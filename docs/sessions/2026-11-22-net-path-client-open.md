# 2026-11-22 — NET-PATH: a client opens a further path

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), "Client
open". Follows the [server-side open note](2026-11-22-net-path-server-open.md)'s Next.

## CI first

Nothing to fix. No hosted run on this track's commits has finished: fuzz host-checks 44969571822 (a7367245) and
44963728694 (d8163867) are still queued, and CI 44970457476 (e446680a) is pending.

## What landed

- `QuicConnection::add_path` and `QuicClient::add_path` (RFC 10069 section 3.1). The client takes the smallest path
  ID within both limits that holds IDs both ways and opens it in a free slot. Its 4-tuple is trusted and validated
  at once. Refusals: NoMultipath, NoPathId, NoConnectionId, plus `probe_path()`'s own. `disable_active_migration`
  forbids the handshake address alone (section 2.2).
- `free_slot` and `claim_slot`, shared with the server's `begin_path`. `carries_data` gates data and the PMTU search
  on a further slot: a server's 4-tuple is ready once validated, a client's once its expanded challenge is answered.
- PATHS_BLOCKED, sent only when the server's limit is the one that blocks, and PATH_CIDS_BLOCKED, whose Next
  Sequence Number comes from the new `QuicCids::peer_next` (sections 3.2.1 and 4.7). Each is sent again when lost
  while what it reports still holds. The receiver counts them in `paths_blocked`.
- Data waits for validation, although section 3.1.2 would allow it earlier. That way section 3.1's "If validation
  fails, the client MUST NOT use the path" never costs data sent into a path that then fails.

## Evidence

- `crd-net-tests "[multipath]"` on linux-gcc-debug (WSL): 46 cases pass. They cover:
  - path ID 1 opened from a second socket over three seeds of loss, duplication and jitter, with a 96 KiB stream
    numbering packets in both slots;
  - path IDs 1 and 2 at limit 2, and PATHS_BLOCKED at a server limit of 1, lost once and sent again;
  - PATH_CIDS_BLOCKED while the server issues no IDs, and the open once they arrive;
  - every refusal, and two path IDs sharing the handshake 4-tuple;
  - a path that fails validation carrying only its challenges and their probe timeouts;
  - the hosted QuicClient-to-QuicServer case through both ingresses.
- Teeth: removing the client's branch of `carries_data` fails the failed-path case, because the stream never finishes.
  Removing the resend of a lost PATHS_BLOCKED fails its case.
- `crd-net-tests "[net],[cids]"`: 894 cases, 887 passed, 7 skipped.
- `ctest -R "crd-check|crd-no-|quic|net-" -E "netns|router"`: 238 of 238 pass.

## Next

- PATH_ABANDON both ways, with three PTOs of retention and the slot and ID space freed. A failed validation abandons
  its path ID, which ends the probe timeouts on a dead path. Today an abandon of any path ID closes the connection.
- MAX_PATH_ID rises as path IDs close, and concurrent validations are paced (section 7.2).
- A validated path that goes dark must not hold data back: the teeth run shows a dead path keeps its share of the
  stream.
- The fuzzers open path IDs, and `QuicHost::build`'s clamp gets a test.
