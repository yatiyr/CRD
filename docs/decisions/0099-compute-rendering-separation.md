# ADR-0099 — `crd-gpu-context`: a backend-agnostic GPU **context manager**; compute and rendering as independent, composable consumers

- **Status:** Accepted (2026-07-08) — user decision. (Supersedes this ADR's first Proposed draft, which mis-argued "share the device"; the audit below overturned it.) **Refines ADR-0080.** **§6 SUPERSEDED by ADR-0103 (2026-07-10)** — see the strike-through in *Decision*.
- **Phase:** 3.1.6 v17 (GPU compute) — foundational architecture slice **v17-i**, laid **before** resuming GPU-compute feature work (tensor fan-out, fusion, …).
- **Tags:** `gpu-context` `rhi` `compute` `rendering` `architecture` `vulkan` `cuda` `metal` `dx12` `webgpu` `hip` `kir` `geometry-bvh-gpu` `separation` `headless` `context-manager` `substrate`
- **Depends / amends:** ADR-0080 (compute surface *in* the RHI — refined: compute becomes its own interface *over a context*) · ADR-0098 (CKIR — its six backends become context *consumers*).

## Context

v17-h shipped the coopmat2 tensor tier through `kir-vulkan` at **70.9 TF = CUDA-`wmma2` parity**. Auditing GPU-context creation across the engine (2026-07-08) revealed fragmentation:

- **All six CKIR backends self-create their context independently:** `kir-cuda` (`cuInit`), `kir-metal` (`MTLCreateSystemDefaultDevice`), `kir-dx12` (`D3D12CreateDevice`), `kir-webgpu` (adapter request), `kir-hip`, and `kir-vulkan` — the **only** one coupled to the *rendering* module (`crd::rhi::create_vulkan_instance` → a rendering-flavored device: forced `VK_KHR_swapchain` + graphics queue).
- **`geometry-bvh-gpu`** (real GPU compute — LBVH: Morton + radix sort, hand-written `.comp`) takes a **rendering `crd::rhi::Device&`**.
- **No deeper context module exists.** A renderer and `kir-vulkan` each create a **separate `VkDevice`** today — so the "shared-device interop" defended in the first draft *isn't even happening*: we already pay for separate devices with none of the interop, plus duplication.

**User decision (2026-07-08):** a deep module **owns and manages** the GPU contexts (configurable, like engine options — swappable at runtime). Compute and rendering are **independent consumers** that draw contexts from the manager. A context can be **shared** (a Vulkan renderer + Vulkan compute on one device → zero-copy handoff) or **separate**; some are **headless** (CUDA/HIP have no rendering). The share-or-separate choice lives in the **composition layer**, not baked into the compiler or the renderer. Concerns fully separated; usable together by choice.

## Decision

1. **`crd-gpu-context` — the GPU context manager (deep foundational module).** Owns + manages live GPU contexts. Backend-agnostic **`IGpuContext`** = a live GPU foundation (instance/device/queues/allocator), with **no** compute or rendering semantics. **Per-backend lean impls** (`gpu-context-vulkan`, `-cuda`, `-metal`, `-dx12`, `-webgpu`, `-hip`) so a Vulkan-only build never pulls CUDA/Metal headers (lean-consumer, matching CKIR's `kir-*` split). A **`GpuContextManager`** holds the configured set — which backends, headless vs windowed, shared vs separate — reconfigurable at runtime (engine options).
2. **Compute and rendering are independent interfaces *over* a context.** `ComputeDevice` (compute pipeline / storage buffer / dispatch / compute-queue) and `RenderDevice` (swapchain / graphics pipeline / render pass) each wrap an `IGpuContext`; neither depends on the other. The manager may hand the **same** context to both (shared device) or **separate** contexts — by config.
3. **Consumers take contexts from the manager.** CKIR backends take a `ComputeDevice` (`kir-vulkan` **stops calling `create_vulkan_instance`** → uniform with its five native siblings). `geometry-bvh-gpu` takes a `ComputeDevice`. The renderer takes a `RenderDevice`. CUDA/HIP contexts are headless.
4. **Pipeline cache at the compute layer** — compile a kernel once (key = hash of shader source), reuse across dispatches; removes recompile-per-`run()` waste **and the reason the bench-timing exists**.
5. **No measurement in the production backend** — remove `bench_contract`/`bench_tensor` `reps`/`out_ms`; perf timing moves to a test/tooling harness over the cached `run()` path.
6. ~~**Shaders — one compute path, two authoring modes.** `crd-shader` stays the single shared GLSL/HLSL→SPIR-V/DXIL compiler for **both** *authored* compute shaders (hand-written `.comp`, hand-tuned — geometry's LBVH, living with their module) and *generated* ones (CKIR emits from the graph). Both compile via `crd-shader` and dispatch via `ComputeDevice`.~~
   > **⛔ SUPERSEDED by [ADR-0103](0103-gpu-context-owns-every-gpu-program.md) (2026-07-10).** This clause made a portable mid-level
   > module the owner of two backend languages and forced `crd-kir-vulkan` to depend on it — directly contradicting
   > **ADR-0101** ("backend languages are outputs only; never authored or stored"). Two Accepted ADRs disagreed, and a D-007
   > plan followed this one. **The rule now:** no module outside a backend names a shading language or a bytecode;
   > `crd-gpu-context` owns every GPU program via `create_program(KGraph, KEntry) → IGpuProgram`, and each backend owns its
   > language + compiler privately. `crd-shader` keeps Effect/reflection/runtime and loses `compile.hpp`.

## Migration plan (v17-i — incremental; each step ships + validates independently; do this BEFORE more GPU-compute features)

- **i-a** — `crd-gpu-context`: `IGpuContext` + `GpuContextManager` + the Vulkan context (headless-aware creation; coopmat2 features guarded). No consumer changes yet.
- **i-b** — `ComputeDevice` interface; `kir-vulkan` → a `ComputeDevice` from a manager context (drop `create_vulkan_instance`) + headless/compute-queue + **pipeline cache** + **remove bench timing**. Re-verify **70.9 TF** + full `kir-vulkan` suite green.
- **i-c** — `RenderDevice` over a context; rendering consumers migrate; **`geometry-bvh-gpu` → `ComputeDevice`**. Re-verify its GPU tests (LBVH correct + ValidationCapture-0).
- **i-d** — the other CKIR backends (cuda/metal/dx12/webgpu/hip) draw contexts from the manager registry (uniform ownership). Re-verify all `kir-*` suites.
- **i-e** — shader-org note: confirm both authoring modes route `crd-shader → ComputeDevice`; document the authored-vs-generated home.

## Consequences

- **+** Concerns fully independent (compute can't see rendering; renderer can't see compute internals); composable via the manager; **one** place owns + configures contexts; `kir-vulkan` uniform with its five siblings; interop available **by choice**, never forced or forbidden; pipeline cache benefits every op; bench scaffolding gone.
- **−** A foundational refactor touching an RHI interface split, a new module, and 2+ consumers — contained by the incremental plan (each step independently green). Deliberately laid **first** so all further GPU-compute work builds on the clean foundation.
- Keeps ADR-0080's "compute is not its own device" spirit but corrects the coupling: compute + rendering are **independent consumers of a manager-owned context**, and the device-sharing decision moves to the composition layer.
