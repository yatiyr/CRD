# 2026-11-24: NET-PATH, the board's idle-trim row

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows [the idle trim](2026-11-24-net-path-idle-trim.md), whose second
next item this is. Everything ran on this PC (WSL2).

**CI first:** nothing to fix: CI 45401974013 and 45399560511 (4f83e628) have no failed job; no run containing
da311710 has finished. Fuzz host-check 45446014204 was dispatched on b2ee3435.

## What landed

- `crd-quic-board --bursts N --gap MS`: a client's one connection opens a new stream per exchange, `gap` after the
  last answer came; a server answers each stream as it comes. Each end reports its `exchanges`, the client the bursts
  it began `trimmed` (its slot's buffers given back), and each end the `allocations` its connection took once the
  second exchange ended: every 20 ms run of 50 bursts showed one server allocation right after the first exchange,
  outside the streams, keys and table bytes (the session ticket's CRYPTO buffer).
- `quic_board.py --workload bursts` (crd-net's arms only): CPU per burst as the slope over burst counts at a gap below
  three PTOs (20 ms, kept) and past them (300 ms, trimmed), the latency past the gap and the loop's waits, and the
  retake round by round: the long gap over the short, and that over a `--baseline` build without the trim. It refuses
  a short gap with a trimmed burst or a whole allocation per burst, and an arm of this tree whose long gap left a burst
  untrimmed or an end with no retake; `--check` runs it (108 runs pinned to two cores, 48 of them four at once, pass).
- At 64 KiB a kept server buffer may still reach its high-water mark late (one doubling at exchange 4 to 26 in 4 of 12
  runs), so the short gap allows growth below one allocation per burst instead of none.

## Evidence

Boards interleaved with f73663f9's engine built with this board program (it trims grown tables, not stream buffers).
CPU per burst both ends, 20 / 300 ms gaps; the retake is the per-round difference of differences (median, range).

| bursts | arm | CPU us per burst | ms past the gap | allocations per burst at 300 ms (server, client) | retake over f73663f9 |
|---|---|---|---|---|---|
| 4 KiB, 7 rounds, 10 and 110 | crd-net | 307.3 / 1,914.7 | 0.35 / 0.70 | 1.00, 1.00 | 8.0 us (-194.2 to 197.5) |
| | no offload | 318.8 / 1,842.9 | 0.31 / 0.66 | 1.00, 1.00 | -162.6 us (-219.1 to 556.3) |
| | f73663f9 | 313.1 / 1,957.0 | 0.36 / 0.72 | 0.00, 0.00 | |
| 64 KiB, 5 rounds, 10 and 70 | crd-net | 404.9 / 2,040.5 | 0.37 / 0.81 | 4.90, 4.00 | 49.6 us (-155.8 to 234.1) |
| | no offload | 532.4 / 2,040.4 | 0.47 / 0.91 | 4.95, 4.03 | -9.3 us (-214.4 to -1.2) |
| | f73663f9 | 450.5 / 2,016.8 | 0.40 / 0.86 | 0.00, 2.97 | |

- The retake (one 4 KiB buffer per end, or a 64 KiB buffer's doublings and the regrown sent tables) sits inside the
  rounds' spread, about 200 us per burst on this host, and adds no latency past the gap. A 300 ms gap's own 1.5 ms per
  burst is the board loop's waits, capped at 10 ms (about 56 more per burst at both builds), not the trim.
- `quic_board.py --check` with msquic passes; Allman, repository, generated and master-plan validators pass.
  clang-tidy was not shown to run on the board program here: CI's tidy job is the check.

## Next

1. CI: the first finished run containing da311710 and 175933e2 (tidy included), and fuzz host-check 45446014204.
2. A held server end below msquic's best rounds: the 25 KiB connection object.
