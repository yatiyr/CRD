# Phase 5 — Cerid UI + node editor + advanced rendering

**Status:** ⏳ planned

Replace ImGui as the user-facing UI surface. ImGui remains as the debug
overlay forever. Renderer grows beyond the Forward+ baseline.

## Slices

| Slice | Module / Topic                       | Notes                                                                |
| :---: | ------------------------------------ | -------------------------------------------------------------------- |
| 5.0a  | `crd-ui` core                        | retained-mode tree, layout, input routing, theming                   |
| 5.0b  | `crd-ui` widgets                     | text, button, list, tree, table, splitter, dock host                  |
| 5.0c  | `crd-ui` styling                     | TOML theme via `crd-config`; runtime swap                             |
| 5.0d  | `crd-ui` text                        | shaping, fallbacks, complex scripts, SDF rendering                    |
| 5.0e  | `crd-ui` ↔ scene tree                | UI Control nodes coexist with Spatial nodes in `crd-scene`            |
| 5.1a  | `crd-node-editor` runtime            | typed graphs, evaluation, serialization                               |
| 5.1b  | `crd-node-editor` view               | pan / zoom / connect / group / subgraph                               |
| 5.1c  | shader graph                         | node-authored materials, compiled to `crd-shader` programs            |
| 5.1d  | script graph                         | optional node-authored scripting layer above `crd-scripting`          |
| 5.2a  | Hi-Z occlusion culling               | depth pyramid + GPU readback or async culling pass                    |
| 5.2b  | GPU-driven rendering                 | indirect draw, persistent draw streams                                 |
| 5.2c  | mesh shaders                         | optional, hardware-conditional path                                   |
| 5.3a  | Deferred render path                 | second `IRenderPath`; G-buffer, light pass; coexists with Forward+    |
| 5.3b  | Visibility Buffer path               | third `IRenderPath`; software/visibility-driven; high-density scenes   |
| 5.3c  | render path selector                 | per-scene profile picks the best path; user override available        |

## Decisions

- ADR-0023 — UI architecture (retained-mode, ImGui debug-only)
