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

## References

- `docs/phases/phase-5-ui-rendering.md`
