# Phase 3.0 — Scene / ECS Foundation

**Status:** ⏳ planned — begins after Phase 2.8 ships
**ADRs pending (write before coding):** see "Design decisions to lock" below
**New modules:** `crd-scene`
**Depends on:** Phase 2.8 complete (MaterialResource with PSO state + pass-keyed variants)

---

## Goal

Introduce the scene foundation: a hybrid spatial hierarchy + SoA component system that gives every existing
engine consumer (renderer, physics, animation) a stable address space for objects and transforms.

This phase delivers:
1. An `Entity` / `EntityId` system with stable generational handles.
2. Component storage (SoA, archetype-oriented or sparse-set — see ADR below).
3. `Transform` + `HierarchyNode` built-in components; dirty propagation → world-matrix cache.
4. A `Scene` container: parent/child hierarchy, TOML authoring format cooked to binary.
5. Renderer integration: `ForwardRenderPath` enumerates a `RenderableComponent` view each frame instead of
   an explicit `DrawList`.
6. TOML scene authoring → `cook_scene` cooker handler → cooked `SCEN` artifact loaded via `ResourceManager`.

**Not in this phase (explicit deferrals):**
- Physics component (`RigidBodyComponent`) — Phase 3.1
- Skeletal mesh / skinning components — Phase 3.2
- Audio source components — Phase 3.3
- GPU instancing (`DrawIndirect`) — Phase 3.2 (after stable GPU scene buffer)
- UI/Control nodes in the scene tree — blocked on `crd-ui` (Phase 5)
- Editor GUI for the scene tree — Phase 7
- Scripting components — Phase 4.0

---

## Architecture (preliminary — final shape locked by ADRs below)

Per ADR-0020 (Accepted): **Hybrid model.** SoA component storage for cache-friendly iteration; hierarchical
scene tree for traversal and authoring. Not pure ECS, not naive scene graph.

```
crd-scene
  Entity / EntityId / EntityRegistry
  Component storage (archetype chunks or sparse sets — ADR pending)
  Transform + HierarchyNode components (built-in)
  Scene container (asset-loadable, CRDS binary format)
  SceneLoader (ILoader<SceneResource> registered with ResourceManager)
```

The renderer is NOT inside `crd-scene`. Instead, `crd-renderer` queries `crd-scene` for a
`RenderableComponent` view (iterator pattern) each frame. Dependency direction: `crd-renderer` → `crd-scene`
(allowed — renderer already depends on math/resources).

---

## Design decisions to lock before coding

These are the architectural questions that need individual ADRs **before the first line of `crd-scene`
implementation is written.** Starting coding without answers to 1–4 means refactoring the whole storage
layout mid-phase. ADRs 5–7 can land early in the phase (once the storage is live).

### ADR-A: Entity identity and slot-map storage

**Questions to resolve:**
- Generational index format: `[gen:16 | idx:16]` (64 k max entities, 64 k max generations) vs
  `[gen:8 | idx:24]` (16 M max entities, 256 max generations) vs full `u64` with configurable splits.
- Slot-map backing array: plain dense array + free list vs pointer-stable `ChunkedSlotMap`?
- Tombstone semantics: slot freed immediately on destroy, or deferred one frame (avoids in-frame UAF)?
- Zero-value sentinel: `EntityId{0}` == invalid? Or a separate `kInvalidEntity` constant?

### ADR-B: Component storage layout

**Questions to resolve:**
- Archetype-based SoA (all components of a given type-set packed together) vs sparse-set per component type?
- Chunk size (e.g. 16 KB archetype chunks) and alignment for SIMD (32-byte or 64-byte)?
- Maximum component count per entity (fixed upper bound for compile-time type IDs)?
- Tag components (zero-size) — stored as bitsets or as empty arrays?
- Mutable vs immutable components — separate storage pools, or same pool + const accessor?

### ADR-C: Transform hierarchy update model

**Questions to resolve:**
- Dirty-flag propagation: push (parent marks children dirty on write) vs pull (query recomputes lazily)?
- Timing: single-threaded serial traversal each frame vs jobified parallel traversal?
- World matrix caching: store `world_matrix` in the component vs recompute on demand?
- TRS decomposition: store separate translation/rotation/scale (more composable) or `Mat4f` directly?

