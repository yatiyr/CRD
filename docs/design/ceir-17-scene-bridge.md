# CEIR-17 — Scene / ECS / geometry bridge (design + STATUS ledger)

> Band contract (D-007 tracker CEIR-17, master roadmap §CEIR-14 = §45/§46/§47): **implement scene/query/resource
> resolver semantics; PROOF = rigid / skinned / indirect scene rendering + GPU culling, as CEIR.** This is a
> PROMOTION, not a rewrite (SANITY #8) — the resolution logic already ships (the REN-37 material/technique stack, the
> `scene_renderer.cpp` variant ladder, the frame_runtime draw build, the CKIR cull/skin kernels). CEIR-17 gives that
> machinery its declared CEIR identity + re-expresses the ORCHESTRATION (query → cull → skin → drawlist) as CEIR.

## 0. What CEIR-16 left, and what CEIR-17 adds

CEIR-16 made `scene.raster` a CEIR program that consumes a **resolved** `DrawList` — the host still builds that list in
C++ (`scene_renderer.cpp` extract → cull → skin → resolve material/technique/program/geometry → the `RenderResolvers`
seam feeds `record_ceir_render`). CEIR-17 pushes the **resolution + orchestration** into CEIR as intrinsics/programs.
The roadmap target shape (§ roadmap L3417):

```
material  = scene.resolve_material(draw)
technique = scene.resolve_technique(material, phase)
program   = scene.resolve_program(technique, draw)
geometry  = scene.resolve_geometry(draw)
```

⛔ These names do NOT exist in the code today — they are the TARGET. The logic exists as REN-37 composition + the
`program_skinned_textured_shadowed` variant ladder + `frame_runtime` draw build. 17b DECLARES over that, never invents a
parallel chain.

## 1. Locked constraints (bound every slice)

