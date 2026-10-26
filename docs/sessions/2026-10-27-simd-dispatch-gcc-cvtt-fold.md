# SIMD-DISPATCH: GCC folds a truncating conversion as a C cast

<!-- doc-role: historical -->
> Dated evidence. Live owner: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Contract:
> [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md). Previous:
> [stats kernels](2026-10-27-simd-dispatch-hesap-stats-kernels.md). Rules: [AGENTS](../../AGENTS.md).

## CI read

- Completed runs 40341016749 to 40416850706 are cancelled with no failed job; 40351178841's win-clang-cl failure is
  the flight-ring case `13f8e651` repaired (win-clang-cl passes on 40409214033).
- In-progress run 40409214033 (head `cf4b8a13`): job 121876231527, linux-gcc-relwithdebinfo, failed
  `simd convert_truncate gives INT32_MIN for NaN and out-of-range lanes` (`test_simd.cpp:662-663`): lane 2 (2^31)
  read 0x7fffffff, lane 3 (NaN) read 0. The -O0 Linux lanes, win-clang-cl and win-public-checks passed it. The
  optimising GCC lanes (linux-gcc-release, linux-gcc-shipping) were still running and take the same fix.
- Needs CI audit: no complete-tier run has finished since 40307186916; DIAG.11c and DIAG.12 are unchanged. One run is
  in progress and one pending, so nothing was dispatched.

## Cause

GCC 13.3 folds `_mm_cvttps_epi32` on known operands as a C float-to-int conversion once a lane is read: an
out-of-range lane saturates and a NaN gives 0, while `cvttps2dq` gives INT32_MIN. A four-lane probe printed
`2147483520 -2147483648 2147483647 0` at -O1, -O2 and -O2 -flto, the correct `... -2147483648 -2147483648` at -O0;
clang 18 at -O1/-O2 and MSVC /O2 (both widths) gave the instruction's values. Runtime operands always run the
instruction, so only folded code disagreed with every other build of the same function.

## Fix

- `crd/math/simd/convert.hpp`: both truncating conversions pass their operand through `detail::fold_barrier`, an
  empty `asm("" : "+x"(v))` on GCC only (no volatile, no memory clobber; it emits no instruction and leaves CSE and
  hoisting alone). The rounding paths of `truncate` and `round_nearest` need none: their magnitude mask replaces every
  lane at or above 2^23 and every NaN.
- NEON: FCVTZS saturates and gives 0 for a NaN, so the header's "INT32_MIN on every backend" was false there. A lane
  not below 2^31 now takes INT32_MIN (`fcmgt` plus `bsl`, read in clang 18's aarch64 output). No hosted lane runs
  NEON yet; the cross-level probe covers it when one does.

## Evidence

- Break: HEAD's crd-math-tests on a freshly configured linux-gcc-relwithdebinfo mirror tree failed the same two
  assertions as the hosted job.
- Fixed: the same tree passes all 171 cases (55,586 assertions) and the five `crd-math-kernels-at-<level>` CTests;
  win-debug passes the exe and its six CTests inside the MSVC environment; the 15 guard CTests, strict tidy, Allman and
  the format oracle (no finding on an added line) are clean.

## Intel SDE

The user approved Intel SDE 10.13.1 on 2026-10-27; it is installed here, so the AVX-512 arms no longer wait on the
user. Source `https://downloadmirror.intel.com/924984/sde-external-10.13.1-2026-07-28-{win,lin}.tar.xz`, SHA256 win
`74e626ede09b0baa5011fc9e51b58627ea92c3fc0bae5fd7db34b490f335f651`, lin
`94e97d623fec54385686e1e7ba65ebc9941748c05ee451423948334892bf2b50`; Windows
`%USERPROFILE%\.cerid\tools\sde\10.13.1\win\sde-external-10.13.1-2026-07-28-win\sde.exe`, WSL
`~/.cerid/tools/sde/10.13.1/sde-external-10.13.1-2026-07-28-lin/sde64`. The hosted arms take a pinned install
script with these hashes.

## Left

- GCC prints `-Wpsabi` notes ("the ABI for passing parameters with 32-byte alignment has changed in GCC 4.6") for
  `simd_level_probe.cpp` and `vec4d.hpp:430`, and with 64-byte alignment for `scene/world.hpp:145`. They are
  informational, one per unit; passing the probe's vectors by reference only moved the note to the next
  by-value function, so the settlement is a follow-up batch.
