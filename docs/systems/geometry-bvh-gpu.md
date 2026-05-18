# crd-geometry-bvh-gpu

GPU implementation of the LBVH (Karras 2012) build pipeline. **Sibling** module to `crd-geometry-bvh` (CPU-only), mirroring the `crd-rhi` / `crd-rhi-vulkan` split pattern: the existing CPU BVH module stays CPU-only (no Vulkan dependency); GPU kernels + dispatch live here.

> Module path: `engine/geometry-bvh-gpu/`
> Target: `crd-geometry-bvh-gpu`
> Namespace: `crd::geometry::bvh_gpu`
> Opened: Phase 3.1.7 v9a-a (2026-05-18)
> Status: 🚧 ACTIVE — v9a-a ✅ Morton shipped; v9a-b1/b2/c/d/close to come.

## Public surface

| Header | Purpose |
|---|---|
| `crd/geometry/bvh_gpu/morton.hpp`   | CPU `compute_morton_codes_cpu` + `spread_bits_30` / `morton3_30bit_*` / `quantize_to_morton_grid` primitives |
| `crd/geometry/bvh_gpu/dispatch.hpp` | `MortonGpuPipeline` — cached compute pipeline + per-call dispatch |

## v9a-a — 30-bit Morton-code generation

```cpp
namespace crd::geometry::bvh_gpu {

[[nodiscard]] Array<u32>
compute_morton_codes_cpu(ConstSpan<AABB3<f32>> aabbs, IAllocator* alloc);

[[nodiscard]] Array<u32>
compute_morton_codes_cpu(ConstSpan<AABB3<f32>> aabbs,
                          const AABB3<f32>& scene_aabb, IAllocator* alloc);

class MortonGpuPipeline {
  MortonGpuPipeline(rhi::Device&, StringView shader_dir);

  [[nodiscard]] Array<u32>
  dispatch_morton_codes(ConstSpan<AABB3<f32>> aabbs,
                        const AABB3<f32>& scene_aabb,
                        IAllocator* alloc);
};

} // namespace crd::geometry::bvh_gpu
```

### Algorithm

Per AABB centroid, normalise into the scene AABB and quantise each axis to 10 bits ⇒ `[0, 1023]`. Bit-interleave the three 10-bit components ⇒ 30-bit `u32` Morton code with bit pattern `z9 y9 x9 z8 y8 x8 ... z0 y0 x0`. Pure deterministic function of input.

**The CPU implementation IS the algorithm definition.** The GLSL shader (`runtime/examples/shaders/compute_morton_codes.comp`) is a verbatim mechanical translation of the per-element kernel body — same bit pattern, same FP quantisation, same scene-AABB normalisation. Any divergence is a bug. Test: `bit_compare(cpu, gpu)` must be byte-identical for any finite input.

### Scope: 30-bit vs 60-bit (D-132)

30-bit Morton at 1024³ resolution per axis means primitives smaller than `scene_extent / 1024` along any axis collide into the same Morton bin (tiebreak via `original_index` in the sort key). Practical breakpoints:

| Scene extent | Resolution per axis |
|---|---|
| 100 m  | ~10 cm |
| 1 km   | ~1 m   |
| Orbital scale | ~km |

30-bit is the v9a-a lock — game / sim / CAD scenes at ≤ 100 m extent. Eylem-aero (planned) and CAM at km-scale hit this wall; **`v9a-60bit` follow-on slice filed** (u64 60-bit Morton, 20 bits per axis ≈ 1M³ resolution).

### Determinism

CPU + GPU paths are both bit-deterministic given builder rejects NaN/Inf at the API boundary. `gpu_determinism_check(3 rounds)` is part of the v9a-a test corpus — passes on RTX 4070 Ti SUPER (validated 2026-05-18); contract holds for any GPU honouring IEEE arithmetic on a pure-function kernel.

### Performance

Per-dispatch cost is CPU-side staging + GPU kernel + readback. Pure GPU kernel is sub-millisecond on 256k AABBs (RTX 4070); end-to-end wall-clock budget at v9a-a is 200 ms / 256k AABBs (generous, includes staging upload + fence wait + readback). The published `<0.5 ms / 1M prims` GPU-only budget at v9a-close requires separating kernel timing from staging/readback — done at v9a-close with timestamp queries.

