# Renderer, crd-ui and CR-D007 execution contract

<!-- doc-role: contract -->
> Current shared contract; status lives in ROADMAP. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

This is the implementation contract, not a second tracker. **Order, status and every child slice live only in
[ROADMAP](../ROADMAP.md#master-table).** The [system audit](../research/2026-09-12-system-audit.md) explains the starting
point. The [inherited catalogue](rendering-ui-contracts.md) preserves the full renderer and UI requirements.

## Purpose and sequence

Execution obeys [strict master-row order](../ROADMAP.md#strict-sequential-execution). Finish each row's complete
contract before the next; prerequisites and unavailable review/hardware/publication gates cannot be bypassed.

Cerid is a modular application substrate for games, engineering, simulation, scientific computing and creative tools.
CR-D007 is its first complete editor application. A later DAW, notebook host or CAD workbench assembles the same public
modules; none requires a copy of the editor or a mandatory physics/3D dependency.

The 2026-09-12 direction is: **complete the retained renderer feature library, then build the full UI and CR-D007,
then finish hesap GPU/notebook, then resume media and the remaining D-007/engine plans.** Windows and Linux qualify
first. macOS and web are explicit later deliveries. The notebook is a CR-D007 document workspace and also has a thin
standalone host. These sequencing decisions replace the notebook-first and eylem-before-UI schedules.

Renderer work before UI includes RAH, RPL, GVA, LSH, ARG, RTX, MAT, TPR, VFX, TXS, VGE and OFF. Required compute/neural
dependencies are pulled forward as named renderer prerequisites. Their larger science/application programmes remain
tracked. Full runtime features, authored assets, text editing, reload, diagnostics schemas and measured qualification
precede UI. Actual visual-editor acceptance follows I2D-9; parent completion never skips that dependency.

This audit changes documentation only. Geometry, physics, numerical kernels, audio and unrelated implementation
modules are untouched. Future renderer slices consume their existing APIs. If a real prerequisite requires changing
an excluded module, identify that dependency before implementation; do not hide it inside renderer work.

## Execution and authoring

**CHIR is the high-level authoring language; CEIR is the canonical executable representation; CKIR is the shader/kernel
representation.** This follows ADR-0108/0109/0128. A UI-specific VM, node scheduler or scripting language would duplicate
the investment. UI documents describe stable identity, hierarchy, layout, style and bindings. Behaviour is authored
CHIR/CEIR, paint/compositing is authored CEIR frame structure, and device algorithms are authored CKIR.

CHIR-0 is a five-construct prototype, not a complete application language. LANG rows close real expressions, typing,
functions, structured values, state, events, errors, async/cancellation, diagnostics and execution parity before any
UI feature depends on them. There is no placeholder-constant implementation of a live binding. CEIR remains directly
authorable while CHIR grows. C++ provides owned memory, platform adapters, compiler/provider mechanisms and native
intrinsics; it does not become a privileged home for a shipping rendering technique or UI behaviour.

The replacement test applies to every shipped rendering/UI algorithm: change its asset, or shadow its `engine://`
name from an application, cook and install it while the application runs, and observe the new behaviour without an
engine rebuild. Delete the superseded algorithm builder. Graph-editor layout metadata is separate from semantic
identity; moving nodes must not recompile a program. Domain frontends all converge into the same canonical assets.

The authoring host may read source and invoke cookers. The execution runtime installs verified cooked artifacts.
Shipping builds consume cooked packs; source import/compiler availability is an explicit development-host capability.
Small application configuration remains the existing documented config exception. Do not interpret runtime editing
as permission for an uncooked source fallback hidden in the shipping renderer.

## RAH completion

RAH-0 refreshes every field, constructor, consumer, backend and serialized representation against the current tree.
It records old/new ownership, migration order, removed flags, asset-field survival and a no-loss proof corpus. The
August audit is evidence of the original design, not the current census.

RAH-1a.2 removes `IGBufferTarget`, its creation/draw paths and `RenderingDesc::gbuffer` only after ordinary attachments
can express every former case. `RAH-1a-close` is a migration gate, not the parent close. The remaining children cover
signed/unsigned/float clear values, spans bounded by actual device limits, independent depth/stencil state and access,
typed views/subresources, MSAA/resolve, multiview and shading-rate/foveation contracts. Field-survival tests compare the
parsed source with the loaded artifact; writer/reader agreement alone can hide fields both dropped.

RAH-2 separates immutable resource descriptions, typed views, binding contracts, table allocation and residency. Stable
generation-checked indices must survive unrelated edits. Reuse CEIR effects/lifetime analysis; implement its physical
provider realization. Cover buffers, images, texel views, samplers, acceleration structures, arrays, access intent,
visibility, dynamic ranges and BDA capability. Stress above the old eight-texture cap; exercise eviction, reuse and
in-flight reload. Unsupported features reject before recording or choose an explicitly authored fallback.

RAH-3 replaces opaque command/geometry payloads with strong variants. RAH-4 supplies production AS/SBT/RT descriptions
and lifetime operations. RAH-5 supplies region-aware transfers and sparse/streaming operations. RAH-6 checks immutable
contracts at cook/install and dynamic values at recording. RAH-8 publishes actual limits, feature requirements,
selected fallback and evidence. A backend emitter is not a working platform runtime.

RAH-7 is a shared dependency registry, accessible without depending on `scene-render`. An edit stages source,
dependencies, interfaces, variants and every required backend object; publication is atomic only after the complete
replacement succeeds. A late compile/allocation/link failure retains the last good generation. GPU retirement follows
real completion across queues/windows, not an assumed number of application frames. Replace manual retire-all caches
and per-technique cook tags with registered generic recipes. Test unaffected programs retain identity, failed reloads
do not advance the visible generation, and graph/material/frame edits propagate along the exact dependency closure.

## Renderer library and qualification

Every inherited family and technique remains in scope. The catalogue is normative, including the full OFF-1…9 offline
integrators, advanced material domains, large-world geometry, virtual texture/geometry streaming, hybrid GI and neural
rendering. A reference integrator is an asset too. A feature-specific proof in CEIR-18 or CEIR-19 is reusable evidence,
not blanket production completion of the corresponding family.

Existing rendering implementations and their evidence are the starting point of each reuse census. Preserve their
capabilities while bringing them into the shared authoring, execution and editor contracts. Real-time viewports and
offline renders consume consistent scene/material/program identities, units and color-management contracts. Different
integrators and quality tiers may produce different images; unsupported settings must be diagnosed explicitly.
Every applicable renderer setting, variant, output and diagnostic must ultimately be accessible through CR-D007's
shared property/command/asset services, including preview, render progress, cancellation and saved output.

Each slice starts with a reuse census and a concrete scene/workload. Its detail note identifies knobs, units, error
model, supported capabilities, backend fallbacks and a CPU/reference path. Complete asset authoring, command emission,
both-backend execution, negative/error cases, reload and measured quality together. A supported CPU fallback/export
path is product work when contracted, not merely a test helper.

PQP-0/1 establish the benchmark corpus and comparison method before broad library work. Pin the peer revision,
machine, driver, compiler, precision, resolution, scene, sample count, timing boundary and warm/cold state. Compare
like fidelity and the peer's proper optimized path. Capture CPU/GPU time, compile/install latency, p50/p95/p99 input or
frame latency, VRAM/RAM, transient high-water marks, descriptor pressure and quality error. Record full boards in
`docs/bench/` at measurement time, including losses. There are no invented performance numbers in this plan.

Numerical equality is selected per operation: exact integer/serialization identity; bit equality where the specified
determinism tier permits it; stated ULP/absolute/relative tolerances for floating computation; statistical confidence
and convergence for stochastic estimators; perceptual and temporal quality for images. “Bit exact everywhere” must not
silently change the requested algorithm or disable the performance tier. Every tier has a tested, visible contract.

Full Windows qualification includes Vulkan and DX12. Linux includes real Vulkan plus software/reference lanes where
useful; a software renderer is not proof of discrete-GPU performance. CI carries broad configuration sweeps and
vendor/driver diversity. Device removal, OOM, corrupt packs, failed cooks, resize/minimize, multiple views, HDR/SDR,
suspend/resume and long sessions are explicit gates. Hardware-only features are tested on capable hardware and their
rejection/fallback is tested elsewhere. Metal/WebGPU releases require runtime, presentation, input, cook and execution
proofs on those platforms, not just generated shader text.

PRESENT establishes native multi-surface/window/DPI/HDR lifecycle and qualification before the renderer
library. WINDOW later integrates retained UI focus, dialogs and modal ownership on that same service.
Virtual-shadow-map page/residency work starts at LSH-3.core before VGE-4; LSH-3.runtime closes the combined
VGE/VSM rendering proof. Neither dependency is deferred past the renderer runtime gate.

## Shared application services

**Reflection:** `crd-reflect` owns stable property/type/schema identity, units, enums, ranges, labels, serialization
metadata, change events, typed property paths and flags for animation/undo/transactions. C++ annotation/codegen is
one producer. CEIR/CHIR and runtime-authored schemas must also participate; C++ declarations cannot be the only schema
source. Inspectors, animation, style, bindings, commands and MCP consume the same descriptors. Do not reuse ADR-0084:
that number already belongs to hesap resources.

**Commands:** evolve the existing hesap registry and `ceridc` handlers into an acyclic shared surface. A GUI action,
CLI call and RPC/MCP request address the same typed operation. Invocation is in-process for local UI, not a spawned
CLI process or text parser on each pointer move. Stable owner scopes remove callbacks before native unload. Commands
declare effects, capabilities, cancellation and validation. Schemas are versioned; unknown/stale handles reject with
specific diagnostics. Transport and script execution are not granted blanket filesystem/process authority.

**Transactions:** one document edit journal serves commands, bindings, gizmos, widgets and agents. A modal drag is a
preview transaction, with constraints, snapping, precision modifiers and numeric entry mid-drag. Escape restores the
exact original document; release commits one undo entry. Tooling selection/hover/gizmo construction is transient state,
not document edits. Undo is not automatically obtained merely by serializing commands: inverse/state snapshots,
coalescing, external effects, failed edits and cross-document boundaries need explicit contracts and tests.

**Documents/resources:** stable IDs, multi-document workspaces, asset dependency queries, deterministic cook, crash-safe
save/autosave/recovery, dirty/conflict states, schema migration, undo and reload integration. EDITOR.1 must reconcile the scene/öbek follow-ons linked in its prerequisites before claiming complete
project authoring: OCHN dependency reload, extract, cooker dispatch, composition/overrides, names and migrations.
Reuse CRDR/ResourceManager,
öbek/SCEN where appropriate. UiWorld data is its own schema; sharing persistence mechanisms does not make it a gameplay
entity. Reload preserves compatible focus, selection, scroll, editing and dock state; removed nodes cancel captures
and callbacks. Imported/untrusted files have bounded parse/cook resource use.

## Platform input and accessibility

Extend the existing platform events and `crd-app` propagation instead of creating another application event bus.
Input includes physical keys separately from committed text, composition/preedit/ranges/candidate geometry, pointer
identity/capture, high-resolution scroll, touch/pen pressure and tilt, gamepad navigation, relative pointer mode,
clipboard, file drops and drag/drop MIME data. Coordinate spaces are explicit: logical units, device pixels, window,
viewport and world. DPI/monitor changes reflow text and preserve pointer mapping; physical quantities use `crd-units`.

Window services cover multiple independent windows, presentation surfaces, focus, modal ownership, native dialogs,
cursor semantics, monitor work areas, DPI, color/HDR and lost/minimized surfaces. Windows and Linux input/IME adapters
are separately tested. Linux Wayland and X11 support are explicit matrix entries; the present CMake configuration
forces X11 off, so an X11 release cannot be claimed from existing CI alone.

Accessibility is a semantic tree present from the first retained nodes. Stable identity, role/name/value/state,
actions, text ranges, focus, live announcements and virtualized children feed native adapters: Windows UI Automation,
Linux AT-SPI, later macOS accessibility and a web accessibility projection. A bitmap on screen is not an accessible
control. Test actual keyboard-only and assistive-technology workflows, including node graphs, tables and timelines.
Localization includes logical start/end, plural/format rules, pseudo-localization, expansion and full RTL mirroring.

## Canvas, text, vector and retained UI

`UiWorld` owns interaction and semantic state with `UiNodeId`, independently of `SceneWorld::EntityId`.
`CanvasDisplayList` is an immutable typed paint product: shapes, images, glyph runs, paths, gradients, transforms,
clips, layers and material/effect references. `CanvasCompositor` executes authored frame/program assets through the
existing CEIR/render-graph services. Custom effects declare bounds, sampling dependencies, cacheability, isolation and
damage expansion. Arbitrary shader behaviour cannot silently violate clipping, batching or cached layers.

Retained layout includes constraints/flex/grid, min/max/intrinsic size, baseline, overflow, scrolling and transforms.
It has deterministic measurement, bounded invalidation, subtree caching and inspectable layout results. Document
assets, reconciled C++ builders and direct APIs produce one retained model. No editor-only fast path bypasses the
public downstream API. Every supported UI behaviour is replaceable through the authored program contract.

Cerid owns font parsing/shaping/rasterization. FreeType/HarfBuzz/msdfgen are comparative oracles where appropriate,
not product dependencies. Font assets carry outlines, metrics, fallback, variations, shaping and color-font data;
offline cooking and runtime glyph cache both work. Small text uses coverage/hinting; larger text uses suitable
MSDF/MTSDF/vector representations. World-space and extruded 3D text from the older font contract remain explicit.

Text editing uses grapheme boundaries rather than bytes/code points, logical/visual bidi cursor mapping, word/line
breaks, selection affinity, ligatures, fallback, composition, rich spans, clipboard and undo. Turkish dotted/dotless I,
Arabic/Hebrew, Indic, CJK, emoji/ZWJ and combining marks are required corpora. Font changes invalidate shaping/atlas/
layout through dependencies. Large editable documents require an appropriate persistent text structure and incremental
layout; a giant string relayout on every keystroke cannot close the code-editor slice.

Vector work retains the compute-first design and a supported CPU/hybrid route. Compare complete pipelines for tiny
icons, mostly static panes, huge paths and animated graphs before choosing schedules. Curves, joins/caps, dash,
winding, self-intersections, transforms and coverage must agree with a reference; clipping and overlapping alpha
must be seam-free. Backdrop effects need real region capture, correct ordering, nested layers and damage tracking.
The CEIR-31 rectangular glass proof is only a starting fixture.

## Complete widget and editor requirements

Visual quality is a completion requirement (user reaffirmed 2026-09-12). I2D-5 establishes a coherent, authorable
design system: typography, spacing, color, contrast, icons, focus/selection states and purposeful motion. I2D-PQ
reviews real editor workspaces at multiple DPI scales and densities, with golden images and interaction evidence;
a functionally complete collection of visually inconsistent widgets cannot close the UI programme. Renderer PQP
likewise judges representative real-time and offline images, temporal stability and convergence alongside timings.
The aesthetic direction is demonstrated in the CR-D007 shell and refined through actual editing workflows.

I2D-8 includes labels/buttons/toggles, check/radio, numeric/unit/expression fields, sliders/dials, color/gradient
editors, progress/status, combos, menus/context menus, tooltips/popovers/modals, splitters/tabs/docks, file dialogs,
virtual lists/trees/tables/data grids, property grids, search/filter/sort, rich text and drag/drop. Every control has
keyboard focus, accessibility, disabled/error/empty/busy states, localization and theme behaviour. Huge virtual data
sets preserve selection and accessibility without instantiating a UI node for every underlying item.

CR-D007 begins as the I2D-4 shell after the renderer, Canvas, text and retained UI foundations. The remaining widgets
are built in that real shell. Product panels use `crd-ui`; ImGui remains a debug/recovery overlay. The shell supplies
workspace persistence, project create/open, assets/import/cook/dependency browser, outliner, reflected inspector,
console/log, command palette/keymaps, documentation/help and CPU/GPU/memory profiling.

The viewport has orbit/fly/orthographic navigation, multi-view layouts, selection/picking, transform gizmos, snapping,
local/world/pivot modes, grids, measurements, overlays, camera/light/material authoring and scene/öbek workflows.
Game view and edit view have separate transient state. Play/pause/step/stop restores the edited document; applying
play-mode changes is an explicit command. Backend/adapter selection is exposed in menus with capability/fallback
diagnostics. Switching rebuilds GPU state transactionally while preserving document/undo/UI state; failure retains a
usable previous backend or a clear recovery surface. A menu item that merely changes a label is not integration.

The shared graph widget owns presentation/interaction only. CEIR owns executable semantics. Typed domain adapters
provide schemas, pin types, validation, serialization and previews. Required interactions: search/palette, pan/zoom,
box/multi-select, connection/reconnection, reroutes, groups, subgraphs, copy/paste, diff/merge, comments, breadcrumbs,
layout, diagnostics, progress/cancel and undo. Frame graphs, CKIR, materials/techniques, CHIR behaviour, geometry/
deformation and compute/AI workflows must each edit a real persisted asset and execute/preview through its provider.
Comfy-like workflows additionally need typed image/tensor/model ports, execution provenance, caching, queue/progress
and cancellation. Blueprint-like behaviour requires state/event/async semantics, not a graph-shaped shader editor.

Sequencer/timeline uses rational time, tracks/clips, trimming/slip/ripple, snapping, markers, time scales, keyframes,
curve/tangent editing, playback/scrub, loop/range, undo and previews through the existing timeline/animation substrate.
Payload adapters allow cinematic shots, automation and later audio/MIDI tracks. This ships the full reusable widget;
DAW audio engines/codecs/plugins remain their separate, retained programme. Code editor includes diagnostics/source
mapping, syntax, completion/search, large documents and real CHIR/CEIR/CKIR editing. Inspectors include resources,
passes, hazards, aliases, timings, variants, dependencies, rejected reloads and capture/regression evidence.

Renderer-wide visual authoring acceptance occurs after the corresponding I2D-9/D7E widget exists: use CR-D007 to
change each technique, persist/recook/reload, undo/redo and reopen. C2's completed schemas are reused. They are not
counted as completed widgets. No extra standalone D7E UI framework is created.

## Publishing and application acceptance

CR-D007 must create a project, import/cook content, edit a scene and behaviour, play it, save/reopen it and export a
standalone application. Export has authored target/configuration/module/asset-root/entry-point presets, transitive
asset closure, builtin/app override resolution, program variant/cache policy, build diagnostics and reproducible
pack assembly. An exported game runs on a clean Windows/Linux machine without editor/source/build-tree dependencies.
CLI/MCP invokes the same export. A resource archive alone does not satisfy “publish a game.”

Sample acceptance includes a 3D game with viewport/gizmos/UI, a 2D scene with the SPR renderer, an engineering viewer,
a frame/material/CKIR authoring workspace, a typed AI graph and a cinematic sequencer. Test scale, recovery and
accessibility on real documents. Application packaging strips unused modules and editor tooling. Native extension
reload and versioned ABI are tested separately from CEIR asset reload. Remote collaboration is a retained explicit
networking/application extension; document conflict handling and semantic diff are required now.

## Hesap notebook and continuation

After full UI/CR-D007, HGP finishes the scientific GPU operation families and notebook. The notebook has persisted
ordered cells, run/stop/restart, dependency-aware execution, errors with source locations, CPU/GPU selection, workspace
variables, virtual tables, 2D/3D plots, units, profiling, reproducibility, import/export and headless agent execution.
Use the shipped hesap command corpus and CPU oracles; complete missing CHIR scientific syntax by extending the same
language. A notebook cell must compute and display a real result, survive save/reload and support cancellation.
Both the CR-D007 workspace and standalone host consume the same public document/kernel/UI APIs.

Geometry/eylem notebook adapters are retained after those consumers are ready; they do not hold the usable scientific
notebook hostage to an unrelated full physics phase. The HGP-6 parent keeps that integration child visible until done.
Media, codecs, audio devices, plugin hosting and later DAW/NLE applications follow. The original MED-1…12 codec IDs
remain canonical; MED-GPU-* distinguishes the conflicting PR-7 media architecture numbers. No dated patent-expiry
claim is treated as automatic permission to ship a codec; MED's distribution review resolves its existing exclusions
and contradictory scope before implementation. This is a recorded unresolved contract, not a legal conclusion.

## Working discipline

The [system quality contract](system-quality-contract.md) adds allocator/job lifetime, trust, target hardware,
agent control and close-out obligations. The [whole-system review](../research/2026-09-12-cerid-whole-system-review.md)
extends future domains. It preserves this renderer → UI/CR-D007 → notebook order and the full retained catalogue.
AI training and inference qualify together; the neural renderer's required training/runtime dependencies land before UI.

The one table uses stable IDs. Existing IDs survive; additional child IDs are planning decomposition, not claims
that an ADR previously specified them. A parent closes only when all children and inherited requirements pass.
Before implementing a large row, write a focused design note with the reuse audit, exact increments, tests, budget
and deletion targets, link it from that row, and obtain the applicable architecture review. This plan does not pretend
to have designed hundreds of future algorithms in implementation detail.

Local verification is changed modules plus their blast radius, scoped CTest and incremental tidy, bounded GPU tests
with validation capture and CPU/reference comparisons. Broad platform/configuration sweeps are CI work. Docs changes
check references, unique slice IDs, ordering/dependencies and scope conservation. Session logs and boards retain dated
evidence; they do not carry a second live Next list. The user owns commits; no agent commit/push or AI trailer.

## Original GPU programme reconciliation

The [v17 source plan](../phases/phase-3.1.6-v17.md) remains a requirements record. Its a/b/c compiler/runtime
scope is verified at V17-RECONCILE; e maps to AS-4/6, f to CGP-4, g/k to HGP-0, h to CGP-1, i to CGP-0/HGP-4,
j to HGP-1, l to MLR-0/1 and m to HGP-4. Its d/n/o backend scope remains explicit MAC/WEB/ROCM work.
HESAP-GPU-ALL closes the original v17-z six-backend/peer-board contract after those releases; a usable
Windows/Linux notebook need not falsely claim all six backends are already qualified. Original IDs are
also routed in the audit JSON. D-004 replay and D-005 remaining reload consumers retain their own master rows.
