# ADR-0100 — CKIR is the one GPU compute manager: a *kernel-source-agnostic* dispatch surface serving both compiler-authored and hand-written kernels

- **Status:** Accepted (2026-07-08) — user decision ("all of this is just GPU compute, we should have one GPU compute manager"). **Amends ADR-0098 (CKIR) + ADR-0099 (crd-gpu-context).**
- **Phase:** 3.1.6 v17 (GPU compute) — architecture slice **v17-i** (compute/rendering separation), refinement **i-c→i-f**.
- **Tags:** `kir` `gpu-context` `compute` `architecture` `dispatch` `kernels` `geometry-bvh-gpu` `ray-tracing` `abstraction` `substrate`
- **Depends / amends:** ADR-0098 (CKIR — the kernel compiler) · ADR-0099 (crd-gpu-context — the shared device foundation; and the concrete `VulkanComputeContext`/`VulkanComputeDevice` shortcut this ADR corrects).

## Context

Post-i-c, `geometry-bvh-gpu` (LBVH/morton/radix) runs on a **concrete** `crd::gpu::VulkanComputeContext` — so `lbvh_gpu.cpp` includes Vulkan headers and names Vulkan types. That was an i-c tractability shortcut; ADR-0099 actually specified an *abstract* compute interface. **User objection (2026-07-08):** consumer code (LBVH, and the many GPU-compute techniques coming — ray tracing traversal, path tracing, more) must **never** touch Vulkan or any specific API. The backend is selected under a public API; backends *implement/derive* it. And ultimately these techniques should be *"our own versions for different backends… using the crd-hesap GPU compute power"* — i.e. authored through CKIR.

The load-bearing distinction that shapes the fix: a GPU-compute technique has **two** things coupling it to a backend, and they resolve differently.
1. **The C++ dispatch** — buffers, dispatch, barriers, submit, backend selection, profiling. This is **universal** across every technique.
2. **The kernels** — the actual GPU programs. Two kinds: **(a) CKIR-authored** (elementwise/GEMM/reduce/scan/gather-scatter/tensor — the graph compiler emits them to all six backends); **(b) irregular / hand-tuned** — LBVH's Karras build + atomic upsweep, radix scatter, and **especially ray-tracing traversal / wavefront path tracing**, which have data-dependent control flow and hand-tuned intrinsics that either *cannot* be a clean CKIR dataflow graph or *shouldn't* be (contorting the IR throws away the hand-tuning that makes traversal fast). Today's CKIR IR cannot lower LBVH-class kernels at all (no atomics / irregular control flow).

## Decision

**CKIR is the one GPU compute manager** — it owns the context (device/queues/backend selection, from crd-gpu-context), the **dispatch runtime**, and the **kernel compiler**. The single load-bearing rule:

> **CKIR's dispatch surface is kernel-source-agnostic: it dispatches a *compiled kernel* regardless of origin — CKIR's compiler output OR a hand-written `.spv`/`.ptx`/`.metallib`.**

This is what lets LBVH and ray tracing live under the one manager without pretending they are dataflow graphs. Concretely:

1. **One abstract dispatch interface — `crd::gpu::IComputeContext`** (+ `IComputeBuffer`/`IComputePipeline`/`IComputeRecorder`), backend-agnostic, in `crd-gpu-context`. Buffers (GpuOnly/CpuToGpu/GpuToCpu × storage/transfer), pipelines, a multi-pass copy/barrier/dispatch recorder, submit. **No SPIR-V, no Vulkan, no file formats** in the interface — pipelines are requested by **kernel name** (`create_pipeline(shader_dir, "lbvh_fat_build", n_bindings, push_size)`), and the *backend* resolves the name to its own cooked kernel (Vulkan → `<name>.comp.spv`; CUDA → `<name>.ptx`; …).
2. **Backends implement it** — `VulkanComputeContext : IComputeContext` (CUDA/Metal/DX12/WebGPU/HIP later). This **unifies the two Vulkan compute layers** the i-b/i-c shortcut created (`VulkanComputeDevice` for CKIR + `VulkanComputeContext` for geometry) into ONE source-agnostic dispatch surface. The Vulkan impl keeps a *backend-specific* `create_pipeline_from_spirv(bytes)` for CKIR's runtime-compiled kernels — off the abstract interface, used only by `kir-vulkan` which is inherently Vulkan.
3. **Two on-ramps onto the dispatch:** (a) CKIR graph → compiled kernel (all backends free); (b) hand-written per-backend kernel (irregular stuff). Techniques migrate (b)→(a) as CKIR's IR grows.
4. **Consumers depend on `IComputeContext`, never on a backend.** `geometry-bvh-gpu` (and ray tracing, and future techniques) take `IComputeContext&`, request kernels by name → **zero Vulkan in the consumer library**. The consumer *library* stops linking any backend; only the composition layer (app/test) instantiates a concrete backend and hands it in.

**Honest caveat (recorded, not glossed):** CKIR's *compiler* value-adds — certified bit-exact, autotuning, one-source-to-six-backends — apply to on-ramp (a). A hand-written LBVH/RT kernel gets the manager's dispatch + backend-selection + profiling, but its determinism/portability is *its own* story (LBVH already has it: bit-exact vs the CPU oracle, per-backend kernels). That is fine; it is just not the compiler doing it.

## Consequences

- **+** Consumer code (LBVH now; ray tracing, path tracing, more later) is backend-agnostic — zero API includes; backend chosen above it. One manager for context + dispatch + backend selection. The two-Vulkan-layers smell collapses to one. Irregular/hand-tuned kernels are first-class, not second-class.
- **−** An interface extraction + a re-migration of geometry (morton/radix/LBVH) off the concrete `VulkanComputeContext` onto `IComputeContext` + kernel-by-name; plus later folding `kir-vulkan` onto the unified dispatch. Contained (geometry has no external consumers; blast radius is geometry + kir-vulkan).
- Reaffirms ADR-0098's CKIR-as-substrate + ADR-0099's manager-owned context; corrects the i-c concrete-type shortcut. The endpoint — geometry & RT kernels authored *through* CKIR — is a genuine leg of the v17 journey (CKIR IR must grow atomics + irregular control flow), tracked as a follow-on, not blocked by this ADR.

## Rollout (v17-i)

- **i-c-2 (now):** extract `IComputeContext`; `VulkanComputeContext` implements it + name-based `create_pipeline`; move geometry onto `IComputeContext` (kernels-by-name); geometry lib drops the Vulkan-backend dep. Re-verify geometry 850968 green.
- **i-d (unify):** fold `kir-vulkan` off `VulkanComputeDevice` onto the one `VulkanComputeContext` dispatch (+ `create_pipeline_from_spirv`); delete the duplicate layer. Re-verify kir-vulkan 32928 + tensor 70.5 TF.
- **Journey (post-v17-i):** grow CKIR IR to express LBVH/RT-class kernels → re-author geometry/RT compute as CKIR kernels → all six backends from one source.
