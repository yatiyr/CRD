# ADR-0057 — Scene/ECS: UI nodes in scene tree (boundary declaration)

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, ui, arch

---

## Context

ADR-0020 (cornerstone) committed Cerid to a Godot-style "UI lives inside the scene tree" model — Spatial nodes (3D) and Control nodes (UI) coexist as children of the same root. UI is part of the scene, not an overlay.

The cornerstone declaration is sufficient for the scene-graph philosophy. What was missing — and what this sub-ADR locks — is the **module boundary**: which UI-related types live in `crd-scene` vs `crd-ui`, what `crd-scene` reserves for `crd-ui`'s use, and what guarantees `crd-ui` (Phase 5) can rely on from day one.

Without this boundary, Phase 3.0 would either:
- Couple `crd-scene` to UI assumptions (layout coordinates, anchor metaphors) that are `crd-ui`'s problem to define.
- Leave `crd-ui` to reach into `crd-scene` internals later, creating a cyclic dependency.

This ADR prevents both.

---

## Decision

### 1. `crd-scene` owns the entity, hierarchy, and tree traversal — nothing UI-specific

`crd-scene` provides:

- Entity / EntityId / SlotMap (ADR-0049)
- Component storage (ADR-0050)
- `Relation<ChildOf>` and tree traversal (ADR-0051)
- `Transform` with translation/rotation/scale (ADR-0054)
- Query DSL, schedule, indexes (ADRs 0052, 0053)

That's all. None of these are UI-specific. A `Control` node and a `Spatial` node both have a `Transform` and a `ChildOf` relation; they differ only in *which other components* are on them.

### 2. `crd-scene` reserves a `ControlNodeTag` empty component

```cpp
namespace crd::scene
{
    // Empty tag component. Marks an entity as a UI control node rather than
    // a 3D spatial node. crd-scene treats it as opaque — only the renderer
    // (Phase 5) and the layout system (crd-ui) consume it.
    struct ControlNodeTag {};
}
```

This is a 0-byte tag (Layer 5 stores presence-bit only — see ADR-0050). Zero memory cost when not used. Reserving the type in `crd-scene` lets the renderer (Phase 5) categorise draws by tag presence without `crd-renderer` depending on `crd-ui`.

### 3. `crd-ui` (Phase 5) owns all UI-specific components

Components that `crd-ui` will register on entities tagged with `ControlNodeTag`:

- `Style` — colour, padding, font, border, etc.
- `Layout` — flex/grid/absolute layout parameters.
- `Bounds` — computed pixel rectangle (output of layout pass).
- `Renderable2D` — what to draw at the bounds rectangle.
- `EventTarget` — input-event handlers (click, hover, drag).
- Widget-specific components (`ButtonState`, `TextInput`, `ScrollPosition`, etc.).

These all live in `crd-ui` and are unknown to `crd-scene`. They are registered with `crd-scene`'s component system at startup like any other component.

### 4. Layout coordinates: `Transform` semantics for Control nodes

A Control entity has a `Transform` like any other. Its semantics differ:

- **3D spatial entity:** `translation` is a world-space position in metres. `rotation`, `scale` apply normally.
- **Control entity:** `translation` is parent-relative pixels (or layout-anchored offset, depending on `Layout` policy). `rotation` is rotation around the rectangle's pivot for transformed UI. `scale` scales the rectangle.

The difference is interpreted by `crd-ui`'s layout pass, not by `crd-scene`. The transform propagation system (ADR-0054) treats both uniformly — it propagates `local_matrix → world_matrix` regardless of whether the entity is 3D or UI.

This works because:
- 3D rendering composes via the world-matrix view transform.
- UI rendering composes via an orthographic view-projection where the world-matrix's translation already encodes pixel-space offset.

The math layer doesn't care. The semantics are an interpretation imposed by which renderer consumes the entity.

### 5. UI tree traversal is the same as 3D tree traversal

`crd-ui`'s layout pass walks the `ChildOf` relation hierarchy with `traverse_relation<ChildOf>` (ADR-0051), exactly like `TransformPropagation`. UI doesn't ship its own tree-walking code.

