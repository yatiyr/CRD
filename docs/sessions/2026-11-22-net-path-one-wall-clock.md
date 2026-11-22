# 2026-11-22 — NET-PATH follow-up: one wall clock for TLS and its provider

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows the [abandon note](2026-11-22-net-path-abandon.md)'s
unreplayable connection fuzzer event. Design: "One wall clock" in [network-foundation.md](../design/network-foundation.md).

## CI first

CI 45013880738 (e446680a): its GCC LTO `-Wnull-dereference` links and ASan `[cids]` failures are the ones 905fa518 and
batch 11's test fix cleared. Fuzz host-checks 45021997098 and 45017650858 are still queued.

## The event

`fuzz_quic_connection.cpp:1072` (`offered == (early && !switching)`): a client holding a fresh ticket offered no early
data. The artifact (15:34:48.320) coincided to the second with a WSL journal `Clock change detected` (80 in that boot,
about every 31 s). The provider dates tickets on the system clock and offers none dated after "now"; a step back
between the ticket and its offer does that (inferred: the journal gives no direction; the -1 s teeth case shows the
mechanism). The server-side replay filter the abandon note suspected cannot stop a client offering.

## What landed

- `crd::time::WallClock::pin` / `unpin`: every read on every thread returns the pinned instant until changed.
- TLS reads one clock: `unix_now` and every context's provider time callback (`provider_time`) read `WallClock`. The
  callback counts from 2020-01-01: the provider only compares its own readings, and its `timeval` seconds are a 32-bit
  `long` on Windows (a layout-matched struct there, no Winsock header), which would otherwise saturate in 2038.
- `tests/foundation/crypto/pinned_wall_clock.hpp`: the TLS, QMux early and QUIC connection fuzz targets pin it before
  their first session and seed their replay filters from it.
- The lifetime case no longer sleeps 2.1 s: at a pinned 2026 second and at one past 2038, a one-second ticket is offered
  in its second and refused two seconds on and one second before it came.

## Evidence

- linux-gcc-debug: `crd-crypto-tests` whole (1450714 assertions), `crd-net-tests` 1107 cases (1100 passed, 7 skipped),
  `crd-time-tests [wall]`, the `crd-check*`/`crd-no-*` guards and the three fuzz corpora. win-debug: `[tls]` (74 cases)
  and `[wall]`.
- Teeth: no provider callback fails the case at +2 s and -1 s (Linux); no 2020 epoch fails the 2039 base (win-debug).
- linux-clang-fuzz: the artifact replays clean; quic-connection seed 8404, 300 s, 31143 runs, clean across WSL's clock
  steps; crypto-tls seed 8405, 150 s (171558 runs) and qmux-early seed 8406, 100 s (7717 runs), clean.

## Next

- Unit cases that resume on the unpinned clock (`test_tls_early`, `quic_connection_fixture.hpp`) can still fail once
  under a host clock step: recognise a single local red there.
- NET-PATH: path ID 0 abandoned while others live; MAX_PATH_ID raised as path IDs close; validations paced.
