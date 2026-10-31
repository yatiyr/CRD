# 2026-10-31 — SIMD-DISPATCH: the Student-t sum against the pre-row engine

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (the sse2 boards' findings). Before:
[the sse2 and sse4.2 boards](2026-10-31-simd-dispatch-sse2-boards.md). Evidence:
[sse2 boards, the Student-t attempt](../bench/2026-10-31-simd-dispatch-sse2-boards.md#the-student-t-attempt).

## CI first

- No completed run has a failed job. Scheduled run 41188120176 is queued; it tests GitHub's copy of main, still
  `d9fdeae3`: the GitLab push mirror has carried nothing since then, so no later commit has a hosted run. Nothing was
  cancelled or dispatched.
- Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run. Repository variables `FTL_GCP_PROJECT`,
  `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only), no secrets; the Firebase lane is wired in `ci/`,
  the AMD and Mac lanes wait on their accounts.

## The attempt

The Left list named the Student-t sum's cause as `e0f539cb`'s second division in the log reduction and its fix as one
reciprocal for the quotient pair. The fix ran in the scalar core and the SSE2, AVX2 and AVX-512 twins: `ru = 1/u`,
`t = f·ru`, `tl` the FMA remainder times `ru`.

- **Accuracy.** A Python model of `crd_log1` (the same operations through `math.fma`, against 300-bit mpmath, 74,424
  points: the seams at √2, √½ and 1 at exponents 0, ±1, ±1000 and -1022, subnormals and random points) measured
  0.511 ulp against 0.516 for two divisions, `t + tl` within 2^-103.3 of the quotient; 15 results differ. The C++
  accuracy gates passed on win-debug and linux-gcc-release; only the golden checksums moved.
- **Board** (five interleaved rounds, one core, busy host: 14.4 of 63.7 GiB available, other processes 1.2 cores):
  before over after 1.005 [1.00-1.02] at avx2 and 0.991 [0.986-0.993] at sse2. No win, so it was reverted.

## The cause

The two loops' disassemblies: pre-row's sum is inlined into `main`, about 45 operations and two divisions per four
elements; HEAD's avx2 unit runs the same loop with about 65 operations and three divisions. With the divisions
equalised nothing moved, so the cost is the work `e0f539cb` added to take log from 7 ulp to 0.52: `m + 1`'s exact
error, the quotient's remainder, the carried term and the exact head sum. Pre-row over HEAD stays 0.766
[0.758-0.773], its spread within a round about 3%. Accepted under the user's 2026-10-30 board rule ("if we are not
losing too much ground ... it is fine since we got healthier and stronger and more correct"); a second log core for
reductions would duplicate the one log, and a table-driven log is a redesign, not one focused attempt. The sum stays
2.2x ahead of JAX.

Also measured: the sse2 Student-t body runs 49.5 ms against 1.19 at avx2 (the exact FMA emulation of the sse2
twin), the body judged against `portable@sse2` in the previous board.

## Left

The row's Left list keeps the cnn rows' 11% to 16% and the scalar transcendentals of baseline general code (Gamma;
nn's gelu, tanh and sigmoid spans), then the jobs-retune thread sweep and fft f32 against MKL.