### Dispatch lifecycle

`MortonGpuPipeline` caches the heavyweight Vulkan objects (compute pipeline, descriptor set layout, pipeline layout, compiled SPIR-V module, descriptor allocator) across dispatches. Compiling SPIR-V + creating a `VkPipeline` is in the tens-of-milliseconds range; reusing the pipeline across many `dispatch_morton_codes` calls means the per-dispatch overhead is just buffer upload + recording + fence wait.

Sync-compute submission goes through `graphics_queue()` — `create_command_buffer()` allocates from the graphics-family command pool, so submission MUST go to a same-family queue. On GPUs with a dedicated compute family (e.g. RTX 4070), `compute_queue()` is a DIFFERENT queue family and would trigger `VUID-vkQueueSubmit-pCommandBuffers-00074`. True async-compute (compute-family command pool + submit) is the **`v9a-a-async-compute` follow-on** when the RHI surfaces a compute-family command pool.

## Pinned design decisions (carried for ADR-0076 §25 amendment at v9a-close)

- **D132 (v9a-a)** — Module structure: NEW sibling module `crd-geometry-bvh-gpu`, NOT additions to existing `crd-geometry-bvh`. Mirror of `crd-rhi` / `crd-rhi-vulkan` split. Keeps the CPU BVH module CPU-only (no Vulkan dependency); future GPU geometry kernels land here too.
- **D133 (v9a-a)** — 30-bit Morton bit depth as v9a-a default. 60-bit u64 path filed as `v9a-60bit` follow-on for scenes at km-scale.
- **D134 (v9a-a)** — CPU reference IS the algorithm definition. Write CPU sequential first; GPU GLSL is a mechanical translation. Any divergence in GPU output vs CPU output is a bug, asserted by `bit_compare`.
- **D135 (v9a-a)** — `MortonGpuPipeline` value-type, ctor-cached pipeline objects, sync-compute via `graphics_queue()` (compute-family command pool absent from RHI). True async-compute = `v9a-a-async-compute` follow-on.
- **D136 (v9a-a close, 2026-05-18 — REVISED same day)** — v9a-a follow-ons ship IN-LINE before v9a-b1: `-typed` + `-60bit-cpu` + `-async-compute` + `-60bit-gpu`. Original deferral (D136 v1) overturned per user direction: "building substrate, not consumer-specific paths; ship fully now while harness is fresh." Substrate work ≠ speculative consumer-specific work — see refined [[ship-at-consumer-template-from-day-one]] rule. Forward-compat for v9a-b1 sort key width still achieved via `sort_morton_pairs<KeyT>` template, NOW with both `KeyT=u32` AND `KeyT=u64` instantiated and tested from day 1 (not just one).
- **D137 (v9a-a-typed)** — `morton_typed.hpp` strip-compute-retag wrappers shipped at v9a-a in-line. CPU + GPU entries; Morton codes stay raw `u32` (dimensionless bit indices, not lengths). `AABB3T<D, T>` typed AABB type at the API boundary.
- **D138 (v9a-60bit-cpu)** — u64 60-bit Morton CPU oracle shipped at v9a-a in-line. 20 bits per axis (~1M³ resolution). CPU sequential IS the algorithm definition (D134 discipline scales); GPU 60-bit (D140) is mechanical translation. CALIBRATION-FIRST tests + km-scale discriminator (30-bit collides where 60-bit resolves).
- **D139 (v9a-a-async-compute)** — `Device::create_command_buffer_for_queue(Queue&)` virtual appended (D135-compliant). Routes by pointer-identity per D9 contract. Vulkan backend lazy-creates a compute-family `VkCommandPool` when dedicated compute family exists. `MortonGpuPipeline::dispatch_morton_codes_async` opt-in path; output byte-identical to sync path, ValidationCapture stays silent under cross-queue-family submit.
- **D140 (v9a-60bit-gpu)** — `Device::supports_shader_int64()` virtual appended (D135-compliant). `shaderInt64` Vulkan core 1.0 feature probed + enabled at device init when supported. New `MortonGpu60BitPipeline` sibling class graceful-degrades to invalid pipeline if feature unavailable; consumer falls back to 30-bit `MortonGpuPipeline`. Sync-only at v9a (compute-family submit could be added by D139 pattern when a consumer asks).

