# ADR-0044 — Phase ordering: material PSO/variant completion precedes scene/ECS

**Status:** Accepted
**Date:** 2026-05-04
**Tags:** arch, renderer, scene, resources

---

## Context

Phase 2.6 (crd-resources) shipped complete on 2026-05-04. Phase 2.7 (asset import: TextureResource,
MeshResource, material parameters/textures, GPU upload) is designed and next in queue.

The material system as shipped in Phase 2.6 v1e is a loader proof-of-concept with five known gaps
documented in `docs/debt.md`:
1. Material parameters (uniforms + textures) — **closed by Phase 2.7 v1c**
2. Per-material PSO state (blend, depth, cull) — **open**
3. Pass-keyed shader variant awareness (`VariantKey` per render pass) — **open**
4. Artifact-driven descriptor layout — **open**
5. Additional shader stages (compute, mesh) — **open**

The question is: which subset of items 2–5 must close before Phase 3.0 scene/ECS, and which can wait?

---

## Decision

**Material debt items 2 and 3 ship as Phase 2.8 before Phase 3.0 scene/ECS.**

**Material debt items 4 and 5 are deferred to Phase 3.5+ (CSM / post-FX / compute consumers).**

**Skeletal animation data in MeshResource is deferred to Phase 3.2 (Animation foundation).**

Phase ordering locked as:
```
Phase 2.6 — crd-resources (COMPLETE)
Phase 2.7 — Asset import: TextureResource + MeshResource + material params + GPU upload
Phase 2.8 — Material completion: per-material PSO state + pass-keyed variants + depth-only prepass
Phase 3.0 — Scene / ECS foundation
Phase 3.1 — Physics (PhysX 5 backend)
Phase 3.2 — Animation (skeletal, blend trees, IK)
Phase 3.3 — crd-font (MTSDF, HarfBuzz)
Phase 3.4 — Audio (spatialized, mix graph, DAW host scaffold)
Phase 3.5 — PBR + lighting + NPR (IBL, CSM, PCSS, area lights, SSS, NPR)
Phase 3.6 — Atmosphere + volumetrics (Hillaire sky, fog, clouds, aurora)
Phase 3.7 — Post-processing (bloom, GTAO, SSR, TAA, DoF, motion blur, upscaling)
Phase 3.8 — GPU-driven rendering + particles + water (Hi-Z, indirect, ocean, decals)
Phase 3.9 — GI pre-RT (SSGI, DDGI, lightmap baking)
```

---

## Rationale

### Items 2 and 3 are scene/ECS prerequisites

The scene system's job is to **classify draw items and hand them to the renderer each frame.** Without
items 2–3 already in place, the scene system must be designed around two known limitations:

- **Without item 2 (PSO state):** Every renderable shares one pipeline regardless of blend mode, alpha
  mode, or cull hints. The scene cannot express the difference between an opaque wall and a transparent
  window at the material level. Any classification logic in the scene system becomes placeholder code that
  will change when item 2 lands.

- **Without item 3 (pass-keyed variants):** Multi-pass rendering — the depth prepass that already exists in
  `ForwardRenderPath` v1g, and future shadow map passes — has no mechanism for a draw item to declare which
  shader it uses in each pass. The scene system's render-extract phase would need to synthesize this logic,
  again as placeholder code.

Installing these two items before the scene system is designed means the scene system sees the real interface
on day one. They are prerequisites, not cleanup.

### Items 4 and 5 have no current consumer

Item 4 (artifact-driven descriptor layouts) has value when materials drive complex per-material descriptor
sets at set 1. The current `VulkanDescriptorAllocator` ring pool works for the current use case. The real
demand arrives when CSM (Phase 3.5) or post-FX materials require GPU-sampled shadow maps or G-buffer reads
bound at set 1+ via material-owned layouts.

Item 5 (compute/mesh shader stages in MATR) has no consumer until post-FX (Phase 3.7: bloom compute, TAA,
DoF) and GPU-driven rendering (Phase 3.8: particle simulation, indirect-draw culling) land. Changing the
MATR format now for a hypothetical future consumer violates the "real workload before optimization /
scaffolding" principle.

### Rigging deferral

glTF files can contain joint hierarchies, skin weights, and morph targets. Adding these to `MeshResource`
in Phase 2.7 stores bytes with no consumer: there is no scene transform hierarchy for joints to live in,
no animation sampler, and no skinning pass in `ForwardRenderPath`. Cooking joint data also locks a binary
format before the animation system (Phase 3.2) has expressed its own format requirements. The glTF re-cook
cost when Phase 3.2 lands is acceptable — the cooker is designed for idempotent rebuilds from source.

---

## Consequences

- Phase 2.8 is introduced between 2.7 and 3.0. It is a small slice (three sub-slices: PSO state in MATR,
  pass-keyed variants, depth-only prepass). Estimated scope: one session.
- `ForwardRenderPath` gains a per-material pipeline cache keyed by `(VariantKey, RasterState)` in v2.8a.
- `MaterialResource` gains a `HashMap<PassType, ResourceHandle<ShaderResource>>` replacing the flat
  `vert_handle + frag_handle` in v2.8b.
- Material debt items 2 and 3 are closed. Items 4 and 5 remain open in `docs/debt.md` until their
  consumers land (item 4 → Phase 3.5 CSM; item 5 → Phase 3.7 post-FX or Phase 3.8 GPU-driven).
- Phase 3.0 receives a material system where draw items carry real PSO state and per-pass shaders,
  eliminating one class of architecture churn.
- glTF files with rigging data will need re-cook when Phase 3.2 animation lands. Acceptable by design.

---

## References

- `docs/debt.md` — Material system v1 gaps (items 2–3 closed here; 4–5 deferred)
- `docs/phases/phase-2.7-asset-import.md` — Asset import phase (item 1 closed there)
- `docs/phases/phase-2.8-material-completion.md` — Execution plan for this decision
- `docs/phases/phase-3.0-scene-ecs.md` — Scene/ECS phase that benefits from this ordering
- ADR-0020 — Scene & ECS hybrid (cornerstones accepted; sub-ADRs pending for Phase 3.0)
- ADR-0025 — Shader mechanism policy
- ADR-0030 — Shader / PSO boundary
