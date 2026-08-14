# CEIR-16 — Executor migration (decision packet)

> The band that lifts the §176 pause. **Sources read:** roadmap `# 127.` (executor migration) + `# 128.` (the
> `scene.raster` decisive proof); `## CEIR-13` band section (L4576, old numbering); `docs/design/ceir-0a-execution-path-inventory.md`
> §1–4a (the tree reality, "final list from 0a, not from docs"); `docs/design/ceir-0h-migration-and-deletion-tables.md`
> §3 (the E-series deletion list = 16z checklist); ADR-0106 Decision #4 (the parity-gate template); `record_scene_raster`
> (`engine/render-graph/src/frame_graph.cpp:283`, read to verify the atomic-vs-composite reading). **Substrate: RUN this
> session, GREEN** (engine-first). Advisor-consulted 2026-08-12.

> **⛔⛔ STATUS 2026-08-12 — 16b FULLSCREEN EXECUTOR MIGRATED + gated 4-config (deletion-is-the-proof).** `fullscreen.raster`
> records ONLY through the generic CEIR replay `record_ceir_render` (frame_graph.cpp; renamed from `record_fullscreen_ceir`
> at 16b-mesh-1 when it gained a 2nd executor), driven by a per-pass
> `build_fullscreen_ceir` plan built at frame LOAD (`build_frame_plans`, frame_runtime.cpp; `FramePlans`/`CeirPlanTable`
> keyed by pass name_hash). The imperative `record_fullscreen_raster` (108 lines) + `fullscreen_via_ceir()` + the
> `CRD_FULLSCREEN_VIA_CEIR` A/B flag are DELETED; `register_builtin_records` registers the replay UNCONDITIONALLY (count
> stays 14). `bind_atlas`/`attach_textures` KEPT (scene.raster/16d shares them). 5 shapes (procedural/plain/atlas/bindless/
> composite) chosen by CEIR TYPE, materialized generically. Device pixel-verified plain+depth_as_float+bindless on real
> Vulkan+DX12; shipped 11-pass frame renders via CEIR (sandbox `--smoke-test 2`, 63fps). Gate: win-debug + win-asan +
> linux-gcc-debug(llvmpipe) + clang-tidy LLVM-20 all GREEN.
> **⛔ SCAR (feedback_plan_table_must_rebuild_at_every_frame_install_site):** a plan-driven executor's plan table must be
> rebuilt at EVERY frame-install site — `set_frame_graph_toml`, the reload `frame_commit`, `set_soft_shadows` (the
> forward_csm⇄forward_csm_moment tier swap), AND resolved fallbacks (forward_agx/srgb carry a fullscreen `post`) — or the
> new frame's fullscreen passes get a null plan → `ctx.fail()` → black frame. The flag-ON sweep of the REAL renderer
> (scene-render device + sandbox smoke, `run_authored_cb`) caught it; the `execute_frame` device A/B did not.
>
> **⛔ STATUS 2026-08-12 — 16b-mesh-1 MESH.INDIRECT MIGRATED + gated 4-config.** `mesh.indirect` records through the same
> `record_ceir_render`, replaying a `build_mesh_indirect_ceir` plan (a `render.scope` clearing the target + one
> `render.mesh_dispatch_indirect(%args)`, no bindings). Fixed a LATENT bug: the materializer never resolved the args buffer for
> `MeshletIndirect` (14z wired only args_offset) → a mesh-indirect draw dispatched off a NULL args buffer; added it +
> device-free test. `record_mesh_indirect` DELETED (count stays 14). ⭐ No shipped asset uses mesh.indirect, so per the advisor
> the device A/B is vacuous (both paths feed the encoder the SAME packet); the DELETION GATE was a device-free
> DESCRIPTOR-PARITY A/B (migration doc §6): legacy vs CEIR record into a capturing encoder, RenderingDesc + RasterDrawPacket
> field-identical (store/blend/clear/args/offset/cmd/geo). ⭐ INSIGHT: build_fullscreen_ceir does NOT carry the clear COLOR
> (fullscreen triangle covers all pixels); mesh/amplify/scene builders MUST (a dispatch can leave uncovered pixels). The flag
> dance stays MANDATORY for amplify (16b-mesh-2 = new per-draw-loop mechanism over SHIPPED scene_mesh/scene_tess).
>
> **⛔⛔ STATUS 2026-08-12 — 16b-mesh-2 AMPLIFY (mesh.raster + tess.raster) MIGRATED + gated (the FIRST per-draw LOOP).**
> `mesh.raster`/`tess.raster` record through `record_ceir_render` replaying a `build_amplify_ceir` plan whose ONE
> `render.mesh_dispatch_list` (a new op: ZERO operands, `primitive="meshlet"|"patches"`, `fallback_count`) the record-time
> walk EXPANDS over the host DrawList (`ctx.draws()`) into N draws in ONE scope — the §8a shape-(i) mechanism that ALSO
> governs 16d. Added the PATCHES vocab (the materializer had none). `record_amplify_raster` + both wrappers + the temp
> `CRD_AMPLIFY_VIA_CEIR` flag DELETED; count stays 14. Resolver is count+item (no alloc; the RecordContext-owned DrawList
> is the backing store). ⭐ THE FLAG-ON REAL-RENDERER SWEEP EARNED ITS KEEP (the fullscreen-moment lesson): the device-free
> descriptor-parity PASSED, but the flag-ON sweep of shipped scene_mesh/scene_tess rendered BLACK — `build_frame_plans` read
> the schema name `amplify_count`, but the shipped assets author `groups`/`patches` (the recorder maps them → amplify_count),
> so `fallback_count`=0. Fixed (read groups‖patches‖amplify_count). Post-deletion the REN-38-F6 device gates render
> scene_mesh/scene_tess via CEIR by default = the permanent live-path regression. Gate: win-debug (rg 116 + scene 1539 +
> rg-gpu 357 both backends + smoke) + win-asan + clang-tidy; linux pending. **The mechanism (draws-resolver + walk expansion
> + per-item packet) reuses for 16d; the TEMPLATE does not — scene.raster is per-item VERB selection + atlas + MRT.**
>
> **⛔ 16b-tail DECISION 2026-08-12 (timeboxed classification, NO code — the render pattern exists, so the seam question is
> now decidable; advisor: defer the seam, no speculative infra).** Classified all remaining non-scene executors from source:
> - **transfer.clear / copy / blit / resolve = ATOMIC** — each `record_*` builds ONE `TransferDesc` + one `encoder.transfer`,
>   no loop/orchestration ⇒ 1:1 with a `ceir.transfer` op (the op EXISTS). NOT a composite; NO `build_*_ceir` plan needed.
> - **raytrace.dispatch / pipeline = ATOMIC** — one `DispatchDesc`/`TraceDesc` (accel + grid + `bind_storage_run` bindings) +
>   one `encoder.dispatch`/`trace_rays`; 1:1 with a `ceir.compute`(+accel) / `ceir.rt` op (exist).
> - **compute.dispatch = HYBRID** — its single-dispatch arm is ATOMIC (1:1 `ceir.compute`), but its per-item CULL loop
>   (`for draw in ctx.draws(): dispatch(it.dispatch_groups)`) is COMPOSITE **scene-cull** work → belongs to **16c** (§4 block
>   3/4/5/8), NOT here. (It could reuse the amplify draws-resolver + a `compute.dispatch_list` op — a 16c call.)
> - **visbuffer.raster** stays untouched (dissolves into scene.raster + a uint attachment POST-16d, per §b).
>
> **Consequence:** the atomic transfer/rt executors need a frame-graph *transfer/rt seam* (the compute/transfer analogue of
> `record_ceir_render`/`execute_render_lowered`) to migrate — but its SHAPE depends on how scene (16c/16d) drives compute/
> transfer/rt, so building it now is speculative. DEFER: these migrate when 16c/16d builds the scene compute+transfer seam,
> or at 16z (which deletes every legacy `record_*` regardless — the E-table is the coverage inventory). NO 16b-tail code.
>
> **⛔⛔ 16c/16d SCOPE + DOCTRINE (advisor 2026-08-12, full-loop read done).** Q1 mechanism (one scope, N draws, walk
> expansion — §8a) and Q3 verb-selection-is-LOWERING-not-IR (§4b) are CLOSED. The open half (Q2) resolved: 16c does NOT
> collapse into the draws-resolver — 16z must DELETE the E1 block-7 ladder (scene_renderer.cpp ~6275: resolve_base_color +
> the skinned/shadowed/textured program-variant if-ladder) + E2 (render() assembly/cull), so the resolve logic must move
> BEHIND the declared seam (fork c: registered §45, replaceable), not survive inline. Split:
> - **16c = the scene ITEM VIEW** (extend the amplify `RasterAmplifyItem` → a RenderDrawItem projection: program/storage/
>   texture/args+offset/index_count/instance/first_index/vertex_count/indexed + the pass-texture triple pass_texture/is_depth/
>   comparison) **+ the declared resolver registrations** (fork c, the fs_draws/fs_program pattern extended). Smaller than the
>   doc's original framing — fullscreen/amplify already built the resolver plumbing.
> - **16d = the scene TEMPLATE op** (ONE op) **+ the expansion** (the §4b lowering ladder: per-item VERB routing [args→
>   DrawIndexedIndirect+first_draw_index=i | index_count+tex→DrawIndexed | combined/tex→StoragePull Draw | plain→COALESCE a
>   run of compatible items into ONE DrawMulti/DrawMultiIndexed, draw_count=run] + attach_textures + MRT + depth-only) **+
>   scene.resolve_program** (the ladder behind the seam) **+ parity + delete record_scene_raster**.
> **⛔ TRAPS (advisor):** (1) COALESCING is a COMMAND-STRUCTURE gate, not just pixels — REN-38 asserts "two mesh groups → ONE
> multi-draw batch"; if the expansion emits N packets where legacy coalesces to one, PIXELS MATCH but that gate FAILS + the
> perf contract silently dies. The expansion MUST reproduce the run-coalescing (record_scene_raster L538-595). (2) MRT is a
> DIFFERENT shape (N scopes per item, ⛔⛔ LOAD-not-clear) + depth-only/color-optional (reverse-Z depth_compare) rides the
> same template — decide op-mode vs 2nd-op after design. (3) extract scene params by AUTHORED desc names (to_authored_pass
> ~L211-270: load/load_depth/depth_compare/clear_depth/blend0..3/material_pass — the groups/patches lesson verbatim).
> **⛔ DOCTRINE (a READING of §128, SURFACE to user — same class as the §4b parity-doctrine change, NOT silently landed):**
> the IR carries ONE scene template op, NOT the pseudocode's literal `resolve_material→technique→program→geometry→draw` chain
> — a STATIC plan cannot instance per-draw ops (fork c's callback shape); the resolve chain is host-side behind the seam, the
> DrawList carries pre-resolved handles, the template op expands. **⛔ PROCESS: the flag dance is MAXIMALLY mandatory** —
> scene.raster is the geometry executor of EVERY shipped frame (forward/csm/prepass), so the flag-ON sweep = the ENTIRE
> scene-render suite + smoke (largest blast radius, strongest live-path gate); 16-3c-5-prereq build_frame_plans is the early
> gate for free (its scene arm fires on every shipped asset).
> NEXT (locked order) = 16c (item-view + resolver registrations + compute-cull-list) → 16d (template op + expansion + parity + delete; advisor before 16d code) → 16z (delete all + visbuffer dissolution).

> **⛔⛔ STATUS 2026-08-14 — 16d-live-1 (the DEPTH BRANCH — the TWICE-BITTEN class) DONE + gated 2 Win + 2 Linux + tidy.**
> Advisor-consulted before writing (the 3-mode fork). The "3 depth-acquisition modes" collapse to TWO record-time queries
> (verified vs `record_scene_raster` head L302-310): mode 1 = the explicit `"depth"` slot (a depth-only shadow cascade /
> prepass); modes 2+3 = the colour target's BUNDLED depth (its own `create_color_depth_target` OR a shared_depth
> `ImageWithDepth` companion, REN-40-G3 — both surface as `color->has_depth()`).
> - **1a (ceir-gpu `materialize_rendering_desc`):** a `render.depth_attachment` op whose target resolver returns NULL DROPS
>   the attachment (depth stays disabled — runtime-dynamic, matching legacy leaving `depth` null); `extent_from_target` falls
>   back to `out.depth.target` for a 0-colour depth-only scope (`dims = color ?: depth`); a ZERO-attachment scope FAILS
>   materialize (never `begin_rendering` empty). Device-free test `ceir 16d-live-1a` (4 cases via a kind-branching resolver).
> - **1b (render-graph `fs_target`):** branches on the attachment OP KIND; the depth branch mirrors legacy's EXACT
>   fall-through (`"depth"` slot first, else `color->has_depth()`). Gate = a REAL-vs-REAL command-parity A/B
>   (`ceir 16d-live-1b`, 49 asserts): `record_ceir_render` (build_scene_ceir plan) == `record_scene_raster` for the depth
>   desc across forward+bundled-depth / forward+no-depth / depth-only. `FakeTarget` gained a `bundled_depth` ctor arg;
>   `CapturedDraw` gained `depth_tgt`/`depth_load`/`depth_cmp`.
> - Gate: crd-ceir-gpu 448/31 + crd-render-graph 165/12 GREEN on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan;
>   tidy clean on all 4 changed files. ⚠ TOOLING: `scripts/run-ctest.bat` hardcodes the **14.50** ASan DLL but the toolset
>   is **14.51** → a win-asan ctest run crashes-on-load (false red); run the exe with the matching 14.51 DLL on PATH (what
>   per-slice-check.ps1 does). Small stale-path fix owed to run-ctest.bat.
> **⛔⛔ STATUS 2026-08-14 — 16d-live-2 (the `build_frame_plans` SCENE ARM) DONE + gated 2 Win + 2 Linux + tidy + the
> real-GPU consumer.** The scene arm in `build_frame_plans` (frame_runtime.cpp) classifies `pass_is_scene_raster` (all of
> `raster.geometry` / `raster.depth_only` / `raster.mrt` = kExecSceneRaster) and populates `SceneBuildDesc` from the authored
> `FramePassDesc`: `has_color=!depth_only`, `has_depth=true` ALWAYS (the depth op is a TEMPLATE `fs_target` gates at record),
> `load`=kLoad BASE ONLY (⛔ NOT `load_override` — per-for_each-instance frame-varying, 16d-live-2b), `load_depth`=kLoad‖kLoadDepth,
> clear/clear_depth/depth_compare from the authored params.
> - ⛔ `pass_is_scene_raster` added to the STORAGE-RESERVE COUNT LOOP + the `nfs==0` early-out (advisor: else `out.storage`
>   relocates mid-build and dangles every earlier `CeirPassPlan`'s `cmds.data()` pointer).
> - **mrt reject = SKIP-the-plan, NOT a LOUD `return false`** (decision refined WITH the advisor's rule after confirming the
>   antecedent): a `false` return DISABLES the whole frame (scene_renderer L876-885 FATAL), which would break a working
>   C++-path mrt≥2 asset flag-OFF. So a scene pass with ≥2 COLOUR attachments (count excludes buffer writes + depth-format
>   writes — matching the resolver's routing, so a velocity `[colour, depth]` = 1 colour) SKIPS its plan (records via
>   `record_scene_raster`, which handles mrt≥2) + an INFO diag (`UnsupportedPassKind`, never silent). No shipped asset or test
>   drives mrt≥2 through `build_frame_plans` (WBOIT uses `build_frame_graph_template`); a null-plan mrt≥2 pass fails loud at
>   record only once the flag flips scene.raster→record_ceir_render — correct, mrt≥2 is migrated at 16d-live-4 before then.
> - Tests: `ceir 16d-live-2` (forward / depth-only / mrt≥2-skip / velocity-not-mrt, 15 asserts) + the **3c-5 prereq** now
>   builds scene plans on all 8 shipped assets (62 asserts). Gate: crd-frame-cook 2751/96 on win-debug + win-asan +
>   linux-gcc-debug + linux-gcc-asan; tidy clean; **consumer crd-scene-render 1539/72 on the real GPU** (build_frame_plans at
>   frame-install disables NO shipped frame; rendering unchanged — the plans are INERT until the flag dance).
> - **⛔ NEXT (locked): 16d-live-2b** = per-instance `load_override` at record. Shadow-cascade caching (REN-40-E2,
>   `for_each_load`) is FRAME-VARYING per-instance (a cached cascade LOADs its atlas layer + records 0 draws / 0 program) → a
>   static plan cannot bake it. `record_ceir_render` must read the effective per-instance `load` from the RecordContext/payload
>   (`to_authored_pass` sets it = kLoad‖load_override per instance) and force the materialized colour+depth LoadOps to Load.

> **⛔⛔ STATUS 2026-08-14 — 16d-live-2b (per-instance load override at record) DONE + gated 2 Win + 2 Linux + tidy + the
> real-GPU consumer.** Advisor-consulted before writing. ⭐ DOCTRINE this establishes: **the plan bakes AUTHORED STATICS; the
> payload carries PER-INSTANCE RUNTIME DELTAS.** Impl: `RenderResolvers` gained `bool force_load`; `record_ceir_render` reads
> `bool_param(payload, "load")` (= kLoad‖load_override, set per-for_each-instance by to_authored_pass L219) → `r.force_load`;
> `execute_render_lowered`'s BeginRender forces every colour + the depth LoadOp to Load when set — MONOTONE (Clear→Load only;
> Store + clear values untouched; idempotent when the base baked Load). Boolean algebra verified vs legacy (colour load =
> `load`; depth load = `load ‖ load_depth`); the depth's kLoadDepth arm has NO force-side read, so the baked plan's depth load
> stays load-bearing (never strip it). ⛔ The `"load"` payload is set on only TWO arms — scene (L219) + fullscreen composite
> (L267, same predicate as its bake ⇒ provably idempotent on the already-live fullscreen path).
> - **Cached-cascade parity RESOLVED (do NOT reproduce the fallback — record nothing):** legacy's `draws.count==0` fallback
>   (a graph-shape-gate remnant) records ONE NULL-program packet, which `detail::CommandEncoder::draw` SKIPS
>   (command_lowering.hpp L130-132 — VERIFIED at the consumption point, not assumed) → a device no-op. The CEIR path records
>   the semantically-correct 0 draws; both open a Load-only scope that PRESERVES the atlas layer ⇒ pixel-identical. §4b already
>   demoted command-parity to structural sanity for this class; reproducing the fallback would special-case around
>   `execute_render_lowered`'s own null-program correctness guard.
> - Tests: `ceir 16d-live-2b` extends the real-vs-real A/B — a cached section (payload load=true, 0 draws, null program →
>   BOTH depth_load==Load; CEIR 0 draws, legacy 1 NULL-program draw = the KNOWN pixel-safe delta, `a.prog==nullptr` asserted)
>   + a no-clobber section (payload load absent → baked Clear survives). Gate: crd-ceir-gpu 448/31 + crd-render-graph 196/13 on
>   win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean; **consumer crd-scene-render 1539/72 real GPU** (the
>   advisor's point 1: record_ceir_render is on the LIVE fullscreen path today, so the device suite MUST re-run — green).
 **⛔⛔ STATUS 2026-08-14 — 16d-live-3a (N-draw COMMAND-PARITY) DONE + gated (test-only, no engine change).** A new
> `SceneParityEncoder` (per-draw capture: command/geometry/draw_count/first_draw_index/program/binding-count/first-tex-slot)
> drives the real-vs-real A/B `ceir 16d-live-3a` across the FULL verb ladder and asserts `record_ceir_render`'s stream is
> field-by-field identical to `record_scene_raster`: plain-COALESCE (3 same prog+storage → ONE DrawMulti — the REN-38
> command-STRUCTURE gate, not pixels), storage-break (2 draws), GPU-indirect (DrawIndexedIndirect, first_draw_index=i),
> shadow-atlas (atlas@4 + comparison@5, no coalesce), depth-only 0-colour (plain, no textures), indexed-sampled (DrawIndexed +
> map@1). Gate: crd-render-graph 333/14 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean. (No consumer
> re-run needed — no engine touch; the device-free ceir 16d test already covers emit_scene_list's ladder in isolation.)
> - ✅ **16d-live-3b DONE (RECLASSIFICATION + the null-program coverage gap) — advisor-confirmed, gated.** See the E1
>   reclassification decision block in §5: the variant ladder is ALREADY host-side (fork c / §4b), so 16z deletes the executor
>   not the ladder; the one code deliverable was the record-time null→default fallback A/B section (`emit_scene_list` already
>   matched legacy — locked). `scene.resolve_program`'s 0d-registry declaration named-forward (registry not built yet).
> - ✅ **16d-live-3c DONE (the FLAG DANCE `CRD_SCENE_VIA_CEIR`) — gated + the LARGEST-BLAST-RADIUS device proof PASSED.**
>   `register_builtin_records` (frame_graph.cpp) now swaps scene.raster's recorder record_scene_raster (flag-OFF, default) ⇄
>   record_ceir_render (flag-ON, env `CRD_SCENE_VIA_CEIR` present) — a runtime env read (`_dupenv_s`/`getenv`, the
>   CRD_FRAME_VIA_CEIR precedent), swapping WHICH fn not the count. **PROOF:** count==14 both flag states (raf7 47/5);
>   **the WHOLE crd-scene-render suite flag-ON == flag-OFF = 1539/72 on the real GPU (both Vulkan+DX12** — REN-36 pixel
>   asserts, CSM, RAF-10 app-composed, RAF-11 reload); **`crd-sandbox --smoke-test 2` flag-ON PASS** (the shipped
>   `forward_csm_agx` frame — 11 passes, 4 PCSS cascades, 5446 instances — EXIT=0). ⇒ every shipped scene pass
>   (forward/csm/prepass/velocity) records through the CEIR replay behaviour-identically; the plan-table rebuilds at every
>   frame-install site for scene plans (the twice-bitten scar clean); no shipped asset trips the mrt≥2 skip. Gate:
>   crd-render-graph 363/14 on win-debug + win-asan + linux-gcc-debug (+ flag-ON count) + linux-gcc-asan; tidy clean. ⚠ the
>   flag DEFAULT stays OFF (legacy shipped) — the flip to UNCONDITIONAL CEIR + the record_scene_raster deletion + the flag
>   removal happen at 16d-live-4/16z, AFTER the mrt≥2 N-scope representation lands (a null-plan mrt≥2 pass would ctx.fail()
>   under unconditional CEIR).
> - **⛔ NEXT (locked): 16d-live-4 → 16z.** (1) The **mrt≥2 sub-slice** — its own N-colour-scope CEIR representation (the
>   16d-live-2 skip currently keeps mrt≥2 on C++; ⛔ ADVISOR before its code; a DELETION prerequisite proven via the gpu-test
>   2-colour scopes). (2) Convert the ~8 `run_*_gpu` scene.raster device tests to the CEIR path (run_shadow_gpu = the 0-colour
>   device proof). (3) **DELETE `record_scene_raster` + the frame_graph.cpp scar helpers** (bind_map/bind_atlas/attach_textures)
>   + flip scene.raster to record_ceir_render UNCONDITIONALLY + delete the `CRD_SCENE_VIA_CEIR` flag (the-deletion-is-the-proof).
>   Then **16z**: the legacy composite-executor path deleted; `crd-sandbox --smoke-test` + REN-38-F6 green through CEIR;
>   visbuffer.raster dissolution (§b: re-author onto scene.raster + a uint attachment, delete record_visbuffer_raster).

> **⛔⛔ STATUS 2026-08-14 — 16d-live-4 STARTED (the deletion slice). ADVISOR-LOCKED PLAN (option a: BUILD mrt≥2, do NOT
> defer).** mrt≥2 scene is an AUTHORABLE shipped capability (the cooker schema carries blend_count/blend0..3; 15c enforces
> CompositeNeedsBlend; `run_mrt_blend_gpu` device-tests it on the REAL scene.raster) — deleting record_scene_raster's MRT arm
> without a CEIR replacement would be a capability regression ("unreachable library" class). So 16d-live-4 builds it.
> **REPRESENTATION = op-mode, NO dialect change:** ONE `render.scope` with N `color_attachment` ops (blend = the existing
> per-attachment attr) + depth template + the SAME `scene_draw_list`. The per-item begin/draw/end is a LOWERING artifact (same
> class as the slot-4 atlas ruling, §4b — never in the IR): at BeginRender, if materialized rd has ≥2 colours DEFER the
> `begin_rendering` (stash rd); at the Draw, `scene_draw_list` → the per-item MRT expansion (begin(rd)/StoragePull-draw/end per
> item, byte-identical to legacy L377-398 — skip null storage, null→default program, no textures, NO coalescing); ⛔ ANY OTHER
> draw op → issue the deferred begin + proceed (mandatory — else the 14z-4c gbuffer single-draw MRT tests break); EndRender →
> expansion ran ⇒ skip, deferral unresolved (0 items) ⇒ emit nothing (matches legacy's 0-iteration loop). Reproduce the
> IDENTICAL command stream; let the converted `run_mrt_blend_gpu` pixel-prove.
> **⛔ FOUR GAPS (advisor):** (i) ✅ **4a-1 DONE + gated** — `blend_of` vocabulary was alpha/additive/premultiplied only, so
> RevealageMultiply/Multiply silently folded to Opaque; the full 7-mode `BlendMode` enum is now the closed CEIR `blend` vocab
> both directions (materializer `blend_of` + verifier `kBlendVocab`/7 + op-def doc regen drift-clean), proven by
> `ceir 16d-live-4a-1` (7 verify+map, garbage rejects); crd-ceir-gpu 477/32 + crd-ceir 3317/433 + opgen-drift on
> win-debug/asan + linux-gcc-debug/asan, tidy clean. (ii) verify `find_render_misuse` accepts a ≥2-colour scope containing
> `scene_draw_list` (the builder never emitted one — extend + test if a rule rejects). (iii) the 16d-live-2 "mrt≥2 SKIPS the
> plan" logic + its test section INVERT when the skip is deleted + `SceneBuildDesc` extended (colour count + per-attachment
> blend, read from `blend0..3` like `to_authored_pass`) — flip deliberately. (iv) legacy MRT IGNORES the `load` param (descs
> hardcode Clear) — `force_load` must NOT apply on the MRT expansion; add a parity section (mrt + load=true → still Clear).
> **SEQUENCING:** ✅ 4a-1 blend vocab → ✅ **4a-2 desc/builder DONE** (`blend_str` forward emitter + `SceneBuildDesc`
> mrt_n/blend[4]; `build_scene_ceir` emits N color_attachment ops with per-attachment `blend`, all Clear, when mrt_n≥2 — SAME
> render.scope + scene_draw_list [op-mode]; single-colour path byte-identical; ⭐ GAP ii CLEAR — `find_render_misuse` ACCEPTS
> a ≥2-colour scope containing scene_draw_list, no verifier change needed; `ceir 16d-live-4a-2`; crd-ceir-gpu 489/33 4-config)
> → ✅ **4a-3 walk expansion DONE** (execute_render_lowered DEFERS a ≥2-colour scope's begin_rendering; emit_scene_list_mrt
> opens a scope PER ITEM at the Draw = the legacy MRT arm, no textures/no coalescing; a non-scene-draw op flushes the deferred
> begin first [14z-4c single-draw preserved]; force_load NOT applied to MRT [gap iv]; `ceir 16d-live-4a-3` device-free +
> render-graph real-vs-real A/B record_ceir_render==record_scene_raster MRT structural [begins==2/item]; crd-ceir-gpu 499/34 +
> crd-render-graph 384/15 4-config) → ✅ **4a-4 un-skip build_frame_plans DONE** (sets SceneBuildDesc.mrt_n = the colour-write
> count + reads blend0..3; the 16d-live-2 mrt≥2-SKIP is INVERTED [gap iii] — an mrt≥2 pass now BUILDS its N-colour plan;
> shipped assets stay mrt_n=1; crd-frame-cook 2752/96 4-config + scene-render 1539/72 consumer). ⭐⭐ **4a COMPLETE — the
> mrt≥2 CEIR representation is DONE (authorable G-buffer / WBOIT scene passes now render through CEIR).** → ✅ **4b DONE
> (advisor-hybrid: on-device legacy-vs-CEIR A/B, the strongest deletion proof).** Only **3** tests use scene.raster
> (run_scene_drawlist_gpu / run_mrt_blend_gpu / run_graph_gpu; the rest use bespoke test executors — left alone). A
> `GraphExecutorTable::replace_record` seam swaps scene.raster→record_ceir_render in a builtin table (count-preserving); each
> converted test runs execute_frame with a `build_scene_ceir` plan. run_scene_drawlist_gpu + run_mrt_blend_gpu run BOTH paths
> into separate targets + assert IDENTICAL pixels (record_ceir_render == record_scene_raster, both backends); run_graph_gpu
> drops the geometry-slot fallback for a DrawList (⛔ 4c note: the count==0 geometry-slot fallback loses its only consumer —
> deleted with zero replacement, a test-only shape). ⭐⭐ **The A/B CAUGHT A REAL BUG the device-free tests missed:**
> `fs_target` resolved EVERY colour attachment to the "color" slot, so MRT color1..3 stayed UNWRITTEN — fixed with a
> `build_scene_ceir` `color_slot` index attr (the undeclared-int convention, like binding `source`) fs_target maps to
> color/color1..3 (slot names host-side; ceir-gpu has no pass_param_id). Gate: crd-render-graph 384/15 + crd-ceir-gpu 499/34 +
> gpu-tests raf7 395/2 (real GPU both backends) + llvmpipe 197/1; **scene-render flag-ON 1539/72** (color_slot safe on the
> live path); win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy. → ✅✅ **4c DONE (THE DELETION — the-deletion-is-
> the-proof).** `record_scene_raster` + its scar helpers (`bind_map`/`bind_atlas`/`attach_textures`) + the orphaned
> `kMaxSceneRun` constexpr + the `scene_via_ceir()` helper + the `CRD_SCENE_VIA_CEIR` flag + the now-unused `#include <cstdlib>`
> are DELETED from frame_graph.cpp; `register_builtin_records` maps scene.raster → `record_ceir_render` UNCONDITIONALLY (count
> still 14). ⭐ **The 4 device-free A/B tests (1b/2b/3a/4a-3) + the 2 on-device A/B tests (run_scene_drawlist_gpu /
> run_mrt_blend_gpu) were converted single-path with ABSOLUTE asserts** — post-deletion the "legacy" arm would be a vacuous
> CEIR-vs-CEIR (an A==A can't-fail gate; run_mrt_blend_gpu is the color_slot-bug catcher, so its teeth had to become absolute
> per-attachment blend values, not `==` the sibling). 3a/4a-3 transcribe emit_scene_list's exact verb ladder (command kind /
> geometry kind / coalesce factor / DrawIndex row / binding count / texture slot); run_graph_gpu was already single-path.
> **PROOF (no flag):** crd-render-graph device-free 309/15 + gpu raf7 363/2 (Vulkan+DX12) / 181/1 (llvmpipe); the WHOLE
> **scene-render suite 1539/72 UNCONDITIONALLY** + `crd-sandbox --smoke-test 2` PASS (forward_csm_agx, 11 passes) + frame-cook
> 2752/96 + ceir-gpu 499/34; win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean. **scene.raster is now a pure
> CEIR-authored executor — the §128 migration is COMPLETE.** → 16z (visbuffer.raster dissolution).

> **⛔ STATUS 2026-08-14 — 16z STARTED (visbuffer.raster §41 dissolution). ADVISOR-LOCKED sub-slicing (fork-b is settled at
> §b: fold into scene.raster + uint attachment + typed clear, DELETE executor #13, count 14→13).** The one open design point —
> how a procedural (no-storage) draw is signalled — is DECIDED: a **DECLARED scope-level mode**, never inferred from
> `storage == nullptr` (that null-skip is the resolve-failure guard gated at 1539/72; inferring would turn a resolve failure
> into a wrong draw — the `varying decl contract` scar: declared > inferred). Sub-slices, each independently gated
> (2 Win + 2 Linux + tidy):
> - ✅ **16z-1 DONE + gated — UINT CLEAR through the scene builder.** `SceneBuildDesc` gained `clear_is_uint` + `clear_uint`;
>   `build_scene_ceir` emits the colour attachment with a **u32-format** image (`type_int(32,false)`) + `clear_kind="uint"` +
>   `clear_uint` when uint, the float `clear_r/g/b/a` otherwise (the materializer already reads them, RAH-1a.1; the verifier's
>   `ClearKindFormatMismatch` scar REQUIRES the uint image type — build_scene_ceir runs find_render_misuse, so the u32 image is
>   mandatory). `ceir 16z-1` device-free test (build_scene_ceir(clear_is_uint) → materialize → `ClearKind::Uint`+id, float
>   companion stays Float). Gate: ceir-gpu 511/35 win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean.
> - ✅ **16z-2 DONE + gated — PROCEDURAL scope-mode (CEIR representation).** A closed-vocab attr on `render.scene_draw_list`
>   (`geometry = "storage" | "procedural"`, absent = storage), wired the 4a-1 way: `render.ceirop.toml` + `ceir_opgen.py` regen
>   (crd-ceir-opgen-drift #634 clean) + `kGeometryVocab` + `RenderMisuseKind::GeometryModeInvalid` (append-at-end enum + the
>   name switch — the `-Werror=switch` discipline) enforced in `scan_render_region`'s draw arm; `ceir_render_string_ok` returns
>   true when absent ⇒ storage default (the `.valid()`-safe reader). `SceneBuildDesc.procedural` + `build_scene_ceir` sets the
>   attr. `emit_scene_list` procedural branch transcribes legacy `record_visbuffer_raster` EXACTLY: skip `vertex_count==0` (NOT
>   storage — the storage-null skip is the RESOLVE-FAILURE guard, a different concern), null→def_prog, `command=Draw`,
>   `GeometryKind::None`, `vertex_or_index_count=vertex_count`, **ZERO bindings**, no textures, no coalescing, one scope. Storage
>   mode byte-identical. Tests: `ceir 16d` procedural section (nbinds==0 discriminates the ladders) + `ceir 14a` GeometryMode
>   verifier (bogus→GeometryModeInvalid; procedural/absent accepted). Gate: ceir-gpu 528/35 + ceir 128/13 + opgen-drift on
>   win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean.
>   - ⛔ **DEFERRED to 16z-3 (coupled to the re-author):** the host DrawList-build trace — a re-authored SCENE pass's procedural
>     items must SURVIVE the scene draw-build arm (the null-storage skip recurs at frame_runtime ~L185/448/1165; the visbuffer
>     arm L321-330 that sets `has_storage=false` dissolves). Only testable end-to-end once the asset is `raster.scene`.
> - ◧ **16z-3a DONE (win-debug) — the RE-AUTHOR / FLIP (A-wiring), legacy still in-tree.** Advisor-decided design: (Q1) uint
>   clear DERIVES from the R32Uint target format (`fg_format_is_uint`, RAH-1 — not an author boolean); `clear_uint = clear_id`
>   param. (Q2) the kind STRING `raster.visbuffer` is KEPT and its KindRow row REMAPPED to `kExecSceneRaster` + a new
>   `procedural` role bit (+ `pp::kProcedural`), exactly like `raster.depth_only`/`raster.mrt` — zero asset churn. (Q3) the 15c
>   `VisbufferNeedsUintTarget` contract is RE-KEYED off a UNION trigger (`pass_is_scene_raster && (pp::kProcedural ‖
>   pass_has("clear_id")) ⇒ the R32Uint colour write`, depth writes skipped), NOT `pass_is_visbuffer`. ⛔ Two arms guard two
>   author mistakes: the **procedural arm** (`procedural ⟺ visbuffer` — the ONLY procedural kind) catches "authored a visbuffer,
>   forgot the uint format" (REN-38-A11's `raster.visbuffer` writing RGBA with NO clear_id); the **clear_id arm** catches "any
>   scene pass declared id semantics on a target that can't hold them" (a storage pass with `clear_id` on a float target —
>   `clear_is_uint` would derive false from the format and the id semantics silently vanish). The advisor's initial Q3
>   (clear_id-only) was corrected against REN-38-A11: procedural-only reopens the clear_id hole, clear_id-only drops the
>   procedural arm. `MissingDrawList` stays keyed on procedural alone. Wired: `add_draws_scene(procedural)` carries
>   no-storage gl_VertexIndex draws (`has_storage=false`); `build_frame_plans` scene arm sets `sbd.procedural/clear_is_uint`
>   (from colour-0's format)/`clear_uint`; scene.raster schema gains an optional `clear_id`; the scene `to_authored_pass` arm
>   carries clear_id. **FLIP GATE (the shipped asset now routes scene+CEIR, legacy dead-but-present):** frame-cook 2752/96 (the
>   shipped `scene_visbuffer` cooks as scene+procedural; the re-keyed contract test green with a clear_id added) + **scene-render
>   1539/72 incl. the DEVICE id read-back** (test_scene_render_gpu §1483: the re-authored asset renders two primitives → two
>   different non-background ids through the CEIR procedural path) + render-graph 309/15 (count still 14). win-debug proven.
> - ✅✅ **16z-3b DONE — executor #13 DELETED, FULL gate green, §128 CLOSED (14→13).** The now-dead legacy is deleted:
>   `record_visbuffer_raster` + its record registration; the `visbuffer.raster` **schema** registration (`register_builtin_executors`
>   14→13 TOO, not just records); `map_visbuffer`; the visbuffer `to_authored_pass` arm (frame_runtime L321-333); `pass_is_visbuffer`;
>   `kExecVisbufferRaster`; the frame_asset.cpp `{"raster.visbuffer", kExecVisbufferRaster,…}` row is already remapped (keep it);
>   frame_ceir.cpp `builtin_executor_name` visbuffer line + the "14 built-in NAMES" comment → 13. Repo-wide `== 14U` → `== 13U`
>   sweep across BOTH `register_builtin_executors` AND `register_builtin_records` asserts (grep every `14U`). Fix test_frame_asset.cpp:38
>   (`kExecVisbufferRaster == executor_type_id("visbuffer.raster")` — delete). ⛔ KEEP the gpu-context `draw_visbuffer`/
>   `create_visbuffer_target` verbs (B4-vis-4 raster-context consumers — scope discipline). Verify the gpu-context dx12/vulkan
>   frame-graph visbuffer tests (author `raster.visbuffer` + `clear_id` — should stay green post-remap+re-key). FULL GATE 2 Win +
>   2 Linux + tidy: render-graph device-free + gpu [raf7] both backends + frame-cook + scene-render (device id read-back) +
>   sandbox --smoke-test 2 + the count sweep. CLOSE-OUT: project_ceir16_live_position memory + context.md (strip stale
>   CRD_SCENE_VIA_CEIR/16c/16d) + docs/sessions/2026-08-14-ceir-16d-scene-raster-migration.md + mark CEIR-16 ✅ + a
>   Conventional-Commits msg (NO AI trailer).

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
- **The scene_renderer `render()` program-variant ladder** (block 7, `scene_renderer.cpp` group-consolidation loop
  ~L6280–6379: the `program_[skinned_][textured_][shadowed]` + `_idx` twin selection filling `SceneDraw.program`) = the
  OTHER composite half (`scene.resolve_program`). ⛔ ANCHOR CORRECTED 2026-08-14 (was `:6169–6260`, which drifted onto the
  REN-40 GPU-cull params). **This ladder is HOST-side (in `render()`) BY DESIGN** — see the 16d-live-3b reclassification note.

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

## 4b. ⭐ THE BINDING MODEL + PARITY DOCTRINE (advisor 2026-08-12; ⚠ USER-SURFACED doctrine change)

**Finding (engine-first, verified in the COMMITTED `render.ceirop.toml`):** the `ceir.render` binding model is ALREADY
forward, not slot-based — `render.draw` bindings are TYPED RESOURCE OPERANDS whose *"KIND derives from its CEIR-3c type"*
(the 12a one-source-of-truth doctrine); attachments are general typed values (G-buffer/visbuffer = program-defined
contracts, "NOT special ops", sec-41); RAH-2 resident tables are already the named successor (CEIR-14d). **So there are
NO `input0..7` slots, no `bind_atlas`-at-slot-4, no bindless-of-one in the IR** — the advisor's feared "canonize the
legacy dispatch scars into the IR" is STRUCTURALLY IMPOSSIBLE here. The legacy `RasterDrawPacket` binding vocabulary
(`SampledTexture` / `BindlessTextureArray` / `bind_atlas` slot-4 = the ⛔⛔⛔ atlas-by-SLOT scar / bindless-of-one =
verb-steering) is an ENCODER-DISPATCH artifact, not semantics.

**Resolution:** (1) **No dialect change** — the binding model stays as-is (typed operands). (2) **The 16b gap is the 14z
MATERIALIZER**, which resolves only `IStorageBuffer`; extend `materialize_draw_packet` to resolve TEXTURE-typed operands →
the encoder's texture/bindless bindings, deriving the `ResourceBinding` KIND from the operand's CEIR-3c type. (3) The
slot/sampler/verb choice (slot-4 shadow atlas, bindless-of-one blend-load) lives in the LOWERING — it reproduces the
legacy mapping for PIXEL-parity now, and is REPLACEABLE when RAH-2/CEIR-14d lands resident tables (never an IR contract).

**⚠ PARITY DOCTRINE CHANGE — for CROSS-mechanism migration (executor→IR-lowering), pixel-parity is PRIMARY, not
command-parity.** ADR-0106 Decision #4's command-parity was written for SAME-mechanism migration (old-switch→executor,
where the command stream must be bit-identical). This band migrates executor→IR-lowering, where the binding MECHANICS may
legitimately differ (a forward binding lowering vs. the legacy slot tricks) while PIXELS must not. So the per-item gate is
**pixel-parity readback + `crd-sandbox --smoke-test` both backends** (the semantic gate; the ⛔⛔ A/B-deterministic-clock +
gates-run-configs scars apply to it), with **`MockEncoder` DEMOTED to a structural-sanity check** (B/D/E scope shape +
draw count — NOT descriptor/slot equality). ⛔ This is a doctrine call with a user-visible tradeoff (weaker structural
gate, stronger pixel reliance) — RECORDED here as the recommended resolution and SURFACED to the user (not silently
landed); it is the same class of decision as the Option-A pull-forwards the user has ratified.

## 5. The 16z deletion list = 0h §3 E-table (per-item proof gates ALREADY WRITTEN)

E1 (variant ladder, `scene_renderer.cpp` ~L6280–6379 — ⛔ ANCHOR CORRECTED, was `:6169–6260`) → `scene.resolve_program`,
proof = §128 scene.raster pixel-parity both backends incl bindless/multi-draw = **16d**; E2 (`render()` fg assembly + cull) →
`ceir.frame` + `ceir.scene` resolvers, proof = `crd-sandbox --smoke-test` + REN-38-F6 green through CEIR; E5
(`SceneRenderer::render` composite) → orchestrator only, no private C++ render path (§128) — all deleted at **16z**. RAF-12
scar governs: **coverage before inline deletion** — the E-table IS that coverage inventory.

> **⛔⛔ E1 MEANING RECLASSIFIED 2026-08-14 (16d-live-3b, advisor-confirmed — a decision-record change of the same class as the
> §4b parity-doctrine change, recorded LOUD not silent).** Investigating 16d-live-3b (`scene.resolve_program`) found the
> variant ladder is ALREADY the ratified architecture: fork (c) (2026-08-12) says `scene.resolve_*` is "IMPLEMENTED host-side
> behind the `IFrameGraphHost` seam; currency = pre-resolved handles, NEVER ECS types," and the §4b doctrine says "the resolve
> chain is host-side behind the seam, the DrawList carries pre-resolved handles." The ladder at ~L6280–6379 runs in the scene
> renderer's `render()` (the host = `IFrameGraphHost`) during scene consolidation and fills `SceneDraw.program` with the
> resolved FS-variant twin; `to_authored_pass` selects program/program_depth/program_velocity by pass role; so the DrawList's
> `RenderDrawItem.program` reaching BOTH recorders is PRE-RESOLVED, and neither `record_scene_raster` nor `record_ceir_render`
> re-resolves per-draw (both read `it.program`). **∴ E1 is NOT "delete the ladder"** — the ladder is `scene.resolve_program`'s
> host implementation and STAYS behind the seam; **16z deletes the EXECUTOR (`record_scene_raster`), not the ladder**, and
> verifies the ladder sits behind the host seam (which it does). 16d-live-3b is therefore a RECLASSIFICATION, NOT a code move,
> and it does NOT block 16d-live-3c (both recorders consume the same pre-resolved DrawList; the flag flip never touches the
> ladder). **3b's ONE code deliverable = the record-time TAIL of resolution, which WAS untested:** the `it.program == nullptr →
> pass default` fallback is LIVE on every shadow cascade (`add_draws_scene` nulls per-item programs when `program_is_instance`
> — the PRECEDENCE scar, frame_runtime L105-108), and its interaction with the coalesce predicate (`nprog = nx.program ?:
> def_prog`) had never been A/B'd for scene. `emit_scene_list` (render_materialize.cpp L453/475/545) already matches legacy
> (null→def_prog, coalesce compares resolved programs) — now LOCKED by a `ceir 16d-live-3a` section (two null-program items +
> non-null default → ONE coalesced DrawMulti, resolved program non-null, both paths) + a universal `progs[i] != nullptr`
> invariant. Gate: crd-render-graph 363/14 (167 in the 3a case) on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan;
> tidy clean. **Named-forward:** `scene.resolve_program`'s formal DECLARATION in the CEIR-0d native-intrinsic registry
> (ADR-0110) is deferred — that registry is NOT built as code yet (grep: only `engine/ceir/ops/README.md` names it); building
> a registry for one entry is speculative infra, so the declaration lands WHEN the 0d registry lands (a later band). Until
> then the ladder IS the host resolver behind `IFrameGraphHost` — the ratified fork-(c) shape.

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
