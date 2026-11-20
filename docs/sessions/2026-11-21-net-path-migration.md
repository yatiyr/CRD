# 2026-11-21 — QUIC paths: a server follows its client's rebinding and migration

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration).

## CI first

Run 44807121941 (730a2e86) was still running with no failed job, and the next push run (a8e4b6e8) was pending. The
scheduled failures on 6f4cfed5 were handled last batch. Nothing to fix.

## What landed

- `QuicPath` in the sans-I/O interface (`receive(path, ...)`, `sent_path()`), and `QuicPaths`: four paths, admission
  only after a packet opened, per-path three-times credit, PATH_CHALLENGE validation with doubling resends, the
  section 8.2.4 timeout and expanded challenges, responses on their own path.
- A confirmed server follows the highest-numbered non-probing packet, with an unused peer ID when the client changed
  its own, challenges the old path (9.3.3), reverts or closes silently on failure (9.3.2). Unless only the port
  changed, RTT, controller, pacer, ECN validation and the PMTU search restart; recovery's path floor and
  `off_path` keep old-path and probing packets out of the estimate.
- The host follows the connection's path; the server drops disable_active_migration and gives a token per validated
  address. Defects found and fixed on the way: an owed ACK under an exhausted credit spun the timer; active-path
  path frames were held by the window; an off-path packet acknowledged as the largest took an RTT sample.

## Evidence

- win-debug (MSVC 14.51): 963 `crd-net-tests` cases; WSL linux-gcc-debug: 964 cases; win-tidy (clang-tidy 20) clean
  on the 14 changed sources after two fixes (a local constant's name, a nested conditional).
- New: `test_quic_path.cpp` (7 cases), 8 raw-peer path cases, the restart cases in `test_quic_recovery.cpp` and
  `test_quic_ecn.cpp`, transfers both ways through a rebinding then an address change at 2% loss, 1% duplication and
  10 ms jitter (3 seeds), and a hosted client rebinding and moving with a token for each address.
- linux-clang-fuzz: `crd-fuzz-net-quic-connection` bit 7 (paths). Its first runs found four oracle faults in the
  fuzzer's own bookkeeping (kept in the corpus); then 421 s, 56304 runs, 2088 new units, nothing found.
- linux-gcc-public-checks: the ngtcp2, nghttp3, picoquic, quiche, Quinn, webtransport-go and QMux interop suites
  all pass (7 of 7) with the server no longer sending disable_active_migration.

## Next

Client-initiated migration (a second socket, probing, the switch, disable_active_migration respected),
preferred_address, interop with ngtcp2's migrating client, then LEO handover.