- **CURRENCY RULE (the band's spine, ADR-0106 #3):** the ECS extract stays HOST; CEIR sees pre-resolved **handles / ids
  ONLY**, NEVER ECS types. §22-18: id/hash-keyed tables, no string lookup at record.
- **The seam already exists:** the `RenderResolvers` callback shape (`Operation* + void* user → handle`, in ceir-gpu) is
  the proven host-intrinsic dispatch. **Extend it — do NOT invent a second seam.**
- **Intrinsics are ordinary CEIR ops** (ADR-0110): defined via the CEIR-2 per-dialect TOML with `intrinsic = true`
  (`op_info.intrinsic` + `native_provider` promoted at CEIR-7a; `program_asset` tracks them). No separate registry to
  build. The scene resolvers are the **replaceable convenience tier** (a legitimate intrinsic a user MAY replace with a
  CEIR program — ADR-0110 §3).
- **`scene_renderer` is the LIVE PERF PATH** (chunk-grain extract, velocity OOM@1M, TAA prev_world scars): orchestration
  re-expression must NOT touch the extract/upload hot loops; any perf-adjacent change gets **median-of-five**.
- **Every new execution path runs the FULL suite on a REAL device** (not a cook roundtrip). At each deletion, ABSOLUTE
  asserts (the `delete-ref-degrades-to-A==A` scar) — never a vacuous CEIR-vs-CEIR A/B.
- **No new ADR at band-open** — 0106#3 (seam) + 0109 (ownership) + 0110 (intrinsic schema) already decided it. Revisit
  ONLY if 17c's query representation turns out to be a genuinely new architectural commitment (decide there).

## 2. Decomposition — six gated rows (advisor-locked 2026-08-14)

Each row gates 2 Win (win-debug + win-asan) + 2 Linux (linux-gcc-debug + linux-gcc-asan) + clang-tidy.

- ✅ **17a DONE + gated — DECLARED the `scene.resolve_*` intrinsic ops.** A NEW `scene` dialect (advisor: the roadmap
  names parse as dialect `scene`; render is the GPU-command dialect): `scene.ceirop.toml` with resolve_material /
  resolve_technique / resolve_program / resolve_geometry, each `[op.native] provider=host` (the FIRST TOML intrinsics —
  ADR-0110; replaceable convenience tier) + honest read effects (SceneRead for the scene-assigned material/geometry,
  HostStateRead for the hot-reloadable technique/program registries — NOT Pure, so no CEIR-26 CSE across a hot-reload) +
  ExternalNondeterminism + hot_reload_safe=false. FIVE opaque dialect-owned Extern type-classes (draw/material/technique/
  program/geometry — the attachment precedent, each a distinct TypeId; hand-written `scene.hpp`/`scene.cpp`, mirroring
  render.hpp) so the chain is TYPE-checkable. `phase` is a closed-vocab ATTR (opaque|transparent|shadow|depth|velocity),
  not an operand. `find_scene_misuse` (its OWN misuse enum — no widen of RenderMisuseKind) walks the type-chain
  (mistyped operand → named misuse; phase vocab). Tests `ceir 17a` (well-formed chain + 4 mistyped negatives + phase
  vocab + the intrinsic reflection op_info.intrinsic/native_provider=="host" + type-class distinctness). Gate: ceir
  [scene] 30/3 + full 3349/436 + opgen-drift on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean.
  ⛔ preset-reconfigure done for the new source files (the new-source-file scar).
- ✅ **17b DONE + gated — the host-impl seam + the chain evaluator.** Extended `RenderResolvers` (render_materialize.hpp)
  with the four scene-resolve `(fn, user)` pairs; the currency is an opaque `u64 SceneResolveHandle` (id-keyed §22-18 —
  NOT a device pointer; resolve_program's u64 → IRasterProgram* at the seam by the HOST, never CEIR). A chain-shaped
  `evaluate_scene_resolve(ctx, module, resolvers, draw_seed, draw_handle, out)` (NOT a general interpreter — that's
  CEIR-26): VERIFIER-FIRST (find_scene_misuse → `ExecuteError::SceneChainMisuse` on a mistyped chain, never a garbage
  handle), walks the resolve ops SSA-order binding op-result→handle in a small fixed map, calls one callback per op with
  the RESOLVED upstream handle(s); the evaluator owns the phase-attr read; a null/0 callback ⇒
  `ExecuteError::UnresolvedSceneHandle`. Tests `ceir 17b` (device-free, SENTINEL callbacks): the chain THREADS (technique
  receives material's output; program receives technique + draw; UNEQUAL sentinels per stage) + per-phase distinct + the
  two refusals. Gate: ceir-gpu [scene] 12/2 + full 540/37 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy
  clean; no new files (no reconfigure). ⛔ **BOUNDARY (17c inherits as a contract):** 17b proved the chain THREADS through
  the seam, NOT yet "== the C++ path's handles" — the REAL REN-37 ladder + the draw-handle table are 17c (they touch the
  live perf path). `[op.native] thread_safe` stays UNDECLARED — re-verifies at 17c with the real ladder (a sentinel proves
  nothing about record-time behavior; setting it now would be the overclaim-the-optimizer lie).
- **17c — the query → DrawList bridge, RIGID first.** The `[[draw_list]]` query declaration (all=components + cull/sort
  policy) already exists as asset data; this slice builds the CEIR semantics underneath it. Run as a PARALLEL path
  A/B'd against the host builder while both exist (the 16d flag-dance method — whole-suite-on-real-device).
- ✅ **17d DONE + gated 2026-08-14 (advisor RE-SCOPED at band-entry): PROOF slice + shipped-config verify gate.** The GPU cull +
  INDIRECT orchestration is ALREADY authored CEIR — the LIVE 1M config `forward_csm_gpu.frame.toml` records the cull as
  `kind="compute"` passes (`cull_reset`/`cull_view0..4`/`occlusion_*`) with host-resolved kernels, `cull_args` an
  `indirect_args` resource, the raster passes drawing off the device count (`DrawItem::args`). So 17d does NOT re-express
  anything — it ASSERTS the existing `gpu_cull_verify` CPU-vs-GPU verdict compare (today INFO-logged, never CHECKed) on a
  SHIPPED config. See the STATUS block for the SINGLE gate (view0 `==` via the unoccluded-scene construction) + the phase-2
  trap + the discovered stale `scene_gpu_cull` artifact (→ 17z). Zero renderer changes (test-only).
- **17e — SKINNED as CEIR.** REN-40-F palette-compute orchestration + skinned variant selection flowing through
  `resolve_program`.
- **17z — band proof + 0h deletion.** Rigid / skinned / indirect through CEIR both backends + sandbox smoke; DELETE the
  replaced host orchestration parity-gated, ABSOLUTE asserts at each deletion.

## STATUS

> **✅✅✅ 2026-08-15 — CEIR-17 BAND CLOSED + 4-config gated (17a-e + 17z all ✅).** Scene/query/resource resolver
> semantics DECLARED (the `scene.resolve_*` intrinsic family over an EXTENDED `RenderResolvers` seam — no second seam,
> ADR-0106 #3 held: ECS extract stays HOST, CEIR sees handles/ids only) and rigid / skinned / indirect / GPU-cull PROVEN
> as CEIR (a PROMOTION, SANITY #8 — the machinery was already authored CEIR; the band gave it declared identity + proof).
> ⭐⭐ **17z band-close = a REGRESSION HUNT** (deletion-gate RE-RUN + a HEAD `git stash` baseline — the GATE-reverifies-rows
> discipline): re-running the gate against a clean HEAD surfaced **3 latent CEIR-replay drops that EVERY per-slice gate
> missed** — all fixed forward:
> 1. **§128 null-plan (silent render-0):** a migrated executor that got no CEIR replay plan rendered NOTHING silently
>    (the imperative fallback that used to cover it is deleted). FIX: `FrameExecError::MissingCeirPlan` raised LOUD at the
>    `ap.plan == nullptr && pass_is_migrated_ceir(d)` install site + `execute_frame_graph` now stack-builds `FramePlans`
>    via `build_frame_plans` and fails closed. (Also caught REN-38-B1's 2-frame loop silently rendering 0.)
> 2. **WBOIT composite-blend drop:** `build_fullscreen_ceir` set the composite attachment's `load` attr but DROPPED its
>    `blend` (→ Opaque composite = the asymmetric-transparency scar re-opened). FIX: emit the `blend` attr when
>    `desc.blend != Opaque` (anon-namespace `blend_str`; fixed the C2668 with a scoped forward-decl).
> 3. **VRS shading_rate / conservative drop:** post-migration the pipeline shading-rate + conservative mode were dropped.
>    FIX: payload-forward exactly like `force_load` — RenderResolvers fields → `record_ceir_render` enum_param reads →
>    `materialize_draw_packet` writes `out.state` → `command_lowering` dispatches `draw_vrs`/`draw_conservative`. NO new
>    CEIR dialect attr (the plan/payload split is settled — VRS is record-time state, not IR).
>
> Deleted stale `scene_gpu_cull.frame.toml` (+3 stale refs: bridge test, template-bridge comment, ceir-15 design). GATE:
> **gpu-context-vulkan 260/0 win-debug + win-asan (ASan-clean); 259/1skip linux-gcc-debug + linux-gcc-asan** (the 1 skip
> = VRS caps-guard on llvmpipe = CORRECT skip, not a hole); blast radius **frame-cook 2751/96 + render-graph 309/15 +
> scene-render 1587/74** + tidy GREEN on all 4 configs; band-proof matrix (rigid/skinned/indirect × Vulkan/DX12) +
> sandbox `--gpu-skin --gpu-cull-verify` smoke (185 frames, 5031 instances drawn). 3 memory scars filed
> ([[feedback_device_skin_passes_both_need_khdrgpuskinactive_gate]], [[feedback_deleting_the_imperative_fallback_unmasks_a_migrated_null_plan_hole]],
> + the WBOIT/VRS forward-drops recorded in the rows below). ⛔ **NEXT = CEIR-18** (renderer proof suite:
> Forward+/Clustered/Deferred/Visibility/GPU-driven as CEIR assets — no new pass algorithm; reuse technique/material +
> B8 lighting; base exists: cull=17d, visibility=16z, B8-deferred green; advisor at band-open, expand sub-slices). Only **7 files
> uncommitted** — VRS+WBOIT code (`render_materialize.hpp`/`.cpp`, `frame_graph.cpp`, `render_fullscreen_build.cpp`) + 3
> band-close docs; the §128 fix + REN-38 test plan-builds + `scene_gpu_cull.frame.toml` deletion + 17a-e all landed at
> **8c019fc**, CEIR-16 at c44adbf (user commits). Slice-by-slice detail follows.
>
> **⛔ 2026-08-14 — CEIR-17 BAND OPEN.** Advisor-led decomposition (above) locked; the three band-open verifications
> done: (1) the intrinsic registry EXISTS (CEIR-7a `op_info.intrinsic`/`native_provider`; ADR-0110 schema fixed) ⇒ 17a
> is DECLARE not build; (2) CEIR-0z sized CEIR-1…13 in aggregate (≈34–55 KLOC) — CEIR-17 (old CEIR-14) inherits the
> coarse estimate, sub-sliced here by shape; (3) the resolution lives in REN-37 + the scene_renderer variant ladder +
> frame_runtime draw build (NOT in any `resolve_*` fn today — the roadmap chain is the target shape). No new ADR.
>
> **17a grounding (the RenderResolvers currency, done 2026-08-14):** the proven host-callback seam is a bundle of
> `(fn, user)` pairs, each `Operation* + void* user → device handle` (`render_materialize.hpp` `RenderResolvers`:
> target/program/storage/texture/texture_array + the DrawList `draws_count`/`draws_item` → `RasterDrawItem`). TODAY the
> host PRE-RESOLVES each `RasterDrawItem`'s `program`/`storage`/`texture` handles (material→technique→program→geometry) in
> C++ BEFORE CEIR records; scene.raster consumes the resolved list. 17a moves that resolution INTO CEIR: the four
> `scene.resolve_*` ops (operands = draw-id / phase / a prior handle; results = OPAQUE handle-typed values — the currency
> is handles/ids, never ECS types) resolve via the SAME host-callback seam (EXTENDED, not a second seam).
> - **⚠ 17a-start FORK for the advisor (op-type design):** (a) a NEW `scene` dialect TOML vs EXTENDING `render`;
>   (b) the handle RESULT type — an opaque CEIR-3c resource/external-resource type vs a scalar id (§22-18 id-keyed);
>   (c) whether `resolve_*` are result-producing ops the host callback fills (extend RenderResolvers with a
>   `resolve_material/technique/program/geometry` fn family) vs a single generic `scene.resolve(kind)` op. Decide WITH the
>   advisor at 17a-start, grounded in ADR-0109's finalized type names + the REN-37 ladder's real handles.
>
> **✅ 17a + ✅ 17b DONE + gated 2026-08-14** (see the rows above). NEXT: **17c — RE-SCOPED 2026-08-14 (advisor, at
> band-entry): "REAL-ladder callbacks + the whole-DrawList phase-discriminating parity ORACLE (rigid)".**
> ⛔ **The query→DrawList bridge ALREADY EXISTS** — the ECS query is `frame.draw_list` (an op in the `ceir.frame` dialect,
> band 15, producing a draw-list Extern value), and `IFrameGraphHost::draw_list_query` (REN-36.3-b) hands the host the whole
> `FrameDrawListDesc` (all/any/none + cull/sort/limit); the renderer (which owns the World) runs the query → pre-resolved
> `RenderDrawItem`s (the REN-37 ladder runs INLINE in the scene extract, filling `program`/`program_depth`/`program_velocity`).
> So the **⚠ new-ADR contingency is CLOSED** (the architectural commitment was made at band 15 + REN-36.3-b — NOT a 17c
> decision); the "parallel path A/B'd against the host builder (16d flag-dance)" prescription is MOOT (there is nothing to
> flip — the path exists). Cleared the ⚠ from the tracker 17c row.
> - ⛔⛔ **OPTION (b) REJECTED (making the live draw-build run `evaluate_scene_resolve` per-draw-per-frame):** it contradicts
>   TWO locked decisions — (i) 16d-live-3b's advisor-confirmed reclassification ("the variant ladder is HOST-side by design;
>   the DrawList carries PRE-RESOLVED handles", design-doc §5 / L447-449, fork-c / ADR-0106 #3), and (ii) the band-open perf
>   constraint (no extract hot-loop touches — a per-draw chain interpretation IN extract is exactly that). The scene.resolve_*
>   intrinsics are the DECLARED, replaceable-tier IDENTITY of the resolution, NOT a hot-path interpreter. A future
>   CEIR-authored resolver replaces the ladder AT THE SEAM, WHOLESALE (never per-draw in record). This is architecture, not
>   deferral.
> - **17c deliverable:** (1) a host-side frame-scoped draw-handle TABLE (`draw_handle = DrawList index + 1`, 0=null, id-keyed,
>   never persisted; no IR change). (2) the FOUR real resolve callbacks implemented in the scene-render HOST layer over the
>   SAME tables/ladder the extract uses (if the ladder is private lambdas in `render()`, expose a READ-ONLY resolver entry —
>   do NOT restructure the extract). (3) the PARITY ORACLE: the phase vocab maps onto the program twins —
>   `chain(phase=opaque)==it.program`, `chain(phase=depth|shadow)==it.program_depth?:it.program`,
>   `chain(phase=velocity)==it.program_velocity?:it.program` — a phase-discriminating differential oracle (the `gpu_cull_verify`
>   pattern) over EVERY item in a real scene's resolved DrawList, per phase (not a sample). RIGID first: skip skinned items
>   EXPLICITLY and COUNT them (no silent caps — log skipped/covered so the 17e tail is visible). ⛔ Assert only the PROGRAM
>   (the chain END — the only value the C++ path attests per item); material/technique are host-table identities (assert
>   THREADING through them, done in 17b — not intermediate == the C++ path). (4) re-verify `[op.native] thread_safe` from the
>   REAL ladder (is the extract jobified? the perf-jobs scar says jobs exist here — set it from observation; if mixed, claim
>   `false` or stay undeclared; opgen regen + drift if the toml changes). Gate: the parity test on a REAL device (scene-render
>   headless-Vulkan harness) + scene-render suite green UNTOUCHED + ceir-gpu device-free; 2 Win + 2 Linux + tidy. 17d
>   UNAFFECTED (the cull lives inside the host's query resolution — its CEIR orchestration + `gpu_cull_verify` oracle land there).
>
> **⛔ 17c STATUS 2026-08-14 — RE-SCOPE (above) recorded; thread_safe RESOLVED; the parity oracle IS THE CODE DELIVERABLE, scheduled next.**
> - ✅ **thread_safe re-verification DONE (conclusion: STAYS UNDECLARED).** Grepped the scene-render extract / REN-37
>   resolution for job/thread parallelism (`parallel_for` / `JobSystem` / `std::thread` / worker): NONE — every "parallel" in
>   scene_renderer is an *index-parallel array*, not thread parallelism. The resolution runs SINGLE-THREADED at record, so
>   there is NO evidence the resolve callbacks are ever called concurrently ⇒ claiming `thread_safe=true` would be an
>   unobserved overclaim (the optimizer-trust lie); the honest position is to LEAVE IT UNDECLARED. ⇒ **NO toml/opgen change**
>   (scene.ceirop.toml stays as 17a shipped it — drift-clean).
> - ✅✅ **17c CODE DELIVERABLE DONE + gated — the phase-discriminating parity ORACLE (device-free).** ⛔ **ADVISOR
>   DOUBLE-RETRACTION (honesty, like the 16z union-trigger correction):** the "real scene's resolved DrawList + a
>   scene-render DEVICE test" prescription was RETRACTED against two pieces of evidence I surfaced — (a) UNIFORMITY: `fill()`
>   passes the twins through verbatim + the C++ selection is the uniform 3-line branch at frame_runtime.cpp:192-193 (real
>   items vary twin VALUES, never selection LOGIC; `SceneHost` is .cpp-file-local — un-nameable from a test), and (b) POINTER
>   OPACITY: neither path dereferences a program (assertion is pointer EQUALITY), so distinct sentinel `IRasterProgram*` serve
>   identically to real device programs. ⇒ the oracle is DEVICE-FREE, and 17c makes **ZERO renderer changes** (no read-only
>   accessor — scene-render leaves the blast radius entirely). The GROUND TRUTH is graded for INDEPENDENCE: `to_authored_pass`
>   is .cpp-file-local (no true differential available), so the oracle asserts against the DrawItem twin FIELDS directly
>   (cited to 192-193), with distinct-per-twin sentinels + BOTH fallback arms to catch a one-sided transcription error
>   (residual = a double-transcription of 3 cited lines — the accepted floor); `resolve_geometry==it.storage` has a REAL
>   independent ground truth (storage is attested by the C++ path — `ad.storage`). **PARITY CLAIM (re-worded per advisor —
>   NOT "over a real render"):** *the CEIR resolve chain reproduces add_draws_scene's per-phase twin selection (the ladder's
>   output, externalized on the DrawItem), pointer-equal, EXHAUSTIVE over phases × fallback arms* — 4 items {both twins /
>   program_depth null / program_velocity null / both null} × 5 phases {opaque, transparent, shadow, depth, velocity} = 20
>   chains, each `chain(phase).program == the 192-193 twin` + `chain.geometry == storage`. `ceir 17c` in
>   tests/ceir-gpu/test_execute.cpp (extended the 17b TU — no new file, no reconfigure). Gate: ceir-gpu [scene] 73/3 + full
>   601/38 on win-debug + win-asan + linux-gcc-debug + linux-gcc-asan; tidy clean; frame-cook + scene-render UNTOUCHED (zero
>   engine source change). ⇒ **CEIR-17c DONE.**
>
> **⛔ 17d STATUS 2026-08-14 — RE-SCOPE (advisor, at band-entry): "PROOF slice + shipped-config verify gate".**
> ⛔ **The GPU cull + INDIRECT are ALREADY authored CEIR** (mirrors the 17c discovery). Band-entry orientation, four
> facts verified against the source:
> - **The orchestration IS an authored frame graph.** `assets/frame/scene_gpu_cull.frame.toml` (standalone: `cull_reset`
>   + `cull`, camera view) and `assets/frame/forward_csm_gpu.frame.toml` (the LIVE 1M config: `palette_snapshot`,
>   `gpu_skin`, `cull_reset`, `cull_view0..4` = camera + 4 cascades, `occlusion_reset`, `occlusion_cull`) — EVERY cull
>   pass is `kind="compute"` with an authored kernel URI. Since CEIR-16b migrated `compute.dispatch` → generic
>   `record_ceir_render`, the cull orchestration ALREADY records through CEIR. The five-view dispatch is FIVE AUTHORED
>   PASSES (the suffix bakes `view_index` as a cook-time constant), NOT an inline C++ `for` loop.
> - **Every cull kernel is HOST-RESOLVED, never inline-dispatched.** `cull`, `cull_mark`, `cull_view0..4`, `cull_reset`,
>   `occlusion_cull`, `gpu_skin`, `palette_snapshot` are all registered via `program_registry.register_kernel(prog_id(...))`
>   (`scene_renderer.cpp:2996-3016`); their only other use is COOKING (`ensure_*_kernel` → `cook_*_stage_named`). No
>   `dispatch(kern_cull...)` call site exists — the authored graph asks for the kernel by URI and the host callback returns
>   the cooked `IGpuProgram*`. (Advisor's "check ALL five kernels" — occlusion + mark included — all clean.)
> - **INDIRECT is scene.raster (CEIR-16d).** `cull_args` is declared `kind="indirect_args"`; the raster passes (`csm_cascade`,
>   `depth_prepass`, `forward`, `impostor`) `reads = ["cull_args"]` and draw indirectly off the device-written count
>   (`DrawItem::args`, the indexed-indirect verb that pushes the DrawIndex row). The count never touches the CPU.
> - **The `gpu_cull_verify` differential EXISTS but is NEVER ASSERTED.** `read_gpu_cull_counts` (`scene_renderer.cpp:4354`)
>   downloads post-frame `cull_args` and fills `GpuCullCounts::instances[v]` (Σ over LOD slots of the device `instance_count`)
>   AND `cpu_instances[v]` (the CPU frustum verdict, kept because `gpu_cull_verify` skips the CPU-clear at 5996). The struct
>   doc (`scene_renderer.hpp:527-532`) states the intent: `instances[v] == cpu_instances[v]`. But the only test that runs
>   verify (`test_scene_render_gpu.cpp:4780`, the occ A/B) asserts PIXELS on a BESPOKE single-view inline TOML
>   (`k40gOccCullToml`), and only INFO-LOGS the counts (4826-4830) — the verdict-equality is CHECKed NOWHERE. The F6 gate
>   (1545) runs SHIPPED `scene_cull.frame.toml` but verifies near/far verdicts, not the CPU-vs-GPU compare. ⇒ two scars open:
>   **gates-run-configs-the-app-never-ships** (the differential runs only on a bespoke graph) + the compare is unasserted.
> - ⛔⛔ **THE TRAP (advisor) — count-equality is NOT valid on `forward_csm_gpu`.** Its phase-2 occlusion re-cull
>   (`occlusion_reset` zeroes all 5 commands; `occlusion_cull` refills ONLY view0 with frustum∩HZB, TOML L239-254) means
>   post-frame `cull_args`: view0 = occlusion-filtered ≤ frustum `cpu_instances[0]`, views 1-4 = 0. Since `instances[v]` is
>   read POST-FRAME (4384-4393), `instances[v] == cpu_instances[v]` fails LEGITIMATELY on the CASCADE views (1-4, zeroed) —
>   that is exactly why the occ test asserts pixels, not counts. ⇒ **RESOLUTION (advisor):** on VIEW0 the occlusion refill is
>   a provable NO-OP for an UNOCCLUDED scene (well-separated cubes; every screen rect shows the reverse-Z background clear, so
>   the HZB rejects nothing — REN-40-G3 gates that no-op pixel-wise), so `instances[0] == cpu_instances[0]` IS sound on
>   view0 of the shipped `forward_csm_gpu`. Views 1-4 stay unasserted (zeroed by phase-2). No separate no-phase-2 config is
>   needed — and the only one that would have qualified (`scene_gpu_cull`) is stale/uncookable (struck above).
>
> **⛔ SUPERSEDED 2026-08-14 (in place) — the "2-gate" shape below prescribed a DEAD Gate-1.** ~~Gate-1 = verdict-equality
> on shipped `scene_gpu_cull.frame.toml`; Gate-2 = engagement on `forward_csm_gpu`.~~ Orientation found `scene_gpu_cull` is
> a **stale artifact**: it references the UNREGISTERED kernel URI `engine://scene/cull_compact` (the program_registry has
> only the per-view cooked variants `cull_view0..4` — its "ONE KERNEL, FIVE VIEWS / index stamped on the desc" header is the
> SUPERSEDED per-desc design), it is a **compute-only FRAGMENT** (no `@output` — never rendered standalone;
> `test_frame_template_bridge.cpp:407` excludes it for exactly this), and NO consumer loads it. ⇒ it will not cook; the
> `instances[0]==cpu_instances[0]` gate cannot run on it. **Disposition: fix-or-DELETE at 17z** (the band's deletion slice —
> a replaced-design artifact is precisely 17z's material; do NOT blind-patch a never-consumed asset — the fixture-locked-valid
> scar: an edit with no verifier is worse than documented staleness).
>
> **✅ 17d deliverable — RE-DECIDED (advisor, 2026-08-14): ONE gate on shipped `forward_csm_gpu`, the EXACT `==` via the
> unoccluded-scene construction.** The `<=`-vs-`==` worry (phase-2 occlusion refills view0 with frustum∩HZB) DISSOLVES for a
> scene of well-separated cubes: every cube's screen rect shows BACKGROUND depth (reverse-Z clear = 0), so the HZB test can
> reject nothing — occlusion is PROVABLY a no-op, and REN-40-G3 already gates that exact no-op property PIXEL-wise on the same
> cube fleet. So `gc.instances[0] == gc.cpu_instances[0]` is SOUND on view0 of the real shipping config; if it ever trips, that
> is a real G3 violation surfaced, not flake. This single gate is strictly BETTER than the dead two-gate shape — the verdict
> compare lands on the config the app actually ships. Test-only, zero renderer changes, real device, `read_shipped_asset`
> (F6 pattern + CRD_ASSETS_DIR scar). Assertion ladder:
> 1. **SKIPs:** no graphics Vulkan device; `!renderer.set_shadows_enabled(true)` (the config `requires=["shadows"]`).
> 2. **Ordering (scene_renderer.hpp:714 scar):** `set_gpu_cull(true)` + `set_gpu_cull_verify(true)` + shadows BEFORE
>    `set_frame_graph_toml` cooks the graph (mirror the occ test's sequence).
> 3. **Step-down pin (A==A scar):** `renderer.gpu_cull()` still effective post-render; `raster->compute_dispatch_count() > 0`
>    (the `forward_agx` fallback has NO compute passes — a clean discriminator); `r.draws > 0`.
> 4. **Readback non-vacuous (registered-default-empty scar):** `REQUIRE(read_gpu_cull_counts(gc))`, `gc.groups > 0`,
>    `gc.bounds_checked > 0` (bounds_mismatch==0 with bounds_checked==0 is a pass that checked nothing).
> 5. **THE DIFFERENTIAL:** `CHECK(gc.instances[0] == gc.cpu_instances[0])` + `> 0`; reference sanity
>    `gc.cpu_instances[0] >= 1 && <= near_count` (F6's pattern — proves the far cubes were frustum-culled, so the tie is NOT
>    at `total`).
> 6. `CHECK(gc.bounds_mismatch == 0)` (the device tested the SAME AABBs the CPU did).
> 7. ⛔ Do NOT assert views 1-4 — `occlusion_reset` zeroes their commands post-frame (`forward_csm_gpu` L239-254); a comment
>    cites the TOML lines so nobody "fixes" the zero into a false gate.
> ⛔ No median-of-five (a diagnostic readback, not the hot loop). GATE-re-verifies scar: runs on a REAL device across 2 Win +
> 2 Linux + tidy — NOT inheriting the occ/F6 green. First-ever headless `forward_csm_gpu` run is the largest unknown (TAA
> constants / velocity / impostor per-frame setup) — if render trips, that's TEST-SUPPORT (flag here), still zero renderer
> change. DX12 twin: `test_scene_render_gpu.cpp:5506-5509` is a DX12 gpu-cull test — add the twin IF the body ports (HLSL-masks
> / run-both scars); Vulkan-only is defensible (runs on both OS gates; the DX12 occ twin already covers the indirect layout).
>
> **✅✅ 17d DONE + gated 2026-08-14 — the verdict compare lands GREEN on the shipped live config.** `CEIR-17d GATE`
> (`[scene-render][ceir][ceir17][ren40][gpu][vulkan]`, `test_scene_render_gpu.cpp`) — 18 assertions, the differential
> `gc.instances[0] == gc.cpu_instances[0]` HOLDS on `forward_csm_gpu` (the first-ever headless run of the full 1M config —
> TAA/velocity/impostor all cook + render clean, no test-support plumbing needed), the step-down pin + non-vacuous-readback
> pins all fire, `bounds_mismatch == 0`. Gate: win-debug (ctest #6119) + win-asan + linux-gcc-debug (llvmpipe ran the FULL
> device path, not skipped) + linux-gcc-asan (ctest #5914, ASan-clean under the harness's own options); clang-tidy gated +
> clean. ZERO renderer changes (test-only — one TEST_CASE appended, no new file, no reconfigure). **Vulkan-only** (advisor-
> blessed): the gate runs on BOTH OS gates, and the DX12 GPU-cull twin (`test_scene_render_gpu.cpp:5455+`, k40gOccCull pixel
> A/B) already covers the D3D12 indirect-command layout; the count-readback's backend-specific stride/offset on DX12 stays
> uncovered by inference (a DX12 `read_gpu_cull_counts` twin would require `forward_csm_gpu`-on-DX12, untested headless —
> noted, not blocking). ⛔ Discovered stale `scene_gpu_cull.frame.toml` recorded → **17z** fix-or-delete. ⇒ **CEIR-17d DONE.**
>
> **✅✅ 17e DONE + gated 2026-08-14 (advisor RE-SCOPED at band-entry): PROOF slice — SKINNED as CEIR; SURFACED + FIXED a
> real engine defect.** Orientation (mirrors
> 17c/17d): the skinning is ALREADY authored CEIR. (1) The palette compute is the `gpu_skin` CKIR kernel
> (scene_renderer.cpp:2654+), host-resolved via `program_registry.register_kernel` at 3014, a `kind="compute"` pass in
> `forward_csm_gpu` recorded through `record_ceir_render`. (2) The skinned VARIANT selection is the resolve_program dimension
> (scene_renderer.cpp:6276-6305 — `skinned × {base/textured/shadowed}` → `program_[skinned_]?[textured_][shadowed]`), the axis
> 17c EXPLICITLY skipped ("rigid first"). (3) The CPU-vs-GPU-skin differential ALREADY EXISTS (REN-40-F GATE, Vulkan
> test_scene_render_gpu.cpp:4059 + DX12 4192): GPU palette BIT-IDENTICAL to CPU palette — but on a BESPOKE inline TOML, not a
> shipped config (the gates-run-configs gap, same as 17d). Advisor ruled **(b)** shipped-config skin parity (NOT a device-free
> oracle: 6276-6305 is an 8-way data-dependent branch — a sentinel oracle would re-transcribe all 8 arms, the double-
> transcription the 17c retraction capped).
> - ⛔⛔ **BLOCKER FOUND (empirical + code) — a bit-identical CPU-vs-GPU-skin A/B on `forward_csm_gpu` is STRUCTURALLY
>   IMPOSSIBLE; arm A (CPU skin on the GPU graph) is an UNSUPPORTED CONFIG.** The one-graph-flip gate (both arms
>   `forward_csm_gpu`, flip `set_gpu_skinning`) gave **diffs=2036** — and the SAME 2036 under a two-fresh-renderer / frame-0
>   construction (empty history, identical jitter). A constant diff across radically different temporal states ⇒ a STATIC
>   rasterized difference (palettes differ), NOT TAA (advisor's catch: I first mis-attributed it to velocity/TAA — my own
>   data refutes that). Root cause by CODE: (i) scene_renderer.cpp:5181 `if (impl.gpu_skinning_on)` gates the skeleton/clip
>   UPLOAD — arm A never uploads skel/clip; (ii) the cull path (5753) sets `dispatch_groups>0` regardless of skinning, so arm
>   A's `gpu_skin` pass DISPATCHES and reads unpopulated skel/clip → corrupts the CPU palette. (iii) sandbox main.cpp:844-852:
>   the app selects the GPU graph via `want_gpu_cull||want_gpu_skin` and REQUIRES `set_gpu_cull(true)` — GPU-skin ⇒ GPU-graph
>   ⇒ GPU-cull, so `forward_csm_gpu`+CPU-skin never arises in the APP — but that is an app-level WORKAROUND, not a design
>   contract (the engine API permits the combination and silently renders wrong). ⇒ a real engine defect, see RESOLVED below.
> - **⛔ SUPERSEDED (advisor reconcile 2026-08-14): ~~DIRECTION outcome-1 — GPU-arm-only + deformation-present + REN-40-F
>   citation.~~** That was conditional on arm-A being "unsupported BY DESIGN"; the evidence REFUTES that. The sandbox
>   coupling (844-852) is an APP-level workaround — the engine API freely permits `set_gpu_cull(true)+set_gpu_skinning(false)
>   +skinned mesh+forward_csm_gpu` and silently renders wrong. Downgrading to deformation-present would be
>   document-and-accept a loss when a SOLVE exists.
> - **✅ RESOLVED — it is a REAL ENGINE DEFECT; FIXED (whole-class-effect gate).** `palette_snapshot`'s own comment
>   (2836-2838) names this clobber hazard and gates that kernel on `kHdrGpuSkinActive` (2865); the `gpu_skin` kernel had the
>   IDENTICAL hazard with NO gate — the whole-class-effects scar ([[feedback_effect_narrowing_must_suppress_all_whole_class_effects_not_just_the_obvious_one]]:
>   the gate applied to ONE pass of the class, not both). FIX: wrap the `gpu_skin` kernel body in an `if (active != 0)` guard
>   on `kHdrGpuSkinActive`, mirroring `palette_snapshot` VERBATIM (scene_renderer.cpp ~2696 + ~2698 + ~2818). ⛔ The
>   fix-then-rerun IS the discriminator (I skipped localization): post-fix the 17e A/B on the shipped config gives
>   **diffs==0** — simultaneously CONFIRMING the diagnosis (arm-A corruption, NOT TAA — the constant-2036-across-temporal-
>   states catch was right), the fix, and the gate landing. The strong bit-identical CPU-vs-GPU-skin A/B on the SHIPPED
>   config — 17e's real deliverable — is restored. ⛔ **17e is NO LONGER test-only** (an engine kernel change). ⭐⭐ This is
>   the strongest vindication yet of gates-run-configs: REN-40-F's bespoke cull-less TOML kept `dispatch_groups==0` and
>   MASKED the omission for the entire life of the feature; only running the SHIPPED config caught it. Memory-worthy at close.
> - ✅ TEST DEFECT fixed (advisor): `covered > 500` was VACUOUS (forward_csm_gpu clears to [0.09,0.10,0.13] — whole frame
>   "covered"); now counts pixels ≠ the corner-background instead.
> - **GATE (fix + gate): win-debug FULL scene-render suite 1587/74 + [skinning] 67/3 (REN-40-F Vulkan+DX12 stay
>   bit-identical — gate TRANSPARENT when active=1, both directions covered) + 17e 30/1; linux-gcc-debug + linux-gcc-asan
>   [skinning] 49/2 (llvmpipe device path); clang-tidy clean (scene_renderer.cpp + test); win-asan FULL suite 1587/74
>   (ASan-clean). ⇒ **CEIR-17e DONE.** ⛔ NEXT band-row = 17z (band proof + 0h deletion; carries the stale
>   `scene_gpu_cull.frame.toml` fix-or-delete).**
>
> **◧ 17z OPEN 2026-08-14 (advisor-scoped at band-entry): band proof + 0h deletion.** Three parts:
> - ⛔⛔ **NO WIRING of `scene.resolve_*` into the live path — locked, not deferred.** The-deletion-is-the-proof applies to
>   REPLACED orchestration; the REN-37 variant ladder was never replaced — 17a-c gave it a DECLARED CEIR identity (the
>   replaceable convenience tier). Wiring it now would (i) re-litigate 16d-live-3b's host-side reclassification (ADR-0106 #3 —
>   the DrawList carries PRE-RESOLVED handles) and (ii) touch the extract hot loop the band-open constraint forbids; 17c's
>   OPTION-(b) rejection already states a future CEIR resolver replaces the ladder AT THE SEAM, WHOLESALE. ⇒ **"0h deletion"
>   is LITERAL** — the band's imperative deletion happened at §128/CEIR-16; band 17's slices (17a-e) were PROOF/re-scope
>   (the resolution/cull/skin were ALREADY authored CEIR). The band's only NEW deletion artifact is the stale
>   `scene_gpu_cull.frame.toml`.
> - **DELETE (not fix) `assets/frame/scene_gpu_cull.frame.toml`** (advisor): it references the unregistered per-desc design
>   (`engine://scene/cull_compact`, superseded by the cooked per-view `cull_view0..4`), has ZERO consumers, is excluded by the
>   template bridge — and FIXING it would create a NEW untested shipped asset (a fixture with no consumer to lock it valid —
>   the fixture-locked-valid scar cuts AGAINST fix). Update the 3 references that NAME it (`test_frame_template_bridge.cpp:407`,
>   `test_vulkan_frame_graph.cpp:7831`, `docs/design/ceir-15-framegraph-unification.md:171`) — comments/exclusion lists, or they
>   dangle. Deletion gate: full **frame-cook + gpu-context-vulkan** suites green after removal (their exclusion lists may
>   enumerate it), not just scene-render.
> - **BAND PROOF = a ROW-PER-CLAIM table** (rigid-V / rigid-DX / skinned-V / skinned-DX / indirect-V / indirect-DX / smoke),
>   each row citing its gate + whether RE-RUN at close or inherited (the GATE-reverifies-rows discipline — re-run, don't
>   inherit green). Two real holes: (1) **DX12 indirect** — 17d's count-differential is Vulkan-only; the DX12 arm is the occ
>   pixel-A/B test (`test_scene_render_gpu.cpp:5455+`, `k40gOccCullToml`, `set_gpu_cull(true)`) — cite + RE-RUN it, don't write
>   a new gate. (2) **Sandbox smoke** — the `--gpu-skin` + verify arm (main.cpp:849 pulls gpu_cull + the GPU graph + skinned
>   content), NOT the default frame; mind the CRD_ASSETS_DIR false-green scar + the multi-pass LOAD-not-clear pixel-blindness.
> - **Close sweep (bounded):** 17e's engine change already re-ran the FULL scene-render suite on 4 configs post-fix — so 17z
>   re-runs only the DELETION's blast radius (frame-cook + gpu-context) + the band-proof rows + smoke; cite the already-green
>   at its commit-state, don't re-run the world. ⛔ At 17z CLOSE: compact `MEMORY.md` (approaching the read limit) as a 60s
>   band-boundary task.
>
> **⛔⛔⛔ 17z REGRESSION FOUND 2026-08-14 (the deletion gate's gpu-context-vulkan re-run surfaced a BAND regression — NOT the
> deletion).** The deletion side is CLEAN: frame-cook green at HEAD (2736/95) AND worktree (2751/96); my only gpu-context
> change is a comment (git diff = 2 lines); no test loads the deleted asset. BUT re-running gpu-context-vulkan (the
> GATE-reverifies discipline — don't inherit) showed **worktree = 16 failed**, so I BASELINED (`git stash --include-untracked`
> → rebuild the same targets → run same env/invocation → `git stash pop`): **HEAD (c44adbf) = 12 failed, worktree = 16 failed.**
> - **12 pre-existing** (present at HEAD, NOT the band's — advisor's "green at 16z close" was a mis-reconstruction): filed as a
>   SEPARATE tracked task, environmental/unknown, out of 17z scope (but recorded, not silent — scene-render device tests DO
>   pass, so it is suite-specific).
> - **4 BAND-INTRODUCED** (worktree-only failing lines 2529 / 2852 / 5665+5671 / 6056+6060+6061), all REN-38 AUTHORED-frame-graph
>   gates that render 0 SILENTLY: **REN-38-A5** (authored PRESENT, :2431), **REN-38-A6** (authored CLEAR/COPY/BLIT, :2784),
>   **REN-38 STATE GATE** (authored FACE CULL through the asset, :5620), **REN-38-F11** (authored STENCIL mask-then-test through
>   D24S8, :5993).
> - **HYPOTHESIS (advisor):** 16d-live-4c made `record_ceir_render` UNCONDITIONAL (deleted the imperative recorders that were
>   the FALLBACK). If `build_frame_plans` produces NO plan for these pass KINDS (present/clear/copy/blit) or does not carry
>   these PARAMS (cull_mode, stencil) — or the tests' frame-install path never runs `build_frame_plans` — the pass records
>   NOTHING, SILENTLY. This is the [[feedback_plan_table_must_rebuild_at_every_frame_install_site]] + [[feedback_raf12_executor_coverage_before_inline_deletion]]
>   scars: the deletion removed the fallback that MASKED an uncovered cell; the 16d/16z gates ran scene-render/frame-cook, not
>   these gpu-context-vulkan combinations.
> - **DISCRIMINATORS (diagnosis, in order):** (1) read REN-38-A5 body (2431-2530) — does its install/execute path call
>   `build_frame_plans`? (2) read `record_ceir_render` missing-plan behavior — a silent early-out is BOTH the mechanism AND a
>   doctrine violation (verifier-couldn't-run≠green: a missing plan must be LOUD). (3) the diag counters (record_pass_calls /
>   record_pass_compute) on one failing test → record-called-but-empty (plan-builder gap) vs never-called (install-site hole).
> - ✅ **ROOT CAUSE CONFIRMED (by code, all 3 discriminators):** `register_builtin_records` (frame_graph.cpp:648) maps
>   `scene.raster` → `record_ceir_render` (the imperative `record_scene_raster` was DELETED by §128, 16d-live-4c). The 4
>   failing tests each have a `raster.geometry` pass → `scene.raster` executor, but call `FrameRecorder::record(desc, fgraph,
>   raster, host, &err, &where)` with **NO `plans`** (the param `const FramePlans* plans` defaults null — frame_runtime.cpp:526).
>   So `ap.plan` is null (1245), and `record_ceir_render` **silently records nothing** (`err==Ok`, renders 0) instead of the
>   loud `ctx.fail()` the 1243-1245 comment PROMISES for a migrated executor with a null plan. At HEAD the deleted imperative
>   `record_scene_raster` drew any raster pass with no plan — its deletion removed the fallback that MASKED both the missing
>   install-site plan-build AND the un-wired loud-fail. (A5's present + A6's clear/copy/blit are imperative executors that work;
>   they render 0 because their CONTENT raster pass drew nothing.)
> - **⛔ SUPERSEDED (advisor reconcile): ~~FrameRecorder::record builds plans internally + per-slot FramePlans arena.~~** The
>   arena would add 4 failure modes (FramePlans has no clear contract; storage pointers dangle on clear; ctx accumulates;
>   multi-recording) to solve a lifetime problem that only exists from building in the WRONG function. The ⚠ fork is CLOSED:
>   `build_frame_plans`' is_scene branch (frame_runtime.cpp:1558) builds a TEMPLATE plan (begin/scene_draw_list/end; fs_target
>   resolves attachments + the Host provides draws at record) — valid for a bare-triangle raster pass. No mapping question.
> - ✅ **FIX = FORWARD ONLY, advisor-locked (FrameRecorder untouched — its contract stays "plans are the CALLER's; a migrated
>   pass with no plan is a loud error"):**
>   1. **LOUD-FAIL (the doctrine the 1243-1245 comment promised, never implemented) — two layers:** (a) at the `ap.plan`
>      assignment in `FrameRecorder::record`, if the resolved plan is null AND the pass's executor is MIGRATED (the same
>      `pass_is_fullscreen||mesh_indirect||tess||mesh||scene_raster` set build_frame_plans counts at 1445-1446), `return
>      fail(...)` with a NEW named `FrameExecError::MissingCeirPlan` carrying the pass name (mirrors `NoPresentSurface`); add its
>      string to the error-name switch (~1316). (b) `record_ceir_render` itself: null plan → `ctx.fail()` (defense-in-depth for
>      non-FrameRecorder entry points). The install-site check is what the tests exercise + fires BEFORE any device work.
>   2. **CLOSE THE HOLE at the SYNCHRONOUS wrapper, not the recorder:** `execute_frame_graph` (record→build→execute in one
>      function) builds a STACK-LOCAL FramePlans (`build_frame_plans` → pass the pointer) — exactly the right lifetime for free,
>      no Impl member, no UAF surface; covers every engine-path caller. ⛔ Put the plan-build AFTER the capability/surface-shape
>      checks so A5's part-1 arm still fails with `NoPresentSurface` (not a plan error).
>   3. **The 4 tests' DIRECT `rec.record(...)` calls** conform to the API they exercise: stack FramePlans + build_frame_plans +
>      pass the pointer (~3 lines each) — demonstrating the real contract, NOT a test-only workaround (the engine hole is closed
>      in execute_frame_graph). Do NOT resurrect the deleted recorders.
> - **SEQUENCE:** loud-fail first → rebuild → run the 4 (confirm they now fail with `MissingCeirPlan`, proving detection) →
>   execute_frame_graph stack-build + test-site plan-builds → rerun expecting 4 GREEN + **EXACTLY 12** total reds → blast radius
>   (frame-cook + render-graph + scene-render + `[ceir17]` + template-bridge) on 2 Win + 2 Linux + tidy. Scene-render path is
>   untouched by design (it passes plans — what keeps 17d/17e green) but VERIFY, don't assume. ⛔ **17z CLOSE BLOCKED** until the
>   4 are green.
> - **✅ FIX LANDED (win-debug) 2026-08-14.** Implemented exactly as above: (a) new `FrameExecError::MissingCeirPlan` +
>   `pass_is_migrated_ceir` helper (frame_asset.hpp) + the install-site loud-fail in `FrameRecorder::record`; (b)
>   `execute_frame_graph` stack-builds a `FramePlans` (post create_frame_graph) and passes it; (c) REN-38-A5's two DIRECT
>   `rec.record` sites build + pass a stack plan. CHECKPOINT confirmed: after loud-fail-only the 4 failed with `MissingCeirPlan`
>   (detection proven), not silent render-0. After the plan-builds: the 4 GREEN (114 assertions). **FINDING (advisor: "any
>   other number is a finding"): the full gpu-context-vulkan suite is now 257/260 pass — 3 red (after B1, below), NOT 12.** My fix closed 12
>   null-plan failures, not 4 — because c44adbf ("working on CEIR-16") had §128 ALREADY partly committed, so ~8 of the "HEAD-12"
>   were already null-plan silent-render-0 (not environmental as first assumed). The monotonic 16→4 (fix adds only plan-builds +
>   a loud-fail; blast radius all-green ⇒ zero new breakage) means the remaining 4 are the truly-environmental subset (task
>   #16). Blast radius GREEN win-debug: frame-cook 2751/96 + render-graph 309/15 + scene-render [ceir17][skinning] 85/4. ⛔ STILL
>   OWED: confirm the 4 remaining ⊂ the original 16 (line check), part-1b defense-in-depth (record_ceir_render null-plan
>   ctx.fail — NOTE: 1b was ALREADY present at frame_graph.cpp:605 [CEIR-16-3d-3], but it ctx.fail()s at EXECUTE time, never
>   mapped to the caller's FrameExecError, which is exactly why the render-0 was SILENT; the install-site 1a surfaces
>   MissingCeirPlan at RECORD time), then win-asan + 2 Linux + tidy. tidy clean (frame_runtime.cpp + test).
> - **✅ 4-LINE CHECK DONE — the loud-fail surfaced a 5th silent bug, now fixed.** The 4 failing cases were REN-38-A12 (WBOIT),
>   the 4008-test, REN-38-A13 (SHADING RATE) — all at HEAD-12 lines, device-feature VALUE checks (via execute_frame_graph which
>   now HAS plans ⇒ NOT MissingCeirPlan) ⇒ truly environmental — PLUS **REN-38-B1** (authored PING-PONG history): a test that
>   was PASSING while silently rendering 0 (it checks err/submit/transient, NOT pixels; a direct `rec.record` on the TAA graph's
>   fullscreen pass). NOT a new regression — a pre-existing silent bug the loud-fail EXPOSED (exactly what the doctrine is for);
>   fixed with the same test-site plan-build. ⇒ **gpu-context-vulkan 16 → 3** (257/260); the 3 are the truly-environmental
>   subset (task #16 NARROWED 12→3: ~9 of the HEAD-12 were null-plan from the partly-committed §128, now fixed). Monotonic
>   16→3, blast radius all-green ⇒ zero net new regression. ✅ re-tidy done (test). CROSS-CONFIG: **win-asan** blast radius GREEN
>   (frame-cook 2751/96 + render-graph 309/15 + scene-render [ceir17][skinning] 85/4); **linux-gcc-debug** GREEN + gpu-context
>   257/2-fail/1-skip (llvmpipe SKIPs one feature; 2 environmental fail — a SUBSET of the win-debug 3, NO new regression).
>   PENDING (background, ASan slow): linux-gcc-asan (bdccybu8k) + win-asan gpu-context (bqj45ofjh) — expect ≤3 environmental,
>   no MissingCeirPlan. Then task #15 DONE → resume 17z close.
> - **✅ linux-gcc-asan COMPLETE (2026-08-15): matches linux-debug** — fc 2751/96 + rg 309/15 + sr [ceir17][skinning] 67/3 +
>   gpu-context 257/2-fail/1-skip (same environmental subset, NO MissingCeirPlan, ASan-clean). ⇒ the §128 null-plan fix is
>   verified on win-debug + win-asan (blast radius) + BOTH Linux configs; only win-asan gpu-context (bqj45ofjh) still finishing
>   (expect 3 environmental, no new). **Task #15 effectively DONE** (pending that last confirm).
>
> **✅ 17z BAND-PROOF MATRIX (row-per-claim, each RE-RUN at close — GATE-reverifies discipline, not inherited):**
>
> | claim | Vulkan | DX12 | re-run at close |
> |---|---|---|---|
> | **rigid** render as CEIR | 17c parity oracle + scene-render suite green | scene-render `[dx12]` 285/17 | ✅ re-run |
> | **skinned** as CEIR | 17e shipped-config bit-identical (30/1) + REN-40-F Vulkan | REN-40-F DX12 (in `[dx12]` 285/17) | ✅ re-run |
> | **indirect** (GPU cull) as CEIR | 17d verdict-compare gate + occ A/B (Vulkan) | occ pixel-A/B `k40gOccCullToml` (in `[dx12]` 285/17) | ✅ re-run |
> | **end-to-end smoke** | sandbox `--gpu-skin --gpu-cull-verify`: 185 frames / 3.01s, 61.5 fps, **5031 instances drawn** (real geometry, NOT pixel-blind), full 22-pass authored GPU graph (palette_snapshot→gpu_skin→cull_view0..4→occlusion→forward→TAA), exit 0 | (same shipped binary, DX12 selectable) | ✅ re-run |
>
> ⇒ rigid/skinned/indirect all render through CEIR on BOTH backends + the live 1M sandbox path presents real geometry with
> gpu-skin + gpu-cull + CPU-vs-GPU verify engaged. Band proof COMPLETE. ⛔ OWED before flip: win-asan gpu-context confirm +
> MEMORY.md compaction + the §128 memory entry + commit proposal; task #16 (the 3 environmental) for "everything green".
