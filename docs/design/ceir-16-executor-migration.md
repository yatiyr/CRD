# CEIR-16 — Executor migration (decision packet)

> The band that lifts the §176 pause. **Sources read:** roadmap `# 127.` (executor migration) + `# 128.` (the
> `scene.raster` decisive proof); `## CEIR-13` band section (L4576, old numbering); `docs/design/ceir-0a-execution-path-inventory.md`
> §1–4a (the tree reality, "final list from 0a, not from docs"); `docs/design/ceir-0h-migration-and-deletion-tables.md`
> §3 (the E-series deletion list = 16z checklist); ADR-0106 Decision #4 (the parity-gate template); `record_scene_raster`
> (`engine/render-graph/src/frame_graph.cpp:283`, read to verify the atomic-vs-composite reading). **Substrate: RUN this
> session, GREEN** (engine-first). Advisor-consulted 2026-08-12.

## 0. Band contract (§127/§128, tracker CEIR-16)

Inventory every executor **from source** (§127: "do not assume the current executor list from old docs"); classify
**composite** (→ CEIR asset/program) vs **atomic native capability** (→ native intrinsic / op lowering). The decisive
proof (§128): **`scene.raster` as ordinary CEIR** —

```text
render.begin attachments
foreach draw in draw_list:
    material  = scene.resolve_material(draw)
    technique = scene.resolve_technique(material, phase)
    program   = scene.resolve_program(technique, draw)
    geometry  = scene.resolve_geometry(draw)
    bindings  = render.build_bindings(...)
    render.draw(...)
render.end
```

— **no private C++ algorithm path**, pixel-identical to the C++ executor on the live sandbox scenes, both backends.
16z: the legacy composite-executor path deletable AND deleted per 0h; broad feature development resumes as CEIR assets.

## 1. ⭐ SUBSTRATE ALREADY EXISTS + is GREEN (engine-first inventory, 2026-08-12)

The "execute a `ceir.render` draw on device" on-ramp was built in the (closed) execution bands and is a verified
foundation — **not** something 16 must build:

- **The render dialect is complete** (`engine/ceir/ops/render.ceirop.toml`): `render.scope` (the pass region, carries a
  draw region), `render.color_attachment` / `render.depth_attachment`, `render.draw` / `draw_indexed` / `draw_indirect`
  / `draw_indirect_count`, `render.mesh_dispatch` / `mesh_dispatch_indirect`. Plus `compute.dispatch`, the `transfer.*`
  ops, and `rt.*` (the atomic verbs already have ops).
- **Lowering + execution exist** (CEIR-14b render.scope→BeginRender/Draw/EndRender; CEIR-14z-1 render materializers;
  CEIR-13z execution seam ADR-0126). **RUN GREEN this session:** `crd-ceir-gpu` 230/25 (device-free) + `crd-ceir-gpu-vulkan`
  419/21 + `crd-ceir-gpu-dx12` 310/15 (device, both backends).
- The uncommitted parallel-track cluster (`render.ceirop.toml` +ops, `ceir-gpu/execute.cpp`, dx12/vulkan raster_context
  command lowering +140/+63, `test_execute.cpp` +196, `ckir_raster_triangle`/`vertex_pull` headers) IS this groundwork —
  verified a foundation, not half-done.

**Consequence:** 16 is mostly ASSEMBLY (compose the existing render ops into the scene.raster loop) + host-resolver
intrinsics + migrating the scene_renderer C++ orchestration — NOT building a new execution engine.

## 2. Numbering translation (⚠ three live vocabularies; the TRACKER is the authority)

| Concept | roadmap / 0a / 0h (old) | current tracker | status |
|---|---|---|---|
| FrameGraph unification | CEIR-12 | **CEIR-15** | ✅ CLOSED |
| Executor migration | CEIR-13 (§127) | **CEIR-16** | THIS band |
| Scene/ECS/geometry bridge | CEIR-14 | CEIR-17 (verify at open) | — |
| Renderer proof suite | CEIR-15 | CEIR-18 (verify at open) | — |

⚠ **Code labels `CEIR-13z` / `CEIR-14b` / `CEIR-14z` are CURRENT numbering** = the closed execution-primitive bands
(13/14 — they built the CKIR/render dialect + ceir-gpu execution). Do NOT confuse them with the roadmap's old
CEIR-13/14. ⚠ context.md L138 ("frame+executor→CEIR-12/13") is OLD numbering — stale, do not trust it over the tracker.
0h's "CEIR-13z" (the E-table) = today's **16z**.

