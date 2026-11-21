# 2026-11-21: NET-PATH, learning a LEO link's handover phase

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S76](../design/network-foundation.md#handover).

## CI first

No finished run contains c20b43c6..70796a17 yet (44880441302 pending). In run 44861565940 (15bea98a)
win-shipping-profile (MSVC 14.52.37025) fails six hesap-tensor tests, not net: the f32 batched check at avx512 and
avx512+vnni (44 mismatches, f64 passes), so the self-check refuses avx512 ("selected avx512, running avx2"),
consistent with the MSVC 14.52 failure batch 11 saw on the `-sde-skx` arms. win-shipping's log ends at test 3914 of
11086 (cause unread). No track owns the tensor failure; it keeps both shipping lanes red.

## What landed

- `crd/hesap/net/handover_phase.hpp`: `HandoverPhaseLearner`, epoch folding of a connection's losses and RTT rises at a
  known period, an exact binomial lock test, a Theil-Sen phase that follows a skewed clock (Cerid's own design; the
  published LEO controllers read UTC or detect each reconfiguration live).
- `crd-net`: `HandoverLearning` and `starlink_handover_learning()`; `QuicConnectionConfig::handover_learning`,
  `set_handover_learning()`, `handover_learned()`, `handover_locks`. The learner is fed ahead of the coverage gates,
  lives in a slot (no public header includes HESAP-NET's), and restarts on a move that restarts the path's state.

## Evidence

- WSL linux-gcc-debug: crd-hesap-net-tests 192 cases (`[handover]` 8); crd-net-tests `[handover]` 12 cases; the
  guard, net and hesap fuzz-corpus CTests (37) pass; the whole crd-net-tests (1010 cases) passes; both fuzz
  targets' seed sets replay clean, the timetable seed reaching a lock.
- libFuzzer (linux-clang-fuzz, 241 s each): `fuzz_estimators` 2,135,907 runs and `fuzz_quic_connection` 30,953
  runs, both clean.
- clang-tidy 22.1.8 (`tidy-files.py`) on the nine changed files: clean after renaming four local constants and one
  widening cast.
- Teeth: no binomial test locked on random losses (unit) and on a congested CUBIC path (connection); a plain median
  lagged a 1000 ppm skew by 121 ms (limit 20); feeding the learner behind the coverage gate lost a loss-only lock.
- Measured (2 s period, 10 Mbit/s, BBRv3): lock at 19 s, phase 2 ms off; 13 reductions after 9 handovers against the
  authored schedule's 10 and none's 28; on a clock 1000 ppm slow, 16.

## Next

The GSO/GRO, io_uring, USO/URO and RIO fast paths with their portable fallbacks.