This means features that work for 3D scenes work for UI for free:
- Hot-reload: re-cooking a scene reloads UI hierarchy.
- Time-tunneling: `History` on `Bounds` lets the editor scrub UI animations.
- Change detection: layout pass only re-runs for entities whose `Style` or `Layout` changed.
- Scene serialization: UI scenes save to TOML alongside 3D scenes (when `crd-ui` registers serialization for its components in Phase 5).

### 6. Ordering: render passes for 3D and UI

`crd-renderer` (today) and the future `crd-renderer-2d` (Phase 5) consume the same scene tree but produce different draw lists:

- 3D pass: `query<Transform, Renderable>().without<ControlNodeTag>()`
- UI pass:  `query<Transform, Renderable2D>().with<ControlNodeTag>()`

Composition (3D under, UI over, or interleaved with depth) is the renderer's concern, not `crd-scene`'s. Frame graph (ADR-0032) wires this in Phase 5.

### 7. What this ADR explicitly does NOT decide

- The `Style` field set, `Layout` algorithm choice (flex / grid / Yoga / custom), font rendering integration. All `crd-ui`'s problem (Phase 5).
- Multi-window / multi-viewport UI (likely needs a `Viewport` component — `crd-scene` reserves no API for it now; Phase 5 adds if needed).
- Editor UI vs runtime UI separation. Phase 7 builds the editor on top of the same machinery.

---

## Rationale

### Why declare the boundary now

`crd-scene` ships in Phase 3.0; `crd-ui` ships in Phase 5. In between, `crd-scene` will have many consumers (renderer, sandbox, animation, physics). If we don't declare the UI boundary now, those consumers will have implicit "UI doesn't exist" assumptions baked into their code paths — assumptions that need rewriting when `crd-ui` arrives.

By stating *now* that "UI is just entities with different components on the same hierarchy," every Phase 3.x consumer treats UI as a transparent special case rather than a new code path.

### Why a tag rather than a separate entity registry

A single shared entity / hierarchy machinery is the entire point of ADR-0020. Splitting Control entities into a separate registry would re-create the "UI overlay" problem ADR-0020 rejected. The tag is the minimal information needed to disambiguate — and because tags are 0-byte (presence bit only), the cost of the discrimination is essentially nothing.

### Why `crd-scene` knows nothing about UI semantics

Coupling `crd-scene` to UI anchor models or layout algorithms would lock those choices into the scene module forever. Keeping them in `crd-ui` lets that module evolve (add Yoga support, add a custom flex variant, integrate with HarfBuzz text) without touching the scene foundation.

---

## Consequences

- `crd-scene` defines `ControlNodeTag` as an empty struct in a public header. No other UI types in `crd-scene`.
- `crd-renderer`'s 3D path filters on `without<ControlNodeTag>()` from Phase 3.0 onward. The filter is a no-op until UI exists, but the call site is in place.
- `crd-ui` (Phase 5) registers `Style`, `Layout`, `Bounds`, `Renderable2D`, etc., with `crd-scene`'s component system.
- UI scenes serialize to TOML through the same `cook_scene` handler as 3D scenes (ADR-0055). UI components register their `ComponentSerialize` traits when `crd-ui` ships.
- Editor (Phase 7) inspector treats Control entities and Spatial entities uniformly — both are "entity with components" in the inspector tree.
- The "UI as overlay" anti-pattern (separate immediate-mode UI imgui-style for game UI) is closed off. `crd-imgui` remains debug-only forever (ADR-0023, ADR-0024).

---

## References

- ADR-0020 — Scene & ECS hybrid + UI in scene tree (cornerstone — this ADR confirms the boundary)
- ADR-0023 — UI architecture (the `crd-ui` design that consumes this boundary)
- ADR-0024 — ImGui single-viewport (debug-only contract; UI is `crd-ui`'s job, not ImGui's)
- ADR-0032 — Frame graph (composes 3D and UI passes)
- ADR-0049 — Entity identity (UI nodes are entities)
- ADR-0050 — Storage backends (`ControlNodeTag` is a presence-bit-only tag)
- ADR-0051 — Relations (`ChildOf` is the UI tree backbone)
- ADR-0054 — Transform hierarchy update (handles UI and 3D uniformly)