## 3. Classification — executors are ATOMIC at the graph level; the composite is the `record_*` INTERNALS

The §127 "likely composite" list (scene/fullscreen/compute/RT/transfer) is a HYPOTHESIS; the 0a source inspection found
the executor LAYER is already atomic verbs (RAF-6/12 retired the FramePassKind switch). Verified at source:
`record_scene_raster:283`'s body is composite orchestration (resolve color/depth targets → gather MRT color1..3 +
per-attachment blend → record clearing storage-pull draws) — i.e. begin/resolve/build-bindings/draw. So:

- **The graph-level pass** (`frame.pass executor="scene.raster"`) = atomic (one verb, already a `ceir.frame` op — band 15).
- **The `record_*` internal loop** = the composite that §128 makes a `ceir.render` program.
- **The scene_renderer `render()` program-variant ladder** (block 7, `scene_renderer.cpp:6169–6260`) = the OTHER composite
  half (`scene.resolve_program`).

⭐ **THE CLASSIFICATION CRITERION (advisor — disposition, distinct from size-order):** an executor's `record_*` is
**ATOMIC** iff its body maps **1:1 onto one existing `ceir.*` op's canonical lowering** (a copy, a dispatch, a single
draw) — it becomes that op's lowering (mostly already built, ceir-gpu execution). It is **COMPOSITE** iff it **sequences
decisions** (resolve targets → gather MRT → per-attachment blend → choose draw verb; or resolve program-variant per draw)
— it becomes a `ceir.render` PROGRAM / `ceir.scene` resolver. **Sizes ORDER the work; the criterion DECIDES the
disposition.** (The §3 table's disposition column applies this criterion.)

### `record_*` sizes (MEASURED — the 16a order input; smallest composite first)

| record_* (frame_graph.cpp) | lines | 16 disposition |
|---|---:|---|
| transfer.copy :817 / blit :821 | 4 / 4 | atomic → `ceir.transfer` op lowering (exists) |
| transfer.clear :786 / resolve :825 | 14 / 29 | atomic → `ceir.transfer` |
| mesh.raster :966 / tess.raster :970 | 4 / 6 | thin wrappers over amplify → `ceir.render.mesh_dispatch`/patch |
| raytrace.dispatch :854 / pipeline :875 | 21 / 25 | atomic → `ceir.rt` |
| mesh.indirect :976 | 25 | `ceir.render.mesh_dispatch` (indirect) |
| visbuffer.raster :1001 | 47 | ⚠ §41: should DISSOLVE into scene.raster + uint attachment (fork b) |
| amplify_raster :900 | 66 | `ceir.render.mesh_dispatch` (amplification) |
| compute.dispatch :705 | 81 | atomic → `ceir.compute` op lowering (exists) |
| fullscreen.raster :597 | 108 | `ceir.render` fullscreen scope |
| **scene.raster :283** | **314** | **the COMPOSITE → §128 proof = 16d (LAST)** |
| present :1048 | small | native intrinsic (`ceir.io`/present, §100 §177) |

## 4. Block ownership (0a §4a `render()` catalog) — scope IN vs OUT of band 16

| 0a §4a block | in 16? |
|---|---|
| 9 — frame-graph assembly + `execute` | ✅ DONE (band 15, `ceir.frame`) |
| 2 / 6 — shadow **fit+cull seam** (`compute_csm_cascades_from_vp` + shadow-caster cull) | ✅ IN **16c** (host resolver intrinsics). ⚠ 0h E3a: "must migrate at 16 or E5/16z cannot close." (The shadow PASSES are already authored — `forward_csm`, band 15.) |
| 2 / 6 — shadow **technique corpus** (§88 fancy techniques) | ⛔ OUT (a later band) — pointer only |
| program hand-list (`register_default_programs`) | ⛔ OUT (RAH-7 dependency-driven registry) — pointer only |
| 3,4,5,8 — scene consolidation / cull / draw-item assembly | 16c (`ceir.scene` resolvers, §45) |
| 7 — program-variant selection ladder | **16d** (`scene.resolve_program` host intrinsic — the core §128 deletion) |

## 5. The 16z deletion list = 0h §3 E-table (per-item proof gates ALREADY WRITTEN)

E1 (variant ladder :6169–6260) → `scene.resolve_program` + authored `ceir.frame`, proof = §128 scene.raster pixel-parity
both backends incl bindless/multi-draw = **16d**; E2 (`render()` fg assembly + cull) → `ceir.frame` + `ceir.scene`
resolvers, proof = `crd-sandbox --smoke-test` + REN-38-F6 green through CEIR; E5 (`SceneRenderer::render` composite) →
orchestrator only, no private C++ render path (§128) — all deleted at **16z**. RAF-12 scar governs: **coverage before
inline deletion** — the E-table IS that coverage inventory.

## 6. Parity gate (per slice) = ADR-0106 Decision #4 (harness FOUND, not invented)

Each migrated executor/asset passes, before ANY legacy deletion: **(a) command-parity** — a capturing mock encoder
asserts the old `record_*` canonical command stream ≡ the new CEIR-lowered stream; **(b) pixel-parity readback**;
**(c) `crd-sandbox --smoke-test` green both backends** at every increment. One kind at a time (the RAF-8 discipline).

⭐ **The command-capturing encoder EXISTS** (engine-first, don't rebuild): `tests/render-graph/test_frame_graph.cpp`
**`MockEncoder : ICommandEncoder`** — records the canonical stream as a `String ops` (`B`egin_rendering / `D`raw /
`E`nd / `C`ompute-dispatch / `T`ransfer / `R`trace) + per-verb counts; the RAF-7 gate ("hand-built == authored records
identical commands") asserts `ea.ops == eb.ops`. That IS the command-parity instrument, reused per migrated executor:
record the LEGACY `record_*` into MockEncoder A, the NEW CEIR replay (fork a: cooked plan → replay fn, same
`(PassPayload, RecordContext, ICommandEncoder)` signature) into MockEncoder B, assert `A.ops == B.ops`. ⚠ `ops` captures
command KIND+ORDER (structural parity), NOT descriptor VALUES — so per item pair command-parity (structural) WITH the
pixel-readback gate (values); where descriptor equality matters more than pixels (e.g. a transfer's src/dst/size), extend
MockEncoder to append the descriptor to `ops` for that verb (a targeted capture, not a new harness).

## 7. Proposed slice plan

- **16a** — the classification + migration order (§3 table) + the per-item named parity gate. DEVICE-FREE doc/inventory
  slice; lands the order + the gate harness (the capturing mock encoder) alone.
- **16b** — ⭐ CORRECTED (advisor 2026-08-12, engine-first evidence): **FULLSCREEN.RASTER FIRST**, not transfer. "Smallest
  composite first" ranks COMPOSITES (things becoming CEIR programs); transfer/compute/rt are **ATOMIC** (their `record_*`
  ARE their canonical `ceir.transfer/compute/rt` lowering) AND have NO frame-graph execution substrate — building one first
  is speculative infra for the smallest verb while the FINISHED render on-ramp sits unused. The smallest composite WITH a
  substrate = **fullscreen.raster (108 lines): one scope, one draw, real resolve logic (inputs/blend/program), no
  draw-list/for_each/MRT/variant-ladder**. Sequence: fullscreen.raster → amplify/mesh family → scene.raster (16d).
  **SHAPE (i)** (§8a): cook-lower the `ceir.render` asset (`find_render_misuse` → 14b lowering) to a CACHED
  `Array<LoweredCommand>` + pre-resolved program/target identities; a generic replay fn drives `execute_render_lowered` on
  the cached slice through the pass's frame-recording `ICommandEncoder` — reusing the CEIR-14z walk VERBATIM inside
  band-15's EXISTING pass (wire it into a `PassRecordFn`, NOT `execute_render_frame` — that builds its OWN passes = the
  standalone test surface). Barriers stay the frame graph's job (the INERT-barrier contract render_materialize.hpp L51/L68
  — satisfied BY the pass-level integration). Host-backed resolvers (target/program/binding) via `RecordContext` through the
  ADR-0106 Decision #3 seam = **fork (c)'s first concrete piece** (14z fakes → real, id/hash-keyed per §22-18); build once
  for fullscreen, 16c/16d inherit. Gate: MockEncoder command-parity (extend `D` to capture program identity + binding
  count) + pixel readback + smoke both backends; delete `record_fullscreen_raster` ONLY after both pass.
- **16b-tail** — the ATOMIC transfer/compute/rt executors: a lowering-CLASSIFICATION slice (`record_*` = the canonical
  `ceir.*` op lowering); the execution-seam question (a distinct frame-graph transfer/compute seam vs. a classification
  note) is decided AFTER the render-integration pattern exists — no speculative infra (render_materialize.hpp L74).
- **16c** — `ceir.scene` resolver intrinsics (blocks 3,4,5,8) + `scene.resolve_material/technique/geometry`.
- **16d** — **the §128 `scene.raster` proof**: the full resolve+draw loop (block 7 + `record_scene_raster`) as a
  `ceir.render` program driven by an authored `ceir.frame`, pixel-identical both backends incl bindless/multi-draw.
- **16z** — delete the legacy composite path per the 0h E-table; sandbox smoke + REN-38-F6 green through CEIR.

## 8. FORKS — ✅ DECIDED (advisor 2026-08-12, with source anchors)

- **(a) `engine://ceir/*` executor asset → `GraphExecutorTable` = LOWER-ONCE-AT-COOK, not interpret-per-record.**
  Anchor: `test_execute.cpp:13` — the core Interpreter REFUSES a dispatch (typed `NoSemantics`); the architecture already
  ruled GPU-effecting ops don't interpret, so interpret-per-record would REVERSE a closed decision. §22-18 (no record-time
  string lookup — interpreting resolves `program=@p` symbols per frame) + PSO-cache-by-content (pipeline identity settled
  at cook) both point the same way. **Precise shape:** lower the asset ONCE at cook/load-commit into a resolved,
  symbol-free PLAN; the per-frame record REPLAYS that plan parameterized by HOST data — because `for_each` counts + draw
  lists are host-provided at runtime (`frame_runtime.cpp:756`, band 15), the plan cannot bake them; it is the SAME
  static-skeleton / runtime-data split `FrameRecorder` already does (ADR-0106 amendment #1). Plug-in shape to VERIFY not
  invent: ONE generic "replay lowered plan" record fn with the existing `(PassPayload, RecordContext, ICommandEncoder)`
  signature, N cooked plans — the `run_authored_cb` pattern again. Hot reload composes via RAF-11 stage/commit re-lowering.
  ⭐ **SHAPE REFINEMENT (advisor 2026-08-12): what is cooked-once is the VERIFIED LOWERED LIST + resolved program/target
  IDENTITIES (shape i), NOT pre-materialized `RenderingDesc`/`RasterDrawPacket`s (shape ii).** Replay drives the CEIR-14z
  `execute_render_lowered` walk (materialize-at-record) on the cached `Array<LoweredCommand>`. Shape (ii)'s cook-time
  desc-baking is a DEAD END: 16d's foreach-draw resolve chain must materialize packets per-draw at record from host data,
  so baking them at cook would be unwound at the proof. "Lower-once" = the LIST is verified+lowered once; MATERIALIZATION
  stays at record. The INERT-barrier reconciliation (render_materialize.hpp L51/L68) is satisfied by the pass-level
  integration: the executor asset replays WITHIN a band-15 `ceir.frame` pass, so the frame graph still derives barriers
  from the pass's declared reads/writes — the CEIR `Barrier` commands stay inert, as designed.
- **(b) visbuffer.raster §41 dissolution = IN band 16, sequenced AFTER 16d, NO interim migration.** §41: a visibility
  buffer is not a canonical concept; 0a's note says the current model "could not yet express it as pure data on
  scene.raster" — 16d builds exactly the model that can. So do NOT give `visbuffer.raster` a ceir form in 16b (building
  CEIR support for an op we're deleting). Leave it on the legacy path until 16d lands, then re-author `scene_visbuffer`
  onto `scene.raster` + a uint attachment + typed clear, and delete executor #13 + `record_visbuffer_raster:1001` as the
  FIRST 16z deletion, pixel-parity gated. Support chain already exists: `render.ceirop.toml` `clear_kind=uint` +
  the 15c `VisbufferNeedsUintTarget` contract row.
- **(c) `scene.resolve_*` = DECLARED in the CEIR intrinsic registry (0d schema); IMPLEMENTED host-side behind the
  `IFrameGraphHost` seam; currency = pre-resolved handles, NEVER ECS types.** ADR-0106 Decision #3 defines the seam for
  exactly this; the 14z materializer resolvers (`resolve_target`/`resolve_rprog` — `Operation* + void* user → gpu handle`)
  are the PROVEN callback shape. `scene.resolve_*` takes DrawItem-level data → handles; the host registers bindings through
  the PUBLIC seam (the `register_pass_executor` precedent = §45's replaceable tier for apps). §22-18 applies INSIDE the
  intrinsic too: id/hash-keyed tables, no string lookup at record.
