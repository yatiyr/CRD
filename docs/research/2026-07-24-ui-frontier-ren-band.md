# 2026-07-24 — The Interactive Frontier: UI-as-ECS, text, vector, docking (the REN·B recipe)

> STUDY→RECIPE for the REN band's interactive half. User mandate (2026-07-24): *"completely future proof,
> completely world class frontier gold standard"* — one architecture that can build ANY program beautifully:
> the editor with full game-engine + Blender + Cascadeur + DAW + CAD/CAM abilities, the hesap MATLAB
> environment, and every future Cerid application. Four decisions locked interactively with the user
> (recorded in §4).

## 1. The survey — what the frontier actually does

### 1.1 UI object models

| System | Model | The lesson for Cerid |
|---|---|---|
| **Bevy UI** | Widgets ARE ECS entities (Node/Style components, Taffy flexbox, same World) | The only production UI-in-ECS. Proves entity-per-widget + layout-as-system works; its pain points are (a) UI and sim sharing one World entangles schedules and document reload, (b) tree algorithms (layout, bubbling) run against ECS grain without ordered-children support, (c) no reflection-driven inspector story at Blender depth. We adopt the model and fix exactly these three. |
| **Blender** | Retained C structs (Window→Screen→Area→Region→Panel) + **RNA property reflection** over ALL of DNA | The load-bearing insight of this whole recipe: Blender's superpower is not its widgets — it is that **every property in the program is reflected** (name/type/range/units/animatable), so inspectors are auto-generated, ANY property is keyable/drivable, and Python (for us: agents + bindings) can address everything by path. The areas/regions docking model + workspace persistence is the proven multi-viewport UX. |
| **Qt/QML** | Retained scene graph + declarative bindings | Property bindings (`width: parent.width/2`) are the productivity frontier; a separate DSL is NOT required to get them — the binding graph is the value, the syntax is incidental. C++-only doctrine (ADR-0081) keeps the graph, drops the DSL. |
| **Flutter/Skia** | Retained tree, diff/rebuild, ONE batched GPU canvas | The paint architecture to copy: everything renders through a small set of batched primitives (rects/rrects/glyphs/paths/images) into few draw calls. Per-widget draw calls kill frame rate; batching is non-negotiable. |
| **Unity UIToolkit** | Retained VisualElement tree BESIDE the ECS, USS styling | Evidence that a company with a mature ECS still chose a separate retained tree — the counter-argument we answer with: Cerid's ECS already has the four things theirs lacked (relations, change-detect, command buffers, cooked-scene persistence), so a *dedicated UI World on the same machinery* gets retained-tree semantics without a second object model. |
| **Our Machinery** | "The Truth" central data model + immediate-mode UI | The Truth ≈ our reflection layer + command buffers (undo, collaboration, data-driven everything). Their immediate-mode choice traded away docking/animation/accessibility depth — the reason we go retained. |
| **Dear ImGui (docking branch)** | Immediate, multi-viewport OS windows | Ships real tear-off-to-OS-window + reattach — proof the workflow is tractable; stays our debug overlay only. |

### 1.2 Text

- **Parsing**: sfnt tables (cmap/glyf/loca/CFF/CFF2/hmtx/GSUB/GPOS/GDEF/fvar/gvar/avar). Variable fonts are
  table-driven interpolation — no rasterizer dependency. FreeType is the reference ORACLE only (codec doctrine).
- **Rasterization**: **MSDF** (Chlumský 2015) is the settled GPU-text frontier for UI scale ranges — sharp
  corners preserved, one atlas entry serves 8 px→200 px. Slug (Lengyel) renders outlines directly per-pixel
  (heavier per-fragment, no atlas) — MSDF wins for UI workloads; huge-zoom fidelity can fall back to
  path-rendering the outline through crd-vector (we get this free once REN-15 exists).
- **Shaping**: the honest hard part. HarfBuzz took ~15 years: UAX-9 bidi, UAX-14 line break, UAX-29
  segmentation, script itemization, GSUB/GPOS state machines (Arabic joining, Indic reordering, mark
  stacking). We own it (doctrine) with HarfBuzz as the conformance oracle + the Unicode UCD/UAX test files as
  hermetic fixtures. Scope ladder inside the slice: Latin+kern+ligatures → bidi → Arabic joining → Indic.

### 1.3 2D vector / path rendering (user chose COMPUTE-FIRST)

