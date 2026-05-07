# ADR-0017 — Culling strategy

**Date:** 2026-04
**Status:** Accepted
**Tags:** [culling] [renderer]

## Decision

- Phase 2.4 Renderer v1: brute-force frustum culling against the
  renderable list. Acceptable while scenes are flat and small.
- Phase 3.5: BVH-accelerated frustum culling, implemented as the
  `SpatialBVHIndex` from ADR-0053. Lands alongside the PBR + lighting
  push that brings high light counts and many renderables. The
  `crd-scene` registration grammar already accepts `SpatialBVH{}` from
  Phase 3.0; queries with `.in_aabb()` / `.within_radius()` work against
  the index when it lands.
- Phase 3.8: Hi-Z occlusion culling. GPU-driven, depth-pyramid-based.
  Lands with the rest of GPU-driven rendering (compute cull → indirect
  draw count) in the same phase.
- Per-light culling is part of the clustered Forward+ path itself
  (cluster light lists), shipped in 2.4.

## References

- `docs/phases/phase-2-graphics.md`
- `docs/phases/phase-3-simulation.md`
- `docs/phases/phase-5-ui-rendering.md`
- ADR-0053 — Component index slot framework (defines `SpatialBVHIndex` API; impl Phase 3.5)
- `docs/ROADMAP.md` — authoritative phase numbering
