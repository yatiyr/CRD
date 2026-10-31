# 2026-10-31 — SIMD-DISPATCH: the sse2 and sse4.2 bodies' boards

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards: the sse2 and sse4.2 bodies). Before:
[the permute board](2026-10-31-simd-dispatch-permute-board.md). Evidence:
[sse2 boards](../bench/2026-10-31-simd-dispatch-sse2-boards.md).

## CI first

- No completed run has a failed job: push run 41167488972 (`d9fdeae3`) passed. GitHub's copy of main still stands
  at `d9fdeae3` while GitLab's is at `7aeacb8e` (60 commits): the push mirror has carried nothing since 07:10, so no
  later commit has a hosted run. No complete-tier run was dispatched (it would test `d9fdeae3`).
- Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run. Repository variables `FTL_GCP_PROJECT`,
  `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only), no secrets: the AMD and Mac lanes wait on their
  accounts.

## The boards

The five drivers had only been syntax-checked; their first runs built and passed every gate. The nn and log-density
drivers were then refused by `wsl-lane.sh run`'s mirror check: another session was editing `ci/` in the checkout.
The board ran the same binaries straight from the lane tree with `run-lane-bench.sh`'s environment.

The drivers' `portable` arm keeps the host's glibc, which runs glibc's `fma` and `libm` in their FMA bodies; an
SSE2 machine running the portable loop gets the non-FMA ones. The board's `portable@sse2` arm hides those features,
and every sse2 body was judged against it (special's portable Lanczos: 47 ns on the host's glibc, 241 at sse2).

Findings, all in every round:

- Every sse2 body beats the portable loop (1.11x to 5.48x) but the round-to-nearest narrowings to f16, e4m3 and
  e5m2 (0.62x to 0.72x): the four-lane transcription runs the subnormal path on every step.
- The conversion family's sse4.2 unit beats sse2 (1.16x to 1.30x); the hesap-tensor family's does not (mlp i8
  0.92x, the rest 0.97x to 1.01x).
- Against the pre-row engine at avx2, dtypes and reduce are level or faster (Q8_0 quantize 7.5x), and three rows
  regressed: the nn nets (cnn 0.27x to 0.38x, mlp 0.58x to 0.70x), the Gamma O(1) gradient (0.122x) and the
  Student-t sum (0.761x).
  - Gamma: general code is built at the baseline ISA in this row, so the scalar log and digamma cores call glibc's
    `fma` (88 `call fma@plt` sites in HEAD's bench, none in pre-row's). It is the first measured case of a class:
    scalar transcendentals in baseline general code.
  - Student-t: `e0f539cb` gave `crd_log4` a second division to carry the quotient's rounding (log had reached 7 ulp).
  - nn: not located yet.
- Peers: MATLAB-1T lgamma 7.49 and tgamma 14.13 ns, scipy 8.41 and 15.06, against 4.85 and 5.92 at avx2; Boost's
  scalar digamma now leads Cerid's (21.9 against 24.8 ns), the Gamma class again.

## Folds

- **Narrowings.** `narrow_rne_lanes` (`float_convert_kernels.cpp`) runs the subnormal path only when a nonzero lane
  takes it (a zero lane's result is the class selection's anyway); the bits are unchanged. One focused attempt, kept:
  every sse2 and sse4.2 narrowing now beats `portable@sse2` (1.43x to 1.73x), and the avx2 fp8 narrowings, on the
  same transcription, run 1.44x pre-row.
- **hesap-tensor sse4.2 unit removed** (`LEVELS portable sse2 avx2 avx512`): an sse4.2 selection runs the sse2 table;
  its `maddubs` branch is gone and the level test holds that no sse4.2 table exists.
- **`run-lane-bench.sh`** takes `<level>@<glibc level>` entries, so the drivers can run `portable@sse2` and
  `sse2@sse4.2`.

Checks: linux-gcc-release `crd-math-tests` and `crd-hesap-tensor-tests` built, the conversion and tensor kernel
cases and every `kernels-at-<level>` arm pass (47 and 102), the v14-a gates pass; win-debug builds of both targets
and their kernel cases; Allman braces, clang-format (no new finding).

## The nn regression

`perf` runs in WSL with the 6.8 linux-tools build and the cpu-clock software event (the kernel's own perf is absent),
so both binaries were profiled; callgrind counted 59.8 G instructions in HEAD against 48.2 G pre-row, too few to
explain 3.7x, which pointed at serial chains:

- The convolution's fused 2x2 pool called the scalar IEEE `maximum` three times per output (54% of HEAD's samples).
  `max_pool2_row` now takes the vertical maximum and the column pairs' maximum (`load_complex_deinterleaved`'s even
  and odd lanes) on vector lanes, with zero-filled vector tails; the fused pool and the stand-alone 2x2 pool share
  it. The same expression per output, so the bits are unchanged.
- The mlp's layernorm stayed in general code, so its f32 `fma` chain ran the exact emulation at the baseline ISA.
  It is now `nn_layernorm_rows`, a kernel of the nn unit (`NnLayerNormF32`), in the same operation order; a new
  level test checks its bits on every level against the scalar order (constant rows, a large offset, NaN, a variance
  below eps and one that overflows).

After them the mlp rows are at pre-row within the spread but Q8_0 at batch 16 (0.91x), and the cnn rows trail by
11% to 16% in every round: the convolution kernel takes 18% more samples than pre-row's and the pool and image loop
55% more. Linux: the 199 nn and tensor kernel cases pass.

## Left

The row's Left list carries: the cnn rows' remaining 11% to 16% (the convolution step's code against pre-row's, the
per-row pool call), the scalar transcendentals of baseline general code (Gamma; nn's gelu, tanh and sigmoid spans,
whose erf, tanh and exp cores are general code too), the Student-t quotient's second division, then the jobs-retune
thread sweep and fft f32 against MKL.
