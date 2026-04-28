# ADR-0011 — First triangle milestone

**Date:** 2026-04
**Status:** Accepted
**Tags:** [vulkan] [rhi] [renderer]

## Decision

- Triangle path: Instance → device → swapchain → command buffer → shader
  modules → graphics pipeline → vertex buffer → draw → present.
- Dynamic rendering chosen as the minimal rendering path (no early
  heavyweight render-pass architecture).
- Triangle path stays narrow: no descriptors, materials, scene graph,
  camera system, or allocator policy creep.
- Shader compilation joins the build graph (glslangValidator).
- Validation matrix conservative: Debug + ASan run the real triangle
  integration test; Release keeps it lightweight to avoid driver variance
  becoming noise.

## References

- `docs/phases/phase-2-graphics.md`
- `docs/sessions/2026-04-28-rhi-vulkan-first-triangle.md`
