# 2026-11-22 — NET-PATH: the handshake slot reclaimed once path ID 0 closes

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Lifecycle bullet in
[network-foundation.md](../design/network-foundation.md). The tree was clean at 8b5c59ec, level with origin/main.

## CI first

Nothing to fix. Fuzz host-check 45017650858 (9f652553) passed. Host-check 45021997098 and CI 45017637925 have no
failed job so far. Every later CI run was cancelled by a newer push, and CI 45039796257 (8b5c59ec) is pending.

## What landed

- Once path ID 0 closes, slot 0 is free and holds a later path ID. It uses the connection's own controller, the
  recovery's own state (`QuicRecovery::first_path()`) and the first Application storage. Every close now raises the
  limit, so 1 + initial_max_path_id path IDs stay open; at initial_max_path_id 3 one was lost before.
- `handshake_slot()` keys path ID 0's roles to path ID 0, not to slot 0. These are the handshake spaces, the
  amplification guard, `addressed()`, `carries_data()`, the preferred address, deferred moves, RFC 9000's fallback and
  the challenge PING. `migrate()` and `probe_path()` still answer `Abandoned`. The ACK form follows the path ID.
- `server_address()` keeps the server's address for `add_path()`. The lead is the lowest sending slot that carries
  data, so a path still validating never takes `path()` or the close; that was wrong in a freed slot 1 too.
- ACK receive timestamps are read only for path ID 0's open slot.

## Evidence

- linux-gcc-debug (WSL): `crd-net-tests` ran 1118 cases: 1111 passed, 7 skipped. Two connection cases are new.
  - Path IDs 1 to 3 hold slots 1 to 3, and either end abandons path ID 0 over a hostile link (four seeds). Path ID 4
    then opens in slot 0 on both ends and does not lead while it is validating. It alone carries 64 KiB once 1 to 3
    leave. Path ID 5 then opens in slot 1, and the close goes on path ID 4.
  - A path ID in a reclaimed slot 0 that never validates is abandoned. The server's stream, and a tail only its PTO
    recovers, still complete on path ID 1.
- A new `[quic-packet]` section seals in reclaimed slot 0 under the new path ID's nonce, numbered from 0. Path ID 0's
  packet and the same packet in slot 1 are both forged. The packet fuzzer now also reclaims slot 0.
- Teeth that bite: the ACK form keyed to the slot, the old lead, the `spare` rule, the server's address read from slot
  0, and `carries_data()` keyed to slot 0. The two amplification-guard sites survive: these runs never left a
  reclaimed slot's send below a packet's credit. The second case does not prove the guard.
- linux-gcc-release (LTO): `[multipath],[cids]` 67 cases pass. Pinned clang-tidy 22.1.8 is clean on the changed files.
- linux-clang-fuzz: the corpora replay clean. Clean runs: quic-cids seed 9001 for 120 s (302589 runs),
  quic-connection seed 9002 for 300 s (31354), quic-packet seed 9003 for 180 s (649375).

## Next

NET-PATH: prove the guard with a raw client that goes quiet on the new path ID, checking `loss_timer_ns()`. Then pace
concurrent validations (section 7.2), and schedule a PTO's resend on another path.
