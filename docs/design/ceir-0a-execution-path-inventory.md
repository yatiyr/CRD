# CEIR-0a — Execution-path inventory (FROM CODE)

> **Band:** D-007 · CEIR-0 (repository inventory + architecture ADRs). **Slice:** CEIR-0a.
> **Tracker row:** `docs/detours/D-007-ceir-tracker.md` → CEIR-0a. **Law:** the mission constitution
> `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` §127 (executor migration), §128
> (`scene.raster` proof), §126 (framegraph migration).
> **Status: ✅ COMPLETE** — every execution-program representation is inventoried from code with file:line evidence
> and classified composite-vs-atomic; the `IComputeContext` consumers (§5), the `render()` composite catalog / CEIR-13
> migration order (§4a), and the programmatic-builder sites (§9.3) are all enumerated. Gate met: table complete with
> file:line evidence, zero "per the docs" entries, §9 empty. Evidence from CODE (`git` working tree at commit
> `e24322e`), never from other docs (§127). Feeds CEIR-0h (deletion tables) + CEIR-0z (§184 report + sizing).

---

## 1. Headline finding — RAF already did the hard atomic-vs-composite split

The mission doc's central fear (§7) is a giant enum of composite algorithms (`ForwardPlus`, `Deferred`, `Nanite`…).
**That composite-algorithm-as-op enum does not exist in the tree** — RAF-6/RAF-12 already retired the `FramePassKind`
combinatorial switch and replaced it with a small set of **atomic capability verbs** (executors) that authored
**frame graphs** sequence. So the CEIR migration is largely a **promotion** of an already-well-factored structure
into a typed, verifiable, extensible IR — not a rewrite.

⚠ **Precise scoping — semantically right, mechanically pre-§6/§8.** The executor layer has the RIGHT *granularity*
(atomic verbs, not composite algorithms — the §100 `ForwardPlusExecutor`-is-forbidden rule is already honored). But
the *mechanism* is still pre-CEIR: `register_builtin_executors` is a **closed-world central C++ list in one
function** with a **hand-rolled schema struct** (`ExecutorSchema`), not §6/§8's generated open-world dialects with
verifier/printer/reflection hooks. Apps can call `register_executor`, but there is no dialect registry, no ODS-style
generator, no textual/binary IR form, no interfaces analyses dispatch on. **That gap is exactly what CEIR-1d
(dialect registry) + CEIR-2 (schema generator) build** — so the "promotion, not rewrite" framing applies to the
verbs' *semantics*, while CEIR-1/2 still have full substrate work to do on the *mechanism*. Do not read this
finding as shrinking CEIR-1/2.

Three layers already exist and map cleanly onto CEIR:

| Today (RAF) | CEIR target | §§ |
|---|---|---|
| Authored **frame graph** (`.frame.toml` → `FrameGraphTemplate` → `execute_frame`) — the composite SEQUENCER | `ceir.frame` dialect (CEIR-12) | §39 §126 §159 |
| The **14 executors** (typed schema + `record_*` command lowering) — ATOMIC verbs | `ceir.render`/`compute`/`transfer`/`rt` ops, each `record_*` → the op's canonical-command lowering (CEIR-10/11) | §40 §42 §50 §49 §158 |
| `scene_renderer.cpp` **orchestration + program hand-list** — composite logic still in C++ | CEIR program assets (CEIR-13/14) + host resolver intrinsics (§45) + the RAH-7 registry | §127 §128 §45 |

**Consequence for sizing (feeds CEIR-0z):** CEIR-13 ("executor migration") is misnamed for this tree — the
executors are already atomic and mostly stay as `ceir.*` op lowerings. The real CEIR-13 work is migrating
`scene_renderer.cpp`'s **C++ orchestration** (frame-graph construction, CSM setup, the program families' sequencing)
into authored CEIR assets, with the scene resolvers becoming host intrinsics.

---

## 2. The frame graph — the composite sequencer (→ `ceir.frame`, §39 §126)

