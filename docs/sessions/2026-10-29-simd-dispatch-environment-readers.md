# 2026-10-29 — SIMD-DISPATCH: one environment reader, and the guard on tsan

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `43eac5d2`. Follows
[the exact fma](2026-10-29-simd-dispatch-exact-fma-and-guard-route.md) and finishes the environment item that
[the direct peer note](2026-10-29-simd-dispatch-direct-eigen-peer.md) left for the next batch.

## CI first

Push run 40784988994 (`92db578d`) still runs win-asan and linux-gcc-asan; nothing was cancelled. Its jobs finished
since the last read:

- win-release: the `crd-math-`, `crd-hesap-direct-` and `crd-hesap-tensor-kernels-at-{portable,sse2,sse42}` arms
  (signed zeros and double roundings of the UCRT's software fma) and the guard's 1,800 s timeout, both fixed at
  `dd3d54c4` and `e17984e3`.
- win-tidy: green.
- linux-clang-tsan: only `crd-check-isa-isolation` failed, its first hosted object-route run (133 kernel and 1,835
  general objects, 233.9 s beside the tsan tests at `ctest -j4`). The dense and fft kernel objects define
  `DW.ref.__gcc_personality_v0`: TSan's function-exit cleanups give a function without a personality the C one.
  Fixed in `6161c3bd`: the reviewed data entry covers the C and C++ references, with a fixture assertion.

No completed run carries hosted evidence for DIAG.11c or DIAG.12; both stay Needs CI.

## What changed

- `crd/core/environment.hpp` adds `read_environment_integer` (a whole decimal integer in a range, through
  `std::from_chars`) and `read_environment_text` (any length into a growable string).
- Every engine and bench reader goes through it; the wrappers are deleted with their `#pragma warning(disable :
  4996)` and NOLINTs: jobs' `policy_getenv`, hesap-direct's `mf_getenv` and the `CRD_CDIV_BLOCK` profile knob,
  perf's `env_present` and the doctor's `read_env`, platform's headless flag and `user_config_dir`, the Vulkan
  GLSL and HLSL compilers' `VULKAN_SDK`, the renderer's `renderer_getenv`, and `bench_nn`, `bench_sparse_tensor`,
  `bench_hesap_cholesky_vs_cholmod` and `bench_hesap_qr_vs_spqr`. An integer knob now needs the whole value to be
  a number in its range (`atoi` read `12x` as 12); an empty value reads as unset on every platform, as before on
  Windows; `CRD_PLATFORM_HEADLESS` is read into 32 bytes, so a value of 32 characters or more no longer counts.
- `crd-check-isa-isolation` runs alone (`RUN_SERIAL`): its pool takes every core, so beside other tests it starves
  them and its own time is not the one its timeout needs.

## Checks

- win-debug: crd-core, jobs, perf, platform, gpu-context-vulkan, hesap-direct, hesap-eigen, hesap-ode, scene-render
  and their tests build; `[cpu]` (with a new case for the integer and text readers), perf `[budget]`, the doctor
  cases and the platform config case pass.
- tidy-files.py clean on the 14 analysable files (a static constant renamed `kWidth`); the four benches are not in
  win-debug's database (retained benches off; CHOLMOD and SPQR local) and are compiled by linux-gcc-debug or at the
  board pass. clang-format adds no finding.

## Left

The environment readers of tests and sandbox (`CRD_PLATFORM_HEADLESS` helpers in 15 test files, the dump and corpus
knobs, the `_CRT_SECURE_NO_WARNINGS` definitions in their CMake files) and a guard that rejects `std::getenv` and
`_dupenv_s` outside `environment.cpp`; then the iterative Eigen benches.
