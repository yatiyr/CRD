# Cerid system and documentation audit

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The execution plan is [ROADMAP](../ROADMAP.md#master-table). This report is a dated evidence record, not a second
tracker. Implementation rules are in the [execution contract](../design/renderer-ui-execution-contract.md); the full
inherited requirements are in the [catalogue](../design/rendering-ui-contracts.md).

## Direction established

Cerid began this detour to finish hesap GPU computing. Portable kernels required CKIR; rendering exercised CKIR;
editable rendering structure required authored frame graphs; general authorable execution required CEIR and CHIR.
CEIR-1…35 now supplies that execution foundation. It does not by itself deliver every renderer family, a full
application language, UI widgets or an editor.

The confirmed order is **the entire retained renderer feature library → full crd-ui and CR-D007 → hesap GPU and
MATLAB-like notebook → media and remaining D-007/engine programmes**. Windows/Linux release first; macOS and web
explicitly follow. The notebook is both a CR-D007 workspace and a thin standalone host. Redundant files may be removed
after unique material is preserved. Geometry, physics and unrelated implementation modules are outside this audit's
mutation scope. The single table retains their future work.

## Coverage and limits

The initial inventory contained **874 Markdown files under docs/**: 429 sessions, 124 decisions, 90 benchmark records,
60 system docs, 47 top-level phase docs, 44 research docs, 30 design docs, 17 lessons, 9 detour docs and the remaining
rules/reference material. Including AGENTS, root README and context produced 877 captured documents. There are **96
engine module CMake files**. These counts describe the scanned working tree, not a claim that 96 production-ready
modules exist. The working tree already contained a large uncommitted CEIR batch.

Review depth: all documentation paths/classes and phase-plan structures were inventoried; module declarations,
dependencies, public-header/source/test locations and overview coverage were scanned across all 96 modules. Renderer,
asset reload, CEIR/CHIR, input/window/application and command/publication boundaries received focused source review.
The requested core rules, memory index, current state, roadmap, relevant ADRs and CEIR-31/32/33/34/35 close evidence
were read. Future domain specifications were reviewed for scope conservation and dependency placement.

This is an **architecture and planning audit**, not an exhaustive line-by-line correctness audit of every algorithm.
No new engine build, GPU run or benchmark was performed. Dated test results are attributed to their session/bench
records; they are not represented as newly rerun results. Negative source observations identify missing contracts or
coverage questions, not an unobserved crash. The machine-readable [inventory and migration record](2026-09-12-system-audit.json)
preserves source hashes, module records and source-ID routing for subsequent review.

## The system that exists

**Foundation:** core, memory/VM, containers, math/units/time, logging, jobs, platform/app, config and perf provide the
owned C++ substrate. Resources supplies CRDR identity, loading, ownership, streaming and reload mechanisms. Scene is
the separate gameplay/simulation world with hierarchy, components, queries, relations, transforms and persistence.
These are reusable foundations; UI must not duplicate their mechanisms or inherit a mandatory game-world identity.

**Numerics and geometry:** hesap spans dense/sparse linear algebra, orderings, iterative/direct methods, optimization,
ODE/DAE, FFT, DSP/wavelets/comms, special functions/stats, interpolation/quadrature/differentiation/motion, tensors and
autodiff. CPU v0–v16 close evidence exists; GPU completion and notebook remain. Geometry supplies reusable primitives,
spatial queries, mesh processing, curves and other algorithm families. Eylem remains paused at its recorded v1b scope.
Their overviews/phase contracts remain accessible; no implementation work was performed in these families.

**Executable assets:** CHIR is the high-level source model; CEIR the canonical execution IR/compiler/lifecycle;
CKIR the device kernel/shader representation. The host/GPU/cook bridges are separate modules. GPU-context owns the
public device/program/command facade, with Vulkan/DX12 graphics providers and CUDA compute. CKIR also has Metal,
WebGPU and HIP emitters; this is not evidence of full CR-D007 runtime support on those platforms.

**Rendering:** render-asset-core, render-program/material/pass/graph, the cookers, scene-render and draw consume
authored assets. `.frame.toml` converges through CEIR under ADR-0127. Resources, programs, materials and graphs have
their own identities. The scene renderer loads algorithms instead of constructing their KGraphs; hardening still
must remove special cases in resource/command descriptions and manual cache/reload policy.

**Application consumers:** anim/timeline/preset/profile and the audio substrate already exist. `ceridc` and the hesap
command registry are real reuse candidates. Sandbox uses ImGui for development. There are no `engine/ui`, `canvas`,
`font`, `vector` or `reflect` module directories at the audited baseline. A semantic UI, OS text/accessibility services,
transactional document tools, large graph widgets, CR-D007 and game export remain explicit work.

## Findings and their owning work

<a id="finding-a01"></a>
**A01 — Active scheduling drift.** The old roadmap retained eylem-before-UI and C++-only planning. The previous
renderer/UI plan put a notebook slice before full UI. Both are superseded by the confirmed direction above.
Owner: AUD-1. Historical prose is preserved in the consolidation archive; it no longer controls execution.

<a id="finding-a02"></a>
**A02 — UI identity contradiction.** PRINCIPLES, AGENTS, ADR-0020/0023/0057 and old UI phases contained UI-in-SceneWorld
language. ADR-0107 D2 records the later separate-UiWorld choice. In-place supersession notes now identify the current
boundary. Owner: I2D-0; the whole ADR-0107 remains Proposed until accepted.

<a id="finding-a03"></a>
**A03 — Retired graphics APIs in plans.** Old UI/font/platform docs name `crd-rhi`, `crd-renderer`, `crd-shader` and
`IRenderPath` as future targets. Those modules were retired under ADR-0105. Current contracts name gpu-context,
CEIR/CKIR and authored render graphs. Historical contracts retain unique requirements but not execution authority.

<a id="finding-a04"></a>
**A04 — RAH-1 parent scope was understated.** `command_model.hpp` retains fixed color/binding arrays, combined depth
description and legacy G-buffer pointer. Float/Uint clear migration is a subset of the full signed/unsigned/float,
view/subresource, depth/stencil, resolve and multiview requirements. Owner: RAH-1 and its visible children.
Evidence: [command model](../../engine/gpu/gpu-context/include/crd/gpu/command_model.hpp), [RAH-0 note](../systems/rah-0-canonical-model-audit.md).

<a id="finding-a05"></a>
**A05 — Resource planning is not resident binding implementation.** CEIR-12d/14d analysis and structural tests do
not replace provider-managed descriptor lifetimes. `ResourceBinding` still has small pointer arrays and
[BindingKind](../../engine/rendering/render-asset-core/include/crd/renderasset/binding.hpp) has six kinds. Owner: RAH-2.

<a id="finding-a06"></a>
**A06 — Untyped geometry and RT/transfer special cases remain.** The canonical model still contains `native_args`,
`ray_tracing_pipeline` and a limited transfer description. Full strong variants, views and production RT/streaming
contracts are not implied by one working draw or ray-tracing demo. Owner: RAH-3/4/5/6.

<a id="finding-a07"></a>
**A07 — Reload is not yet generic and granular.** The manifest replaced one imperative list, but
[scene_renderer.cpp](../../engine/rendering/scene-render/src/scene_renderer.cpp) retains `CookTag`, per-technique loaders,
`retire_all_programs`, `prepare_reinit` and `rebuild_programs`. Owner: RAH-7. CEIR-34's recorded closure remains valid
for its scope; it did not claim this complete registry refactor.

<a id="finding-a08"></a>
**A08 — Late replacement failure has a coverage concern.** `src_commit` discards the result of `rebuild_programs`
and advances a generation, while staging validates source earlier. A failure after successful parse needs an injected
backend compile/allocation/link proof. This is a source-observed risk, not a reproduced failure. Owner: RAH-7.d.

<a id="finding-a09"></a>
**A09 — Reload service ownership and GPU retirement need completion proofs.**
[reload.hpp](../../engine/rendering/scene-render/src/reload.hpp) lives privately in scene-render and includes a frame-count
retirement queue. UI should consume a shared service without a 3D dependency. Multiple queues/windows need explicit
completion ownership. Owner: RAH-7.a/b/d.

<a id="finding-a10"></a>
**A10 — UI draft lost real scope.** I2D-5 was defined in U-14; I2D-8 includes the complete widget library, not merely
platform adapters. The restored catalogue and I2D child rows retain both. Owner: AUD-1/I2D-5/8.

<a id="finding-a11"></a>
**A11 — More renderer work survived than the prior summary carried.** TXS-0…3 are real table IDs. D6 joint VS/FS
specialization, OFF-1…9, B16 follow-ons and unfinished AS-4/6 must be routed explicitly. Source row glyphs sometimes
disagree with their embedded close text; recensus instead of declaring either automatically correct. Owner: D6,
OFF, REN-OCEAN and AS rows.

<a id="finding-a12"></a>
**A12 — CEIR qualification was narrower than renderer production quality.** CEIR-35 chose close-and-route for
feature qualification; C2 closed domain contracts and retained widgets in I2D-9. Its three A/B boards were execution
measurements, not claims of beating renderer peers. Owner: PQP and renderer family acceptance.
Evidence: [35 close](../sessions/2026-09-11-ceir-35z-band-close.md), [decisions](../design/ceir-35z-user-decisions.md),
[D7E census](../design/ceir-33-d7e-orientation-census.md).

<a id="finding-a13"></a>
**A13 — CHIR-0 is not full application programming.** The prototype has no complete type system; `parallel_for`
does not yield a CEIR result and some feedback resolves to a constant. Multi-handler state and realistic app events
need work. The text/graph parity and identity/reload proof is valuable but not a complete behaviour language.
Owner: LANG. Evidence: [lowering](../../engine/execution/chir/src/lower.cpp), [32 close](../sessions/2026-09-06-ceir-32z-band-close.md), ADR-0128.

<a id="finding-a14"></a>
**A14 — UI rendering proof is not a UI framework.** CEIR-31's frosted-glass fixture uses full backdrop copy and a
fixed rectangular mask. It does not provide arbitrary retained panels, clipping, damage, layout, input or accessibility.
Owner: I2D-1/3/7. Evidence: [31 close](../sessions/2026-09-06-ceir-31z-band-close.md).

<a id="finding-a15"></a>
**A15 — Input readiness is incomplete, propagation already exists.**
[platform input](../../engine/foundation/platform/include/crd/platform/input.hpp) supplies a limited key/mouse queue.
[application.cpp](../../engine/foundation/app/src/application.cpp) already supplies handled event propagation. Extend these;
do not build a duplicate event bus. Text/IME, pointer/touch/pen/gamepad, clipboard/drop and DPI/native accessibility
require their own public contracts. Owner: INPUT/WINDOW/I2D-8.

<a id="finding-a16"></a>
**A16 — Command and reflection coverage is partial.**
[hesap registry](../../engine/numerics/hesap/include/crd/hesap/cli/command_registry.hpp) and
[ceridc MCP](../../tools/ceridc/src/mcp.cpp) exist. They do not constitute a full shared command/transaction/reflection
platform. The old platform plan incorrectly reserved ADR-0084, already used for matrix resources. Owner: REFLECT/CMD/DOC.

<a id="finding-a17"></a>
**A17 — Game publishing was missing as a concrete editor acceptance gate.** A shell and six panels do not prove
standalone game export. Project/module/asset closure, authored behaviour, play/edit isolation, target presets,
clean-machine execution and CLI parity are now explicit EDITOR children. Runtime backend selection must exercise
the actual device/renderer lifecycle, not only a settings field.

<a id="finding-a18"></a>
**A18 — Platform support was overstated by old prose.** The platform plan called macOS supported while describing
its backend as future. Root CMake forces Linux X11 off, despite suggesting an override. Compiler/emitter support,
runtime/device execution and application qualification are distinct. Owner: WINDOW.3, MAC, WEB and PQP-3.

<a id="finding-a19"></a>
**A19 — Font ownership contradicted newer direction.** ADR-0047/Phase 3.3 specified product FreeType/HarfBuzz;
U-20 specified Cerid-owned fonts and shaping, with third-party oracles. Preserve the older offline/dynamic atlas,
world-space and extruded text requirements, supersede the dependency choice in place. Owner: I2D-2/6 and FONT-3D.

<a id="finding-a20"></a>
**A20 — Media had overlapping identifiers and contradictory codec scope.** The main table uses MED-1…12 for codecs;
PR-7 reused MED-0…3 for architectural bands and named HEVC despite the codec-band exclusion. MED-GPU-* now identifies
the latter. Exact patent-expiry and royalty-free assertions in old notes remain unverified; no release can inherit
legal clearance from them. Owner: MED-REVIEW. Media implementation remains after the notebook.

<a id="finding-a21"></a>
**A21 — Audio CEIR migration is incomplete beyond the proof.** The acyclic CEIR audio proof compares with the kept
audio graph; feedback execution and full real-time/device deployment are separate. Linux audio and PLG-1…6 remain
tracked. Owner: AUDIO-CEIR/PLG. Evidence: CEIR-31 close and [audio sources](../../engine/media/audio/src/audio_graph.cpp).

<a id="finding-a22"></a>
**A22 — Stale overview/maturity claims.** The systems index simultaneously said CEIR had no overview and linked
one, and described completed bands as in progress. CEIR's overview carried an old feature count and called CEIR-33
unparked. Use the capability generator for counts, close records for scope, and this roadmap for planned work.
Owner: AUD-1. A generator is a view, not another editable tracker.

<a id="finding-a23"></a>
**A23 — Old debt is evidence requiring recensus.** Scene cooker, schema, transform and öbek follow-ons span older
states; some declaration fields may now exist without the whole consumer path. Keep every numbered obligation and
test the current consumer before marking it done. Known measured numerical losses are preserved, not silently renamed
as successes. Owner: DEBT rows; unrelated code remains untouched.

## Module boundaries

<a id="finding-a24"></a>
**A24 — Declared foundation dependency cycle.** Directly inspected CMake edges are
`memory → log → containers → memory` and `memory → vm → log`.
The containers comment discusses breaking a direct containers→log link, but the indirect cycle remains in the
declarations. Static libraries being linkable would not make the one-way-module principle true. This report makes
no claim of a build failure. Record the ownership problem and resolve it at the authorized foundation slice.
Evidence: [memory](../../engine/foundation/memory/CMakeLists.txt), [log](../../engine/foundation/log/CMakeLists.txt),
[containers](../../engine/foundation/containers/CMakeLists.txt), [vm](../../engine/foundation/vm/CMakeLists.txt). Owner: FOUNDATION-AUDIT.

<a id="finding-a25"></a>
**A25 — Generic draw still couples to Vulkan.**
[draw CMake](../../engine/rendering/draw/CMakeLists.txt) publicly links gpu-context-vulkan and
[renderer.cpp](../../engine/rendering/draw/src/renderer.cpp) includes its concrete header; the public constructor now accepts
the generic `IGpuContext`. Recheck actual uses before removing the edge. This is a modularity concern, not a claim
that DX12 drawing fails. Owner: RAH-DRAW.

<a id="finding-a26"></a>
**A26 — Reuse has real dependency costs.** `hesap-interp` links geometry-delaunay/dense/FFT and `scene` links geometry
spatial/primitives. A standalone UI can reuse interpolation but must measure actual binary/dependency exclusion;
“UI has no scene dependency” does not imply that its transitive dependency set is tiny. Existing services should be
factored only against actual consumer needs, not duplicated. Owner: REFLECT/DOC/I2D-3/PQP-4.

<a id="finding-a27"></a>
**A27 — Future ADR references presented as existing files.** Eylem ADR-0062/0067 linked planned ADR-0070…0074
documents that have not been filed. The links now explicitly say planned and lead to the retained phase contract.
No architecture decision was invented or accepted to satisfy a link check. Owner: AUD-1 for reference repair;
the corresponding EYLEM rows retain implementation and decision work.

The final ordering check also exposed two prerequisites now represented directly: PRESENT supplies native
multi-window presentation before renderer qualification; LSH-3.core supplies VSM residency before VGE integration.
Editor-triggered scene/öbek follow-ons were moved to EDITOR.1 prerequisites. Original v17 backend obligations now
have explicit ROCM, MAC, WEB and HESAP-GPU-ALL owners; D-004/D-005 remain in the one master table.

## Research conclusions

Blender's RNA demonstrates why property metadata is wider than an inspector: it serves UI, animation, overrides
and exposed APIs. Cerid should preserve that common descriptor model while admitting runtime-authored schemas,
not only parsed C++ signatures. This is a design recommendation based on the source, not a claim of RNA equivalence.
[Blender RNA documentation](https://developer.blender.org/docs/features/core/rna/), accessed 2026-09-12.

Custom-drawn controls need accessibility providers; drawing pixels does not expose roles, actions or text to
assistive clients. This supports a semantic tree from UiWorld's first nodes, followed by native adapters and real
screen-reader acceptance. [Microsoft UI Automation Providers Overview](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-providersoverview),
updated 2025-07-14, accessed 2026-09-12.

IME composition and committed text have different semantics. Intermediate composition should not become an undo
entry; committed replacement should. Cursor/selection/format queries must cross the native boundary. Qt is used
as a behavioural reference, not as Cerid's implementation dependency.
[Qt QInputMethodEvent](https://doc.qt.io/qt-6/qinputmethodevent.html), accessed 2026-09-12.

Unicode text segmentation defines grapheme/word/sentence boundaries; line breaking and bidi are separate algorithms.
The plan therefore pins conformance data/version and maps logical to visual positions rather than treating a code
point as a user-perceived character. [Unicode UAX #29](https://unicode.org/reports/tr29/), accessed 2026-09-12;
[Unicode report index](https://www.unicode.org/reports/index.html) identifies UAX #9 and #14 as separate specifications.

Vello's current repository distinguishes experimental compute-heavy rendering, CPU rendering and a GPU renderer
with substantial CPU preprocessing intended for production use. This does not overturn Cerid's compute-first
decision; it does refute assuming that all-GPU processing is universally optimal. Benchmark complete representative
workloads and keep the contracted CPU/hybrid path. [Linebender Vello](https://github.com/linebender/vello), accessed
2026-09-12. No Vello performance result is claimed for Cerid.

Godot's export documentation distinguishes an executable project export from exporting only a resource pack, and
uses target presets for command-line export. Cerid's game-publishing gate accordingly requires an actual standalone
runtime plus assets, with GUI/CLI using the same project configuration. This is a product acceptance inference.
[Godot project export](https://docs.godotengine.org/en/stable/tutorials/export/exporting_projects.html), accessed 2026-09-12.

These sources refine acceptance criteria. Algorithm-level research, current standards and matched-peer experiments
remain required when each implementation starts. No external claim here substitutes for an engine proof.

## Documentation disposition

Only **ROADMAP** owns live slice/subslice status; **context** is a short current pointer. The documentation map,
system index, detailed contracts, ADRs and evidence remain useful reference material. Old planning schedules are
archived or replaced. A history document's old “Next” is not a live instruction. Unique old requirements are routed
into the master table and cited detail, rather than deleted because their original architecture is obsolete.

The consolidated archive preserves the pre-audit roadmap, superseded UI/editor/font/platform plans and the previous
renderer/UI proposal. The original D-007 programme remains archival evidence. The redundant UI-only redirect is
removed. The cleanup repairs local references and checks source-ID conservation. Historical sessions, benchmark
boards, recipes and accepted ADR evidence are retained; they are not rewritten to pretend new tests ran.

The external memory directory is read-only in this workspace. Its index was read; no agent-memory edits are claimed.
The requested `advisor` capability was not available in the tool catalogue. This report is a direct source review,
not an advisor approval. Architecture proposals are not silently marked Accepted.

## Module inventory

Final documentation verification (2026-09-12): **658 master rows**, **210 D-007 source IDs** and **16 v17 IDs**
are routed. The structural validator passed. The wider check covered **93 new/changed Markdown documents and
2,613 local links**, with zero unresolved paths/anchors. Ten superseded/redundant files were removed after source
preservation. `git diff --check` passed for documentation/rules/the validator; only Git line-ending normalization
notices were emitted. No engine build, runtime test or benchmark was run. This records documentation integrity,
not completion of the planned renderer/UI features.

The following generated inventory is a static CMake/header/test-location map. “Tests” means a sibling module CMake
file exists, not that its cases passed. Conditional backend edges are included conservatively. Use the linked
manifest and public source for implementation work.

| Module | Declared Cerid dependencies | Public headers / source TUs | Tests |
|---|---|---:|---|
| [anim](../../engine/world/anim/CMakeLists.txt) | containers, core, hesap-interp, math, memory, resources | 2 / 2 | Present |
| [app](../../engine/foundation/app/CMakeLists.txt) | containers, core, jobs, platform | 10 / 3 | Present |
| [asset-io](../../engine/assets/asset-io/CMakeLists.txt) | containers, core, geometry-bvh, geometry-mesh, geometry-primitives, math, memory, time, units | 12 / 11 | Present |
| [audio](../../engine/media/audio/CMakeLists.txt) | ceir, containers, core, hesap-dsp, hesap-interp, jobs, memory, resources, time | 10 / 9 | Present |
| [ceir](../../engine/execution/ceir/CMakeLists.txt) | containers, core, log, memory, units | 50 / 28 | Present |
| [ceir-cook](../../engine/execution/ceir-cook/CMakeLists.txt) | ceir, containers, core, memory, render-asset-core, resources | 4 / 4 | Present |
| [ceir-gpu](../../engine/execution/ceir-gpu/CMakeLists.txt) | ceir, containers, core, gpu-context, kir, memory | 14 / 13 | Present |
| [ceir-host](../../engine/execution/ceir-host/CMakeLists.txt) | ceir, containers, core, jobs, memory, perf, time | 2 / 2 | Present |
| [chir](../../engine/execution/chir/CMakeLists.txt) | ceir, containers, core, log, memory, units | 4 / 4 | Present |
| [config](../../engine/foundation/config/CMakeLists.txt) | containers, core, log, math, platform | 3 / 3 | Present |
| [containers](../../engine/foundation/containers/CMakeLists.txt) | core, memory | 17 / 2 | Present |
| [core](../../engine/foundation/core/CMakeLists.txt) | — | 6 / 3 | Present |
| [draw](../../engine/rendering/draw/CMakeLists.txt) | containers, core, gpu-context, gpu-context-vulkan, kir, log, material-cook, math, memory, scene, vertex-cook | 15 / 9 | Present |
| [draw-imgui](../../engine/rendering/draw-imgui/CMakeLists.txt) | core, draw, imgui | 1 / 1 | No sibling CMake |
| [eylem](../../engine/physics/eylem/CMakeLists.txt) | containers, core, math, memory | 13 / 3 | Present |
| [eylem-rigid3d](../../engine/physics/eylem-rigid3d/CMakeLists.txt) | containers, core, eylem, log, math, memory, scene | 5 / 4 | Present |
| [eylem-viz](../../engine/physics/eylem-viz/CMakeLists.txt) | core, draw, eylem, eylem-rigid3d, math, scene | 1 / 1 | Present |
| [frame-cook](../../engine/assets/frame-cook/CMakeLists.txt) | ceir, ceir-gpu, containers, core, gpu-context, log, memory, render-asset-core, render-graph, render-pass | 6 / 8 | Present |
| [geometry-bvh](../../engine/geometry/geometry-bvh/CMakeLists.txt) | containers, core, geometry-primitives, jobs, math | 11 / 8 | Present |
| [geometry-bvh-gpu](../../engine/geometry/geometry-bvh-gpu/CMakeLists.txt) | containers, core, geometry-bvh, geometry-primitives, gpu-context, jobs, math, memory, units | 8 / 8 | Present |
| [geometry-convex](../../engine/geometry/geometry-convex/CMakeLists.txt) | containers, core, geometry-primitives, math, memory | 13 / 3 | Present |
| [geometry-curves](../../engine/geometry/geometry-curves/CMakeLists.txt) | containers, core, geometry-primitives, math, memory, units | 15 / 2 | Present |
| [geometry-decomposition](../../engine/geometry/geometry-decomposition/CMakeLists.txt) | containers, core, geometry-convex, geometry-mesh, geometry-primitives, jobs, math, memory, units | 4 / 4 | Present |
| [geometry-delaunay](../../engine/geometry/geometry-delaunay/CMakeLists.txt) | containers, core, geometry-polygon, geometry-primitives, math, memory, units | 11 / 10 | Present |
| [geometry-mesh](../../engine/geometry/geometry-mesh/CMakeLists.txt) | containers, core, geometry-bvh, geometry-primitives, math, memory, units | 9 / 6 | Present |
| [geometry-mesh-processing](../../engine/geometry/geometry-mesh-processing/CMakeLists.txt) | containers, core, geometry-bvh, geometry-mesh, geometry-polygon, geometry-primitives, math, memory, units | 18 / 15 | Present |
| [geometry-polygon](../../engine/geometry/geometry-polygon/CMakeLists.txt) | containers, core, geometry-primitives, math, memory, units | 8 / 5 | Present |
| [geometry-primitives](../../engine/geometry/geometry-primitives/CMakeLists.txt) | containers, core, math | 20 / 4 | Present |
| [geometry-shader-helpers](../../engine/geometry/geometry-shader-helpers/CMakeLists.txt) | containers, core, geometry-primitives, math, memory, platform | 7 / 7 | Present |
| [geometry-spatial](../../engine/geometry/geometry-spatial/CMakeLists.txt) | containers, core, geometry-primitives, math, memory, units | 15 / 8 | Present |
| [geometry-viz](../../engine/geometry/geometry-viz/CMakeLists.txt) | containers, core, draw, geometry-bvh, geometry-curves, geometry-primitives, math | 5 / 4 | Present |
| [gpu-context](../../engine/gpu/gpu-context/CMakeLists.txt) | containers, core, render-asset-core | 8 / 2 | Present |
| [gpu-context-cuda](../../engine/gpu/gpu-context-cuda/CMakeLists.txt) | containers, core, gpu-context, memory | 1 / 1 | Present |
| [gpu-context-dx12](../../engine/gpu/gpu-context-dx12/CMakeLists.txt) | containers, core, gpu-context, kir, memory | 5 / 5 | Present |
| [gpu-context-vulkan](../../engine/gpu/gpu-context-vulkan/CMakeLists.txt) | containers, core, gpu-context, kir, memory, platform | 8 / 9 | Present |
| [hesap](../../engine/numerics/hesap/CMakeLists.txt) | containers, core, math, memory | 11 / 4 | Present |
| [hesap-amg](../../engine/numerics/hesap-amg/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-preconditioners, hesap-sparse, math, memory | 8 / 2 | Present |
| [hesap-autodiff](../../engine/numerics/hesap-autodiff/CMakeLists.txt) | containers, core, hesap, jobs, math, memory | 36 / 2 | Present |
| [hesap-comms](../../engine/numerics/hesap-comms/CMakeLists.txt) | containers, core, hesap, hesap-dsp, hesap-fft, hesap-stats, math, memory | 11 / 2 | Present |
| [hesap-dense](../../engine/numerics/hesap-dense/CMakeLists.txt) | containers, core, hesap, jobs, math, memory | 48 / 25 | Present |
| [hesap-diff](../../engine/numerics/hesap-diff/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-fft, math, memory | 6 / 2 | Present |
| [hesap-direct](../../engine/numerics/hesap-direct/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-iterative, hesap-ordering, hesap-sched, hesap-sparse, jobs, math, memory | 21 / 13 | Present |
| [hesap-dsp](../../engine/numerics/hesap-dsp/CMakeLists.txt) | containers, core, hesap, hesap-eigen, hesap-fft, hesap-special, jobs, math, memory, units | 30 / 2 | Present |
| [hesap-eigen](../../engine/numerics/hesap-eigen/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-direct, hesap-iterative, hesap-sparse, jobs, math, memory | 12 / 2 | Present |
| [hesap-fft](../../engine/numerics/hesap-fft/CMakeLists.txt) | containers, core, hesap, math, memory | 13 / 2 | Present |
| [hesap-interp](../../engine/numerics/hesap-interp/CMakeLists.txt) | containers, core, geometry-delaunay, hesap, hesap-dense, hesap-fft, math, memory | 13 / 2 | Present |
| [hesap-iterative](../../engine/numerics/hesap-iterative/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-sparse, math, memory | 19 / 1 | Present |
| [hesap-motion](../../engine/numerics/hesap-motion/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-special, math, memory | 10 / 2 | Present |
| [hesap-ode](../../engine/numerics/hesap-ode/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-direct, hesap-iterative, hesap-sparse, memory | 26 / 2 | Present |
| [hesap-opt](../../engine/numerics/hesap-opt/CMakeLists.txt) | containers, core, hesap, hesap-autodiff, hesap-dense, hesap-sparse, hesap-stats, jobs, math, memory | 49 / 2 | Present |
| [hesap-ordering](../../engine/numerics/hesap-ordering/CMakeLists.txt) | containers, core, hesap, hesap-sparse, memory | 9 / 9 | Present |
| [hesap-preconditioners](../../engine/numerics/hesap-preconditioners/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-iterative, hesap-ordering, hesap-sparse, math, memory | 19 / 1 | No sibling CMake |
| [hesap-quadrature](../../engine/numerics/hesap-quadrature/CMakeLists.txt) | containers, core, hesap, hesap-dense, hesap-special, memory | 16 / 2 | Present |
| [hesap-resources](../../engine/numerics/hesap-resources/CMakeLists.txt) | containers, core, hesap, hesap-sparse, math, memory, resources | 4 / 2 | Present |
| [hesap-sched](../../engine/numerics/hesap-sched/CMakeLists.txt) | containers, core, hesap, jobs, memory | 2 / 1 | Present |
| [hesap-sparse](../../engine/numerics/hesap-sparse/CMakeLists.txt) | containers, core, hesap, jobs, math, memory | 29 / 2 | Present |
| [hesap-special](../../engine/numerics/hesap-special/CMakeLists.txt) | containers, core, hesap, jobs, math, memory | 19 / 2 | Present |
| [hesap-stats](../../engine/numerics/hesap-stats/CMakeLists.txt) | containers, core, hesap-dense, hesap-quadrature, hesap-special, jobs, memory | 33 / 2 | Present |
| [hesap-tensor](../../engine/numerics/hesap-tensor/CMakeLists.txt) | containers, core, jobs, math, memory, platform | 20 / 3 | Present |
| [hesap-wavelet](../../engine/numerics/hesap-wavelet/CMakeLists.txt) | containers, core, hesap, hesap-fft, jobs, math, memory | 11 / 2 | Present |
| [imgui](../../engine/ui/imgui/CMakeLists.txt) | config, gpu-context, gpu-context-dx12, gpu-context-vulkan, log, platform, units | 5 / 3 | Present |
| [jobs](../../engine/foundation/jobs/CMakeLists.txt) | containers, core | 4 / 9 | Present |
| [kir](../../engine/gpu/kir/CMakeLists.txt) | containers, core, math, memory | 59 / 1 | Present |
| [kir-cuda](../../engine/gpu/kir-cuda/CMakeLists.txt) | containers, core, kir, memory | 2 / 2 | Present |
| [kir-dx12](../../engine/gpu/kir-dx12/CMakeLists.txt) | containers, core, kir, memory | 1 / 1 | Present |
| [kir-hip](../../engine/gpu/kir-hip/CMakeLists.txt) | containers, core, kir, memory | 1 / 1 | Present |
| [kir-metal](../../engine/gpu/kir-metal/CMakeLists.txt) | containers, core, kir, memory | 1 / 0 | Present |
| [kir-vulkan](../../engine/gpu/kir-vulkan/CMakeLists.txt) | containers, core, gpu-context, gpu-context-vulkan, kir, memory | 1 / 1 | Present |
| [kir-webgpu](../../engine/gpu/kir-webgpu/CMakeLists.txt) | containers, core, kir, memory | 1 / 1 | Present |
| [light-cook](../../engine/assets/light-cook/CMakeLists.txt) | containers, core, kir, memory | 1 / 1 | Present |
| [lod](../../engine/geometry/lod/CMakeLists.txt) | containers, core, geometry-mesh-processing, math, memory, resources | 3 / 3 | Present |
| [log](../../engine/foundation/log/CMakeLists.txt) | containers, core | 12 / 10 | Present |
| [material-cook](../../engine/assets/material-cook/CMakeLists.txt) | containers, core, kir, memory | 1 / 1 | Present |
| [math](../../engine/foundation/math/CMakeLists.txt) | core, units | 31 / 2 | Present |
| [memory](../../engine/foundation/memory/CMakeLists.txt) | core, log, vm | 21 / 13 | Present |
| [meshgen](../../engine/geometry/meshgen/CMakeLists.txt) | core, math, memory, resources | 1 / 1 | Present |
| [perf](../../engine/foundation/perf/CMakeLists.txt) | containers, core, jobs, memory, time | 13 / 5 | Present |
| [perf-ui](../../engine/ui/perf-ui/CMakeLists.txt) | core, imgui, memory, perf | 5 / 3 | Present |
| [platform](../../engine/foundation/platform/CMakeLists.txt) | containers, core, jobs, log, memory, time | 10 / 9 | Present |
| [preset](../../engine/assets/preset/CMakeLists.txt) | containers, core, memory, resources | 10 / 4 | Present |
| [profile](../../engine/foundation/profile/CMakeLists.txt) | containers, core, memory, resources | 8 / 3 | Present |
| [render-asset-core](../../engine/rendering/render-asset-core/CMakeLists.txt) | containers, core, memory | 7 / 6 | Present |
| [render-graph](../../engine/rendering/render-graph/CMakeLists.txt) | ceir-gpu, containers, core, gpu-context, memory, render-asset-core, render-pass | 1 / 1 | Present |
| [render-material](../../engine/rendering/render-material/CMakeLists.txt) | containers, core, memory, render-asset-core, render-program | 1 / 1 | Present |
| [render-pass](../../engine/rendering/render-pass/CMakeLists.txt) | containers, core, memory, render-asset-core | 1 / 1 | Present |
| [render-program](../../engine/rendering/render-program/CMakeLists.txt) | containers, core, memory, render-asset-core | 1 / 1 | Present |
| [resources](../../engine/assets/resources/CMakeLists.txt) | containers, core, jobs, log, memory, platform | 22 / 20 | Present |
| [scene](../../engine/world/scene/CMakeLists.txt) | containers, core, geometry-primitives, geometry-spatial, math, memory, resources | 30 / 18 | Present |
| [scene-render](../../engine/rendering/scene-render/CMakeLists.txt) | anim, containers, core, frame-cook, geometry-primitives, gpu-context, kir, light-cook, lod, log, material-cook, math, memory, render-asset-core, resources, scene, vertex-cook | 2 / 3 | Present |
| [shader-cook](../../engine/assets/shader-cook/CMakeLists.txt) | containers, core, gpu-context, gpu-context-dx12, gpu-context-vulkan, jobs, kir, memory, platform, resources | 4 / 3 | No sibling CMake |
| [technique-cook](../../engine/assets/technique-cook/CMakeLists.txt) | containers, core, frame-cook, kir, memory | 1 / 1 | Present |
| [time](../../engine/foundation/time/CMakeLists.txt) | core, units | 11 / 4 | Present |
| [timeline](../../engine/world/timeline/CMakeLists.txt) | containers, core, hesap-interp, memory, resources, time | 3 / 3 | Present |
| [units](../../engine/foundation/units/CMakeLists.txt) | containers, core, memory | 13 / 2 | Present |
| [vertex-cook](../../engine/assets/vertex-cook/CMakeLists.txt) | containers, core, kir, material-cook, memory | 1 / 1 | Present |
| [vm](../../engine/foundation/vm/CMakeLists.txt) | core, log | 2 / 2 | Present |