| Element | Location | Currency in → out | Class |
|---|---|---|---|
| Authored frontend | `.frame.toml` → `FrameGraphDesc` (`engine/frame-cook/include/crd/framecook/frame_asset.hpp`) | TOML → `FrameGraphDesc` | **frontend** → `ceir.frame` (§39 §126.1) |
| Cooked→Template bridge | `build_frame_graph_template` — `engine/frame-cook/src/frame_template_bridge.cpp:717`; `map_raster`:277 · `map_compute`:441 · `map_rt_common`:565 · `map_mesh_indirect`:669 | `FrameGraphDesc` → `rg::FrameGraphTemplate` (per-pass `PassPayload`) | **composite lowering** → subsumed by CEIR-12 scheduling |
| Programmatic construction | `FrameGraphTemplate::add_pass` — `engine/render-graph/include/crd/rendergraph/frame_graph.hpp:134` | C++ calls → `FrameGraphTemplate` | **frontend** → CEIR builder (§126.2 §121) |
| Runtime execution (ONE submission) | `execute_frame` — `engine/render-graph/src/frame_graph.cpp:1337`; `RecordContext`:76 | `CompiledFrameGraph` + `GraphExecutorTable` → device commands | **runtime** → executes CEIR plans (§126.5) |
| Validation | the 38-D4-class cook checks (varying contract, declared-header words) in the cookers | desc → diagnostics | **verifier** → CEIR verifiers (§126.3 §115) |