- **Vello** (linebender, 2022-2025) is the frontier: compute-shader pipeline — path flattening → binning →
  coarse raster → per-tile fine raster with analytic-ish AA; winding resolved in tiles; entire scene in a few
  dispatches. Strengths: zoom-independent, massive scenes (CAD sketches, dense node graphs), GPU-resident.
  Risks: conflation artifacts at exact coverage boundaries, watertight flattening (Kurbo's Euler-spiral
  flattening is SOTA), stroke expansion correctness (Nehab 2020-class stroking is its own literature).
- **Pathfinder 3** (tile-based, raster+compute hybrid) and **Slug** (per-pixel outline eval) are the
  alternatives; **Lyon/Skia tessellation** is the classical baseline.
- **Cerid's angle**: the pipeline is compute → *CKIR authors it* (KGraph compute kernels, the C5
  dispatch_indirect capability, the AS autotuner can tune the binning/fine kernels) and the **CPU oracle
  gates it** (the same scanline rasterizer that certifies correctness — our bit-exact-oracle doctrine applied
  to 2D). A tessellation path is NOT built as a product fallback; a scalar CPU rasterizer is the TEST oracle.

### 1.4 Docking, viewports, multi-window

- Blender's Window→Area→Region tree + workspace persistence = the docking data model (a TREE of splits/tabs —
  which is entities + relations for us, serialized as öbek → "record layouts / user preferences" free).