### ADR-D: Scene serialization format

**Questions to resolve:**
- TOML authoring schema: how are entities, components, and child links expressed in a `.scene.toml` file?
- Cooked binary: new `SCEN` CRDR artifact type, or a separate `.scen` binary format?
- Reference resolution: how do cooked scene TOML references to mesh/texture UUIDs resolve to `ResourceId` at cook time?
- Component-type registry: how does the cooker know which component names map to which binary layouts?

### ADR-E: Query/iteration API

**Questions to resolve:**
- View API: `registry.view<Transform, RenderableComponent>()` returning a range vs explicit system registration?
- System ordering: implicit (declared dependencies) or explicit (registration order)?
- How does `ForwardRenderPath` request the list of renderables each frame — pull query or push event?
- Thread safety: are component writes during iteration defined (double-buffered) or forbidden (single-threaded)?

### ADR-F: UI nodes in scene tree (confirm ADR-0020)

ADR-0020 already accepted the Godot-style "UI nodes coexist in the scene tree." This sub-ADR locks the
component contract: what does a `ControlNode` look like at the component level? What does layout mean in
terms of transform (2D pixel coords vs 3D world space anchor)? Blocked until `crd-ui` design matures (Phase 5),
but the architectural boundary must be declared now so `crd-scene` doesn't accidentally couple to it.

### ADR-G: Lifecycle and ticking

**Questions to resolve:**
- System registration: list of `ISystem` implementations, or free functions with a tag?
- Update phases: `PrePhysics / Physics / PostPhysics / PreRender / RenderExtract`?
- Determinism hooks: fixed-step opt-in per system; how does fixed-step interact with variable-rate rendering?
- Shutdown ordering: systems torn down in reverse registration order?

---

## Slices (placeholder — fill in after ADRs A–D are accepted)

| Slice | Contents |
|-------|----------|
| v1a   | `EntityId` + `EntityRegistry` (ADR-A outcome) + entity create/destroy/lookup. Unit tests. |
| v1b   | Component storage foundation (ADR-B outcome) + `Transform` component + archetype/pool bootstrap. Unit tests. |
| v1c   | `HierarchyNode` + parent/child links + dirty-flag propagation + world-matrix cache (ADR-C outcome). Unit tests. |
| v1d   | Scene container: `SceneResource`, `SceneLoader`, CRDS cooked artifact, `mount_scene`. Unit tests. |
| v1e   | Renderer integration: `RenderableComponent` view replaces explicit `DrawList` in `ForwardRenderPath`. Smoke. |
| v1f   | TOML authoring: `.scene.toml` → `cook_scene` cooker handler (ADR-D outcome). Round-trip test. |

Each slice follows the standard Definition of Done: six-configuration green, unit tests, headless smoke
where applicable.

---

## Definition of done (Phase 3.0)

1. All six slices (v1a–v1f) shipped with unit tests.
2. ADRs A–G written and accepted (A–D before v1a; E–G during phase).
3. A real `.scene.toml` asset (with at least one mesh + transform) cooks, mounts, loads, and renders
   via `ForwardRenderPath` — first "scene on screen" milestone.
4. `smoke_scene.exe` (headless): load a cooked scene, assert entity count and transform correctness, exit 0.
5. `smoke_scene_render.exe` (GPU): load scene, render one frame, exit 0.
6. Six-configuration green.
7. `docs/systems/scene.md` written.

---

## References

- ADR-0020 — Scene & ECS hybrid + UI in scene tree (cornerstones accepted; sub-ADRs A–G pending)
- ADR-0044 — Phase ordering decision
- ADR-0036 — `crd-resources` module + loader registry (SceneLoader follows this pattern)
- ADR-0038 — Cooked binary container format (SCEN artifact extends CRDR)
- `docs/phases/phase-2.8-material-completion.md` — Predecessor phase
- `docs/phases/phase-3-simulation.md` — Sibling reference
