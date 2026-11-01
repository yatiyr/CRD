# 2026-11-01 — HW-RESILIENCE.3: known-answer self-check of hesap-dense

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the sparse self-check](2026-11-01-hw-resilience-3-sparse-self-check.md).

## CI read

- Completed runs: push runs up to 41284892084 were cancelled by concurrency and have no failed jobs.
- Running complete-tier run 41269105413 (`b0b1b245`): every failed job is `filesystem.cpp`'s `min`/`max` (fixed by
  9723ac75) or ARM64's C4324 in `checksum.cpp` (fixed by 921e19cc). 41274380605 (`de5e10b0`) is still queued and
  carries HW-RESILIENCE.2's ARM64 clause and the DIAG.11c/DIAG.12 evidence; none was dispatched.
- Account-backed lanes: only the FTL names exist; no AMD or Mac names. DIAG.11b clause (8): no recording newer than
  20261101T004352Z in `build/diag-overhead-wpr/` of the checkout or of the `cerid-diag11b` worktree.

## Self-check

`hesap.dense` (`dense_self_check`, `src/dense_self_check.cpp`) runs all twenty routines of a table, f64 then f32:

- Inputs are small nonzero integers without a period that divides a vector width; every product and sum is exact, so
  the levels that round each step and the fused ones give the same bits in any order, and every zero is +0. The
  answers are those integers, evaluated at compile time and stored as `i16` (a `static_assert` refuses one that
  outgrows it).
- Lengths 77 (f64) and 153 (f32) run every reduction's whole groups, a remaining whole vector and a scalar tail at each
  level's accumulator count and width: the dots, the split-complex dots and the gemvt and cgemvt column blocks
  (`runs_every_dot`, a `static_assert`). gemv, gemvt, symv, trsv (lower, upper, unit and not), cgemvt and chemv read
  Hankel matrices (row i at h + i), so a full-order matrix costs one vector.
- The writing kernels take smaller orders on one stack array (under 10 KiB a type): cher2 at w + 3, the LDLT updates at
  2w + 3 (a 1 by 1 pivot over every row, then a 2 by 2 trailing update whose factors run both, f0 = 0, f1 = 0 and
  none), cholesky and cholesky_trsm at 25 and 45 (diagonal 1, 2 and 4, so the roots and reciprocals are exact; a
  zeroed pivot must stop at its column), whose dots run every phase below avx2.
- GEMM uses the table's own shape: pack_a and pack_b from a row-run and a column-run view each (both pack loops,
  zero padding), then eight blocks of 17 by 2w + 5 over those panels: assign, scale at beta 0 (C is NaN, never
  read) and otherwise, subtract, add at alpha 1 and otherwise, both C layouts, and two lower blocks whose diagonal
  keeps the first tile whole (the fused path), masks the next, or skips a tile row.
- Every output has a NaN guard after it. A failure names the type and routine (`f32 cholesky_trsm`).

## Tests and evidence

- `dense self-check:` cases: every table the host runs passes; each of the twenty routines with one answer a bit off,
  and a zero tile shape, fails under its own name. The process-level case requires the family's resolution `Passed`.
- win-debug: the 21 `[kernels]` cases and the 107 forced `-kernels-at-` arms pass. SDE 10.13.1 `-icx` with
  `CRD_CPU_LEVEL=avx512` passes the self-check and selection cases while running avx512. linux-gcc-debug: the plain
  cases and the ten forced arms pass.
- Teeth: the dot's answer one more failed every table (`f64 dot`) and the selection case's `Passed`; restored, rebuilt
  and passing.
- Guards: 21 of 21 (the reconfigured tree first needed track B's new `test_predicates_vs_shewchuk.cpp` built for the
  ISA isolation guard). Strict tidy and clang-format are clean on the changed files.

## Left on the row

Known-answer checks for hesap fft and tensor, the device path, `ceridc doctor --hardware` and the state-hash
divergence check.
