# 2026-10-29 — SIMD-DISPATCH: forced levels reach MSVC's own dispatch

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `b80f733a`. ADR-0134 §8 (amended).

## CI first

No run completed with results: every run after `f7304724`'s was cancelled before a job started. Push run 40748658864
(`f7304724`) is still running; its finished failures are all the fft kernels' missing `<cstring>` (GCC's
`memcpy` and Clang's `wmemcpy` correction at `fft_kernels.cpp:2273`/`:2434`, x64 and ARM64) or the MSVC ARM64 timer,
both fixed in `fcb3bb42`, and win-tidy's four test findings, fixed in `b80f733a`. No Needs CI row has new evidence.

## What landed

- MSVC's vectorizer emits `__isa_available`-tested AVX-512 and SSE4.1 paths under every `/arch` (the guard's
  `msvc-isa-dispatch`), so a forced arm on a wider host ran them. Each image keeps its own CRT dispatch state in
  `MSVCRT:cpu_disp.obj`: `__isa_available` (5 here), `__isa_enabled` (bit set, `0x2e`), `__isa_inverted`
  (`__IA_SUPPORT_*`, a set bit is unsupported) and `__avx10_version`. Of 2,439 win-debug and 1,415 win-release
  objects only `__isa_available` is read; `msvcprt:vector_algorithms.obj` also reads `__isa_enabled`.
- `cpu_features.cpp`: an applied level lowers all four to its level, never raising them, and below avx2 calls
  `_set_FMA3_enable(0)`, which reaches ucrtbase's libm. A pointer in `.CRT$XCL` (after the C initializers that set
  the CRT state, before every user initializer and thread) selects at startup when `CRD_CPU_LEVEL` is set; crd-core's
  interface links `/INCLUDE:kCrdCpuSelectAtStart` on MSVC x64 so every image keeps it (the crash TLS callback's
  pattern). `cpu_runtime_dispatch()` reports the CRT level and the FMA3 state.
- Boundary (ADR §8): vcruntime140's and ucrtbase's string routines, glibc's ifuncs and zstd's dynamic BMI2 decoders
  keep dispatching on the hardware, with the same results.

## Checks

- win-debug: `crd-core-tests "[cpu]"` (the new child case: sse2 and portable give `crt 1 fma3 0`, sse4.2 `crt 2`,
  avx2 and a refused level keep the CRT's own `5`/FMA3 on); `crd-math-tests "[cpu-level-applied]"` at sse2, sse4.2,
  avx2 and portable. `cpu_features.cpp` compiles under clang-cl (win-clang-cl-shipping); tidy and clang-format clean.
- The ctest guard set passed 16/16 on the second pass. On the first, `crd-diag-coverage` (pure Python, no child
  process) printed its 30 passing cases in 6 s and then did not exit before the 120 s timeout; alone and in the
  rerun it exits in 6 s. Not reproduced; recorded.

## Left

The forced arms' hosted runs with the lowered CRT (a test whose expected value came from the FMA3 libm fails there:
the arm doing its job); the doctor/host tuple/board records item records `cpu_runtime_dispatch()` beside the level.
