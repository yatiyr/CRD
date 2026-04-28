# ADR-0016 — Render path strategy

**Date:** 2026-04
**Status:** Accepted
**Tags:** [renderer] [render-path] [arch]

## Decision

- Renderer v1 (Phase 2.4) ships **Clustered Forward+** as the first
  concrete render path. Reasons over deferred-as-default: native
  transparency, MSAA friendliness, material flexibility (no G-buffer
  format lock-in), works on bandwidth-constrained targets (mobile, VR).
- The renderer exposes an `IRenderPath` interface from day one, even with
  only one implementation. Future paths plug in as additional
  implementations:
  - **Deferred** path (Phase 5.3a) — heavy lighting, unrelated to
    Forward+.
  - **Visibility Buffer** path (Phase 5.3b) — high geometry density / mesh
    cluster pipelines.
  - **Render path selector** (Phase 5.3c) — per-scene profile chooses;
    user override available.
- Material system is render-path-agnostic. Material parameters live in
  named constant buffers + sampled bindings; the path adapts at draw time.
- No early Nanite-style commitment. Phase 5.2 lays the groundwork
  (GPU-driven, occlusion, mesh shaders) and a future decision picks how
  far to take it based on real workloads.

## References

- `docs/phases/phase-2-graphics.md`
- `docs/phases/phase-5-ui-rendering.md`
