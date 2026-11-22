# 2026-11-22 — NET-PATH: path validations paced over every path ID

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Lifecycle bullet in
[network-foundation.md](../design/network-foundation.md). The tree was clean at 5ee4cc3c, level with origin/main.

## CI first

Fuzz host-check 45043107595 (ccc35d64) passed. CI 45044166180 (310a45be) had one red so far: linux-gcc-debug lost its
pinned Ninja to an HTTP 403 from GitHub's release host while the run's other jobs fetched the same file. `pins.py` now
retries a 403 that names a wait or a spent limit (`Retry-After`, `X-RateLimit-Remaining: 0`), any other 403 stays
final, and a final failure prints its URL and both headers; the retry test covers both arms. Whether that 403
carried either header is not known, so the retry may not have covered it; the next one will say. Its win-tidy red
(`kPinned`, `kBases`) was fixed upstream by f3e88cde.

## What landed

RFC 10069 section 7.2: a peer can make the server challenge an address of its choosing on each 4-tuple of every path
ID at once (16 here). Every validation now starts through `validate_path()`:

- A validation of an address validated on no path ID takes one of kQuicMaxPaths turns (`validation_turn`), held until
  its deadline, which it cannot outlive: at most kQuicMaxPaths run at once (every further path ID plus a rebinding),
  however they end, with no hook at any of the places a validation ends. A further one starts when the earliest turn
  ends and runs its own full timeout from then (`QuicPaths::validate` with a later start).
- A validation underway takes no second turn (a repeated probe). An address validated on any path ID
  (`address_proven`) is the peer's, so section 9.3.3's check of the old path and a path ID opened on a validated
  4-tuple (section 5.2) wait for none.
- `quic_validation_deadline` never returns kNeverNs, which marks no validation underway: a saturating timeout (an
  initial RTT above kNeverNs / 3) used to skip validation altogether.

## Evidence

- New RawRun case: path ID 1 opens and is answered at once, its turn still held; 1 ms later path ID 2 opens, path ID
  0 is probed three times from a third address and rebinds to a fourth; the old address and path ID 3 on it are
  challenged at once; a second rebinding's challenge waits for path ID 1's turn exactly (then the only timer), its
  deadline one timeout later, validates, and at most kQuicMaxPaths run at every step.
- New `test_quic_path.cpp` case and static asserts: a saturating timeout validates, its deadline kNeverNs - 1.
- linux-gcc-debug (WSL): `crd-net-tests` 1121 cases, 1114 passed, 7 skipped; the 19 `crd-check|crd-no-` CTests
  pass. linux-gcc-release (LTO, `-Wnull-dereference`): `crd-net-tests` and the connection fuzzer link clean, and
  `[multipath],[quic-path]` (74 cases) pass.
- Teeth, each alone and rebuilt, each fails its case: no turn taken; no gate for a validation underway; validated
  addresses paced; proven only in the path's own slot; `add_saturating` back in `validate`; a deferred start arming
  no timer. Restored and rebuilt: green.
- linux-clang-fuzz `crd-fuzz-net-quic-connection`: the 42-input corpus replays clean, then 306.6 s with 2 jobs,
  91757 executions, corpus 42 to 2190 in a scratch directory, no artifact.
- Pinned clang-tidy 22.1.8 clean on every touched source; clang-format clean on the added lines.

Fuzz host-check 45051033311 was dispatched on 8fff897d.

## Next

NET-PATH: a PTO's resend on another path, and a validated path that goes dark holding no data back (section 5.7).
