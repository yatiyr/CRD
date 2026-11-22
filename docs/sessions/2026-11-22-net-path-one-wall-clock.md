# 2026-11-22 — NET-PATH follow-up: one wall clock for TLS and its provider

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows the [abandon note](2026-11-22-net-path-abandon.md), whose
connection fuzzer run left one event no input replayed. Design: "One wall clock" in
[network-foundation.md](../design/network-foundation.md) (S17's early data bullets).

## CI first

CI 45013880738 (e446680a): the three GCC LTO links (`-Wnull-dereference`, quic_connection.cpp:3122) and the two ASan
`[cids]` failures are the ones 905fa518 and the batch-11 test fix already cleared on HEAD. Fuzz host-checks
45021997098 and 45017650858 are still queued.

## The event

`fuzz_quic_connection.cpp:1072`, `offered == (early && !switching)`: a client holding a fresh ticket offered no early
data. The artifact was written at 15:34:48.320; WSL's journal has `systemd-resolved: Clock change detected` at
15:34:48 (80 such steps, about every 31 s, in that boot). The provider checks a ticket against its own clock
(`ssl_session_is_time_valid`), the system's, and drops one dated after "now" as from the future; a host clock that
stepped back between the ticket and its offer did that. The replay filter the old note suspected is the server's and
cannot stop a client offering.

## What landed

- `crd::time::WallClock::pin` and `unpin`: every read on every thread returns the pinned Unix-epoch instant until
  the next pin or unpin (one atomic; the system clock otherwise).
- TLS reads one wall clock: `unix_now` reads `WallClock`, and every context sets the provider's current time callback
  (`provider_time`) to the same clock, so Cerid's ticket, replay and policy checks and the provider's ticket
  lifetimes and ages always agree, pinned or not (Windows: a layout-matched `timeval`, no Winsock header).
- `tests/foundation/crypto/pinned_wall_clock.hpp` (`PinnedWallClock`, `kPinnedUnixSeconds`): the TLS, QMux early
  and QUIC connection fuzz targets pin it before their first session and seed their replay filters from it.
- The lifetime case no longer sleeps 2.1 s: on a pinned clock a one-second ticket is offered in its second, refused
  two seconds on, and refused one second before it came (a client clock that stepped back).

## Evidence

- linux-gcc-debug (WSL): `crd-time-tests [wall]` (7 assertions), `crd-crypto-tests [tls]` (74 cases), the three
  corpora (`crd-fuzz-crypto-tls`, `-net-qmux-early`, `-net-quic-connection`) replay clean.
- linux-gcc-debug: `crd-crypto-tests` whole (1450602 assertions), `crd-net-tests` 1107 cases (1100 passed, 7
  skipped). win-debug: `crd-crypto-tests [tls]` and `crd-time-tests [wall]` pass (the Windows `timeval`).
- Teeth: without the provider's callback the pinned lifetime case fails at +2 s and at -1 s (offered and resumed).
- linux-clang-fuzz: the artifact replays clean; `crd-fuzz-net-quic-connection-libfuzzer` seed 8404, 300 s, 31143
  runs from both corpora, clean while WSL's journal logged a clock step about every 31 s; `crd-fuzz-crypto-tls`
  seed 8405, 150 s (171558 runs) and `crd-fuzz-net-qmux-early` seed 8406, 100 s (7717 runs), clean.

## Next

- NET-PATH: a path ID 0 abandoned while others live; MAX_PATH_ID raised as path IDs close; concurrent validations
  paced (RFC 10069 section 7.2).
