# ADR-0010 — Vulkan bootstrap

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Date:** 2026-04
**Status:** Accepted
**Tags:** [vulkan] [rhi]

## Decision

- Vulkan instance / physical device selection / logical device + queues /
  surface from `Window::native_handle()` / swapchain (with resize).
- No `Vk*` types in `crd-rhi` public headers.
- Release-safe: failure paths return `nullptr` / empty results, not
  debug-only asserts.

## References

- `docs/phases/phase-2-graphics.md`
