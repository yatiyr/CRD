# ADR-0105 — Retire crd-rhi + crd-renderer: crd-gpu-context IS the graphics layer

**Status:** Accepted (2026-07-23)
**Phase:** D-007 (RET band)
**Tags:** `[gpu-context]` `[rhi]` `[renderer]` `[architecture]`

---

## Context

The engine carries TWO graphics facades over ONE device:

- **crd-gpu-context** (`IComputeContext`/`IRasterContext`/`ITexture`, Vulkan + DX12) — the D-007 stack. Every
  CKIR program (compute, raster materials, mesh shaders, RT, the B8 gold renderer, the whole visual frontier)
  renders through it. ADR-0103: gpu-context owns every GPU program. D-008 C2: gpu-context owns the VkDevice/
  VkInstance; the rhi Device merely ADOPTS them.
- **crd-rhi + crd-rhi-vulkan + crd-renderer** — the Phase-2.x stack (Device/Queue/Swapchain/CommandBuffer +
  resource loaders/GpuUploader/Phase-2.8 materials). Still owns: present (swapchain/window surface), the ImGui
  backend, the crd-draw overlay pipeline, the ADR-0085 GPU suballocator/defrag, the Morton GPU pipeline
  (geometry-bvh-gpu), the cooked-resource loaders' upload path, and 6 test suites.

The split is a standing double-wiring tax — GEO-3 stage 2b wired sRGB through BOTH stacks in one day and the
cooked linear-space mip chains STILL don't reach the renderer that matters (gpu-context re-derives mips naively
and has no sRGB sampling). Two facades also mean duplicated test surface, duplicated format enums, and two
places for every future capability (bindless, async compute, video, …).

## Decision

1. **crd-gpu-context is THE graphics layer of Cerid.** Everything that touches the GPU goes through it.
2. **crd-rhi, crd-rhi-vulkan, and crd-renderer are FROZEN immediately** — no new features, no new consumers —
   and are RETIRED by the D-007 **RET band**: capabilities absorbed into gpu-context, consumers migrated,
   device-level test coverage PORTED (never dropped), then the three modules and their tests are **DELETED**
   (the legacy-GLSL-Effect precedent: frozen → migrated → gone, no zombie paths).
3. CPU-side cooked-resource types (TextureResource/MeshResource et al.) move to a neutral, GPU-free home
   (crd-resources side) — loaders stay GPU-free (the ADR-0042 posture survives; only its rhi upload half dies).
4. Sequencing: GEO-3 closes first (its texture seam into IRasterContext is the RET down payment), then the RET
   band runs to completion BEFORE GEO-4..7 — the scene↔ECS↔renderer integration (GEO-7) lands once, on the
   clean stack, never twice.

## Consequences

- Supersedes the rhi-side halves of ADR-0036/0042/0080/0085 (loader-upload path, compute-queue surface, GPU
  suballocator home) — their CONTRACTS survive, re-homed in gpu-context; the ADRs gain strike-through notes as
  each RET slice lands.
- gpu-context grows: present path (swapchain/surface/frame loop), ImGui backend, cooked-mip + sRGB texture
  upload, the draw/overlay port, allocator parity (suballocation + defrag + relocation), async-compute command
  routing parity.
- The rhi_vulkan/renderer/rhi test suites' assertions (~5k) are ported to gpu-context suites as part of each
  slice's gate — coverage parity is a DELETION precondition, not an afterthought.
- Consumers to migrate: crd-imgui → perf-ui · crd-draw → geometry-viz/eylem-viz/draw-imgui · crd-shader
  (mostly superseded by ADR-0104 deploy — expected to retire WITH the band) · crd-meshgen · geometry-bvh-gpu
  Morton pipeline · sandbox + runtime smokes · tests/resources loader coverage.

## Alternatives not taken

- **Keep rhi as a thin compatibility shim** — rejected: a shim is a permanent second place to wire everything;
  the tax is the reason for this ADR.
- **Retire incrementally with no deadline band** — rejected: half-retired layers rot ("temporarily frozen"
  becomes permanent); the RET band has an explicit deletion slice as its close gate.
