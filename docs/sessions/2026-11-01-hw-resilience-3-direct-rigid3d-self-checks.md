# 2026-11-01 — HW-RESILIENCE.3: known-answer self-checks of hesap-direct and eylem-rigid3d

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the hesap self-checks](2026-11-01-hw-resilience-3-hesap-self-checks.md).

## CI read

- Completed runs: push runs up to 41274373728 were cancelled by concurrency and have no failed jobs.
- Running complete-tier run 41269105413 (`b0b1b245`): clang-cl, win-vs and win-debug failed on `filesystem.cpp`'s
  `max` and `min` (windows.h macros), which 9723ac75 fixed after that commit; win-arm64-debug failed on the C4324
  921e19cc fixed. Running complete-tier run 41254049879 (`31404ec4`): its five failed jobs were fixed by a40af1bf,
  95ec6965, aa05dd37 and a93cf0ca. Nothing new to fix.
- Needs CI audit: complete-tier run 41274380605 (`de5e10b0`, queued) is the first one after 921e19cc, so its
  win-arm64-debug job carries HW-RESILIENCE.2's hosted clause. DIAG.11c and DIAG.12 wait on a green complete-tier
  run; three are queued or running, so none was dispatched. Account-backed lanes: the FTL names exist, no AMD or Mac
  names. DIAG.11b clause (8): no new recording under `build/diag-overhead-wpr/`.

## Self-checks

- `hesap.direct` (`direct_self_check`): each of the six kernels in f64 and f32 over one n by n integer panel, n = 3w
  + 5 for avx512's w lanes (29 and 53), entries in [-2, 2] and a diagonal in [1, 3]; the solves' right-hand sides are
  the panel times solutions in +-{1, 2, 3}. Every product and sum is an exact integer, so the levels that round each
  multiply and add and the fused ones give the same bits in any order, and the answers are integers evaluated at
  compile time. The shapes run every loop and tail: two-vector passes, a whole and a partial vector, a four-column
  block, a partial block and the backward solve's far terms, avx512's sixteen-column reduce group and its tail, and
  w + 3 right-hand sides. A failure names the type and routine (`f32 block_backward_diagonal`).
- `eylem.rigid3d` (`rigid3d_self_check`): three chunks at dt = 1/60, so avx512 runs a sixteen-lane step and an
  eight-lane tail. Free, inv_mass 0, NaN and negative lanes do not integrate (their snapshot still runs); a zero
  quaternion and one of length 1e-6 are kept by try_normalize; one lane has signed-zero velocities and one is damped
  past 1/dt. The answers are the integrator's operations in the kernel's order evaluated at compile time, each rounded
  once as every level rounds it; the square root is the integer root of the scaled significand, rounded to nearest.
  All 26 float columns of every lane are compared.

## Tests

- `direct self-check:` and `rigid3d self-check:` cases: every table the host runs passes (`[kernels]`, so the forced
  arms run them). A table with one routine wrong fails in that routine: each direct routine once (one answer a bit
  off, over both types), a rigid3d step with its last lane one bit off and one that skips a free lane's snapshot. New
  selection cases require each family's resolution `Passed` on the table that runs.

## Evidence

- win-debug: crd-hesap-direct-tests `[kernels]` and self-check cases (21 cases) and crd-eylem-rigid3d-tests (38)
  pass, and so do the 102 direct and 27 rigid3d forced `-kernels-at-` arms. SDE 10.13.1 `-icx` with
  `CRD_CPU_LEVEL=avx512` passes both families' self-check and selection cases while running avx512.
- Teeth: one known answer per family changed (the last rigid3d lane's rot_w one bit off, the first reduce answer of
  direct one more) failed every table and each selection case's `Passed`. Both were restored, rebuilt and pass again.
- Guards: 21 of 21. Strict tidy and clang-format are clean on the changed files.

## Left on the row

Known-answer checks for the other four families (hesap dense, fft, sparse and tensor), the device path,
`ceridc doctor --hardware` and the state-hash divergence check.
