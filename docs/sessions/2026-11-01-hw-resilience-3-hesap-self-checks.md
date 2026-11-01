# 2026-11-01 — HW-RESILIENCE.3: known-answer self-checks of three hesap families

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the geometry self-checks](2026-11-01-hw-resilience-3-geometry-self-checks.md).

## CI read

- Completed runs: push runs 41266551828 to 41273181833 were cancelled by concurrency and have no failed jobs.
- Running complete-tier run 41254049879 (`31404ec4`): win-asan shard 1 (job 124693382989) failed
  `sample ring: a copy never straddles a clear` with `samples_seen 0 > 0`. The producer's fixed 3000 rounds take
  about 0.1 s and ended before any copier thread was first scheduled (ASan, ctest -j3 on a hosted runner). Copiers
  delayed 500 ms reproduced it on win-debug every run. a93cf0ca keeps the producer running past those rounds until a
  copier has seen a sample or ten seconds pass, as the contention case already does; the delayed copiers then pass.
  Its win-relwithdebinfo failure is the orchestrator race aa05dd37 fixed.
- Running complete-tier run 41269105413 (`b0b1b245`): win-arm64-debug (job 124750068765) failed to build
  `checksum.cpp`: ARM64 MSVC warns C4324 on the padding `alignas(64)` added to the 1026-byte known-answer stream (x64
  MSVC does not). 921e19cc rounds the storage up to whole 64-byte lines, with a `static_assert` that the alignment
  pads nothing. Ninja stopped at that first error, so the next ARM64 run may show more.
- Needs CI audit: HW-RESILIENCE.2's hosted clause needs a run at or after 921e19cc, since 41269105413's
  win-arm64-debug job did not build. DIAG.11c and DIAG.12 are unchanged; no complete-tier run was dispatched while
  41254049879 and 41269105413 are still running. Account-backed lanes: the FTL variables exist; no AMD or Mac names.
- DIAG.11b clause (8): no new recording under `build/diag-overhead-wpr/`.

## Hesap self-checks

- `hesap.autodiff` (`autodiff_self_check`): each canned function's gradient at n = 32 (four eight-direction passes)
  and n = 13 (a partial pass) at x_i = ((5 i mod 17) - 8) / 4. Every jet product and sum is exact, so the answers
  are the gradients' closed forms evaluated at compile time.
- `hesap.special` (`special_self_check`): lgamma and tgamma over 13 arguments, one eight-lane step, one four-lane
  step and a scalar tail at avx512. The reflection, pole, NaN and infinity lanes sit inside the vector steps, the
  half-plane's ends are lanes, and 171.75 overflows tgamma. The answers are the portable table's bits; each finite
  one is within 2.5e-14 relative of mpmath at 200 bits (the worst is lgamma(-2.5), near a zero; the functions' gate
  is 1e-13). NaN answers are checked as NaN, since a produced NaN's sign differs between x86-64 and AArch64.
- `hesap.stats` (`stats_self_check`): 93 Philox blocks and 47, then 45, Threefry blocks, so every group loop and
  scalar tail of every level runs. The expected blocks are the scalar block functions evaluated at compile time.
  Philox's position crosses 2^32, and Threefry's counter carries out of its low word, inside a group at each level.
  The Student-t score sum over 15 elements with nu = 4 is checked against the portable bits, 0.15 ulp from mpmath.

## Tests

- `special self-check:` and `stats self-check:` cases, and the autodiff self-check case: every table the host runs
  passes (`[kernels]`, so every forced arm runs them). A table with one wrong routine fails in that routine: each
  routine's last answer one bit off, a tgamma that returns 0 for NaN, Threefry wrong only in the 45-block call, and a
  gradient wrong only in a partial pass. The selection cases also require the family's resolution `Passed` on the
  table that runs.

## Evidence

- win-debug: crd-hesap-special-tests (44 cases), crd-hesap-stats-tests (143) and crd-hesap-autodiff-tests (132) pass,
  and so do the 91 forced `-kernels-at-` arms. SDE 10.13.1 `-icx` with `CRD_CPU_LEVEL=avx512` passes each family's
  `[kernels]` cases while running avx512.
- Teeth: one known answer per family changed (tgamma(4.75) and the Student-t sum one bit off, the last Cubes
  component 1/4 off) failed every table and each selection case's `Passed`. All three were restored, rebuilt and
  pass again.
- Guards: 21 of 21. Strict tidy and clang-format are clean on the changed files.

## Left on the row

Known-answer checks for the other six families (hesap dense, direct, fft, sparse and tensor, eylem rigid3d), the
device path, `ceridc doctor --hardware` and the state-hash divergence check.