- ImGui-docking + every DAW (Ableton/Bitwig/Reaper) prove tear-off-to-OS-window/reattach; the platform
  substrate needs N OS windows over ONE GPU context (RET-2's IPresentSurface already supports N surfaces).
- Frame graph implication: N viewports = N scene-view passes + one UI pass per window, all scheduled by REN-1.

## 2. The architecture (the recipe)

**Pillar 1 — UI IS entities, in a DEDICATED UI World per window** (user-locked). Same crd-scene machinery
(archetypes, relations, change detection, command buffers), different World instance: the editor's interface
never lives inside the document it edits (reload-safe), UI ticks on its own schedule, and a viewport widget
holds a typed cross-World reference to the scene World it views. Widget granularity = one entity per widget
(a text field is ONE entity; glyphs are paint-output, not entities).

**Pillar 2 — crd-reflect, the RNA-class property system, is the centerpiece.** Registered descriptors over
component properties: name, type (crd-units-typed!), range/step/soft-limits, flags (animatable · keyable ·
undoable · transactional), path addressing (`entity.Component.property`), change notification riding ECS
change detection. ONE system feeds: auto-generated inspectors, reactive bindings, undo, UI+scene animation
targeting (the ONE curve engine's channels), theming, serialization, and MCP introspection (agents enumerate
and drive any property — the agent-native differentiator no other engine has). This is also the "reconfigure
our ECS data-driven + audit" answer: components become self-describing data.

**Pillar 3 — the ECS audit closes the four UI-shaped gaps first**: (a) ORDERED child relations (deterministic
sibling order — layout order and z-order demand it), (b) observer/event fit for reactivity, (c)
runtime-registered component types (plugins/agents defining properties without recompiling), (d) World
lifecycle + cross-World reference safety.

**Pillar 4 — paint is batched CKIR + an escape hatch.** Core primitives (rounded-rect SDF, MSDF glyphs,
images, gradients, shadows/blur, vector paths) authored in CKIR, batched into FEW draw calls per window
(Flutter/Skia model), linear-space blending (gamma-correct — the #1 text-ugliness bug). A `CustomMaterial`
widget hosts an arbitrary CKIR fragment graph and pays for its own draw (VU meters, node-editor backgrounds,
blur-behind). The KGraph node editor eventually AUTHORS these — the loop closes.

**Pillar 5 — three authoring surfaces** (user-locked): the fluent C++ builder (base) → UI-AS-DATA (panels and
workspace layouts as öbek/SCEN resources: recordable, cookable, diffable, agent-composable) → reactive
BINDINGS (widget property ← expression over reflected properties; the QML power, no DSL — C++-only doctrine).

**Pillar 6 — animation is the ONE curve engine's 4th consumer.** hesap-interp curves target reflected
properties (UI and scene alike): eased transitions, FLIP layout animation, enter/exit, springs (the one
non-curve primitive — an integrator, also deterministic). Same clock discipline as GEO-9.

## 3. Honest sizing + risk register

- **The shaper (REN-14) is the single hardest system in the band** — treat its internal ladder as gates
  (Latin → bidi → Arabic → Indic), never "done" without the HarfBuzz-oracle diff suite.
- **Compute-first vector (REN-15) is month-class**: flattening watertightness, stroke expansion, conflation.
  The CPU-oracle rasterizer is the correctness anchor; the AS autotuner tunes the kernels.
- **Entity churn** (virtualized lists): windowed entity pools + command buffers; gate scroll of 1M rows.
- **Cross-World references**: must be generation-checked handles (a viewport must fail closed when its scene
  World dies).
- **ADR**: REN-17 mints the crd-ui ADR superseding ADR-0023 in place (the strike-in-place doctrine).

## 4. Decisions locked with the user (2026-07-24)

1. **Separate UI World per window** (same ECS machinery; cross-World viewport refs).
2. **All five flagship widgets in-band**: node editor · sequencer · curve editor · file browser · data grid.
3. **Compute-first (Vello-class) vector renderer**, CKIR-authored, CPU-oracle-gated.
4. **All three authoring surfaces staged in-band**: C++ builder + UI-as-data + reactive bindings.

## 5. Slice map

Audit → reflect → font → glyphs → shaper → vector → paint → ui-substrate → widgets → theme+anim →
data+bindings+undo → viewports → multi-window/tear-off → node editor → sequencer+curves → files+grid →
the editor shell + game-product capstones. REN·A runs first — everything above draws through the REN-1
frame graph. (Rows live in D-007; this doc is the WHY, the table is the WHAT.)

## 6. The final-look pass (2026-07-24 — performance, style, platforms)

**6.1 The "millions of things" frontier (REN-4 expanded + REN-33).** The 2015-2026 GPU-driven line:
multi-draw-indirect submission (AC Unity, GDC15) → two-phase HiZ occlusion (Aaltonen — draw last frame's
visible, build HiZ, test the rest; no queries, no popping) → meshlet/mesh-shader granularity (Turing+, our
B11 capability) with per-cluster cone/bounds rejection → GPU LOD selection → and the VISIBILITY BUFFER
(Burns-Hunt 2013; the UE5-era answer to quad overdraw: raster {cluster,tri} ids only, resolve materials in
compute — raster cost decoupled from material cost). BINDLESS descriptor indexing is the enabler that lets
one indirect batch span arbitrary materials. Nanite-class virtualized-geometry LOD DAGs (SIGGRAPH 2021) are
recorded as the FUTURE crush on top of this substrate — the meshlet+HiZ+LOD+MDI floor is the attainable
frontier now, and the 1M-instance @60fps/1440p gate is its honest number. Forward+ (Olsson 2012 clustered
lights, REN-3) stays the transparency/simple path; both paths share the frame graph.

**6.2 Style freedom (REN-34).** "Any style" is an architecture property, not a shader pack: a material-MODEL
seam (OpenPBR default, B-band BSDF families pluggable, custom models as GM-band modules) + NPR standards
(cel/ramp, three outline families — backface hull, JFA post, depth/normal edges — hatching/halftone/kuwahara
as post passes) + everything authorable in the node editor and hot-swappable through the D-band deploy
pipeline. The gate is the proof: one scene, three complete looks, zero engine changes.

**6.3 Cross-platform + web (REN-35).** The decisive fact: CKIR already emits WGSL and MSL — the years-hard
half (a portable shader compiler) is DONE and oracle-certified; what's missing is device backends. WebGPU 1.0
(2023+) has no bindless/mesh shaders ⇒ the scale paths degrade BY DECLARED CAPABILITY TIER (a checked
matrix, never a silent wrong render). Native Dawn/wgpu makes web gates CI-able; the browser build is the
strategic Browser/WASM goal's first light (single-threaded first; SAB threads laddered). Metal/macOS is the
named home waiting on hardware-in-the-loop (the honest-verification rule); Linux surface/present completes
on the user's VM alongside PLG-1.

**6.4 Interaction perfection (REN-17/18/19 amendments + REN-32).** The pointer state machine (hover pairs,
click-vs-drag slop, double/triple-click, pointer lock/wrap), the full typed-payload DnD model with the OS
bridge (files in/out) + OS clipboard with custom formats, state-driven styling, pen pressure/tilt, gesture
arbitration, Blender-class MODAL OPERATORS as preview transactions on the command buffers (numeric entry
mid-drag, Esc = bit-exact rollback), and the synthetic-input record/replay harness that makes interaction
semantics CI-gated — "perfect" as a regression property, not an adjective.
