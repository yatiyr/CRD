# ADR-0009 — RHI v1a scaffold

**Date:** 2026-04
**Status:** Accepted
**Tags:** [rhi] [arch]

## Decision

- `crd-rhi` ships v1a as a backend-neutral interface scaffold validated
  against a fake backend before any Vulkan code is written.
- Public surface: `Instance`, `Device`, `Queue`, `Swapchain`, `Buffer`,
  `Image`, `CommandBuffer`, `ShaderModule`, `Pipeline`.
- No materials, scene structure, ECS, lighting, or render-graph policy at
  this layer.

## References

- `docs/phases/phase-2-graphics.md`
