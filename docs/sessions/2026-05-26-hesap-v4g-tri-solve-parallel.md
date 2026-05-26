# 2026-05-26 — hesap v4g-tri-solve-parallel: level-scheduled triangular solve + large-nonsym ILUT crush

> **Correction 2026-05-26 (v4i-1 close):** the cd2d-200 ILUT crush below used the Krylov
> recurrence residual vs Eigen's true residual ([[feedback_iterative_bench_matched_true_residual]]).
> At MATCHED true residual the honest figure is **1.74×** (Cerid 4 it / 11.11 ms vs Eigen 11 it /
> 19.34 ms), not 2.72×. The large-nonsym ILUT win + the parallel tri-solve bit-exactness hold.

Phase 3.1.6 `crd-hesap` v4, slice **v4g-tri-solve-parallel**. Shared parallel triangular-
solve infra + the large-nonsym ILUT crush demonstration.

## What shipped

- **`crd/hesap/sparse/triangular_solve.hpp`** — `TriSchedule` (topological dependency-level
  schedule: level[i] = 1 + max(level of the rows row i references); rows in a level are
  mutually independent; O(nnz) build) + `tri_solve_lower_levelsched` / `tri_solve_upper_levelsched`.
  Operates on OFF-DIAGONAL CSR + an `inv_diag` array (nullptr ⇒ unit). Each level's rows
  solve via `parallel_for`; **bit-exact** vs the sequential solve (each output element
  computed by exactly one worker in fixed CSR order, deterministic level partition — the v4
  determinism moat).
- **Size-adaptive (D-pin):** parallel only when `max_level_width ≥ 256 AND n ≥ 8192`. Below
  that, the per-level dispatch + barrier (summed over many narrow levels) exceeds the
  sequential solve — measured: sherman3 (n=5005, 689 levels of avg width ~7) ran 2× SLOWER
  when forced parallel, so it correctly stays serial.
- **Integrated into ILUT**: L/U level schedules built once at factor time; the apply runs the
  level-scheduled solves. +1 test: level-sched ≡ sequential bit-exact at n=90000 (2D-grid
  factor, max_width 300, parallel path engaged).

## Honest result

- **Bench addendum `cd2d-200` (2D conv-diff, n=40000): Cerid ILUT CRUSHES Eigen IncompleteLUT
  2.72× (3 it / 7.24 ms vs 11 it / 19.70 ms)** — the large-nonsym crush. gemat11 1.19×.
- **The lever on cd2d-200 is ILUT preconditioner quality (3 vs 11 iters) + the parallel SELL
  spmv, NOT the parallel tri-solve**: the ILUT *fill* makes the factor chain-y (max_width
  12-26), so the tri-solve correctly stays serial there. The parallel tri-solve is correct
  shared infra that engages on WIDE-wavefront factors (verified at n=90000, max_width 300),
  the regime ILU factors rarely hit because fill lengthens dependency chains.
- This matches the advisor's pre-statement: level-scheduling's parallelism is a property of
  the matrix; ILU factors are often chain-y; the slice closes with the honest parallelism
  characterization (the infra is correct, the bench shows the large-nonsym ILUT crush).

## Notes

- IC(0)/ILU(0)/SSOR can adopt the level-sched solver (nice-to-have, deferred — IC(0) is
  spmv-bound with a sparse factor, so the tri-solve isn't its bottleneck; it already crushes).
- Saved to memory: the level-scheduled-tri-solve size-adaptive + chain-y-ILU-factor lesson
  (folded into `feedback_incomplete_factorization_robustness`).

## Next

v4h — ILU(p) level-of-fill incomplete LU (the leveled-dropping machinery; ILUT shipped in v4g).
