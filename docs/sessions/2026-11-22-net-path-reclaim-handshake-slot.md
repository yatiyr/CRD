# 2026-11-22 — NET-PATH: the handshake slot reclaimed once path ID 0 closes

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Lifecycle bullet in
[network-foundation.md](../design/network-foundation.md). The tree was clean at 8b5c59ec, level with origin/main.

## CI first

Nothing to fix. Fuzz host-check 45017650858 (9f652553) passed. Host-check 45021997098 and CI 45017637925 have no
failed job so far. Every later CI run was cancelled by a newer push, and CI 45039796257 (8b5c59ec) is pending.

## What landed

- Once path ID 0 closes, slot 0 is free (`free_slot()` starts at 0) and holds a later path ID. It reuses the
  connection's own controller, the recovery's own state (`QuicRecovery::first_path()`) and the Application space's
  first storage. Every close now raises the limit, so 1 + initial_max_path_id path IDs stay open at every limit. Before
  this, a close at initial_max_path_id 3 left one path ID short.
- `handshake_slot()` keys path ID 0's roles to path ID 0 rather than to slot 0: the Initial and Handshake spaces, the
  amplification guard, `addressed()` and `carries_data()`, the preferred address, deferred moves, RFC 9000's fallback,
  and the PING owed for a challenge. `zero_sending()` replaces `m_controls[0]` as the "path ID 0 is gone" check, so
  `migrate()` and `probe_path()` still answer `Abandoned`. An acknowledgement takes the ACK form by path ID, so a later
  path ID in slot 0 is acknowledged in PATH_ACK frames.
- `server_address()` keeps the server's address that path ID 0 sent to after slot 0's 4-tuples are cleared. Both
  `add_path()` and `toward_preferred()` read it.
- The leading slot is the lowest sending slot whose active 4-tuple carries data. A path ID still validating in a lower
  slot no longer takes `path()` or the close. This was also wrong before reclaim, for a new path in a freed slot 1.
- ACK receive timestamps are read only after the frame's slot is known, so they are path ID 0's packets only. They are
  no longer counted for a stopped or closed slot 0.

## Evidence

- linux-gcc-debug (WSL): `crd-net-tests` ran 1118 cases: 1111 passed, 7 skipped. Two cases are new.
  - The first runs four seeds over a hostile link (loss, duplication, jitter). Path IDs 1 to 3 hold slots 1 to 3, and
    either end abandons path ID 0. Both limits then rise to 4, and path ID 4 opens in slot 0 on both ends. It does not
    lead while it is validating. Path IDs 1 to 3 then leave, and path ID 4 alone carries 64 KiB, numbered from 0 and
    acknowledged. The limits reach 7, path ID 5 opens in slot 1, and the close goes on path ID 4.
  - The second opens a path ID in a reclaimed slot 0 that never validates, because nothing reaches that client
    socket. The server's 96 KiB stream and a tail lost for 300 ms still complete, and the client abandons the path.
- Teeth that bite: a slot-keyed ACK form (the data is never acknowledged), the old lead (a validating path leads), the
  `spare` rule (the limits stay at 3), and the server's address taken from slot 0's 4-tuples (`add_path()` is refused).
  `carries_data()` keyed to slot 0 also bites. The two amplification-guard sites survive as teeth: in these runs the
  reclaimed slot never set the server's guard.
- linux-gcc-release (LTO) links clean, and `[multipath],[cids]` passes: 67 cases. Pinned clang-tidy 22.1.8 is clean on
  the four changed files.
- linux-clang-fuzz: both corpora replay clean. quic-cids ran 120 s on seed 9001 (302589 runs) and quic-connection ran
  300 s on seed 9002 (31354 runs), with no crash.

## Next

NET-PATH: pace concurrent validations (RFC 10069 section 7.2), then scheduling. A PTO resends on another path, and a
validated path that goes dark holds no data back. Then the fuzzers open path IDs.
