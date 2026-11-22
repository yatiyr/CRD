# 2026-11-22 — NET-PATH: the amplification guard proven in a reclaimed slot 0

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Lifecycle bullet in
[network-foundation.md](../design/network-foundation.md). The tree was clean at 310a45be, level with origin/main.

## CI first

Nothing to fix. Fuzz host-check 45021997098 (94a6d70f) passed; 45043107595 (ccc35d64) is still queued and CI
45044166180 (310a45be) pending. The cancelled CI runs 45039796257, 45041469075 and 45043076666 have no failed job.

## What landed

The previous batch left the two amplification-guard sites of `send_on_slot()` unproven: RFC 9002 section 6.2.2.1's
deadlock guard must stay the handshake's (`handshake_slot()`), not slot 0's, once path ID 0 closes. A new RawRun case
proves it:

- The raw client validates path ID 1, abandons path ID 0 and lets it close; the server's PING is left unacknowledged
  on path ID 1.
- Path ID 2 then opens in the reclaimed slot 0 from a third address with one unpadded datagram, so the server's
  credit there drops under one useful packet (64 bytes) at once. The client stays silent everywhere.
- On every step `loss_timer_ns()` stays armed, path ID 1's probe timeout fires at least twice after the credit is
  spent, path ID 2's validation fails and is abandoned, and the connection stays established.

The harness gains `path_pad_to` (send_on_path's padding), `announce_path()` for any path ID, and
`validate_path_one()`, which replaces two copies of the challenge answer.

## Evidence

- linux-gcc-debug (WSL): `crd-net-tests` ran 1119 cases: 1112 passed, 7 skipped.
- Teeth, each alone, rebuilt: `slot == 0U` for `handshake_slot(slot)` at the guard before a held send, and at the
  guard after a sent datagram. Both leave `loss_timer_ns()` at never and fail the case. Restored and rebuilt: green.
- No production code changed; pinned clang-tidy 22.1.8 and clang-format are clean on the file, and
  check-allman-braces passes.

## Next

NET-PATH: pace concurrent validations (RFC 10069 section 7.2), then schedule a PTO's resend on another path.
