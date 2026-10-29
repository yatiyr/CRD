# 2026-10-29 — SIMD-DISPATCH: tests and sandbox on the environment reader, and its guard

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); finishes the Left of
[the environment readers note](2026-10-29-simd-dispatch-environment-readers.md).

## CI first

Every completed run in the last 15 is a pending push run the concurrency group replaced before any job started (no
jobs). Push run 40784988994 (92db578d) still runs win-asan and linux-gcc-asan; its finished failures are the ones
earlier batches traced to fixes at HEAD. No complete-tier run is green, so DIAG.11c and DIAG.12 stay Needs CI.

win-asan runs its suite serially under the default 360-minute job cap. Its last green job (run 40748658864) took
4 h 58 min, 16,639 s of it in the Test step; the five `crd-hesap-fft-kernels-at-*` arms took 3,854 s of that
(about 770 s each).

## What landed

- `crd::platform::headless_requested()` (crd/platform/context.hpp) is the one reading of `CRD_PLATFORM_HEADLESS`.
  `PlatformContext::create()` uses it, and so do the eleven app, platform, BVH-GPU and shader-helper test files that
  had their own `std::getenv` copy of it.
- Tests and the sandbox read through `crd/core/environment.hpp`: asset roots, corpus and fixture directories, dump
  hooks and `CRD_CERIDC_EXE` through `read_environment_text` (any length, so a long path no longer reads as unset),
  the CUDA bench gate through `environment_set`, the DX12 fault child and the specimen runner through
  `read_environment` (a too-long `CRD_CPU_LEVEL` still reads as `?`). The perfetto test's `_dupenv_s` copy and its
  `free` are gone; the RAF-11 reload gate takes `temp_directory()` instead of `TEMP`, which Linux does not set.
- The `_CRT_SECURE_NO_WARNINGS` definitions that existed only for `getenv` are gone (app, platform, BVH-GPU, shader
  helpers, draw, scene-render, anim, ceridc and CUDA tests), and so are the sandbox's C4996 pragmas. asset-io and
  audio keep theirs for `std::fopen`.
- `crd-check-environment-reads` (scripts/check-environment-reads.py) reads every C-family source under the
  source-routes roots, comments and string literals aside, and fails a `getenv`, `getenv_s`, `_dupenv_s`,
  `secure_getenv`, `GetEnvironmentVariable`, `GetEnvironmentStrings` or `environ` read outside
  `engine/foundation/core/src/environment.cpp`, by line; a reader with no read fails as unchecked. Three
  `test-repository-tools.py EnvironmentReads` cases hold it.

## Checks

win-debug: the eighteen touched targets build at /W4 /WX (crd-platform and its tests, app, perf, diag harness,
hesap-tensor and nn, asset-io, audio, anim, ceridc, BVH-GPU, shader helpers, draw, DX12, scene-render, CUDA, the
gizmo probe and the sandbox). The guard passes on the tree and its fixture cases pass. No suites, following the
fast-row order.
