# Phase (unscheduled) — Cerid UI & Tooling Architecture (`crd-ui` · gizmos · editor overlays)

**Status:** 📋 planned — **unscheduled; late cross-cutting system.**
**Detail level:** architecture-deep, **slices provisional** (this captures the design + aspirations from the 2026-05-22 brainstorm; concrete slicing happens when the prerequisites land).
**Sequencing (hard dependencies):** after **renderer maturity** (a UI overlay pass + render-to-texture), **`crd-font`** (MSDF/SDF text substrate), **`crd-scene`** (ECS + ADR-0020 UI-in-scene-tree), and ideally the **command layer** (`phase-4.0-platform.md` — the "UI emits committed command-verbs" model + undo-from-command-log). UI is *consumer-driven*: it sits near/after the platform layer because the command model is its interaction architecture.
**Relationship to other docs:** ADR-0020 (hybrid scene; UI nodes coexist in the scene tree, Godot-style) is the scene-side foundation. `phase-4.0-platform.md` §"The application model" is where the *button = UI entity + command verb + binding* decomposition lives — this doc is the UI/tooling *system* that realizes it. `crd-imgui` is the debug overlay (debug-only forever, CLAUDE.md), **not** this.

---

## 1. Thesis: UI is mostly *not* a rendering system, and gizmos are UI

Two reframes anchor the whole design.

**(a) UI is ~80% not-rendering.** By weight a UI system is layout (constraint/flex solving, anchoring, DPI scaling) + a widget library + input/event routing (hit-test, focus, keyboard nav, drag) + text (font atlases, shaping, MSDF) + styling + scene-tree integration + command-emission. **Rendering is the smallest part (~15–20%)** — batched, clipped, alpha-blended 2D quads + text. So UI is *not* a slice of "advanced rendering" (deferred/GI/shadows/clustered-forward+ are orthogonal — UI rendering is *simple*). It's its own system that *consumes* a small renderer capability.

**(b) Gizmos and UI are the same pattern.** Both are **entities + driving systems + emitted command-verbs.** A panel widget and a transform gizmo differ only in *where they live and what they manipulate* — a gizmo is tooling-UI that targets the 3D viewport instead of a screen panel. Build one interaction model; gizmos are a specialization.

---

## 2. The unified interaction model

### 2.1 Two worlds (the central idea)

The entity space splits in two — this is how every elite editor works (Unity editor-only/`HideFlags.DontSave`; Unreal transient package + editor actors; Godot `EditorPlugin` gizmos; Blender gizmo groups):

- **Document world** — the scene content the user edits. Saved, shown in the scene-tree panel, undoable.
- **Tooling/editor world** — gizmos, selection highlights, manipulators, editor overlays. **Transient: never serialized, hidden from the scene-tree panel, NOT in the user's undo stack.** Composited *over* the document.

Implementation in `crd-scene`: either a **separate registry/world** for tooling entities, or a **component tag** (`EditorOnly` / `Transient` / `HiddenFromTree`) that excludes an entity from (a) the scene-tree panel query, (b) serialization, (c) the document undo log. The tag approach is lighter and fits the query model — the scene-tree panel queries "document entities without `EditorOnly`." *This is the "entities non-visible in the scene tree" the design started from — it's the canonical pattern, not a hack.*

### 2.2 The Logic / Visual / Command triple (the elite discipline)

Every gizmo **and** every widget decomposes into three *separable* parts:

| Part | What | Lives as |
|---|---|---|
| **Logic** | the brain: selection, drag state, hit-testing, "what does manipulating this mean" | a **System** (the "script") |
| **Visual** | the handles / arrows / quads / text the user sees | entities (tooling world) **or** immediate-draw — *swappable* |
| **Command** | the actual edit | a **committed verb** (`editor.transform.set`) |

The discipline: **keep Logic independent of Visual**, so the representation can change without rewriting the brain; and **route every edit through a Command**, so the action is replayable, undoable, and agent-callable. The "gizmo as hidden entity" is just one *Visual* choice; the brain is always a System.

### 2.3 Why this matters: the agent-native invariant holds

Because every edit is a command-verb, **the gizmo is never the *only* path to it.** `editor.transform.set` works headlessly — an agent (or hotkey, or console) moves the object by calling the verb, no gizmo involved. The gizmo is *one emitter* of a verb (ties to `phase-4.0-platform.md` §application model + §Pillar 7 runtime agents). This is the deep reason to separate Logic/Visual/Command.

