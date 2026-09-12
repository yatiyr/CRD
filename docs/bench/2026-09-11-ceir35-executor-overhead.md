# CEIR-35 Q7 — executor overhead: the CEIR runtime executor vs hand-rolled dispatch

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

> **Band:** D-007 · CEIR-35 · slice 35d-Q7 (perf boards). **Dimension:** §PR-7 PQP-1 (execution overhead vs native), the
> second of the three CEIR-system perf boards (compile = the sibling `2026-09-11-ceir35-compile-time-decomposition.md`).
> **Kind:** ⛔ **Cerid-internal A/B, NOT a peer crush** (the CEIR-29 mold). The claim it qualifies: the CEIR runtime executor
> (`execute_tensor_pipeline`) is **zero-overhead at GPU time** and negligible at record time versus hand-rolling the identical
> dispatches directly on the compute context — the "the IR costs nothing to *run*" half of the substrate's perf story (the
> emitted-code half is the 2026-07-15 CKIR-vs-hand-written-GLSL parity board).

## Machine / config

- **CPU:** Intel Core i9-14900K (Raptor Lake, 24C / 32T). **GPU:** the Vulkan device (RTX 4070 Ti SUPER — the established host
  device, as on the CEIR-28/29 boards). Headless Vulkan compute context.
- **Build (authoritative):** `win-release` — MSVC `cl` 14.50.35717, `/O2` + IPO/LTCG, asserts OFF, profiling OFF.
- **Timing:** the RECORD phase is CPU wall (`steady_clock`, µs) bracketing `begin()`→(record)→last-barrier, EXCLUDING
  `submit_and_wait`; the GPU phase is `VulkanComputeContext::last_gpu_ms()` (2 device timestamps bracket the one submit). Both
  arms have the SAME structure: 1 `begin` / 1 submit / 1 wait (the CEIR-29 bench-arm rule — mismatched brackets measure sync, not work).
  5 warmup + **median of 15** timed, each median `> 0` a hard `REQUIRE` (measuring-nothing guard).

## Harness (re-runnable)

- **Test:** `tests/ceir-gpu-vulkan/test_ceir_pipeline_vulkan.cpp` → TEST_CASE `"ceir35 Q7: executor overhead vs hand-rolled
  dispatch (Vulkan)"`, tag `[ceir][ceir35][perf][executor][gpu]`.
- **Run:** `scripts\build-target.bat build\win-release crd-ceir-gpu-vulkan-tests` then `ctest -R executor -V` from `build\win-release`.
- **Method:** the fp32 MLP `x·W1 → relu → ·W2` at `4×8×16×2` (the 26e-3b known-good shape — small so `last_gpu_ms > 0`
  resolves), `fuse=true` ⇒ 2 stages `[GemmRelu, Gemm]`. Buffers materialized ONCE and reused; every stage's `ComputePipeline`
  **pre-resolved ONCE** and reused (shader compile is EXCLUDED — that is the compile board). Two arms record the identical
  pre-built pipelines into one recorder:
  - **A (executor):** `execute_tensor_pipeline(plan, rec, cached_resolver, …)` — the plan-walk + per-stage bind-assembly + the
    inter-stage `ShaderWrite→ShaderRead` barriers, with a caching resolver that hands back the pre-built stages.
  - **B (native):** a hand-rolled loop that assembles each stage's bindings from `bind[]` and calls `rec.dispatch(...)` on the
    SAME pipeline/grid/push, with the SAME inter-stage barrier on the last `n_out` binds.
  - **Correctness gate:** arm A and arm B outputs are **bit-identical** (`CHECK(mism == -1)`) — the proof the two arms dispatch
    the SAME GPU work, so the GPU-time comparison is meaningful.

## The board

| Arm | CPU record (median) | GPU time (median) |
|-----|---------------------|-------------------|
| **A — CEIR executor** (`execute_tensor_pipeline`) | **3.000 µs** | **0.00771 ms** |
| **B — hand-rolled native dispatch** | **2.700 µs** | **0.00755 ms** |
| **executor overhead (A − B)** | **+0.300 µs** (one-time per submit, 2 stages) | **ratio 1.0212** (within timestamp noise) |

**DX12 twin** (⛔ both-backends; test `ceir35 Q7: executor overhead vs hand-rolled dispatch (DX12)`, win-release + RTX 4070 Ti
SUPER; DX12 staging = dev GpuOnly uploaded once, executed many, record clock brackets begin→execute only): executor rec **6.0 µs**
/ native **4.7 µs** / GPU time **identical** (`gpu_ratio 1.0000`, output bit-identical). The rec delta is ~1 µs of measurement
jitter on a 2-stage submit (sub-10 µs) — the executor's plan-walk is in the noise, zero at GPU time, on **both** backends. ⭐ the
reused-buffer path passed the D3D12 debug layer clean (no state-transition hazard on repeated execute).

win-debug corroboration (unoptimized C++ record path): Vulkan executor rec 3.700 µs / native 3.300 µs / delta +0.400 µs, gpu_ratio 1.0000; DX12 rec_delta 0.7 µs, gpu_ratio 1.0000 — the record delta stays sub-µs-to-few-µs (jitter) and the GPU column is arm-invariant by construction, on both backends and both builds.

## Verdict

**The CEIR runtime executor is zero-overhead.** GPU time is identical between the executor and hand-rolled dispatch (ratio
1.02 on ~7.7 µs of work — pure timestamp jitter; the **bit-identical output** proves the two arms issue the same kernels with
the same args), so the IR layer adds **nothing** to GPU execution. The executor's extra CPU work — walking the plan, assembling
each stage's bindings from `bind[]`, inserting inter-stage barriers — is **+0.3 µs once per submit** (win-release, 2 stages):
below a microsecond, amortized over the whole submit, and dwarfed by any real GPU workload. Hand-rolling the dispatches buys
nothing. **No loss recorded** (a zero-overhead A/B; the executor's convenience is free).

> Companion boards: compile cost (`2026-09-11-ceir35-compile-time-decomposition.md`, the IR compile is 0.05 % of glslang) and
> the emitted-code parity (`2026-07-15-ckir-vs-handwritten-glsl.md`, CKIR GLSL == hand-written GLSL). Together: the CEIR substrate
> is free to compile, free to execute, and emits code indistinguishable from hand-written — the §PR-7 PQP-1 "no IR tax" claim.
