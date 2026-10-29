# 2026-10-29 — SIMD-DISPATCH: the SSE2/SSE4.2 boards' drivers and gates, and the ARM64 lanes green

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 6, the boards of the sse2 and sse4.2 bodies).
Before: [the permute board's HPTT peer](2026-10-29-simd-dispatch-hptt-peer.md).

## CI first

- No completed run has a failed job; the later push runs were replaced while queued (no jobs). Nothing cancelled or
  dispatched.
- Push run 40929108987 (1ebe9dc4), still running: linux-gcc-arm64-asan passed 2479 of 2479 in 4,760 s of test time
  (the guard 79.7 s), the first run to complete the lane, after a1901325 stopped GCC's AArch64 use-after-return
  frames; linux-clang-arm64-asan 2479 of 2479 (6,627 s, the guard 96.5 s), win-arm64-debug and both arm64 debug
  lanes green. Every ARM64 lane has now passed, so that clause leaves the Left list. win-asan, the first run under
  the three-job bound, is still running.
- Needs CI audit: DIAG.11c and DIAG.12 wait on a green complete-tier run; none has completed.

## What landed

Item 6's five boards each have a driver and a gate. A gate checks the timed output itself, so no speed figure stands
on wrong values. Each runs once per CPU level through `run-lane-bench.sh`, one binary.

- **Gates**, each exiting 1 on a miss and printing the level line:
  - `bench_dtypes`: after each timed row, the portable conversion or Q8_0 kernel's bytes over the same input; the SR
    batches against the per-element scalar converts (the pinned keying). `dequantize_q8_0` is scalar everywhere.
  - `bench_reduce`: Tier D against the portable block kernel folded left to right (the serial tree), Tier R against
    the portable binned deposits.
  - `bench_logpdf_grad`: the Student-t sum against the portable sum at the first and last nu the row times. The
    hardcoded host name is gone.
  - `bench_special_batch`: both 4 Mi batches against the portable Lanczos kernels, value by value.
  - `bench_nn`: the input is now the corpus's frozen rows repeated to fill each batch, so every output row has a
    reference: the f32 net at the corpus batch, itself within 1e-6 of torch's frozen outputs. The f32 tier must stay
    within 1e-6 of it (the dense-vs-direct test's bound), Q8_0 and i8 within 2e-2 (their tests' bound). A whole-net
    bit gate against portable is not possible: the f32 dense tier's GEMM declares per-level arithmetic, not bits.
- **Drivers**: `scripts/run_bench_nn.sh` (`CRD_NN_CORPUS`, since lane runs start in a scratch directory; torch and
  onnxruntime), `run_bench_logpdf_grad.sh` (JAX and PyTorch) and `run_bench_special_batch.sh` (the one-thread rows,
  scipy; MATLAB's column on Windows, Boost's from `run_bench_boost.sh`), each after its focused CTest gate.
- Strict tidy's naming and object-comparison findings in the five files are fixed.

## Checks

Objects-only GCC 13 compiles of the five benches with the linux-gcc-debug tree's flags (`-Werror`); strict tidy
(LLVM 20) clean through a scratch database; clang-format; `bash -n` on the three drivers; Allman braces, the ctest
guards, the plan and repository checks. As the fast-row order says, no bench ran; the board runs at row close.

## Left

The boards at row close. The scout brief's 128-bit levers (Q8_0 tile width, Tier R stream count) are Phase 3 levers,
taken only where the board shows a loss.
