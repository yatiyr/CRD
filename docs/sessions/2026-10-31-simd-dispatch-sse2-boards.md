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

## Left

The row's Left list carries: the nn avx2 regression, the scalar transcendentals of baseline general code (Gamma), the
Student-t quotient's second division, the hesap-tensor family's sse4.2 unit, the three narrowings, then the
jobs-retune thread sweep and fft f32 against MKL.
