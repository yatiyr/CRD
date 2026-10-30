# 2026-10-30 — SIMD-DISPATCH: dense QR's and trsm's crd-jobs picks

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)). Before:
[batch_gradient's and the task schedulers' levers](2026-10-30-simd-dispatch-jobs-autodiff-sched-picks.md).

## CI first

- Push run 40976392370 (c28aaa54), still running: its eight failed linux-gcc and linux-clang-tsan jobs all stop at the
  test_cholesky.cpp:340 double promotion fixed at bc2bdb83 (the clang-tsan log reread; HEAD casts both factors).
  Every completed run since was cancelled before a job started; 40991777471 (70dd4d44) is pending. Nothing cancelled
  or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has run. Nothing flips. The first
  green win-asan run under the three-job bound is still to come (40976392370's win-asan job is in progress).

## What landed

- **`detail::QrPick`** (factor_arms.hpp): `unblocked_max` (128), `block` (32, was `kQrBlockSize`), `trailing_min` (each
  of the panel's two trailing gemms, W = V^T A2 and A2 -= V W', on the jobs from this m n k on; default 256K,
  `gemm_parallel_auto`'s cut, so production is unchanged) and `workers`. `factor_qr` is `factor_qr_arm(QrPick{})`;
  `kQrBlockSize` and `kQrUnblockedMax` are gone. Kept serial and timed by the board, not levers: the panel's
  transpose in and out, `materialize_panel_v`, the V^T V and T^T W gemms (nb by nb and nb by n).
- **`detail::TrsmPick`** (gemm_arms.hpp, now the blas3 arms): `parallel_min` (whole columns of B across the jobs from
  n n cols on; default never, today's serial solve) and `workers`. `trsm` is `trsm_arm(TrsmPick{})`; its four
  Side by Trans bodies are one `trsm_columns` (scale by alpha, then the forward or backward solve per column, the same
  operations in the same order).

Determinism: the jobs, `trailing_min` and `parallel_min` decide only which job computes a column, never its value:
every such arm gives the one-job bits. `block` and `unblocked_max` regroup the sums and are held to Householder QR's
columnwise backward-error bound, |a_j - (QR)_j| <= m n eps |a_j| (Higham Theorem 19.4 with c = 1, Q R formed in
double from the stored reflectors).

- **Tests:** `factor_qr_arm` (f32, f64) at 1 x 1, 33 x 32, 100 x 100 (under the cut), 200 x 130 (tall) and 120 x 190
  (wide, always blocked); the default path, always blocked at blocks 8, 32 and 50, always unblocked; random input, IEEE
  specials (NaN, infinity, -0, a subnormal) and a zero column (tau 0). Each blocked path's trailing cut at its default,
  0 and never runs at 4 to 32 jobs against one job, packed and taus bit for bit; every one-job path meets the bound.
  win-debug 18 s. `trsm_arm`: all four types, both sides, the three Trans; one element, one column, fewer and more
  columns than jobs; NaN, infinities, -0 and subnormals in B; cut 0, exactly this solve's work and one past it at 1 to
  32 jobs against `trsm`, over a B whose ld exceeds its columns with guard words around it. win-debug 0.2 s.
- **Board:** `bench_hesap_factor_parallel` gains QR (square and tall 2n by n, the trailing cut always and never, always
  blocked, unblocked to 256, blocks 16 and 64) and trsm (an n by n B, columns split always and from 1M); bit arms gated
  bit for bit, the others by QR's bound, the solve by |B - L X| <= 4 n eps |L| |X| (Higham 8.5); a miss exits 1.
  `scripts/run_bench_factor_parallel.sh` runs the two new cases with the gates.

## Checks

win-debug builds of crd-hesap-dense-tests and the bench; the new cases and the existing `[qr]`, `[trsm]` and
`[lstsq]` cases pass (22 cases); strict tidy (LLVM 20) clean on the seven changed C++ files; clang-format on the
added lines; Allman braces; the ctest guards (19 pass); `bash -n` on the driver. No bench ran: the boards run at row
close.

## Left

The retune's last consumer: the batched fft transforms judged as jobs (hesap-fft has no jobs code; a batch split
across per-worker plans as the board's arm). Then the boards per level with the thread sweep.