**One runtime, verified from code (ADR-0106 holds).** `engine/frame-cook/src/frame_runtime.cpp` (1,388 lines) is
**not a second execution path** — it is the frame-cook ADAPTER that drives a cooked `FrameGraphDesc` *through* the
render-graph runtime. Its header comment (`frame_runtime.hpp:3-9`) is STALE ("record the right `draw_*` per
`FramePassKind`"), but the code is current: `:109` "RAF-8a (ADR-0106) the migration adapter … records through the
render-graph executor," `:201`/`:476`/`:1229` "the FramePassKind switch + per-kind wrappers are **retired**," keying
off executor id + role bits instead. The only residual `switch` (`:426`) is a param-**type** marshal
(Float/Int/Bool/Enum/Vec4 → `TypedValue`), not a pass-kind switch. So there is exactly one runtime (render-graph
`execute_frame`); frame_runtime is its cook-side feeder. **Doc-hygiene follow-up:** the stale `frame_runtime.hpp`
header comment should be corrected (a §29-class code-comment fix).

The `frame-cook ⊥ crd-scene` isolation is held by the `IFrameGraphHost` seam (host resolves ECS → pre-resolved
`DrawItem`; the graph never sees a scene type) — **this invariant must survive CEIR-12** (§126, ADR-0106).

---

## 3. The 14 executors — atomic capability verbs (→ `ceir.render`/`compute`/`transfer`/`rt`)

Schema: `register_builtin_executors` — `engine/render-pass/src/executor_registry.cpp:79`. Command lowering: the
`record_*` functions — `engine/render-graph/src/frame_graph.cpp`. Every one is an **atomic verb** (a draw, a
dispatch, a copy, a trace, a present) — none is a composite algorithm. Header comment "8 executors" is stale;
there are **14**.

| # | Executor (queue) | Schema line | `record_*` line | CEIR op → dialect | §§ |
|---|---|---|---|---|---|
| 1 | `scene.raster` (Graphics) | :85 | ✅ MIGRATED 16d (2026-08-14): the generic `record_ceir_render` replays a `build_scene_ceir` plan (single-colour ladder + mrt≥2 per-item scope); `record_scene_raster` + its `bind_map`/`bind_atlas`/`attach_textures` scar helpers DELETED | `render.scope`+`render.scene_draw_list` → `ceir.render` | §40 §128 |
| 2 | `fullscreen.raster` (Graphics) | :130 | ✅ MIGRATED 16b (2026-08-12): the generic `record_ceir_render` replays a `build_fullscreen_ceir` plan; `record_fullscreen_raster` DELETED | `render.scope`+`render.draw` → `ceir.render` | §40 |
| 3 | `compute.dispatch` (Compute) | :154 | `record_compute_dispatch`:705 | `compute.dispatch` (+ indirect) → `ceir.compute` | §42 |
| 4 | `transfer.clear` (Transfer) | :180 | `record_transfer_clear`:786 | `transfer.clear` → `ceir.transfer` | §50 |
| 5 | `transfer.copy` (Transfer) | :187 | `record_transfer_copy`:817 | `transfer.copy` → `ceir.transfer` | §50 |
| 6 | `transfer.blit` (Transfer) | :193 | `record_transfer_blit`:821 | `transfer.blit` → `ceir.transfer` | §50 |
| 7 | `transfer.resolve` (Transfer) | :201 | `record_transfer_resolve`:825 | `transfer.resolve` → `ceir.transfer` | §50 |
| 8 | `raytrace.dispatch` (Compute) — inline ray query | :212 | `record_raytrace_dispatch`:854 | `rt.inline_dispatch` → `ceir.rt` | §49 |
| 9 | `raytrace.pipeline` (Compute) — SBT | :228 | `record_raytrace_pipeline`:875 | `rt.pipeline_trace` → `ceir.rt` | §49 |
| 10 | `mesh.raster` (Graphics) | :242 | ✅ MIGRATED 16b-mesh-2 (2026-08-12): `record_ceir_render` replays a `build_amplify_ceir` plan whose `render.mesh_dispatch_list` the walk EXPANDS over `ctx.draws()`; `record_amplify_raster` + wrappers DELETED | `render.mesh_dispatch_list` → `ceir.render` | §40 |
| 11 | `tess.raster` (Graphics) | :251 | ✅ MIGRATED 16b-mesh-2 (2026-08-12): same `record_ceir_render`/`build_amplify_ceir` (primitive=patches → DrawPatches, the added Patches vocab) | `render.mesh_dispatch_list` → `ceir.render` | §40 |
| 12 | `mesh.indirect` (Graphics) | :260 | ✅ MIGRATED 16b-mesh-1 (2026-08-12): `record_ceir_render` replays a `build_mesh_indirect_ceir` plan; `record_mesh_indirect` DELETED (descriptor-parity gated — no shipped consumer) | `render.mesh_dispatch_indirect` → `ceir.render` | §40 §43 |
| 13 | `visbuffer.raster` (Graphics) | :271 | `record_visbuffer_raster`:1001 | `render.draw_list` into a uint attachment (§41 — no special op) | §40 §41 |
| 14 | `present` (Graphics) | :278 | `record_present`:1048 | native intrinsic (`ceir.io`/present, §100 §177) | §100 §177 |

**§41 is the TARGET, not yet the tree's reality — `visbuffer.raster` is a residual special-case executor.** The
mission says a visibility buffer is not a canonical concept — it is `scene.raster` writing a `R32_UINT` attachment
(§41). But in the tree today it exists as a **separate registered executor #13** with its own `clear_id` param and
its own `record_visbuffer_raster` lowering (schema `:271`, record `frame_graph.cpp:1001`) — evidence the current
model *could not yet* express it as pure data on `scene.raster`. RAH-1a.1 fixed the typed-clear half; the separate
executor remains. **CEIR-11a is where §41 becomes true:** `visbuffer.raster` dissolves into `scene.raster` +
`render.begin` over a uint attachment with a typed clear. Until then, this is a named residual special-case, not a
proof of §41. (The typed `PassPayload` — `executor_registry.hpp:155`, tagged-union `TypedValue`:70, no `void*` — IS
already close to the shape CEIR attributes want; CEIR-2's generator formalizes it.)

**Slot arrays that RAH-2 retires (§156 §157):** the fixed `input0..7` / `storage0..3` / `color1..3` slots
(`executor_registry.cpp:109-123, 141-149, 166-175`) + `kMaxExecutorSlots = 16` (`executor_registry.hpp:36`) are the
concrete "fixed small bindless array" the mission says must not be the long-term model. CEIR-11d/RAH-2 replace them
with resident resource tables.

---

## 4. `scene_renderer.cpp` — the orchestrator + hand-list (the real CEIR-13 target)

`engine/scene-render/src/scene_renderer.cpp` (6610 lines). Programs are authored in CKIR (good — those are kernels/
`ceir.compute`); the **orchestration and the program hand-list** are the composite C++ that CEIR-13/14 migrate.

| Element | Location | Class → CEIR |
|---|---|---|
| Program hand-list + `engine://` registry | `register_default_programs`:2863 · `init_programs` (raster programs :2867, kernels :2892) | **composite (hand-listed)** → RAH-7 dependency-driven registry, then CEIR-7 asset deps. The code SAYS SO: `:1050-1053` "bespoke and hand-listed; the dependency-driven Program Registry that replaces it is post-RAF band RAH-7." |
| Frame-graph contribution/orchestration | `SceneRenderer::contribute`:3132 (builds + runs the frame graph) | **composite algorithm** → a `ceir.frame` asset (CEIR-13d, the §128 `scene.raster` proof) |
| CSM cascade setup + shadow config | `set_csm_config`:3021, `cascades()`:3144, the shadow setters :3015-3099; `engine/scene-render/src/csm.cpp` | **composite algorithm** → an authored CEIR shadow asset (CEIR-15 shadow corpus §88) |
| Program families (authored CKIR, C++-sequenced) — TAA resolve :1772, HZB build :1986, impostor billboard :2055, moment atlas :1768, cluster mesh (S4-0) :1606, skinning palette :2616/:2684, velocity/motion :429/:2011 | throughout `Impl` | **kernels = atomic** (`ceir.compute`, unchanged); **their sequencing = composite** → CEIR assets (CEIR-14/15) |
| Scene resolvers | `asset_resolver.hpp`; resolve material/technique/program/geometry | **atomic host capability** → host intrinsics (§45's replaceable convenience tier, §100) |
| Hot reload | RAF-11 `init_programs` re-run guard :943-1136 | **runtime** → CEIR-7 hot-reload machinery (reuse verbatim; the reentrant guard scar applies) |

### 4a. `render()` composite-block catalog — the CEIR-13 migration order (§128)

`SceneRenderer::render` (`:5708`, ~900 lines; `contribute`:3132 wraps it with a borrowed `IFrameGraph`) is the one
composite orchestration to migrate. Its distinct blocks, in execution order — **each becomes a node in the authored
`ceir.frame` / `ceir.scene` asset, or a host resolver intrinsic** (§45). This is CEIR-13's migration list (smallest
composite first — CEIR-13a):

| # | Block | Line | → CEIR |
|---|---|---|---|
| 1 | TAA reproject-matrix + constants upload (REN-41) | :5744 | frame-time constant op / `ceir.frame` input |
| 2 | CSM cascade fit — `compute_csm_cascades_from_vp` (REN-3.2-b) | :5735 | host resolver intrinsic (shadow-camera fit) + `ceir.frame` shadow asset (§88) |
| 3 | Scene-buffer consolidation — the ONE multi-draw batch (REN-38) | :5776 | `ceir.scene` build-draw-list op (§45) |
| 4 | CPU frustum + min-draw screen-size cull + BVH broadphase (40-A) | :5848 | `ceir.scene` cull op — replaceable by a compute cull (§45's "user can write a different culling algorithm") |
| 5 | Per-cascade culling (38-G1) | :5843-area | same, per shadow view |
| 6 | Shadow-caster screen-size cull | :6115 | host resolver + shadow asset |
| 7 | **Program-variant selection matrix** — the hand-coded permutation ladder (skinned × textured × shadowed × indexed × depth × velocity), :6169–6260 | :6169 | ⭐ the core of §128: replace with `scene.resolve_program(technique, draw)` host intrinsic (§45) driven by a `ceir.frame` asset — this ladder is exactly the "no private C++ algorithm path" the proof deletes |
| 8 | Draw-item-per-(group, LOD slot) assembly + impostor slots (REN-40-C2/C5) | :6260-area | `ceir.scene` draw-list resolve (§45) |
| 9 | Frame-graph assembly (shadow cascade passes → optional HZB/depth-prepass/velocity → forward → TAA resolve → present) + `execute` | tail of `render` | the authored `ceir.frame` itself (CEIR-12) |

Blocks 3–8 are the composite "scene → draw list + program selection" logic that §45's `ceir.scene` resolvers +
§128's authored `scene.raster` frame graph replace; block 9 is the frame graph (CEIR-12); block 2/6 shadow setup is
a shadow-corpus asset (CEIR-15, §88). None of this is a *new* algorithm — it is a promotion of existing C++ into
authored assets + a handful of host-resolver intrinsics.

---

## 5. `IComputeContext` — the atomic compute dispatch surface + its consumers (→ `ceir.compute` provider, §42 §85)

`engine/gpu-context/include/crd/gpu/compute.hpp` (ADR-0100). Kernel-source-agnostic: pipelines requested BY NAME
(`create_pipeline`:132), a record/copy/barrier/dispatch recorder (`ComputeRecorder`:94, `dispatch`:104,
`dispatch_indirect`:109), `submit_and_wait`:137, `last_gpu_ms` (CGP-0). Three backends implement it: Vulkan · DX12 ·
**CUDA** (`gpu-context-{vulkan,dx12,cuda}`). **This IS the provider `ceir.compute` lowers onto** (§42 §85); it does
not change. The consumers (each → a CEIR compute program at CEIR-10/19):

| Consumer | Location | Class → CEIR |
|---|---|---|
| CKIR compute dispatch backend | `engine/kir-vulkan/src/backend_vulkan.cpp` | **the CKIR→device bridge** — how every CKIR kernel runs; CEIR-10c references CKIR programs by identity through this |
| GPU BVH (Morton · radix sort · LBVH build/refit) | `engine/geometry-bvh-gpu/src/{dispatch,dispatch_60bit,radix_sort,lbvh_gpu}.cpp` — **24 `create_pipeline`/`dispatch` sites** | **compute programs** → CEIR-10 (LBVH) / CEIR-14 (geometry) assets |
| CKIR compute proof harnesses (reduce/scan/sort/FFT/NRC/RT/gsplat/hair/visbuffer/abuffer) | `tests/**` — **258 `IComputeContext`/`create_pipeline`/`dispatch` occurrences across 14 files** (heaviest: `test_vulkan_context.cpp` 148 · `test_dx12_compute.cpp` 48 · `tests/gpu-shared/ckir_*.hpp`) | **test-only** → become CEIR compute-program tests at CEIR-10/19 (the bit-exact oracles the proofs reuse) |

**Verdict:** `IComputeContext` is atomic + stays; every consumer is a compute *program* (bvh, ckir kernels) or a
*test* — none is composite orchestration. So CEIR-10 wraps them with `ceir.compute` ops without touching the
dispatch surface.

---

## 6. The five cookers — frontends and domain assets (§39 §125)

Each cooker owns one asset header. Per §125, material/technique/vertex/light are **domain assets** that compile into
CKIR helper programs — they are **NOT** collapsed into CEIR; only the frame cooker becomes a CEIR frontend.

| Cooker | Asset header | Output | CEIR relationship | §§ |
|---|---|---|---|---|
| `frame-cook` | `framecook/frame_asset.hpp` | `.frame.toml` → `FrameGraphDesc` → blob | **frontend** → `ceir.frame` (§39 §126) | §39 §126 |
| `technique-cook` | `techniquecook/technique_asset.hpp` | `.crdt` (lighting model) | **domain asset** (lighting algorithm) → CKIR; orchestrated by CEIR | §125 |
| `vertex-cook` | `vertexcook/vertex_asset.hpp` | `.crdv` (geometry stage) | **domain asset** → CKIR | §125 |
| `light-cook` | `lightcook/lighting_asset.hpp` | `.crdl` (light set) | **domain asset** → CKIR | §125 |
| `material-cook` | `matcook/material_asset.hpp` | `.crdm` (surface) | **domain asset** (surface response) → CKIR | §125 |

---

## 7. `.crdr` program cook (CKIR) + media codecs — unchanged / native

| Element | Location | Class |
|---|---|---|
| `.crdr` 'SHDR'/VART cook (ADR-0104) | `engine/shader-cook/src/cook.cpp`, `variant.cpp` | **unchanged** — CEIR references CKIR programs by content-hash identity (§85); the interface-hash split (§107) is added at CEIR-7 |
| Overlay/debug draw | `crd-draw` `submit_overlay` — `engine/draw/include/crd/draw/overlay_pass.hpp:85` (→ `IRasterContext::draw_overlay`) | **atomic debug capability** → a `ceir.render` overlay op or a native intrinsic (debug viz) |
| Media codecs | `engine/resources/src/{bmp,gif,jpeg,png,hdr,deflate,…}_image.cpp` | **native capabilities forever** (§177 — codecs stay native); the media WORKFLOW becomes `ceir.media` (CEIR-28 §62) |

---

## 8. Classification summary → the composite-vs-atomic verdict (§127)

- **Atomic native capabilities** (→ CEIR ops / intrinsics; stay native lowerings): the 14 executor verbs + their
  `record_*`; `IComputeContext` dispatch; `present`; scene resolvers; media codecs; swapchain/window/device I/O.
- **Composite algorithms** (→ CEIR program assets at CEIR-12/13/14/15): the authored frame graph (already an
  asset — becomes `ceir.frame`); `scene_renderer.cpp`'s `contribute` orchestration; the CSM/shadow setup; the
  program-family sequencing; every future renderer architecture (Forward+/deferred/visibility — §87–§90 corpora).
- **Domain assets** (stay themselves, compile to CKIR; §125): technique/material/vertex/light (`.crdt/.crdm/.crdv/
  .crdl`); CKIR programs (`.crdr`).

**The decisive proof (§128) is reachable:** `scene.raster` is already a typed executor with a `record_*` lowering;
turning `contribute`'s C++ sequencing into a `ceir.frame` asset that names `scene.raster` per draw is exactly the
§128 shape — no private C++ render path remains.

---

## 9. Sweep completion (✅ all closed)

1. ✅ **`IComputeContext` consumer enumeration** — done (§5): the CKIR backend + geometry-bvh-gpu (24 sites) +
   258 test-harness occurrences across 14 files; all compute-programs or tests, none composite.
2. ✅ **`render()` composite catalog** — done (§4a): 9 blocks enumerated with line evidence; CEIR-13 migration order
   set (smallest composite first, block 7's program-variant ladder is the §128 core).
3. ✅ **Programmatic / builder frame-construction sites** — `FrameGraphTemplate::add_pass` (`frame_graph.hpp:134`)
   is the programmatic seam; the sites are: `SceneRenderer::contribute` (the engine path) + the tests
   `tests/render-graph/test_frame_graph{,_gpu}.cpp` · `tests/gpu-context-vulkan/test_vulkan_frame_graph.cpp` ·
   `tests/gpu-context-dx12/test_dx12_frame_graph.cpp` · `tests/scene-render/test_scene_render_gpu.cpp` ·
   `tests/frame-cook/test_frame_template_bridge.cpp`. All construct `FrameGraphTemplate` directly → **CEIR-12b's
   builder frontend must cover this exact `add_pass` shape** (semantic-equality ctest vs the `.frame.toml` path).
4. ✅ **`frame_runtime.cpp` FramePassKind cross-check** — done (§2): mega-switch retired; cook-side adapter into the
   ONE render-graph runtime; residual `:426` switch is a benign param-type marshal. (Doc-hygiene note left: the
   stale `frame_runtime.hpp` header comment — a §29 one-line fix when that file is next touched.)
5. → Feeds **CEIR-0h** (migration + deletion tables) and **CEIR-0z** (§184 report + honest sizing). The CEIR-13
   sizing input is now concrete: 9 `render()` blocks + the program hand-list, not "the whole 6610-line file."

## 10. Open questions this inventory raises for the CEIR-0 ADRs

- **0c (ownership) — WORKING POSITION (constrained by the tracker's locked module placement):** `engine/ceir` is
  host-only (deps `core/log/memory/containers/units` ONLY), so it CANNOT own GPU lowering. Therefore **gpu-context +
  render-graph are the render/compute PROVIDER (§69); the executor `record_*` functions STAY in `render-graph`** and
  become the render-provider's op-lowering implementations — they do not move. `ceir` holds the dialect/IR/verifier
  and lowers *onto* the provider via the §69 provider interface (born minimally at CEIR-6b). This preserves the
  acyclic one-way graph. 0c may overturn it, but the inventory is not undecided here.
- **0d (intrinsics):** `present`, scene resolvers, `submit_overlay` — confirm each fits the §100 intrinsic schema
  (effects/domain/determinism/capability) cleanly.
- **0f:** the `IFrameGraphHost` ECS-isolation seam (§126) must be named as an invariant CEIR-12 preserves.
