# 2026-11-22: NET-PATH, the path choice and PATH_STATUS

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Scheduling bullet in
[network-foundation.md](../design/network-foundation.md#multipath).

## What landed

- `QuicPathChoice` (config `path_choice`, `set_path_choice()` live): `LowestRtt` (default) and `Turns`. Under
  `LowestRtt` a slot yields its data to the lowest-RTT slot that would send it in the same round (`yields_data()`:
  usable, not backup-held, window and ledger room, pacer released, socket not held, credit), so no slot yields to
  one that sends nothing; a yielding slot reports the application's limit. Ranking on window room alone kept a
  paced bulk transfer on one path, so the pacer is a term.
- RFC 10069 section 3.3: a peer's backup path carries no data while another path it did not mark is usable; with
  every usable path backup, or the others dark, it carries data as any (`backup_held()`).
- `set_path_status()` / `path_status()` (section 4.3): numbered per path ID from 0, nothing for the status in force,
  resent only while newest, never for an abandoned path ID, both ends' entries forgotten at close.
  `QuicPeerPathStatus` is now `QuicPathStatus`.
- Fixture: `Net::socket_delay_ns`, `Net::held` (served again 1 ms on, as QuicHost's send retry), and `run_at()`'s
  stall guard now also fires while later flights are pending (a timer stuck due spun forever before).
- Fuzzer op 0xF4 (choice and status against a model) and an end oracle: each end holds the other's newest status
  for every path ID; seed `script-path-status`.

## Evidence

- Five `[multipath]` cases in `test_quic_connection.cpp` over `Net`, with a send-time watch (no data datagram left
  a slot while one ranked before it, live and not backup, had window, ledger and pacer room): a trickle on the
  faster path alone, a bulk transfer spilling onto the slower one, the RTTs swapped and followed, `Turns` live; a
  held socket's data on the next path and back; a backup path idle under both choices and a full window, taking
  over while the other is dark, every path backup, and back; a raw client reading every PATH_STATUS frame
  (refusals, numbering, repeats, a resend while newest, none after a newer one, forgotten at close).
- Ten teeth, each alone and rebuilt, each fails its cases: no rank yield, no pacer term, no backup hold, backup
  held while the other is dark, no held-socket term, sequence not raised, resent when not newest, never resent,
  kept at close, the status in force sent again.
- linux-gcc-debug (WSL): `crd-net-tests` 1129 cases, 1122 passed, 7 skipped; the 19 `crd-check|crd-no-` CTests pass.
- linux-gcc-asan `[multipath]` (74 cases) and linux-gcc-release (LTO) `[multipath]` pass; the LTO link first failed
  `-Wnull-dereference` on `set_path_status`'s entry, now guarded.
- `crd-fuzz-net-quic-connection`: the 43-input corpus replays clean (debug), then linux-clang-fuzz 241 s with two
  jobs, 47681 executions, corpus 43 to 2069 in a scratch directory, no artifact.
- Pinned clang-tidy 22.1.8 clean on the three touched sources; clang-format 20 applied on added lines only.

CI first: nothing new to fix. CI 45044166180's two reds (win-tidy, the Ninja 403) were already fixed at HEAD;
fuzz host-checks 45057262362 and 45051033311 are still queued.

## Next

NET-PATH's scheduling: redundant sends for latency-critical datagrams, flows pinned to a path, an idle path's RTT
kept fresh, PATH_ACK placement; then the fuzzers opening path IDs.
