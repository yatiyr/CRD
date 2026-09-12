# ADR-0020 — Scene & ECS hybrid + UI in scene tree

<!-- doc-role: decision -->
> Decision record; read status and supersession notes. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**Date:** 2026-04
**Status:** Accepted; UI-in-scene clauses superseded by the user-chosen ADR-0107 D2
**Tags:** [scene] [ecs] [ui] [arch]

## Current boundary amendment — 2026-09-12

The user-chosen **separate retained UiWorld** decision in [ADR-0107 D2](0107-ui-2d-architecture.md#d2--uiworld-is-a-dedicated-retained-world-not-the-gameplay-ecs)
supersedes this record's UI-in-gameplay-scene premise. `UiNodeId` and `EntityId` are distinct;
UI layout, focus, input, semantics and persistence belong to UiWorld. World-space UI uses explicit
scene attachments and coordinate/input projection. Existing scene types are not deleted by this
documentation amendment; compatibility/removal is owned by WORLD-UI in the [master table](../ROADMAP.md#slice-world-ui).
The complete ADR-0107 is still Proposed. Retained UI, debug-only ImGui, reusable scene infrastructure,
standalone UI and modularity remain requirements. The text below preserves the original decision,
including its now-superseded scene/UI ownership assumptions; it must not guide new UI storage.

## Decision

- Scene graph lands in Phase 3.1, not earlier. Renderer v1 starts from an
  explicit `Renderable` list; ECS shape is not chosen until there is real
  multi-system pressure (rendering + physics + animation + audio).
- **Hybrid model: hierarchical scene tree + entity/component storage.**
  Components are stored SoA for cache-friendly iteration; the parent /
  child hierarchy is kept separately as a tree of entity ids. Systems
  iterate components directly; gameplay code traverses the tree. Inspired
  by Bitsquid / Stingray's evolution; opposite end of the spectrum from
  archetype-pure ECS (Bevy / EnTT-pure) and from naive node-with-virtuals
  graphs.
- **UI lives inside the scene tree, Godot-style.** Spatial nodes (3D) and
  Control nodes (UI) coexist as children of the same root. UI is not an
  overlay — it is part of the scene. The render layer composites 3D and
  UI canvases at the end of the frame; they are different *render* paths
  but the same *scene* tree. This unifies authoring, persistence,
  scripting, and editor tooling. (Phase 5.0e ties `crd-ui` into the tree.)
- Authoring scenes are TOML. Runtime scenes are cooked binary —
  the FlatBuffers vs Cap'n Proto deferral is closed by ADR-0055,
  which picks neither: a CRDR `SCEN` artifact reusing the existing
  resource container (chunking, zstd, manifest, hot-reload, mounting).

## References

- `docs/phases/phase-3.0-scene-ecs.md` — Phase 3.0 plan implementing this cornerstone
- `docs/phases/phase-3-simulation.md`
- `docs/archive/2026-09-12-superseded-plans.md#phase-5-ui-rendering`
- ADR-0049…ADR-0057 — eight-layer architecture sub-ADRs (locked 2026-05-06)
- ADR-0055 — closes this ADR's "FlatBuffers vs Cap'n Proto" deferral with `SCEN` CRDR
