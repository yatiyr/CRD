# ADR-0017 — Culling strategy

**Date:** 2026-04
**Status:** Accepted
**Tags:** [culling] [renderer]

## Decision

- Phase 2.4 Renderer v1: brute-force frustum culling against the
  renderable list. Acceptable while scenes are flat and small.
- Phase 3.6: BVH-accelerated frustum culling (dynamic AABB tree). Lands
  once the scene module exists and we have non-trivial scenes.
- Phase 5.2a: Hi-Z occlusion culling. GPU-driven, depth-pyramid-based.
  Requires the GPU-driven rendering plumbing in 5.2b.
- Per-light culling is part of the clustered Forward+ path itself
  (cluster light lists), shipped in 2.4.

## References

- `docs/phases/phase-2-graphics.md`
- `docs/phases/phase-3-simulation.md`
- `docs/phases/phase-5-ui-rendering.md`
