# ADR-0023 — UI architecture

**Date:** 2026-04
**Status:** Accepted
**Tags:** [ui] [node-editor] [arch]

## Decision

- `crd-ui` is retained-mode, not immediate-mode. Rationale: editor + game
  UI + DAW UI all want persistent widget identity, undo/redo,
  accessibility.
- ImGui is debug-only forever. Docking branch used. After `crd-ui` ships,
  ImGui never grows into editor or game surfaces.
- `crd-node-editor` is its own module, built on `crd-ui`. Shader graphs
  first; optional script graphs later.
- Theme/styling via `crd-config` (TOML), not hard-coded.
- UI nodes are scene-tree members (see ADR-0020).

## Clarifications (2026-05)

**Control nodes require a box-model layout engine, not just positional
transforms.** Blender-like editor panels (resizable regions, anchored
widgets, responsive flow containers) demand `HBoxContainer`, `VBoxContainer`,
`HSplitContainer`, and `anchors + margins` semantics — the same primitives
Godot's Control nodes expose. Positional placement alone is insufficient for
tool-quality UI.

**`crd-ui` must be usable standalone, without a 3D scene or `crd-renderer`'s
3D path active.** For DAW / creative-tool consumers, the entire application is
Control nodes with no Spatial nodes anywhere. `crd-ui` must not carry a
mandatory build dependency on `crd-renderer`'s 3D systems. The frame graph
(ADR-0032) composites a UI canvas pass that operates whether or not a 3D scene
pass exists in the same frame.

## References

- `docs/phases/phase-5-ui-rendering.md`
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0032 — Frame graph v1 (UI canvas pass is a frame graph pass)
