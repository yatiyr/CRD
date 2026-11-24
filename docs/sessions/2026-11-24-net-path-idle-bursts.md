# 2026-11-24: NET-PATH, the board's idle-trim row

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follows [the idle trim](2026-11-24-net-path-idle-trim.md), whose second
next item this is. Everything ran on this PC (WSL2).

**CI first:** nothing to fix: CI 45401974013 and 45399560511 (4f83e628) have no failed job; no run containing
da311710 has finished. Fuzz host-check 45446014204 was dispatched on b2ee3435.

## What landed

- `crd-quic-board --bursts N --gap MS`: a client's one connection opens a new stream per exchange, `gap` after the
  last answer came; a server answers each stream as it comes. Each end reports its `exchanges` and the `allocations`
  its connection took once the second exchange ended: every 20 ms run of 50 bursts showed one server allocation right
  after the first exchange, outside the streams, keys and table bytes (the session ticket's CRYPTO buffer), which made
  a first-exchange baseline count it in some runs and not others.
- `quic_board.py --workload bursts` (crd-net's arms only): CPU per burst as the slope over burst counts at a gap below
  three PTOs (20 ms, kept) and past them (300 ms, trimmed), the latency past the gap and the loop's waits, and the
  retake round by round: the long gap over the short, and that over a `--baseline` build without the trim. It refuses
  a short gap that allocated, or an arm of this tree whose long gap did not retake at both ends; `--check` runs it.

## Evidence

Board: 7 rounds, 10 and 110 bursts of 4 KiB, interleaved with f73663f9's engine built with this board program.

| arm | gap ms | CPU us per burst | ms past the gap | loop waits per burst | allocations per burst (server, client) |
|---|---|---|---|---|---|
| crd-net | 20 / 300 | 264.7 / 1,788.2 | 0.31 / 0.66 | 7.9 / 65.0 | 0, 0 / 1, 1 |
| f73663f9 | 20 / 300 | 296.0 / 1,802.4 | 0.32 / 0.69 | 7.9 / 63.2 | 0, 0 / 0, 0 |
| crd-net, no offload | 20 / 300 | 307.2 / 1,856.9 | 0.35 / 0.75 | 8.1 / 64.5 | 0, 0 / 1, 1 |

- The retake, per burst over the baseline's (median of rounds, least to most): 21.3 us (-264.1 to 93.6) with
  offload, 31.9 us (-199.7 to 187.8) without: inside the rounds' spread, and no latency past the gap (0.66 against
  0.69 ms). A 300 ms gap's own 1.5 ms per burst is the board's waits, capped at 10 ms (57 more per burst at both
  builds), not the trim.
- `quic_board.py --check` with msquic passes; Allman, repository and master-plan validators pass.

## Next

1. CI: the first finished run containing da311710, and fuzz host-check 45446014204 (`net-quic-connection`).
2. A held server end below msquic's best rounds: the 25 KiB connection object.
