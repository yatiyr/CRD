# Phase 7 — Editor

**Status:** ⏳ planned

The editor is a Cerid application, not a separate codebase. Built on
`crd-ui` + `crd-node-editor` + `crd-config` + `crd-resources`.

## Sandbox predecessor (`crd-sandbox`)

**Decision (2026-05-05):** Rather than jumping straight from automated smokes to a full editor,
Phase 2.7 v1d introduces `crd-sandbox` — an interactive application on `crd-app` + `LayerStack`
with Dear ImGui panels. It grows alongside each new system (renderer, physics, scene/ECS, animation)
as the primary interactive testbed. When Phase 7 opens, `crd-ui` replaces the ImGui panels; the
`crd-app` + `LayerStack` + `EventBus` foundation carries over unchanged. The editor is therefore
`crd-sandbox` + a professional UI shell, not a rewrite.

**Design decisions carried into Phase 7:**
- `crd-sandbox` is built on `crd-app::Application` + `LayerStack` — the same foundation Phase 7 will use.
- Each system that ships gets an ImGui panel in `crd-sandbox` (mesh/texture inspector, material editor,
  scene outliner stub, job monitor). These panels drive the Phase 7 panel feature list.
- `--headless` flag allows CI-compatible offline rendering for regression tests without a display.
- `OrbitCamera` (smoothed, exponential lerp) provides camera navigation across all sandbox sessions:
  - State: `{yaw, pitch, distance, target}` (input-driven) + `{s_yaw, s_pitch, s_dist, s_target}` (rendered)
  - Update: `s_val = lerp(s_val, val, 1.0f - exp(-SPEED * dt))` (framerate-independent)
  - Controls: left-drag = orbit, Ctrl+MMB = pan, scroll = zoom; pitch clamped to ±89°
- ImGui panels stay debug-only: they are never exposed to game/simulation runtime, consistent with
  `crd-imgui` being debug-only forever (see `CLAUDE.md` architecture overview).

**Phase 7 continuity plan:**
- `crd-ui` panel API will be designed to mirror the ImGui panel contracts already in use in `crd-sandbox`
  (same lifecycle: `on_attach`, `on_detach`, `on_ui_render`).
- Panel-by-panel porting: one ImGui panel → one `crd-ui` panel per slice; no big-bang rewrite.
- `crd-node-editor` (shader graph, material graph) does not exist in `crd-sandbox`; it's a Phase 7 addition.

## Slices

| Slice | Topic                                | Notes                                                                |
| :---: | ------------------------------------ | -------------------------------------------------------------------- |
| 7.0a  | shell + docking                      | window panels, persistence, theme via `crd-config`                    |
| 7.0b  | content browser                      | asset DB view, drag/drop, cooker integration                          |
| 7.0c  | scene editor                         | gizmos, selection, transform tools, undo/redo                         |
| 7.0d  | inspector                            | reflection-driven; introspects components / nodes                     |
| 7.0e  | profiler                             | CPU + GPU + memory + jobs + frame timing                              |
| 7.0f  | shader graph integration             | live preview; compile errors surfaced inline                          |

## Decisions

(none yet — design happens when this phase opens)
