# 2026-10-29 — SIMD-DISPATCH: forced arms run their own level, not every level again

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); follows
[the environment guard note](2026-10-29-simd-dispatch-environment-guard.md), which recorded win-asan's 4 h 58 min job.

## CI first

Every completed run in the last 15 is a pending push run the concurrency group replaced before any job started.
Push run 40784988994 (92db578d) still runs win-asan and linux-gcc-asan, each in its Test step for over four hours;
its finished failures are the ones earlier batches traced to fixes at HEAD. DIAG.11c and DIAG.12 stay Needs CI.

The Test step has grown on every lane (UTC start to end, push and scheduled runs on main):

| Run (commit) | win-asan | linux-gcc-asan | win-debug | linux-gcc-debug | win-release |
| --- | --- | --- | --- | --- | --- |
| 40455017769 (09ba6a3a) | 2 h 58 min | 1 h 19 min | 54 min | 38 min | 13 min |
| 40701630725 (3c1debc6) | 2 h 27 min | 4 h 02 min | 1 h 23 min | 1 h 14 min | 20 min |
| 40748658864 (f7304724) | 4 h 37 min | build failed | 1 h 45 min | build failed | 22 min |
| 40784988994 (92db578d) | over 4 h, running | over 4 h, running | 1 h 54 min | 1 h 23 min | 55 min |

Two causes, from the per-test times of the JUnit logs:

- **Forced arms ran every level again.** Each `<suite>-kernels-at-<level>` arm sets `CRD_CPU_LEVEL`, but every
  differential case looped over `cpu_selection().detected`, so N arms each ran all N levels. On win-asan the five
  `crd-hesap-fft-kernels-at-*` arms took about 770 s each (portable's arm as long as avx512's), 3,854 s in all; on
  linux-gcc-asan 825 s each. win-release's 19 SDE arms, which ran every level under emulation, took 398 s in
  40784988994; most of its growth there was `crd-check-isa-isolation` reaching its 1,800 s timeout on the image
  route, which e17984e3 (pipes read as bytes, ten times faster) already fixed.
  The lanes' `-at-sse2` passes also re-ran every arm unchanged (an arm's own `CRD_CPU_LEVEL` wins) and the guards.
- **Tests common to two runs got slower** (not caused here): linux-gcc-asan's common tests went from 4,714 s to
  8,904 s between 09ba6a3a and 3c1debc6, small cases included (6,996 cases under 0.3 s: 406 s to 635 s), and on
  win-asan from 8,784 s to 16,599 s between 3c1debc6 and f7304724 (7,479 small cases: 496 s to 845 s; AMG 307 s to
  950 s). Both runs used image
  20261021.278.1 (Windows) and 20261012.334.1 (Ubuntu); the runner CPU is not in those logs.

## What landed

- `tests/support/kernel-levels/kernel_levels.hpp`: `crd::test::kernel_level_runs_here`, the one rule for which levels
  a differential case runs. Unforced, every level the host executes; under an applied level, portable (whose bits
  the others match) and the selected level; a refused level runs every level. A feature variant also needs its
  features usable, so the SDE spr arm still runs tensor's VNNI variant (the arm's masked `kernel_features` would hide
  it). It replaces the twelve files' own `runs_here` and the two inline checks in crd-math's tests.
- `crd_add_kernel_level_tests` puts the header on each suite's include path, and the portable, sse2 and neon arms now
  also run `[cpu-level-applied]` (every CPU of the architecture runs those levels, so a refusal is a defect).
- `test_simd_levels`'s claim that the level above portable runs holds unless an arm forced portable.
- The `win-debug-at-sse2` and `linux-gcc-debug-at-sse2` test presets exclude `-kernels-at-`, `^crd-check-` and
  `^crd-no-`. ADR-0134 §8, ci-tiers.md and the workflow comments say so.

- fft's every-band case computes each input's reference once for both of its shifts (it computed it twice).

Per arm the differential work drops from N levels to two (one at portable). Portable dominates in unoptimized code:
on win-debug the fft `[kernels]` cases took 154 s unforced, 95 s at portable and about 100 s at sse2 (this hybrid host
varies by tens of seconds between runs), so a plain arm costs about 0.6 of what it did. The SDE arms, which emulated
every level, gain too.

## Checks

win-debug: the twelve kernel suites build at /W4 /WX. `crd-math-tests "[kernels]"` passes unforced (598,032
assertions), at portable (150,591, with `[cpu-level-applied]`) and at sse2 and avx2 (299,739); `crd-hesap-fft-tests
"[kernels]"` passes unforced (5,035), at sse2 (3,451) and at portable (1,867). Tidy on the changed C++, clang-format
(one older finding in test_dense_vector_kernels.cpp fixed), Allman, master-plan, repository and the pinned
actionlint 1.7.12 pass. No suites, following the fast-row order. One Allman run, during the build, died in its
bracket matcher with "'tuple' object is not an iterator" on a string; four reruns passed (recorded, not chased).

## Left

- A plain arm still repeats portable, the cost that dominates. Its level ran natively in the lane's unforced run of
  the same binary, so its differential cases add only the lowered CRT, which the `-at-sse2` pass already covers: the
  plain arms should run only the cases the selection decides, and the SDE arms keep the differential ones. That also
  covers a refused arm (at-avx512 on a runner without AVX-512), which still runs every level as the unforced run does.
- The slowdown of common tests above: read the `cpu detected=` lines 92db578d's records print in the next green run's
  evidence, compare the runner CPU between runs, and bisect locally only if it matches.
