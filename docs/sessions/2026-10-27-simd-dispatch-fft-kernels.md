# 2026-10-27 — SIMD-DISPATCH: the hesap-fft kernels

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- Complete run 40515770381 (`31e2b9ce`, still running) failed win-release, win-shipping and win-shipping-profile on
  an MSVC LTCG crash linking `crd-hesap-direct-tests`; fixed in `14840123`
  ([direct note](2026-10-27-simd-dispatch-direct-kernels.md#ci-fix-msvc-ltcg-crash)). Every other completed run was
  cancelled with no failed job.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI.
  40515770381 predates `7eec1bf7` and fails three Windows lanes, so it serves neither; the next complete run at or
  after `14840123` is the candidate.

## Slice A: experiment gates

The 23 `CRD_FFT_*` experiment macros no build defines (the `DISABLE_*` escape hatches, the opt-in IP4, FS5, R4,
IP4_PAD and P1_NT paths, the M16-B/M17/M18/M19 POCs and the M16-B/M18 ablations) are resolved to their default
branches in `fft.hpp` and `hier_codelets.hpp` (a partial preprocessor; 10,963 lines go). Preprocessed `fft.hpp`
(MSVC `/EP`, assert file and line normalized) is token-identical to HEAD under avx2 release and debug, scalar and
`CRD_FFT_PROFILE`, apart from the hand deletions that followed: the FS5 members and their release, the empty
`m_abuf`/`m_bbuf`, the dead `m16b_*` locals and three f64 stage-1 codelets no path calls (64x16, 16x64, 32x64).
Four more unreferenced codelets (`codelet{32,64,128,256}_stage1_fused_sh`) are in the generated batched header and
go with its emitter edit. The plan's state no longer depends on a macro. `detail/aos_codelets.hpp` (raw AVX2,
included by nothing) is deleted with its manifest entry; its generator stays as the research tool
[fft-mkl-crush](../research/fft-mkl-crush.md) names. `CRD_FFT_PROFILE` and `prof::` stay until the kernel unit
decides where its counters live.

The M18 fused chain (f32 1M forward, 0.85x MKL against the gather fusion's 0.69x on the 2026-06 harness, never made
default; [design §18](../design/hesap_fft_generated_codelets.md)) went with the gates. The row names it as the
f32 1M lever for the row-close board.
