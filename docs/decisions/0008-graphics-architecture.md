# ADR-0008 — Graphics architecture

**Date:** 2026-04
**Status:** Accepted
**Tags:** [rhi] [vulkan] [arch]

## Decision

- `crd-rhi` is the explicit low-level graphics module. Owns minimal GPU
  abstraction only.
- `crd-rhi-vulkan` is a separate backend. Vulkan types do not leak.
- High-level rendering moves out into `crd-renderer` (+ `crd-resources`).
- Vertical slice first. First triangle is a hard milestone gate.
- ImGui is not part of RHI. Integrates after the triangle as a debug layer.
- Pragmatism over forced modernity: sync2 detected but classic
  submit/barrier flow used; revisited when there's a measured reason.

## References

- `docs/phases/phase-2-graphics.md`
- `docs/systems/rhi.md`
