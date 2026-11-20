# 2026-11-21 — QUIC paths: a server follows its client's rebinding and migration

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration).

## CI first

Run 44807121941 (730a2e86), on runner image 20261111.635 with MSVC 14.52.37025, failed three jobs:
- win-tidy: the image's STL needs Clang 22 (STL1000), against the pinned LLVM 20; hosted tidy confirms nothing now,
  and this batch's tidy evidence is local (below). It also flags `cpu_features.cpp`'s redundant declarations:
  14.52's `<isa_availability.h>` declares two of the CRT externs, 14.51's does not; they stay, under a NOLINT.
- win-release and win-shipping: `test_kernel_dispatch.cpp:497`, f16-to-f32 of the top normals and NaNs, 578 wrong
  at every level. The last pass (44723313119, 6f4cfed5) ran MSVC 14.51.36231. The only math commit between is this
  track's 6951b10a (division guards in `mat_types.hpp`, other code). Told apart since: 578 = 34 x 17, every guard
  word of the short calls and no converted value, at n = 0 too; the test's signalling-NaN guard, stored as an f32
  value, is quieted by 14.52's optimiser. The test now writes it as bytes and counts values and guards apart.

## What landed

- `QuicPath` in the sans-I/O interface (`receive(path, ...)`, `sent_path()`), and `QuicPaths`: four paths, admission
  only after a packet opened, per-path three-times credit, PATH_CHALLENGE validation with doubling resends, the
  section 8.2.4 timeout and expanded challenges, responses on their own path.
- A confirmed server follows the highest-numbered non-probing packet, with an unused peer ID when the client changed
  its own, challenges the old path (9.3.3), abandons a spurious one, reverts or closes silently on failure (9.3.2).
  A new path keeps to 1200 bytes and its credit; on confirming it (9.4, checked against rfc9000.txt), unless only
  the port changed, RTT, controller, pacer, ECN and the PMTU search restart, so a forwarded copy costs nothing;
  recovery's path floor and `off_path` keep old-path and probing packets out of the estimate.
- The host follows the connection's path; the server drops disable_active_migration and gives a token per validated
  address. Defects found and fixed on the way: an owed ACK under an exhausted credit spun the timer; active-path
  path frames were held by the window; an off-path packet acknowledged as the largest took an RTT sample.

## Evidence

- win-debug (MSVC 14.51): 964 `crd-net-tests` cases; WSL linux-gcc-debug: 965; local win-tidy (clang-tidy 20)
  clean on the 14 changed sources after two fixes (a local constant's name, a nested conditional).
- New: `test_quic_path.cpp` (7 cases), 8 raw-peer path cases, the restart cases in `test_quic_recovery.cpp` and
  `test_quic_ecn.cpp`, transfers both ways through a rebinding then an address change at 2% loss, 1% duplication and
  10 ms jitter (3 seeds), and a hosted client rebinding and moving with a token for each address.
- linux-clang-fuzz: `crd-fuzz-net-quic-connection` bit 7 (paths). Its first runs found four oracle faults in the
  fuzzer's own bookkeeping, a fifth after the reset moved (kept); then 451 s, 55368 runs, nothing found.
- linux-gcc-public-checks: the ngtcp2, nghttp3, picoquic, quiche, Quinn, webtransport-go and QMux interop suites
  all pass (7 of 7) without disable_active_migration (run before the reset moved to confirmation).

## Next

Tell the f16 failure's toolset from 6951b10a; client-initiated migration (a second socket, probing, the switch,
disable_active_migration respected), preferred_address, interop with ngtcp2's migrating client, then LEO handover.
