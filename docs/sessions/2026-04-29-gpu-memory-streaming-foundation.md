# Session — 2026-04-29 — GPU memory + streaming foundation

## Goal

Reduce the next graphics risk after the ImGui overlay milestone: stabilize the
GPU resource allocation path before shader/renderer growth widens the backend.

## What we built / changed

- **Internal Vulkan allocator layer** (`VulkanAllocator`) inside the backend
  - centralized memory-type selection
  - centralized buffer allocation path
  - centralized image allocation path
  - explicit destruction helper for owned allocations
- **`VulkanBuffer`** now owns a `VulkanAllocation` instead of loose
  `VkDeviceMemory`
- **`VulkanImage`** can now own real image memory (not just swapchain image
  views)
- **`create_image()` implemented** for backend-owned Vulkan images
  - image creation
  - memory allocation/bind
  - image-view creation
  - color and depth/stencil aspect selection
- **Tests/smoke expanded**
  - device-local buffer creation path
  - allocator-backed image creation path
  - triangle smoke now exercises allocator-backed resources without validation
    noise

## Plain-English explanation

Before this session, Cerid could create the triangle resources it needed, but
the GPU memory path was still scattered and too bootstrap-like. After this
session, Vulkan resource creation has a real backend-owned allocation layer.

This is still not the final allocator architecture. There is no suballocation,
no streaming arena, no deferred eviction strategy yet. But the ownership and
allocation decisions are now centralized enough that the shader system and the
renderer can grow without every new resource type re-inventing Vulkan memory
plumbing.

## Decisions made

- This slice stays **Cerid-owned**, not VMA-based. The roadmap already points
  in that direction, and this implementation keeps the engine on that path.
- The allocator remains **simple and explicit** for now. No premature TLSF /
  streaming machinery before the renderer and shader system create real
  pressure.
- The lifetime discipline for this slice stays RAII-first. More advanced
  deferred destruction / residency policy can come once resource uploads and
  streaming paths are more demanding.

## Files touched

- `engine/rhi-vulkan/src/vulkan_backend.cpp` — centralized allocator logic,
  real image creation path, resource ownership cleanup
- `tests/rhi_vulkan/test_rhi_vulkan.cpp` — added allocator-backed buffer/image
  coverage
- `runtime/examples/smoke_rhi_vulkan_bootstrap.cpp` — exercises allocator-backed
  image creation in the smoke path
- `docs/systems/rhi.md` — allocator slice documented
- `docs/phases/phase-2-graphics.md` — 2.2 marked shipped
- `docs/ROADMAP.md` — current graphics status updated
- `context.md` — current focus / last shipped / next up updated

## Tests / verification

- `win-debug`: 210/210
- `win-release`: 209/209 (Debug-only stats test correctly skipped)
- `win-asan`: 210/210, no leaks, no UAF, no OOB
- `smoke_rhi_vulkan_bootstrap` still runs cleanly with allocator-backed image
  creation and no validation-layer errors

## Next session starts with

`crd-shader` / descriptor growth (2.3). The backend allocation path is now
solid enough to support the next real widening step.
