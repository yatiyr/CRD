# 2026-11-01 — HW-RESILIENCE.3: known-answer self-checks of the crd-math families

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the kernel self-checks](2026-11-01-hw-resilience-3-kernel-self-checks.md).

## CI read

- No completed run has a failed job. Complete-tier run 41254049879 (`31404ec4`) is still running; its win-tidy,
  linux-gcc-release and linux-gcc-shipping failures were fixed by a40af1bf and 95ec6965. Complete-tier run
  41269105413 is queued, so none was dispatched.
- Running jobs that already failed: win-relwithdebinfo of run 41254049879 (job 124693382935, CTest
  `crd-ci-orchestrator-e2e`) and the Windows repository job of push run 41262267004 (job 124718831209,
  `scripts/test-ci-orchestrator-e2e.py`), both on `test_1_webhook_build_runs_end_to_end`:
  `{'plan': ('scheduled', False)} != {'plan': ('passed', False), ...}`. The build reads `passed` while its only job
  never ran. Cause, in `ci/` (the operator's; reported, not edited): `create_build` writes the build with
  `jobs: []`, indexes it active, then uploads the plan step. A watchdog tick in that window runs `settle`, whose
  `build_outcome(build, [])` is `passed` (no job is non-terminal); its compare-and-swap then marks the build passed
  over the record the upload has just changed, without recomputing the outcome, and drops it from the active index,
  so the plan is never dispatched. A scratch script that runs a tick between the build's creation and the upload
  reproduces the exact view (`passed [('plan', 'scheduled')]`, active index empty). Fix for the orchestrator:
  `build_outcome` returns None for a build whose job list is empty or not fully read, `settle`'s change recomputes
  the outcome on the record it updates, and `jobs_of` does not drop an unreadable job. Until then every
  Windows CTest lane and the Windows repository job can fail on it, which blocks a green complete-tier run.
- Needs CI audit: no completed run covers the commits after `c2645814`'s run; HW-RESILIENCE.2, DIAG.11c and DIAG.12
  are unchanged. Account-backed lanes: the FTL variables exist; no AMD or Mac names.

## Platform fixes found on the way

- `crd-platform` stopped linking GLFW in aa91b382, and `filesystem.cpp` failed to compile without a precompiled
  header (`error C4003` on `std::numeric_limits<i64>::max()`: windows.h's `max` macro). `filesystem.cpp`,
  `durable_file.cpp` and `dynamic_library.cpp` define `NOMINMAX` before windows.h, as crd-core's units do.
- The strict tidy gate refused `filesystem.cpp`'s local constants `kPerSecond`, `kMax` and `kMin` (local
  constants are snake case); renamed.

## Math self-checks

- `math.float-convert` (`float_convert_self_check`): every routine over 45 elements (two 16-lane steps and a tail
  on every width): the conversion tests' 42 special f32 patterns and three plain values, 45 f16 patterns for the
  widening (every class, then a Weyl sequence) and 45 fixed draws for stochastic rounding. The answers are the scalar
  converts evaluated at compile time, so no runtime arithmetic produces them; f32 data moves as bytes, so signalling
  NaNs reach the kernel unchanged.
- `math.scalar` (`scalar_math_self_check`): each of the 23 routines at two arguments (core range and reduction,
  overflow-side or subnormal path; sincos at 2 and 1e22) against fixed bits from the portable table, every one within
  one ulp of mpmath at 200 bits.
- `tests/support/kernel-levels/kernel_levels.hpp` gains `self_check_levels` and `kernel_family` for every family's
  suite; the checksum suite's own lookup is gone.

## Tests

- `float conversion self-check:` and `scalar transcendental self-check:` cases: every table the host runs passes (a
  `[kernels]` case, so every forced arm and SDE runs it), and a table with one wrong routine fails in that routine
  (each of the 9 conversions with one bit of its last, tail element wrong; each of the 23 transcendentals one ulp
  off). The selection cases also require the family's resolution `Passed` on the table that runs.

## Evidence

- win-debug: crd-math-tests passes (196 cases), forced portable, sse2, sse4.2 and avx2 `[kernels]` arms pass, and
  SDE 10.13.1 `-icx` with `CRD_CPU_LEVEL=avx512` passes the self-check cases; the doctor and checksum cases pass.
  GCC 13 and Clang compile the new units and the test (syntax only, the checkout against the WSL mirror's commands).
- mpmath: 42 of the 46 transcendental answers are correctly rounded; four (atan, asin and acos at 0.3, sinh at 0.2)
  are one ulp off.
- Teeth: one bit of exp's first answer wrong made every scalar table fail (`portable exp`) and the selection case
  fail; the f16 answer of the tail element wrong made every conversion table fail (`portable f32_to_f16`). Both
  restored, rebuilt and passing.
- Guards (`crd-no-*`, `crd-check*`, diag coverage and seeded faults) pass serially. At `-j4`,
  `crd-check-capability-owners` died twice (SegFault) and a concurrent run of the script raised CPython's
  `RuntimeError: internal error in regular expression engine`; alone it passes every time. That is this host's
  recorded fault under load (impossible CPython errors in native Windows validators), not the script.

## Left on the row

Known-answer checks for the other eleven families (geometry mesh and primitives, hesap autodiff, dense, direct, fft,
sparse, special, stats and tensor, eylem rigid3d), the device path, `ceridc doctor --hardware` and the state-hash
divergence check.