## Follow-on slices — ✅ ALL 4 PAID 2026-05-18 (same day as v9a-a base)

Per user direction at v9a-a close: "We are building the engine substrate; ship it fully." Decision was to ship all four follow-ons in-line BEFORE v9a-b1, which we did. 5-config DoD PASS in 39 s.

| Follow-on | Status | Summary |
|---|---|---|
| `v9a-a-typed` | ✅ 2026-05-18 | `morton_typed.hpp` strip-compute-retag wrappers around `compute_morton_codes_cpu` + `MortonGpuPipeline::dispatch_morton_codes`. ADR-0078 §5 D34. ~50 LOC + 3 tests / 16 round-trip assertions. |
| `v9a-60bit-cpu` | ✅ 2026-05-18 | CPU oracle u64 60-bit path: `spread_bits_60` + `morton3_60bit_from_ints` + `quantize_to_morton_grid_20bit` + batch driver. CALIBRATION-FIRST lane-pinned tests + km-scale discriminator (30-bit collides at 1m/bin; 60-bit resolves at 1mm/bin). 7 tests / 59 assertions. |
| `v9a-a-async-compute` | ✅ 2026-05-18 | RHI surface: new `Device::create_command_buffer_for_queue(Queue&)` virtual (appended at END per D135). Vulkan backend lazy-creates compute-family `VkCommandPool`. `MortonGpuPipeline::dispatch_morton_codes_async` opt-in path. Cross-queue-family ValidationCapture silent on RTX 4070 dedicated compute family. 1 test / 9 assertions. |
| `v9a-60bit-gpu` | ✅ 2026-05-18 | New GLSL shader `compute_morton_codes_60bit.comp` + RHI `Device::supports_shader_int64()` capability accessor. `MortonGpu60BitPipeline` sibling class (graceful skip when feature unavailable). bit_compare<u64> CPU vs GPU byte-identical. 2 tests / 12 assertions. |

**Combined v9a-a + follow-ons**: 26 tests / 175 assertions across the new module. ValidationCapture silent everywhere. Pinned D137-D140 for ADR-0076 §25 amendment at v9a-close.

## Coming slices

Plan: `docs/phases/phase-3.1.7-geometry.md`. ADR-0076 §25 amendment locks v9a cluster decisions at v9a-close.

| Slice | Status | Summary |
|---|---|---|
| v9a-a  Morton codes      | ✅ 2026-05-18 | 30-bit Morton CPU+GPU, byte-identical conformance. |
| v9a-b1 CPU radix sort    | 📋 planned    | Deterministic `(morton, index)` sort. |
| v9a-b2 GPU radix sort    | 📋 planned    | Blelloch parallel-scan, 4-bit digit × 8 passes. |
| v9a-c  LBVH tree         | 📋 planned    | Karras 2012 binary tree from sorted morton codes. |
| v9a-d  AABB upsweep      | 📋 planned    | Bottom-up parent-AABB propagation; atomic-on-parent. |
| v9a-close                | 📋 planned    | §25 amendment + first-light smoke + 18-config sweep. |

## Bug surfaced

Phase 3.1.7 v9a-a is the FIRST consumer to attach a `ValidationCapture` to a real `VkInstance`. It surfaced a pre-existing rhi-vulkan bug: device unconditionally enables `VK_KHR_swapchain` without the instance enabling `VK_KHR_surface` first (VUID-vkCreateDevice-ppEnabledExtensionNames-01387). When GLFW is initialised, `glfwGetRequiredInstanceExtensions` adds VK_KHR_surface; without GLFW (typical compute-only tests), nothing did. **Fixed in `engine/rhi-vulkan/src/vulkan_backend.cpp` as part of v9a-a** by unconditionally adding `VK_KHR_surface` to enabled instance extensions when available, defensively. Per `feedback_never_defer_solve`: solve, don't defer.
