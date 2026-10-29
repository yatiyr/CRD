# 2026-10-29 — SIMD-DISPATCH: the f32 FFT board's one-binary driver

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 1, fft's f32 1M forward);
follows [the tiled four-step](2026-10-29-simd-dispatch-tiled-four-step.md) (its Left: the three arms and MKL from one
binary). Also this batch: [the retune fold](2026-10-29-user-decision-fold-jobs-retune.md).

## CI first

Scheduled run 40849112570 (fea7ee0a) is still running (win-asan, linux-gcc-asan, linux-gcc-arm64-asan); nothing was
cancelled. Its newly completed failure, linux-clang-tsan, fails only `crd-generated-sources` and the provenance case of
`crd-repository-tools` on `transcendental_refs.inc`, the refs manifest pair 74a95125 fixed. The push runs since
13a4b48c were replaced before any job failed. DIAG.11c and DIAG.12 stay Needs CI: no green complete-tier run yet.

## What landed

`runtime/examples/bench_fft_f32.cpp` is rewritten as the board's driver:

- Every power of two from 2^10 to 2^22 on the f64 board's deterministic input (random in [-1, 1), so the error form
  `max |x - truth| / (1 + max |truth|)` is not divided by a two-tone spike), each plan and MKL's in-place
  `DFTI_SINGLE` descriptor built outside timing.
- At 2^20 the three `LargeForward` arms (deep-split, six-step, tiled four-step) run in this one binary, interleaved
  with MKL every rep, best of 30, and the winner line names the fastest arm and its ratio.
- The gate: each Cerid arm and MKL against an FFTW f64 transform (`FFTW_ESTIMATE`, planned before the buffers are
  filled) of the same f32 values, failing above the ADR-0092 §5 f32 class (2e-4, 1e-3 from 2^19). The old program
  compared against MKL, printed it and always returned 0.
- `crd::containers::Array` buffers replace its three raw allocations, so their check-allocation-results exemption
  is gone. MKL's version string is printed first.

## Checks

One build and one run at the detected level on linux-gcc-release through `run-lane-bench.sh` (not a board, host not
quiet): every row inside the gate, Cerid 1.2e-7 to 3.1e-7, MKL 1.1e-7 to 2.2e-7 (MKL 2020.0.4). At 2^20 the tiled
four-step was fastest (2.95 ms, 0.85x MKL; six-step 0.84x, deep-split 0.76x). The f32 forward trailed MKL at every
size in that run (0.58x at 1K to 0.85x), so the row-close board judges f32 at every size, not only 1M. Strict
tidy (LLVM 20 on that tree's database; no Windows tree builds the bench) found one nested conditional, fixed and
rebuilt. Format, Allman braces, the allocation-results guard, the tooling tests, the plan and repository validators.
