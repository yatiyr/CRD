# CEIR-35 Q7 — plan + pipeline reuse (amortization): the "plan-cache hit" dimension

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

> **Band:** D-007 · CEIR-35 · slice 35d-Q7 (perf boards). **Dimension:** §PR-7 PQP-1 (plan-cache hit), the third of the three
> CEIR-system perf boards (siblings: `2026-09-11-ceir35-compile-time-decomposition.md`, `2026-09-11-ceir35-executor-overhead.md`).
> **Kind:** ⛔ **Cerid-internal A/B, NOT a peer crush.** The claim it qualifies: reusing a lowered plan + its built pipelines
> (the §158 lower-once / compile-once / execute-many pattern) makes repeated execution **orders of magnitude** cheaper than a
> cold re-lower + recompile — i.e., the substrate's reuse path pays the compile once and never again.

## ⛔ What is actually being benched (and why not `PlanCache`)

The CEIR-10b `PlanCache` (`engine/ceir-cook/.../plan_cache.hpp`) is a content-addressed **store** that, by its own header, "does
NOT produce plans" and has **no callers in the live execute path** (verified: zero `PlanCache::get`/`put` outside its own TU).
Benching it in isolation would measure a container, not repeated-execute. The **real** reuse mechanism is the §158 split:
`plan_tensor_pipeline` (lower, device-free) is called once, its stages are resolved to `ComputePipeline`s once, and
`execute_tensor_pipeline` then runs that plan many times. This board benches that.

## Machine / config

- **CPU:** Intel Core i9-14900K (Raptor Lake, 24C / 32T). **GPU:** the Vulkan device (RTX 4070 Ti SUPER). Headless.
- **Build (authoritative):** `win-release` — MSVC `cl` 14.50.35717, `/O2` + IPO/LTCG, asserts OFF, profiling OFF.
- **Timing:** CPU wall (`steady_clock`, ms) bracketing the whole run — `begin` → record → `submit_and_wait`. Matched 1-submit /
  1-wait per arm. 3 warmup + **median of 15** timed; each median `> 0` a hard `REQUIRE`.

## Harness (re-runnable)

- **Test:** `tests/ceir-gpu-vulkan/test_ceir_pipeline_vulkan.cpp` → TEST_CASE `"ceir35 Q7: plan+pipeline reuse amortization
  (Vulkan)"`, tag `[ceir][ceir35][perf][reuse][gpu]`.
- **Run:** `scripts\build-target.bat build\win-release crd-ceir-gpu-vulkan-tests` then `ctest -R reuse -V` from `build\win-release`.
- **Method:** the fp32 MLP `4×8×16×2`, `fuse=true` ⇒ 2 stages. Buffers materialized once.
  - **COLD** (a first-run each iteration): re-`plan_tensor_pipeline` (re-lower) + a **fresh `Resolver`** (so every stage's
    `synth → emit GLSL → glslang → ComputePipeline` is recompiled) + `execute_tensor_pipeline` + `submit_and_wait`.
  - **WARM** (every subsequent run): reuse the lowered plan + the pre-built pipelines (a caching resolver hands them back) +
    `execute_tensor_pipeline` + `submit_and_wait` — pays only record + GPU + fence.

## The board

| Arm | Cost per run (median) | What it pays |
|-----|-----------------------|--------------|
| **COLD** — first run of a program | **4.8309 ms** | lower + **recompile both stages' shaders/PSOs** (~4.4 ms, the glslang floor from the compile board) + record + GPU |
| **WARM** — reuse plan + pipelines | **0.0555 ms** (55.5 µs) | record + GPU + submit/fence latency only (the CEIR work is the ~3 µs from the executor board; the rest is submit/fence) |
| **speedup (cold / warm)** | **87.0×** | = the shader compile the cold path repeats and the warm path skips |

**DX12 twin** (⛔ both-backends; test `ceir35 Q7: plan+pipeline reuse amortization (DX12)`, win-release + RTX 4070 Ti SUPER):
**cold 7.7582 ms vs warm 0.0877 ms ⇒ 88.5×**. The cold cost is even larger than Vulkan's (DXC+PSO ~6.55 ms > glslang ~4.37 ms —
see the compile board's DX12 row), so the amortization win is bigger on D3D12; the warm hot path (~88 µs) is again record+GPU+fence.

win-debug corroboration: Vulkan cold 4.7221 ms / warm 0.0598 ms / 79.0×; DX12 cold 8.23 ms / warm 0.0916 ms / 89.8× — the shape is stable across builds (the shader compiler the cold path repeats is config-invariant).

## Verdict

**Reuse is worth ~87× per run.** A cold first-run of a CEIR program costs 4.83 ms — almost entirely the unavoidable glslang
shader compilation (the compile board shows glslang is ~4.4 ms while the CEIR lower+codegen is 2.3 µs). Every subsequent run,
reusing the lowered plan and the built pipelines, costs **55 µs** — record + GPU + fence, with the CEIR executor's share being
the ~3 µs the executor-overhead board measured. So the substrate's reuse path amortizes the one unavoidable cost (shader
compilation) to zero on the hot path, and what remains is the ~microsecond executor + the GPU work itself. **No loss recorded.**

> This is the honest "plan-cache hit" board: the win is real and large, but it lives in the §158 lower/execute split + pipeline
> reuse, NOT in the dead CEIR-10b `PlanCache` container. A persistent on-disk plan/PSO cache (CEIR-11/13, to skip the cold
> compile across process launches) is a named forward — this board is the in-process reuse that exists today.
