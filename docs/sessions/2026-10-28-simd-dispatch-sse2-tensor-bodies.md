# 2026-10-28 — SIMD-DISPATCH: SSE2 bodies of the hesap-tensor family

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).
HEAD before: `342ff7da`.

## CI first

- Push run 40581144513 (`59c79f2f`, still running): its failed GCC and Clang jobs are the `GemmBlock` `beta`
  initializers (`syrk_microkernel.hpp:69`, log of linux-gcc-public-checks) that `90a87169` fixed. Every later push run
  was cancelled before a job failed; complete-tier run 40592200636 (`d57b11cd`) is in progress. DIAG.11c and DIAG.12
  stay Needs CI until a complete-tier run finishes.

## Tier R ladder on SSE2

- `binned_sum_f64` (`kernels/tensor_kernels.cpp`) deposits through one register trait per level (`BinnedIsa`: load,
  store, add, sub, or, and, max): three streams of four lanes on AVX2 (unchanged), three of two on SSE2 and SSE4.2.
  The step, the super-block (`kBinEndurance` steps) and the vector gate (eight steps) follow the level. The binned
  representation is partition-independent, so the lane and stream count cannot change a rung.
- Tests: the binned differential adds lengths around the SSE2 gate (47, 48, 49, 53) and its super-block, and a late
  magnitude in the SSE2 body's second super-block (its speculative rollback); the AVX2 cases stay. Local, win-debug:
  `[kernels]` passes forced at sse2 and avx2, `[repro]` at sse2.

## Board candidates (row-close pass)

- The sse2 Tier R ladder against portable and ReproBLAS (portable was 6.5x behind ReproBLAS); the stream count is a
  board choice.
