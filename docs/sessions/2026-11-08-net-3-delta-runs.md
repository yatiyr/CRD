# 2026-11-08 — NET.3: deltas as word runs

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3). Board: [serializers](../bench/2026-11-08-net-3-serializers.md#deltas-as-word-runs-later-the-same-day).
Design: [S28 baseline deltas](../design/network-foundation.md#baseline-deltas).

## CI first

win-tidy in 42413677756 (d7b506c8) refused two unused using declarations in test_admission.cpp; 20a1cfcd drops them and
the include they came with (strict tidy clean, crd-net-tests pass). The array-bounds link failures in that run and in
42416903705 (linux-gcc-release, -shipping) are older than 8d5c9e63.

## What landed

- The generator gives every delta table row its run: the siblings from it on, of one revision, whose encodings always
  take the same bits (fixed-width values, plain structs and fixed-count lists of them; a varint never, since a hostile
  baseline's continuation bit changes its width). `DeltaField` gains `run_bits` and `run`.
- delta.cpp compares, copies and skips runs as 64-bit words. The writer finds a run's first differing bit and codes the
  fields before it as zero bits; the reader counts leading zero change bits and copies those fields in one move. A plain
  value is read once on each side, and `take` is inlined (the profile had it out of line at 21% of the board).
- test-wire-schemas.py: runs end at an optional, a one-group varuint, a varying list and a later revision (30 tests).

## Evidence

- Delta encode 7659 to 3574 and 3631 ns, apply 6142 to 3182 and 3278 ns at one tick (two runs); bytes unchanged, no
  allocation. The vector files did not change (the layout is the same).
- linux-gcc-debug and linux-gcc-asan: `crd-net-tests` (322,137 assertions), the delta fuzz corpora, the drift check.
- Mutations caught: an apply run count off by one (15 cases fail), an extra zero bit in a writer's run (29), a table
  run one bit wide (3). A writer that picks the field before the differing one is equivalent (it codes it as unchanged).
- Strict clang-tidy 20 clean on delta.cpp and delta.hpp.
- libFuzzer, 90 s and 120 s: coded-frames clean (174 k executions). wire-delta-test and delta-stream stopped twice each
  on corrupted pointers in different places (a bad `this` in the generated decoder, a wild jump, a bad writer and table
  pointer); none of the four inputs replays in 30 runs under libFuzzer or under the ASan lane. Same pattern as the last
  batch's stops ([host-misexecutes]); a hosted libFuzzer run of these targets is still the check.

## Next

- Delta: one scan per run for all its differing fields, and a one-group fast path for folded differences.
- The last 4% to 8% of snapshot decode; FlatBuffers and Cap'n Proto on the board.

[host-misexecutes]: 2026-10-11-diag-10a-container-overflow-control.md
