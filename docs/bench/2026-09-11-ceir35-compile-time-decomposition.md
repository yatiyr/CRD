# CEIR-35 Q7 — compile-time decomposition: the CEIR substrate's own compile cost vs glslang

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

> **Band:** D-007 · CEIR-35 · slice 35d-Q7 (perf boards). **Dimension:** §PR-7 PQP-1 (compile time), the first of the
> three CEIR-system perf boards (this = compile; executor-overhead + plan-reuse are the sibling boards).
> **Kind:** ⛔ **Cerid-internal DECOMPOSITION, NOT a peer crush** (the CEIR-28/29 / REN-1 label). The claim it qualifies:
> the CEIR IR layer's *own* compile cost is a small, **bounded** fraction of the unavoidable external shader compilation —
> not that CEIR beats a peer. glslang is a **named floor**, not a defeated peer.

## Machine / config

- **CPU:** Intel Core i9-14900K (Raptor Lake, 24C / 32T). No pinning; single-threaded measurement (the compile path is serial).
- **Build (authoritative):** `win-release` preset — MSVC `cl` 14.50.35717, `/O2` + IPO/LTCG (`CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`),
  `CRD_ENABLE_ASSERTS=OFF`, `CRD_ENABLE_PROFILING=OFF`. This is the honest perf preset (a debug number is a *ceiling*, per this
  directory's README — the win-debug corroboration row below shows why).
- **External peer / floor:** glslang via `crd::gpu::compile_glsl_to_spirv` (the shaderc/glslang bundled with the
  `crd-gpu-context-vulkan` build; version not separately queried — it is a floor, not a peer, so N/A-with-check).
- **Device:** none. This board is **device-free** — `plan_tensor_pipeline` is the §158 device-free compile half, `synth_gemm` +
  `emit_contract_glsl` are host codegen, and `compile_glsl_to_spirv` is a host shaderc call. It runs on any host (CI Linux incl.).

## Harness (re-runnable)

- **Test:** `tests/ceir-gpu-vulkan/test_ceir_pipeline_vulkan.cpp` → TEST_CASE `"ceir35 Q7: compile-time decomposition -- CEIR
  lowering+codegen vs glslang (device-free)"`, tag `[ceir][ceir35][perf][compile]`.
- **Run:** `scripts\build-target.bat build\win-release crd-ceir-gpu-vulkan-tests` then
  `ctest -R ceir35 -V` from `build\win-release` (verbose to surface the `[CEIR35-Q7-COMPILE]` row; `run-ctest.bat` uses
  `--output-on-failure`, which hides a passing test's stdout).
- **Method:** the fp32 2-layer MLP payload `x·W1 → relu → ·W2` at shape `32×64×128×64`, expanded + planned with `fuse=true`
  (⇒ 2 stages `[GemmRelu, Gemm]`, so the codegen path is pure `synth+emit` with NO `.ckir` file I/O to pollute the emit clock).
  3 warmup + **median of 15** timed sweeps per phase (odd K ⇒ a real sample). Three phases timed separately:
  - **L (CEIR lowering):** `plan_tensor_pipeline` over the expanded module (device-free IR planning).
  - **E (CEIR codegen):** `synth_gemm` + `emit_contract_glsl` per GEMM stage → GLSL source (device-free).
  - **C (external):** `compile_glsl_to_spirv` (glslang) per stage; emit is re-done UNTIMED inside the loop, only the glslang call is clocked.
  - Each median `> 0` is a hard `REQUIRE` (a 0 median ⇒ the clock resolved nothing ⇒ measuring NOTHING — the measurer-measures-nothing guard).

## The board

| Phase | What it times | win-release (authoritative) | win-debug (ceiling corroboration) |
|-------|---------------|-----------------------------|-----------------------------------|
| **L** — CEIR lowering | `plan_tensor_pipeline` (device-free IR plan) | **0.0016 ms** | 0.0135 ms |
| **E** — CEIR codegen | `synth_gemm`+`emit_contract_glsl` ×2 stages | **0.0007 ms** | 0.0060 ms |
| **CEIR-owned (L+E)** | the whole CEIR IR compile pipeline | **0.0023 ms** (2.3 µs) | 0.0195 ms |
| **C** — external glslang | `compile_glsl_to_spirv` ×2 stages | **4.3743 ms** | 4.4994 ms |
| **owned / glslang** | the CEIR overhead FRACTION | **0.0005** (0.05 %) | 0.0043 (0.43 %) |

**DX12 twin** (⛔ both-backends; test `ceir35 Q7: compile-time decomposition -- CEIR lowering+codegen vs DXC+PSO (DX12)`,
`[ceir][ceir35][perf][compile][gpu]`, win-release + RTX 4070 Ti SUPER). Device-gated: D3D12 exposes no device-free HLSL→DXIL
split, so the external column is `create_pipeline_from_hlsl` = **DXC + the D3D12 PSO** bundled. CEIR-owned (lower + `emit_contract_hlsl`)
= **0.0021 ms** vs **DXC+PSO 6.5538 ms** ⇒ owned/ext = **0.0003** (0.03 %). DXC+PSO is *slower* than glslang (a bigger floor), so
the CEIR fraction is even smaller than on Vulkan — the IR compile cost is a rounding error on **both** backends.

## Verdict

**The CEIR substrate's own compile cost is a rounding error next to the shader compilation it feeds.** On the authoritative
win-release build the entire CEIR IR compile pipeline (lower + codegen) is **2.3 µs**, **0.05 %** of the glslang compile
(4.37 ms) — glslang is **~1,900× the cost** of everything CEIR does. The compile-time budget is therefore dominated by, and
bounded by, the unavoidable external shader compiler; the IR layer adds no meaningful compile overhead.

The **debug row is the honesty control**: the CEIR-owned column is our own C++ (debug = unoptimized) and drops 0.0195 → 0.0023 ms
(~8.5×) from debug to release, while glslang — a prebuilt optimized library — is config-invariant (4.50 → 4.37 ms). So the
win-debug number is a strict *ceiling* for the CEIR column, exactly as this directory's README requires; release only widens
the gap. **No loss recorded** (this is a decomposition, and the CEIR fraction is far below the glslang floor under every build).

> Scope note: this isolates the **per-stage-kind** compile decomposition (GLSL for a GEMM is push-constant-driven ⇒ shape-generic,
> so the compile cost is per-stage-kind, not per-tensor-size). Graph-*size* (stage-count) scaling of compile+plan on ~8k-op graphs
> is the CEIR-35 large-graph slice (35d-Q6, already closed) — this board is the novel Q7 compile-decomposition contribution.
