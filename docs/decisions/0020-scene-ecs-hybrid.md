# ADR-0020 — Scene & ECS hybrid + UI in scene tree

**Date:** 2026-04
**Status:** Accepted
**Tags:** [scene] [ecs] [ui] [arch]

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
- Authoring scenes are TOML. Runtime scenes are cooked binary
  (FlatBuffers vs Cap'n Proto chosen during the Phase 3.1c slice).

## References

- `docs/phases/phase-3-simulation.md`
- `docs/phases/phase-5-ui-rendering.md`
