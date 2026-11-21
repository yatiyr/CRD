# 2026-11-21: NET-PATH, the server's preferred address

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S75](../design/network-foundation.md#migration)
(RFC 9000 sections 9.6, 5.1.1 and 18.2).

## CI first

Run 44854356220 (d17003bd), still running: of its finished failures I read win-arm64-debug, win-asan 2/2,
win-debug, win-clang-cl-shipping, linux-gcc-debug, linux-gcc-public-checks, linux-clang-arm64-debug and
android-x86_64-debug. Each fails only `crd-repository-tools` and `crd-generated-sources` on the licence manifest's
recorded bytes, which b0882ee0 already records. win-arm64-debug built, so the C4324 fix (9157ccdb) holds. No run on
e013ea70 or later has finished.

## What landed (f84021db and the follow-up)

- Server: `preferred_socket` with `has_preferred_address`; sequence 1's ID and token in the parameter; answers and
  challenges there at once; moves only once it validated the path and the client's highest-numbered non-probing
  packet arrived there; then forgets and drops its other sockets. Datagrams on any socket but the active one (and,
  until it moved, the preferred one) are dropped.
- Client: the ID held as a NEW_CONNECTION_ID frame's; `follow_preferred_address` (default on) validates the
  address of the active path's family, IPv4-mapped where the path is, and moves on validation; `migrate()` to it
  waits for the validation; a socket change first re-targets it from the new socket; disable_active_migration covers
  the handshake address alone; the first address is refused once moved.
- Defect fixed: a server answered a probe with its own challenge only when it already held a client ID for the
  path, so a path waiting for one was given up for another and the client's move from a new socket stalled.
- Defect fixed (fuzz-found in the new code): a pending move from a socket the client then left drew it back there.
- Defect fixed (review): a client whose path names no server address (port 0) followed the preferred address in
  state alone, retiring its ID and restarting congestion control with nothing moved.

## Evidence

- win-debug (MSVC 14.51): all 997 `crd-net-tests` cases, 7 new `[preferred-address]` and a real-UDP host test, and
  24 `crd-check-*`/`crd-no-*`/repository CTest guards; WSL linux-gcc-debug (GCC 13, -Werror): 802 `[net]` cases;
  the connection corpus (35 inputs) replayed on win-debug and linux-clang-fuzz.
- Teeth: moving before validation fails the lost-response test; no socket gating fails both dropped-datagram
  checks; the probe challenge held for an ID fails the move-before-preferred test's 500 ms bound; no port check
  fails the unbound-client case.
- linux-clang-fuzz `crd-fuzz-net-quic-connection`, op 0xF7 and a preferred address over paths: four inputs kept
  (the defect above, three oracles corrected: a rebinding followed right after the move, an ECN restart with no
  packet held); then 481 s, 60117 runs clean (scratch corpus).
- clang-tidy 22.1.8 clean on the 8 changed sources; clang-format on added lines; Allman.

## Next

Interop with ngtcp2's migrating client and server (its preferred address too) through `cerid_quic_peer`; LEO
handover awareness.