---

## 3. Gizmos as a specialization

### 3.1 Lifecycle (concrete: a transform gizmo)

1. `Selection` (shared state — a singleton component / resource, read by the panel UI, the gizmo, and the command layer) changes.
2. The **GizmoSystem** reacts: spawns/positions handle entities (3 axis arrows, 3 plane quads, center) in the tooling world, tagged `EditorOnly` — or draws them immediate-mode (see §5).
3. Drag → the system updates the target transform *live* (cheap, in-place) for instant feedback.
4. **Release → emit ONE `editor.transform.set` command** (the committed-command rule). That single command enters undo + replay.
5. Deselect → despawn/stop drawing handles.

**Undo invariant:** the *transform* (the command) is undoable; the *gizmo spawn/despawn* is NOT. Tooling churn never touches the user's undo stack. (Trap: get this wrong and Ctrl-Z undoes "showed the gizmo" instead of "moved the cube.")

### 3.2 The elite traps (good → elite)

- **Constant screen-size gizmos** — scale handles by distance-to-camera so they stay a fixed *pixel* size; per-viewport.
- **Overlay pass / depth** — gizmos draw in a viewport overlay pass, usually depth-cleared / always-on-top so geometry doesn't occlude them (optional occlusion for some handles).
- **Pointer capture during drag** — once a drag starts on a handle, that handle captures the pointer so the drag continues when the cursor leaves its bounds. (The #1 "gizmo feels broken" bug.)
- **Selection is shared state**, not gizmo-owned — panel UI, gizmo, and commands all read one `Selection`.
- **Per-viewport / multi-window** — each camera gets its own handle scaling + hit-testing.

### 3.3 The planned cluster

This is the gizmos cluster already flagged as a high-priority future UI cluster (memory `project_gizmos_direct_manipulation_cluster`): transform gizmos + curve control-point gizmos + navmesh editing + Blender-class mesh vertex/edge select. It sequences *after* the command layer because gizmos emit commands. Until it lands, sandbox scenes use ImGui `DragFloat3` as the stopgap.

---

## 4. Rendering integration

### 4.1 Two flavors, one content system

- **Screen-space overlay UI** (HUD, editor panels): a dedicated **2D ordered pass** in the frame graph — painter's order, no depth test, scissor/clip, heavy alpha blend, batched by atlas + clip-rect. It is a **frame-graph pass with a UI material set**, *not* an `IRenderPath` (those are 3D scene strategies). Consumes draw-lists the UI system produces.
- **Worldspace / diegetic UI** (3D panels, VR menus, medical/cinematic overlays, gizmos): **renderable objects** — transforms in 3D, depth-tested in the viewport, in (or composited over) the scene.

**Decouple content from compositing.** The UI system produces a **draw-list / offscreen surface** once; *placement* decides whether it's blitted to screen (overlay) or mapped onto a worldspace quad (diegetic) via render-to-texture. So "is UI a renderable?" resolves to: content is produced once, placement chooses the path. (Worldspace = renderable; screen-space = 2D pass.)

### 4.2 What the shader & resource systems need: ADDITIVE, not structural

The existing systems already anticipate this — you *extend*, you don't retrofit:

- **Shader system (`crd-shader`):** add UI shaders — textured-quad + vertex-color + clip-rect; MSDF text; rounded-rect/SDF-shape. The shaderc→SPIR-V→reflection→`VariantPipelineDesc` pipeline compiles arbitrary shaders already; UI pipeline states (alpha blend, scissor, no depth-test, per-vertex color) are standard RHI state. **No structural change.**
- **Resource system (`crd-resources`):** add resource types + `ILoader`s — a font resource (glyph atlas + metrics), UI texture atlases (already plain textures), UI-scene/theme assets. The manager is loader-extensible (2Q eviction, CRDR pack all still apply). **Add loaders, don't retrofit the manager.**
- **The one genuinely new substrate is `crd-font`** — MSDF/SDF glyph atlases (crisp at any DPI), cooker-generated + runtime shaping. *That's* the real prerequisite, and it's its own module, not a shader/resource edit.

### 4.3 Renderer "keep the door open" items (verify during renderer work, don't build UI then)

- Frame graph allows an ordered transparent **overlay pass** (post-3D, pre-present) with scissor/clip + custom pipeline.
- Render-to-texture (for diegetic UI) — likely already there for post-processing.
- `IRenderPath` / material layering must not assume "everything is a PBR mesh."

---

## 5. Input, hit-testing, and the immediate-vs-retained call

### 5.1 Input layering & hit-testing

Input flows top-down through a stack: **editor overlay → UI → gizmos → 3D scene.** A click tries UI first, then gizmo handles, then falls through to scene picking. Two elite details:
- **Reuse the spatial index** — `crd-geometry-spatial`'s query facade answers "what's under the cursor ray" for scene picking *and* gizmo handles; UI hit-testing is a 2D variant. ECS-native UI gets efficient hit-testing for free.
- **Pointer capture** during drag (see §3.2).
- **Focus model** for keyboard nav / text fields.

### 5.2 Immediate vs retained — the honest tradeoff

- **Logic is always retained** — a System + selection/drag state. No debate.
- **Visual is swappable per-tool:**
  - **Immediate-draw** (rebuild geometry each frame from state, via `crd-geometry-viz` debug-draw): zero entity-lifecycle churn, dead simple — great for *transient* tools like gizmos.
  - **Retained entities** (real entities with UI components): uniform with the panel-UI model, queryable, composable, but you manage spawn/despawn churn.

**Recommendation:** gizmo *visuals* are often best **immediate-draw** (transient, regenerated from state anyway); persistent panel UI is best **retained** (hierarchical, benefits from layout/query systems). Both share the Logic-as-System + Command-as-verb spine. The elite move is making the *visual representation swappable behind the system*, so it's a per-tool choice, not an architectural lock-in.

---

## 6. `crd-imgui` vs `crd-ui` (don't conflate)

- **`crd-imgui`** — immediate-mode Dear ImGui *debug* overlay. Debug-only forever (CLAUDE.md). Dev scaffolding.
- **`crd-ui`** (this doc) — the production *retained* UI: entities in the scene tree (ADR-0020), driven by systems, emitting command-verbs. Layout + widgets + text + theming + events. ImGui is the stopgap until `crd-ui` exists.

---

## 7. Module mapping (Cerid)

| Concern | Cerid module |
|---|---|
| Entities / systems / queries / worlds-or-tags | `crd-scene` (8-layer ECS; ADR-0020) |
| Gizmo & overlay immediate-draw | `crd-geometry-viz` |
| Hit-testing (ray-pick scene + handles) | `crd-geometry-spatial` query facade |
| Text substrate (MSDF/SDF) | `crd-font` (new, prerequisite) |
| UI shaders | `crd-shader` (add shaders) |
| Font/atlas/UI-scene loaders | `crd-resources` (add `ILoader`s) |
| UI overlay pass / render-to-texture | `crd-renderer` (frame-graph pass + material set) |
| Commands / undo / agent-callable verbs | `phase-4.0-platform.md` command layer |
| Coordinates / DPI scaling (typed) | `crd-units` (length-typed UI coords) |

---

## 8. Open questions (decide at execution)

- Two worlds: **separate registry** vs **`EditorOnly` tag** on one registry. (Lean: tag, for query uniformity.)
- Layout engine: flexbox-class vs constraint-solver (Cassowary) vs both.
- Widget set authoring: data-driven UI scene files vs script-built vs both (Godot does both).
- Diegetic-UI compositing: render-to-texture-then-quad vs direct in-scene draw.
- Per-tool visual policy: which gizmos are immediate-draw vs retained entities.
- How much UI state is itself in the command/replay log (panel layout changes? or only document edits?).

---

## 9. References

- **ADR-0020** — hybrid scene model; UI nodes coexist in the scene tree (Godot-style). The scene-side foundation.
- **`phase-4.0-platform.md`** — §"The application model" (button = UI entity + command verb + binding) + §Pillar 7 (runtime agents); the command layer this consumes.
- Memory: `project_gizmos_direct_manipulation_cluster` (the planned gizmos cluster), `project_command_layer_unified_action_interface` (UI/gizmo/agent unified action interface).
- Engine precedents: Unity (editor-only objects, `HideFlags`), Unreal (transient package + editor actors), Godot (editor-as-app, `EditorPlugin` gizmos, UI-in-scene-tree), Blender (gizmo groups, `bpy.ops` operators).
- Text rendering: Valve MSDF (Chlumský `msdfgen`), SDF text (Green 2007).
