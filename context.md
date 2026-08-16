# Cerid — Live Context

> Short-term memory: "where are we now?" The master plan lives in `docs/ROADMAP.md`; the doc map in `docs/README.md`.
> This is a **DASHBOARD, not a changelog.** Each milestone's detail lives in its session log (`docs/sessions/YYYY-MM-DD-*.md`); this file summarises the *current* state and points there. Keep it lean (≤ 300 lines) — prune stale snapshots, don't stack them. (History pruned 2026-08-07 → `docs/sessions/2026-08-07-context-md-history-archive.md`.)

---

## ⛔⛔⛔ STANDING ORDER — **[2026-08-16, user #1 mandate, EMPHATIC] NO C++ KGRAPH BUILDERS FOR RENDERING ALGORITHMS. EVER.** A `build_X(KGraph&)` in a `.hpp` is rendering C++; the `[.emitckir]` "build→emit→keep as regen source" pattern is RETIRED as an anti-pattern. The `.ckir`/`.crdv` (CKIR) / CHIR asset is the SOLE source, authored/edited DIRECTLY (hand-written node-graph text today — see `assets/ckir/deferred_lighting.ckir`; the CEIR-32/33 node editor later). Gates LOAD the committed `.ckir` (`ckir_read`), never call a builder. ⛔ `scene_renderer.cpp` gets ZERO new rendering-technique C++ — every algorithm + every rendering DECISION comes from an asset. Memory: [[feedback_no_cpp_kgraph_builders_author_ckir_directly]]. ⚙ IN PROGRESS (task #34): ✅ ELIMINATED the 2 builders I wrongly introduced this session — `ckir_rt_worldpos.hpp` DELETED + `build_rt_composite` DELETED; the [ceir19b] gates now LOAD rt_worldpos.ckir / rt_composite.ckir (14 assertions green, both-backend emit proven on the loaded asset). REMAINING: eliminate the PRE-EXISTING builders — `build_cluster_light_cull` (ckir_light_cull.hpp; keep only the LightCullParams word-index constants the device-parity test needs, or inline them) + the two `build_visbuffer_fs` (test_ckir_asset inline + `tests/gpu-shared/ckir_raster_triangle.hpp`, called by 4 device gates → re-express to load visbuffer_fs.ckir) + retire the `[.emitckir]` regen tests. ⛔ these `.ckir` files ARE the source now — recommend committing them (they're the only copy till then).

## Current focus — **[2026-08-16 🔄 CEIR-18a-3 (user-directed detour, 19b PAUSED)] — the frame-graph pass `technique` field must DRIVE the forward FS cook (the F2 defect: pass.technique was DECORATIVE; the forward FS was cooked from the set_forward/shadow_technique C++ setters, NOT the asset).** User audit flagged unticked CEIR-18 rows: fixed 18a-2 (stale ⬜→✅) + 18z (`⬜|✅` double marker) markers; 18a-3 is a REAL open defect → user chose "fix now, pause 19b". DESIGN-LOCKED (advisor Option A): init_programs sources the forward technique NAME from the installed frame's `material_pass="Forward"` pass → drives BOTH the flat + shadowed cook (setters demote to a field-less fallback, loud-fail if the graph names an unresolvable technique); set_frame_graph_toml RE-COOKS (init_programs is RAF-11 re-runnable). ⚙ CORE IMPLEMENTED + COMPILES. REMAINING: reconcile the 2 setter-based tests that PROVE setter-drives-technique (the REN-37.2 sample helper + the loud-fail sub-test → asset-driven) + the 18a-3 gate (two graphs differing only in technique + the inverse setter pin) + full scene-render suite + tidy (commit ⑥). ⭐ BUILDER MISSION-PURPOSE (user-flagged): the shipping renderer + device tests run ONLY the `.ckir` (resolve_program_text/ckir_read) — ZERO hand-built KGraph; build_cluster_light_cull/build_rt_worldpos are REGEN-ONLY tooling (the [.emitckir] source), tracked for POST-COMMIT deletion (the uncommitted-delete scar forbids deleting before commit). Detail → tracker (CEIR-18a-3 🔄). **PRIOR (paused) — 19b:** the hybrid RT-shadow renderer — all shaders authored + the scene_renderer wiring done + compiles; REMAINING there = the device gate. Detail → tracker (CEIR-19b 🔄).

## Prior focus — **[2026-08-16 ✅ CEIR-19a DONE + gated 4 configs] — the `ceir.rt` DIALECT is DECLARED: RT orchestration (blas/instance/tlas/sbt build → trace / ray_query) has a typed CEIR identity.** 6 ops from `rt.ceirop.toml`→opgen + hand-written `rt.hpp/cpp` (the scene mold): 3 DISTINCT opaque Extern type-classes (rt.blas/tlas/sbt — load-bearing so `trace` takes %tlas+%sbt, `ray_query` %tlas ONLY) + `find_rt_misuse` type-chain walk (I6-clean). ⛔ ALL 6 ops = native{provider=host, ExternalNondeterminism} + effects[GPUCommand, MemoryReadWrite]; ray_query carries kernel_ref. **NATIVE-vs-device-execute resolved (advisor): all-6-native** — `trace` has no kernel_ref (shaders ride the SBT) so it can ONLY be a host intrinsic; `native`+`kernel_ref` is orthogonal (kernel_ref = cook-time dep + §107 pin, not an execution-tier marker); grep confirmed nothing assumes `kernel_ref⇒not-native`. **VERIFIER GAP-FILL (advisor-caught BLOCKER):** the TOML docs promised enum names + access-arity/operand-type checks the walk didn't implement (thinner than its own `find_dispatch_misuse` mold) → corrected every doc enum name + re-attributed structural claims to the generated verify_*, and IMPLEMENTED the dispatch-shape checks for trace/ray_query (DimNotIndex/AccessTokenInvalid/AccessArityMismatch/BindingNotResource — the mold mirror); AS-build operand-resource-kind DEFERRED to 19b. GATE (80 assertions/5 cases, ALL GREEN): win-debug (full crd-ceir-tests 3429/441 + [rt] + opgen-drift/validator) + win-asan + linux-gcc-debug (`-Werror=switch` clean) + linux-gcc-asan (parse_access ASan-clean) + tidy. crd-ceir NEVER links gpu-context (I3/I4) — DECLARE-only, ZERO renderer changes. ⛔ **NEXT = CEIR-19b BUILD (DESIGN-LOCKED 2026-08-16, advisor at entry + a reconcile round).** The 4 verify-first checks found: RT pass kinds exist (`raytrace.pipeline` F6-proven both backends + `raytrace.dispatch` inline never-run); SVGF is a kernel not a pass (denoise out of the assert path); **the RT frame pass is ALREADY authored-CEIR** (frame.pass op + `ensure_rt_kernel`=cook_stage_named loader over disk `.crdv` + a registered executor — "RT is imperative" was FALSE) ⇒ **Position B: 19b assembles via the existing `raytrace.pipeline` frame.pass; the ceir.rt→gpu BRIDGE is RE-HOMED to 19c** (its real consumer = the §134 wavefront PT). Depth-handoff SOLVED with ZERO executor engine-work: the `compute.dispatch` arm binds a sampled texture (RT arms don't) ⇒ a TWO-pass split (compute reconstructs world-pos from depth → buffer; RT reads buffer+TLAS → shadow mask). AUTHORED HYBRID: raster lit → compute shadow_prepass → raytrace.pipeline rt_shadow → fullscreen composite. BUILD IN PROGRESS: ✅ the hybrid `assets/frame/rt_shadow.frame.toml` + ✅ SHADER UNIT 1 (cook_rt GENERALIZED for shadow rays — `[rt]` to_light/origin_binding/light_xyz/miss_value/hit_value, vertex-cook GREEN 445/25, F6 primary byte-identical + the 3 `scene_rt_shadow_{raygen,miss,chit}.crdv`). ✅ SHADER UNIT 2a (feasibility + raygen guard + TOML): compute texture-sampling CONFIRMED feasible (both emitters have the SampleLod/Texture arms; the worldpos kernel is the FIRST compute-stage caller); the advisor caught + I FIXED 2 defects (1D-launch pin + static params vs the one-ray scar; a sky-pixel NaN-origin validity guard in the raygen — 445 tests still green, F6 byte-identical); rt_shadow.frame.toml gets rt_constants (inverse-VP + ndc_y_sign) + depth_as_float + 1D params. ✅ SHADER UNIT 2b (the worldpos kernel): build_rt_worldpos in ckir_rt_worldpos.hpp (samples the raster depth b8/sampler b9, reconstructs world-pos via inverse-VP, writes worldpos_buf + a .w validity flag) — the FIRST compute-stage texture sampler; ALL wiring verified before authoring (binding layout kMaxKernelBuffers=8, executor routes via dispatch_kernel_sampled/the HZB precedent, all emitter arms both backends); GATE [ceir19b] 13 assertions — builds + byte-exact roundtrip + EMITS GLSL **AND** HLSL + committed rt_worldpos.ckir loads. Tidy clean. ⛔⛔ MATRIX-LAYOUT BUG CAUGHT+FIXED (before any device run): Mat4f is COLUMN-major, so the kernel's inverse-VP read is column-major (row-major would be invᵀ·ndc = garbage) — verified against compute_froxel_aabbs, re-emitted, [ceir19b] re-green. WIRING PLAN GROUNDED in the TAA-constants/froxel install site (~5682): rt_constants = memcpy inverse(view_proj) column-major + W/H + ndc_y_sign(ndc_y_points_down()?1:-1) + upload_storage; worldpos_buf/shadow_mask_buf = SIZED external buffers via the resolver+debug arms; shadow .crdv via cook_stage_named, rt_worldpos/composite via resolve_program_text; gate PINNED 64×64 (static frame.toml params). ✅ rt_composite passthrough (build_rt_composite: fullscreen stage_in-UV → tex_sample scene_hdr[1-read binding1] → @output; STEP-1 passthrough unblocks the frame, the shadow-multiply STEP-2 is a follow-up; [ceir19b] 22 assertions, rt_composite.ckir committed). ⭐ ALL 19b CKIR/shader authoring COMPLETE (worldpos + composite + 3 shadow .crdv). ✅ SCENE_RENDERER WIRING DONE (crd-scene-render COMPILES): the 4 ensure_* loaders + 5 program registrations + sized buf_worldpos/buf_shadow_mask + rt_constants (per-frame column-major inverse-VP upload, the TAA mold) + resolver/debug arms. REMAINING: (5) the ABSOLUTE device gate in test_scene_render_gpu.cpp (Vk+DX12, PIN 64×64, caps-SKIP: DETERMINISTIC receiver+occluder+light(0,8,0), TLAS from same tris; STAGE 1 worldpos-vs-CPU-analytic FIRST → STAGE 2 shadow-mask occluded≈0/unoccluded≈1 → [STAGE 3 real-composite follow-up]; val_err==0) + FIX every never-run-hybrid defect; then 4-config gate + tidy. Detail → tracker (CEIR-19b 🔄). ⚠ 19a files (crd-ceir only — a clean SEPARATE commit ⑤ from the CEIR-18 batch) UNCOMMITTED.

**PRIOR — [2026-08-16 ✅✅✅ CEIR-18 BAND CLOSED] — the RENDERER PROOF SUITE is complete: every renderer (Forward+/Clustered/Deferred/Visibility/GPU-driven/impostor) is an AUTHORED CEIR asset; the shipped path has ZERO hand-built KGraph.** CEIR-18z (band close, advisor at entry+close) FOUND + converted the LAST hand-built KGraph via a full ensure_* audit (26 classified, 25 loaders/cooker-seams + 1 builder = `ensure_visbuffer_fs`, the visbuffer id→grey FS) → authored `visbuffer_fs.ckir` (emit in a [.emitckir] regen, loader in ensure_visbuffer_fs); added the MISSING frame-cook SECTIONs (forward_plus/plus_gpu/clustered_3d_gpu had NEVER been through the A/B + plan-build gate — the latent-drop class 17z catches; now green); ran the BAND-PROOF MATRIX (the 17z full-suite discipline) GREEN — 4 configs + llvmpipe + sandbox smoke: win-debug gpu-context 6152/263+2392/144, win-asan ASan-CLEAN (scene-render full 1903/89 closing the stale-since-17e gap; gpu-context 6152/263+2392/144 closing the no-clustered3d-ASan disclosure), 2×Linux (kir [ceir18z] 7/1 + frame-cook 1803/9 + scene-render 330/13 + gpu-context 874/3; linux-gcc-asan closing the gcc-templatized-header gap), tidy clean, sandbox "PASS 5251 instances" (not overlay-only). ⛔ SHIPPED-DEFAULT CARRIED EXPLICITLY: `forward_csm_gpu` stays default; `forward_plus_gpu`/`forward_clustered_3d_gpu` are proven alternatives (flipping the default = a future PERF-gated slice, not this authored-asset band). Detail → tracker (CEIR-18 + CEIR-18z rows). **PRIOR (this batch) — CEIR-18b — the CLUSTERED 3D-FROXEL renderer works END-TO-END as an authored asset.** The z-slice ships as DATA: the renderer publishes an exponential z-boundary TABLE (header words 115-119) from `compute_froxel_slices` (unproject near/far, GLOBAL clip.w=1/h.w, exp boundaries, LINEAR interp in clip.w per slice); the 3D FS bins each fragment by `clip.w` (=view_proj row 3·world_pos) against that table via a branchless Step-sum → the froxel-AABB cuts + the FS binning agree BY CONSTRUCTION (the boundary-TABLE-not-dual-formula anti-drift discipline — 2 memories written). The device producer is a 2nd parameterization of the ONE cull builder (`scene_light_cull_3d.ckir`, 64 clusters=4×4×4=the wg) through the same ensure/register/frame shape; a `device_light_cull_3d` flag drives dispatch + the per-slice fill; a 3rd technique `forward_authored_clustered_3d` + crdl (grid[4,4,4], slice_bounds_off=115) + frame sibling. The 2D tiled path stays BYTE-IDENTICAL (cook-time grid[2]==1 branch). ⭐⭐ GATED 4 configs + a 3rd device: win-debug ([ceir18b] 70 + full scene-render 1872/87), win-asan (140/7 ASan-clean), linux-gcc-debug/llvmpipe (91/4), linux-gcc-asan (91/4 ASan-clean); gpu-context [clustered3d] VK 589 + DX12 588 + **llvmpipe 874/3** (the advisor-caught blocker: first gcc compile of the templatized kit); the NON-CIRCULAR CPU drift test (matrix-row-3 clip.w → slice → AABB-contains) + the two-depth device IMAGE gate (near/far quad, probe 2 px same tile) PASSED FIRST device run both backends; tidy clean (shared header gate-invisible). Detail → tracker (CEIR-18b row). **PRIOR (also ✅ this batch) — CEIR-18a-2 — the DEVICE Forward+ light-cull works END-TO-END (Stages 1→2b-iv).** The Forward+ clustered renderer is FUNCTIONALLY COMPLETE: a `light_cull` COMPUTE pass (`engine://scene/light_cull`, the re-authored `build_cluster_light_cull` → `scene_light_cull.ckir`) runs on the device each frame, reads the froxel-AABB table + light-input the renderer provisions, and WRITES the per-tile cluster list into the group buffer; the clustered forward FS consumes it. Sub-stages: **2b-i** FS slots are POINT-array indices (`light_base+(first+slot)*stride`, guard `slot<count`, NaN-safe select-zero); **2b-ii** the light-cull re-authored (palette_snapshot header-indirection form, component-wise scalar, `LocalInvocationIndex`, `stmt_materialize` for the compaction cursor — the if-block shared-temp scar's 3rd recurrence) — bit-exact device==oracle==analytic both backends + llvmpipe; **2b-iii** provisioning (compute_froxel_aabbs via `inverse(view_proj)`, finite-far, per-backend `flip_y`) + the `forward_plus_gpu.frame.toml` sibling + the ensure_light_cull_kernel + the device-owns precedence; **2b-iv** the device IMAGE gate. ⛔⛔ TWO first-device defects fixed: (1) the pass didn't dispatch — `dispatch_groups` derives from `d.cull_groups` (0 w/o a GPU cull) → added a `device_light_cull` flag setting `dispatch_groups=1`; (2) the NDC±Y mirror — the CPU froxel (raw view_proj) vs the FS's FragCoord tiling needs the per-backend `!ndc_y_points_down()` sign (memories written). **GREEN: [ceir18a] 103/6 Win Vk+DX12 + llvmpipe 53/3; full scene-render 1833/86; light-cook 151/12; kir [asset] 369/15; all changed files tidy-clean.** Detail → tracker (CEIR-18a row). ⛔ FOLLOW-UPS (filed): the DX12-geometry-Y question (does DX12 forward render world-up at screen-bottom?); the multi-instance-group cull WW race; the froxel-grid config-flexibility. ⛔ **NEXT = CEIR-19 (advisor at band-open; do NOT pre-design in this close).** ⚠ ALL CEIR-18 band work (18a-2 + 18b + 18z + impostor/18p) UNCOMMITTED — proposals ①②③④ above (user commits, NO AI trailer; fresh session `git status` first; `ckir_light_cull.hpp` / `build_cluster_light_cull` / `build_visbuffer_fs` + their `[.emitckir]` regen tests stay in-tree till committed, then delete + retire the regens).

**⚠ PROPOSED COMMITS (user commits, NO AI trailer; 18p + 18a-2 SHARE `scene_renderer.cpp/.hpp` + `test_scene_render_gpu.cpp` — either `git add -p` to split the hunks (impostor edits are in `ensure_impostor_program`; 18a-2 edits in `set_point_lights`/`set_cluster_light_list`/`body_scene_authored`/the layout+upload+header sites) or fold into ONE `feat(scene-render): CEIR-18p impostor + 18a-2 Forward+`):**
- **① `feat(scene-render): CEIR-18p author+delete the impostor program (4 .ckir assets)`** — `ensure_impostor_program` → thin `resolve_program_text` loader; `impostor_{vs,fs}_{plain,dither}.ckir`; builders+emit DELETED; the cull-kernel dither shared-temp fix (`vertex_asset.cpp` `stmt_materialize`) + LOUD `create_program` log (`vulkan_context.cpp`). Files: scene_renderer.cpp/.hpp, vertex_asset.cpp, vulkan_context.cpp, impostor_atlas.hpp, assets/ckir/impostor_*.ckir, test_scene_render_gpu.cpp, test_ckir_asset.cpp.
- **② `feat(scene-render): CEIR-18a-2 Forward+ device light-cull (Stages 1→2b)`** — Stage 1 `set_point_lights` consumed by the live forward pass; Stage 2a `set_cluster_light_list` + `forward_authored_clustered` (scene_forward_clustered.crdl) + viewport-dims header words (E5 frag_xy fix); Stage 2b the DEVICE light-cull: the re-authored `build_cluster_light_cull` (kir `ckir_light_cull.hpp`, KEPT IN-TREE as the regen source) → `scene_light_cull.ckir` (regenerated, 4-light header-indirection form); `ensure_light_cull_kernel` + `engine://scene/light_cull` registration; `compute_froxel_aabbs` + per-frame froxel/light-input provisioning; the `forward_plus_gpu.frame.toml` sibling + the `device_light_cull` dispatch/precedence; FS point-index contract + guard. Files: scene_renderer.cpp/.hpp, lighting_asset.cpp/.hpp, engine/kir/include/crd/kir/ckir_light_cull.hpp, assets/ckir/scene_light_cull.ckir, assets/lighting/scene_forward_clustered.crdl, assets/frame/{forward_plus,forward_plus_gpu,forward_csm}.frame.toml, tests: test_scene_render_gpu.cpp, test_lighting_asset.cpp, test_ckir_asset.cpp, gpu-shared/ckir_light_cull_test.hpp, gpu-context-vulkan/test_vulkan_context.cpp.
- **③ `feat(scene-render): CEIR-18b clustered 3D-froxel renderer as an authored asset`** — the z-slice ships as DATA: an exponential z-boundary TABLE (header words 115-119) + a 2nd parameterization of the ONE cull builder (`scene_light_cull_3d.ckir`, 64 clusters) + the 3D crdl/frame/technique. `compute_froxel_slices` (unproject near/far, GLOBAL w=1/h.w, exp boundaries, LINEAR clip.w-interp per slice); FS z-block = boundary Step-sum on clip.w with a cook-time `grid[2]==1` else-branch keeping the 2D cook BYTE-IDENTICAL; `device_light_cull_3d` flag (scan distinguishes light_cull vs _3d by exact equality) + `ensure_light_cull_3d_kernel`/register + dispatch/precedence/upload-guards + the header boundary publish; grew section consts 128→512/96→384 (2D=prefix); the templatized gpu-shared kit + `ClusterCullScene3D`; the DRIFT-catching CPU test + the two-depth device IMAGE gate (both backends). ⛔ SHARES most files with ② (scene_renderer.cpp/.hpp, lighting_asset.cpp/.hpp, the test files) — either `git add -p` by slice or fold ②+③. NEW files: assets/lighting/scene_forward_clustered_3d.crdl, assets/frame/forward_clustered_3d_gpu.frame.toml, assets/ckir/scene_light_cull_3d.ckir. Also touched: tests/kir/test_ckir_asset.cpp, tests/gpu-context-dx12/test_dx12_compute.cpp. ⛔ `ckir_light_cull.hpp` + `build_cluster_light_cull` STAY (the [.emitckir] regen source for BOTH .ckir assets — delete only AFTER this batch commits).
- **④ `refactor(scene-render): CEIR-18z band close — author the last hand-built FS (visbuffer_fs) + frame-cook coverage`** — the band-close ensure_* AUDIT (26 classified) found the LAST hand-built KGraph, `ensure_visbuffer_fs` (the visbuffer id→graded-grey FS), and CONVERTED it: `assets/ckir/visbuffer_fs.ckir` (emit in a `[.emitckir]` regen `build_visbuffer_fs`, IN-TREE till commit), `ensure_visbuffer_fs`→thin resolve_program_text loader; added forward_plus/forward_plus_gpu/forward_clustered_3d_gpu SECTIONs to the frame-cook A/B fidelity + plan-build gates (they'd never been through frame-cook). Files: engine/scene-render/src/scene_renderer.cpp (the visbuffer loader — SHARES the file with ②③), assets/ckir/visbuffer_fs.ckir (NEW), tests/kir/test_ckir_asset.cpp (regen + `[ceir18z]` load gate), tests/frame-cook/test_frame_template_bridge.cpp (the 3 SECTIONs). ⛔ POST-COMMIT DELETION (do AFTER this batch commits): delete `build_cluster_light_cull` + `engine/kir/include/crd/kir/ckir_light_cull.hpp` + `build_visbuffer_fs` + **(CEIR-19b, user-flagged 2026-08-16 — mission purpose: the .ckir is the SOLE source, no C++ builder) `build_rt_worldpos` + `engine/kir/include/crd/kir/ckir_rt_worldpos.hpp` + `build_rt_composite`**, AND retire the 3 `[.emitckir]` regen tests that call them (won't compile otherwise) — then the committed `.ckir` assets + the load/distinctness/roundtrip gates are the source of truth, git history the escape hatch.
- **⑤ `feat(ceir): CEIR-19a declare the ceir.rt dialect (RT orchestration)`** — a CLEAN SEPARATE batch (crd-ceir only, no overlap with ①-④; the CEIR-19 band opens). The 6-op `rt` dialect: `engine/ceir/ops/rt.ceirop.toml` → opgen → `engine/ceir/generated/crd/ceir/gen/rt_ops.{hpp,cpp}` + `rt.ops.{json,md}`; hand-written `engine/ceir/include/crd/ceir/rt.hpp` + `engine/ceir/src/rt.cpp` (the scene.hpp/cpp mold — register_dialect + the blas/tlas/sbt opaque Extern type-classes + `find_rt_misuse` with the dispatch-shape checks); `tests/ceir/test_rt.cpp` + the `tests/ceir/CMakeLists.txt` entry; `tests/ceir/generated/test_rt_gen_smoke.cpp` (auto-globbed). ALL 6 ops native{provider=host}+GPUCommand; ray_query kernel_ref. DECLARE-only, ZERO renderer changes (crd-ceir never links gpu-context). Gate: 2 Win + 2 Linux + tidy + opgen-drift, all green (80 assertions).

**Parallel — CEIR-18p ✅ COMPLETE 2026-08-15 (cross-cutting: author+DELETE every hand-built `ensure_*` program/kernel in scene_renderer):** ALL builders converted to disk-read `.ckir` assets loaded via `resolve_program_text` (app-first `app://ckir/<name>` ?? `engine://ckir/<name>`) and DELETED. ⭐⭐ **THE IMPOSTOR (the LAST builder) is done end-to-end** — its FS config (grid/tile/mips) demoted to 18 D12 spec-consts with a 16-way PINNED select chain, 4 assets `impostor_{vs,fs}_{plain,dither}.ckir` emitted, `ensure_impostor_program` is a thin `resolve_program_text` loader, and `build_impostor_vs/fs` + the emit are DELETED (−21 KB). Renders PIXEL-EXACT 133/169/36 FROM DISK on all 3 platforms (Win Vulkan+DX12 full 1728/80; Linux llvmpipe 18/1); kir round-trip 361/14 pins the 18 spec-consts / VS-zero (the structure the pixel gate can't see). ⭐⭐ STEP 0.5 also SURFACED + FIXED a **pre-existing shipped-path defect never device-tested**: `forward_csm_gpu`+`scene_default.crdlod` (7 slots, dither) → the cull-compact kernel's DITHER DUAL-WRITE shared `in_band_u`/`alpha_fine` across two sibling `stmt_if` blocks → `create_program`-null → 0 compute passes SILENTLY (the if-block shared-temp scar [[feedback_ckir_if_block_shared_temp_scope_materialize]]). Fix = `g.stmt_materialize(primary_val/sec_vis/sec_val)` in `vertex_asset.cpp` (backend-agnostic → GLSL+HLSL) + a LOUD `create_program` shaderc-error log (`vulkan_context.cpp` `g_log_vkctx`) + defect (b) hardened (the gate now pins `gc.fill_record_ok==1`, the record-success signal that a cook-fail/step-down returning normal-looking draws would otherwise hide). Detail → tracker (impostor entry). ⚠ UNCOMMITTED (4 assets + vertex_asset.cpp + vulkan_context.cpp + scene_renderer.cpp/.hpp + impostor_atlas.hpp + test_scene_render_gpu.cpp + test_ckir_asset.cpp). NEXT = resume the CEIR-18 band order (18a-2 Forward+ consumption, task 26).

**Prior — CEIR-18d (✅ DONE-BY-VERIFICATION 2026-08-15):** the visibility renderer already ships as `scene_visbuffer.frame.toml` (16z: `scene.raster` geometry=procedural + R32Uint clear, `clear_id=7`) + renders through the full SceneRenderer with a device id read-back in `REN-38-F6 GATE` (Vk + DX12). 18d = re-verify green + close an advisor-caught FALSIFIABILITY hole (`a!=b` had an A==A escape — a primitive that fails to draw reads the `clear_id=7` background yet could pass → added `CHECK(a!=7); CHECK(b!=7)` to both twins). Rides commit ③. Detail → tracker (CEIR-18d ✅).

**Prior — CEIR-18c (✅ DONE + FULLY GATED 2026-08-15):** `engine://frame/deferred` is an AUTHORED asset (G-buffer `raster.mrt`/material_pass=GBuffer → `render::deferred_shade` fullscreen) rendering a REAL scene through a REAL G-buffer on BOTH backends — a PROMOTION (pack_gbuffer + deferred_shade + MRT frame graph all shipped). ⭐⭐ The DO-FIRST strategy (build over REAL indexed scene geometry to lock the 18z proof-harness) drove a compile-clean-but-NEVER-RAN path; disciplined isolation (Vulkan validation oracle + `from_ceir_frame` numbers, never pixel-guessing) surfaced + FIXED **6 DEFECTS**: 4 in the never-run indexed-MRT-scene path [program_gbuffer twin never wired live; emit_scene_list_mrt drew indexed as non-indexed (vertex_count=0); NO indexed-MRT draw verb → added `draw_storage_indexed_mrt` (Vk+DX12+command_lowering, pushes first_draw_index + explicit depth); attachment/depth wiring] + 2 PRE-EXISTING holes the THOROUGH gate caught [§128 DX12 MissingCeirPlan latent since CEIR-17's Vulkan-only gate; 4 CEIR-16d em-dash TEST_CASE names]. GATE (ALL GREEN): 2 Win + win-tidy + 2 Linux (ASan-clean); deferred Vk+DX12 twins 5/5 stable, val_err=0, ABSOLUTE lit verdict (135,105,89) NVIDIA vs llvmpipe ±1. Detail → `docs/detours/D-007-ceir-tracker.md` (CEIR-18c ✅).

**Prior — CEIR-17 (✅ 4-config gated):** ⭐⭐⭐ CEIR-17 SCENE-BRIDGE BAND COMPLETE (17a-e + 17z).** Scene/query/resource resolver semantics DECLARED (a new `scene.resolve_*` intrinsic dialect + 5 Extern type-classes) and rigid/skinned/indirect/GPU-cull PROVEN as CEIR — a PROMOTION, not a rewrite (the resolution/cull/skin were already authored CEIR; the band gave them a declared identity + proved them; ADR-0106 #3 held: ECS extract stays HOST, CEIR sees handles/ids only; RenderResolvers EXTENDED not reinvented). ⭐⭐ The 17z band-close (deletion-gate RE-RUN + a HEAD baseline — GATE-reverifies discipline) SURFACED + FIXED **3 latent CEIR-replay drops the per-slice gates all missed**: (a) **§128 null-plan** — a migrated executor with no plan rendered nothing silently → LOUD `FrameExecError::MissingCeirPlan` at the install site + `execute_frame_graph` stack-builds the plan (also caught REN-38-B1 silently rendering 0); (b) **WBOIT composite-blend** — `build_fullscreen_ceir` set the composite attachment's `load` but DROPPED its `blend` (Opaque composite) → emit the blend attr when `!= Opaque`; (c) **VRS shading_rate/conservative** — dropped post-migration → payload-forward like `force_load` (RenderResolvers + `record_ceir_render` + `materialize_draw_packet`). Deleted stale `scene_gpu_cull.frame.toml` (+3 refs). GATE: **gpu-context-vulkan 260/0 win-debug + win-asan (ASan-clean); 259/1skip linux-gcc-debug + linux-gcc-asan** (VRS caps-skips on llvmpipe = correct SKIP-guard); blast radius (frame-cook 2751/96 + render-graph 309/15 + scene-render 1587/74) + tidy green all 4 configs; band-proof matrix (rigid/skinned/indirect × V/DX + sandbox `--gpu-skin --gpu-cull-verify` smoke 5031 instances). Detail → `docs/design/ceir-17-scene-bridge.md` STATUS + `docs/detours/D-007-ceir-tracker.md` (CEIR-17 ✅). ⛔ **NEXT = CEIR-18** (renderer proof suite: Forward+/Clustered/Deferred/Visibility/GPU-driven as CEIR assets, no new pass algorithm; reuse technique/material stack + B8 lighting; base exists: cull=17d, visibility=16z, B8-deferred green; advisor at band-open, expand sub-slices in the tracker). ⚠ CEIR-16 (c44adbf) + most of CEIR-17 (**8c019fc**: the §128 null-plan fix + REN-38 test plan-builds + `scene_gpu_cull.frame.toml` deletion + 17a-e) are **COMMITTED**; only **7 files remain uncommitted** — the VRS+WBOIT code (`render_materialize.hpp`/`.cpp`, `frame_graph.cpp`, `render_fullscreen_build.cpp`) + these 3 band-close docs (user commits; a fresh session `git status` first).

**Prior (this band):** the CEIR-16 sub-slice journey (16a design → 16b render family → 16c item-view → 16d scene.raster live path incl. the flag dance + mrt≥2 + the-deletion-is-the-proof → 16z visbuffer dissolution) is recorded slice-by-slice in `docs/design/ceir-16-executor-migration.md` STATUS blocks. Every render pass now flows through authored frame graphs → cooked `.crdr` → CEIR. Prior CEIR bands (1–15, the CEIR substrate) + the archived band history are below.

> **CEIR-2 ✅ BAND CLOSED (op-definition generator, §8).** Ops DEFINED in `engine/ceir/ops/*.ceirop.toml` →
> `tools/ceir_opgen/ceir_opgen.py` (stdlib) emits committed C++ (self-registers via the CEIR-1d registry) +
> `.ops.{json,md}` + a gated smoke test — **an op is a TOML edit + regen, ZERO central-enum/switch edits** (proven
> end-to-end by adding the full-surface `test` dialect at 2z). Detail → `docs/sessions/2026-08-08-ceir-2-opgen.md`.
>
> **CEIR-3a ✅ (interned structural types, §16) 2026-08-08.** `TypeId` is now a real interned STRUCTURAL type (scalars +
> the §16 aggregate family): `type.hpp` (`TypeKind`/`FloatKind`/`Type`; **`kind` a FIELD** for I6), `Context::type_*` +
> `type_of` (**asserts, no silent fallback**), a canonical `!`-sigil grammar (`!vec<4x!f32>`, `!struct<P,x:!i32,y:!f32>`,
> `!option<!result<…>>`, …) printer+parser byte-exact, and binary **v2** with a **child-first `TYPE` chunk** (content-pure;
> per-result/arg refs so no future v3 bump). Every `TypeId{n}` magic literal migrated to real factories (a stray one
> aliased to incidental intern state — caught only by the dirty-context purity test). 65/65 × 4 configs + tidy +
> invariants. Detail → `docs/sessions/2026-08-08-ceir-3-types.md`.
> **CEIR-3b ✅ (generics, §16/§98) 2026-08-08.** Appended `TypeParam`/`Trait`/`Callable` kinds (reuse the v2 record —
> **no version bump**); `!param<T,!trait<Ord>>` / `!trait<..>` / `!fn<(P)->(R)>` grammar; a live trait-conformance
> registry (`register_conformance`/`satisfies`, transitive over supertraits) + a memoized, diagnostic-bearing
> `substitute` (unbound params stay generic; a constraint violation reports the (param,trait)). Added `type_is_well_formed`
> — the decoder now rejects structurally-invalid records (a latent 3a gap). 70/70 × 4 configs + tidy + GCC
> `-Werror=switch`.
> **CEIR-3c ✅ (resource + view types, §23) 2026-08-08.** Nine appended kinds (Buffer/Image/Sampler/ResourceTable/
> AccelStruct/VideoFrame/AudioBuffer/ExternalResource/View — **no version bump**, reuse the v2 record). **Interp B**:
> a view TYPE carries the underlying resource + a range-dimension presence MASK (`byte/element/mip/layer/aspect`), the
> range VALUES are runtime (tensor/sparse → 3d; §24 domains = value semantics). Grammar `!buffer<plain,!f32>` /
> `!image<d2,fmt>` / `!view<RES,mip,layer>`; tri-split `view_combination_valid` (parser fails / decoder rejects /
> factory asserts). Fixed a corrupt-input abort (parser fell into the asserting factory on a prior error). 73/73 × 4
> configs + tidy + `-Werror=switch`.
> **CEIR-3d ✅ (shapes + tensors, §21/§35) 2026-08-08.** Four appended kinds (Dim/Shape/Tensor/SparseTensor — **no
> version bump**). Type-level foundation only (the `ceir.shape`/`ceir.tensor` op dialects + layout §22 are CEIR-18;
> tensor/sparse returned from the 3c boundary). `!dim<4|dyn|N>` / `!shape<..>` / `!tensor<e,s>` grammar; tri-state
> `shapes_broadcast` (right-aligned incompatible pos) / `shapes_reshape` (overflow→Unknown). Added `type_is_canonical`
> (decoder rejects non-canonical records — a name on an Int etc. — latent since 3a) + `dyn` reservation tri-split. A
> 3b×3d seam test (substitute a param through a tensor shape). 77/77 × 4 configs + tidy + `-Werror=switch` (31 kinds).
> **CEIR-3e ✅ (physical quantities, §17/§18) 2026-08-08.** One appended kind (`Quantity`, no version bump): tags a
> numeric underlying with an 8-base SI dimension (ADR-0078) **bit-packed into count+cols** (crd::units::Dim is
> compile-time-only → CEIR mirrors it at runtime; a static_assert pins the base order). Grammar `!qty<!f32,L1T-2>` /
> dimensionless `!qty<!f32,1>`; `quantity_dimensions_equal`→first_differing_base (the 3z Length+Time diag), `quantity_dim_mul/div`
> (i8 overflow→failure). ⭐ The **`units.erase` op is the FIRST non-reference dialect through the CEIR-2 generator** (a
> `.ceirop.toml` + regen, zero central edits; drift now 3 dialects × 5 files, smoke auto-generated). 83/83 × 4 configs +
> tidy + `-Werror=switch`.
> **CEIR-3f ✅ (ownership/lifetime qualifiers + escape predicate, §19) 2026-08-08.** One appended kind (`Qualified`, no
> version bump): a WRAPPER — `members[0]`=type, `count`=`OwnershipKind` (9 modes imm/mut/borrow/own/shared/weak/state/ext/
> transient) — chosen over nine kinds (a value has exactly one mode; the wrapper composes over resources too). Grammar
> `!qual<borrow,!buffer<plain,!f32>>`; tri-split `qualified_composition_valid` (rejects qual-of-qual/dim/shape/trait). The
> escape rule splits 3f/3z: 3f ships `value_escapes_region(Value*,Region*)` (def-use walk vs directional region
> containment), 3z composes it over `Qualified<BorrowedView>`. ⛔ Two advisor-caught fixes: **`create_operation` never
> wired `Region::m_parent`** (latent since 1a; the first upward walk reported false-positive escapes) and **`substitute`
> bypassed the tri-split** (retroactively closes qty-of-qty/qual-of-qual via a new `SubstResult::failed_compose`).
> Escape contract = direct-use, type-directed. 87/87 (773 assertions) × 4 configs + tidy + `-Werror=switch` (33 kinds).
> **CEIR-3z ✅ BAND-3 GATE (§16/§17/§19/§21) 2026-08-08.** The four band-3 error checks fire as **discriminating pointing
> diagnostics**: Length+Time (`quantity_dimensions_equal`→first_differing_base, base 0 AND base 2), rank-mismatched
> broadcast (`shapes_broadcast`→position 1 + control), borrowed-view escape (a NEW public `find_borrowed_escape(Module&)`
> →`BorrowEscape{value,escaping_use}` module walk over `!qual<borrow,_>` values — both value kinds; owned-escaper +
> inside-borrow correctly ignored), generic-constraint (`substitute`→failed_param/failed_trait, exact (T,Ord) + accept
> control). Pointing upgrade: `value_escapes_region`(bool) gained `first_escaping_use`→`Operation*` (a bool can't point).
> ⭐ **Honest boundary:** pointing predicates + the one escape walk; the op-level verifier wiring of dim/broadcast/
> constraint onto typed operands composes at **CEIR-4** (no producer op yet — 3e "predicate-now" precedent). `find_borrowed_escape`
> is first-offender, structural (not dominance — CEIR-5b), direct-use+type-directed. Dedicated `test_band3_gate.cpp`
> (`[gate3]`, 4 cases). 91/91 (791 assertions) × 4 configs + tidy + `-Werror=switch`. **→ BAND 3 CLOSED.**
> **CEIR-4a ✅ BAND 4 OPENS (§26 effect vocabulary) 2026-08-09.** `effect.hpp`: `EffectFamily` (all 27 §26 families, u8,
> append-at-end, static_assert-pinned to the generator) + `EffectRecord` POD `{family, target(None/Operand/Result),
> index, range_mask}` (range_mask reuses 3c ViewRange; kind-level — per-instance is 4d, callee-derived EffectsFn is
> CEIR-5). ⭐ Attach point **B1**: effects on `OpInfo` via `register_op(...,effects={})` (arena-COPIED), queried by
> `Context::op_effects` — not a 1d interface, not reflection-only. ⛔ **EMPTY≠UNKNOWN**: an empty span is "provably
> effect-free" only when `op_info!=nullptr`; unregistered = maximally effectful. ⛔ **func.call landmine** (a registered
> op defaulting to effect-free reads as *provably* none) → declares a conservative ExternalCall barrier. Pure⇒zero
> effects at both live arms. 2a schema: bare-family string OR `{family, operand|result, range}` table, generator
> validates vocab+index+range; `OpSchema.effects` StringView[]→EffectRecord[]; `.ops.json` string[]→object[] (schema_v1,
> scaffold field). NOT serialized (no version bump). 98/98 ctest (820 assertions) × 4 configs + tidy + opgen(36 py).
> **CEIR-4b ✅ (§27 determinism + §28 numerics) 2026-08-09.** `semantics.hpp`: `DeterminismClass` (5 §27 tiers +
> `Unspecified=0` default, static_assert-pinned; `External`→`ExternalNondeterminism` reconciled) on `OpInfo` via
> `register_op`, `op_determinism`; `CompilerMode` (Normal/Fast/Deterministic/Certified, session state NOT serialized) +
> `determinism_satisfies_mode` (6×4 matrix). `NumericalSemantics` = all 12 §28 knobs (0=Inherit), per-INSTANCE, ONE
> pack/unpack into a reserved `numerics` int attr (unpack validates bounds); `numerics_satisfies_mode` honors the fmad
> scar (Fast admits FMA/fast-math). `find_mode_violation(Module&)` = first op violating the active mode's §27 class OR §28
> numerics OR a corrupt `numerics` attr (violates every mode). Cook-time native≥op consistency; `OpSchema.native_determinism`
> typed. ⛔ EMPTY≠UNKNOWN (Unspecified fails strict modes). Boundaries: pass-wiring→CEIR-6, replay/alternatives→CEIR-5+,
> kind-vs-instance not cross-checked (CEIR-6). arith int ops = BitExact. 106/106 ctest × 4 configs + tidy + opgen(40 py).
> **CEIR-4c ✅ (§15 eval domains + §32 realtime + domain-legality verifier) 2026-08-09.** ⭐ `register_op` consolidated
> into an `OpSpec{traits,verify,effects,determinism,domain}` descriptor (designated initializers; migrated every site).
> `EvalDomain` (10 §15 domains + Unspecified, static_assert-pinned) per-op-KIND on `OpInfo` via the spec + 2a `domain`
> field + `op_domain`; `OpSchema.domain` StringView→EvalDomain. `RealtimeClass` (7 §32 + Unspecified) — a REGION property,
> NOT a schema field. A region's (domain+realtime) rides ONE packed `region_exec` int attr on the region-owning op (module
> CONTENT — survives round-trip; symmetric with 4b's session-only mode). `find_domain_violation(Module&)→DomainViolation`
> — innermost-tag-wins; seeded §32 rule (FileIO/NetworkIO illegal in AudioRealTime/DeviceTime/HostAudioTime); ⛔ an
> UNREGISTERED op in an audio region is flagged (maximally effectful), a registered effect-free op is legal (empty≠unknown).
> The 4c walk does NOT consume kind-domain (→CEIR-6). arith=EitherHostOrDevice, test=HostFrameTime. 115/115 ctest × 4
> configs + tidy + opgen(43 py).
> **CEIR-4d ✅ (effect-derived ordering hazards §26/§116) 2026-08-09.** A SCHEMA-QUIET slice (hazards DERIVED from 4a
> effects — no TOML/generator/regen/py). `hazard.hpp`: `HazardKind{None,War,Raw,Waw}` + `ResourceClass` (14) +
> `effect_access(EffectFamily)` (TOTAL switch, NO default → -Werror=switch guards a 28th). ⛔ `RandomRead` WRITES (a PRNG
> draw advances the stream — else RNG draws reorder, breaking replay); `TimeRead` inert-read; Alloc/Dealloc/Residency in
> Memory (use-after-free visible); IO one rw class; ExternalCall+Synchronization = Universe barrier. `Context::ops_hazard
> (before,after)` (WAW>RAW>WAR over effect-pairs sharing a resource `(class, Value|null)` + overlapping range with ≥1
> write; distinct Values non-aliasing; reports everything; unknown=Universe rw, Pure=None) + `collect_block_hazards` (O(n²)
> all-pairs reference, one block, list order). `Operation::result` loosened to const. 122/122 ctest × 4 configs + tidy.
> **CEIR-4z ✅ BAND-4 GATE 2026-08-09 → BAND 4 CLOSED (4a–4z).** A TEST-ONLY gate (like 3z) composing find_mode_violation +
> find_domain_violation + ops_hazard/collect_block_hazards over ONE curated module — the band's exit criterion "the
> compiler distinguishes reorderable vs ordered ops correctly" met verbatim. Centerpiece: the **WAR-needs-lifetime scar**
> (read(R)-then-write(R) = WAR from effects+SSA identity, no decl-order; different buffer = None). Exact 4-edge
> collect_block_hazards matrix + reverse sweep; ⛔ every gate op BitExact (the Unspecified-default mode-axis trap);
> orthogonality by FULL edge-list identity (numerics→Certified, Unspecified-op→determinism, side audio-RT+FileIO→domain —
> hazards identical each time); hazards survive serialize/deserialize (Value identity through the parser fixup). `test_band4_gate.cpp`
> (`[gate4]`, 5 cases). 127/127 ctest × 4 configs + tidy. → BAND 4 CLOSED.
> **CEIR-5a ✅ (structured control-flow region ops + the constant-cond if fold) 2026-08-09.** Band 5 opens (a GEAR
> CHANGE). The generator gained a region-SIGNATURE schema + THREE variadic axes (operands/regions/results; arg COUNT is
> the verifier contract, MIN-ARITY builders, full arity via create_operation). `ceir.core` = `scope`/`if` (value-producing,
> variadic results), `for`/`foreach` (typed region arg), `while` (cond+body, structural — cond-yields-1-bool→5b),
> `switch`/`match` (variadic case regions; patterns→CHIR), `yield` (variadic-operand Terminator) — all BitExact + ZERO
> effects. `Context::fold_constant_if` splices the taken branch (THEN and ELSE, both tested) + RAUWs the results with the
> yield's operands + erases; BAILS on non-const/multi-block/no-yield, a `region_exec`-tagged if OR any tagged op INSIDE
> the taken block (⛔ a rewrite must audit the attrs it MOVES), and a yield/result COUNT MISMATCH. Design: HOMOGENEOUS
> result types (placeholders until CEIR-6; the fold replaces them — no create_operation overload), MIN-ARITY builders,
> SKELETON-VERIFIES, loop-carried values→5d. ⚠ This turn disproved 2 checkpoint blocker-guesses (builder-count-param,
> create_operation-overload) — mark checkpoint blockers unverified unless the code was read. 138/138 ctest × 4 + tidy +
> opgen(49 py). NEXT = **CEIR-5b** (SSACFG verifier: dominance/terminators/block-args — the general yield contract + the
> 3f back-link earn their keep, §13/§115) → 5c (calls + EffectsFn) → 5d (state/delay) → 5z (executor gate).

> **CEIR-2 generator (reusable for every future band):** `tools/ceir_opgen/ceir_opgen.py` reads
> `engine/ceir/ops/<dialect>.ceirop.toml` → validates (structure+vocab of every §8 + ADR-0110 field) → emits from ONE
> model: `engine/ceir/generated/crd/ceir/gen/<d>_ops.{hpp,cpp}` + `<d>.ops.{json,md}` + `tests/ceir/generated/test_<d>_gen_smoke.cpp`
> (3 TEST_CASEs, globbed via CONFIGURE_DEPENDS). `--check` drift ctest guards all 5 files/dialect; `test_opgen.py` = 26
> validator/emitter unit tests. **Adding a CEIR op = a TOML edit + regen, zero central-enum/switch edits.**

**⛔ THE LIVE TRACKER IS `docs/detours/D-007-ceir-tracker.md`** (CEIR bands 0–32 + the RAH parallel track). CEIR — the
Cerid Execution IR — is the new master architectural spine (user-directed 2026-08-07): every reusable algorithm becomes
a versioned, inspectable, hot-reloadable **program asset**; native C++ only for genuinely new host/hardware capability.
**THE LAW:** `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` (§0–§185). Mantra: *ALGORITHMS ARE
PROGRAM ASSETS · CAPABILITIES ARE NATIVE PRIMITIVES · COMPILERS CHOOSE LOWERINGS · BACKENDS EXECUTE.* The old post-RAF
4-track table in `D-007-gpu-program-system.md` is **re-hung under the CEIR bands** and preserved as history/contracts
(A/RPL→CEIR-15 · C/MLR→CEIR-21 · hesap-GPU→CEIR-19 · frame+executor→CEIR-12/13 · B/I2D→CEIR-28 · D/D7E→CEIR-30).

**CEIR-0 DESIGN PHASE COMPLETE + ACCEPTED (2026-08-07/08):** 0a inventory (headline: RAF already did the atomic-vs-
composite split → CEIR is a promotion, not a rewrite) · **ADR-0108** (owned language stack; C++ no longer the *only*
authorable program — supersedes ADR-0081 §9) · **ADR-0109** (CEIR/CHIR/CKIR one-way layer contract + `crd-ceir`
host-only module + `crd-ceir-host`/`crd-ceir-gpu` dependency-inversion bridges + I3/I4/I5; **binding for CEIR-1**) ·
**ADR-0110** (native-intrinsic schema + plugin levels) · 0e CHIR-0 note · 0g two-axis maturity model · 0h deletion
ledger · 0z §184 report + sizing (CEIR-1…13 ≈ 34–55 KLOC, ~4–8 mo). CEIR-0f (D-007 restructure) executing.

**HOW WE WORK (user-directed):** **strict band order** CEIR-0→32; each band closes at its gate before the next. **RAH
runs in PARALLEL** (the binding/attachment vocabulary CEIR-9/11 lower onto). ⛔ Everything else PAUSED (§176: only bug
fixes, CKIR fixes, tests, docs, RAH, CEIR). ⛔⛔ Foundational/critical-path work done DIRECTLY, never delegated. ⛔⛔
Implementation forks require `isolation:"worktree"` + a tight mandate ([[feedback_implementation_forks_need_worktree_isolation]]).
**User controls commits (no AI trailer).**

## Active state

- **CEIR (spine) — CEIR-1a ✅ CLOSED (4-config per-slice sweep PASS, 2026-08-08).** `crd-ceir` module +
  `Context/Module/Operation/Value/Block/Region` + intrusive in-arena def-use + `crd::memory::GrowableLinearAllocator`
  (moved to crd-memory) + `crd-ceir-invariants` I3/I5 gates — all green across debug/asan/shipping-LTCG/tidy. The full
  sweep peeled **7 pre-existing cross-band blockers** (RAF/REN/CKIR bands never passed shipping-LTCG/asan-complete/tidy);
  all fixed gold-standard (2 real engine bugs: DX12+Vulkan RT pipeline-cache keyed by pointer/handle → content-hash).
  Log: `docs/sessions/2026-08-08-ceir-1a-and-preexisting-fixes.md`.
- **CEIR-1b ✅ CLOSED (2026-08-08).** `SymbolTable` (per-Module, arena-backed HashMap; duplicate-reject) + `Visibility`
  + the `ceir.func` dialect (`func.func`/`func.call`/`func.return`, cross-module resolution by name) — all on the
  generic Context factories (open-world). `tests/ceir` 12/12. **Gated across all 4 configs on crd-ceir** (a complete
  gate — crd-ceir has zero downstream consumers, grep-proven; full-tree sweep re-earns its keep at the band close).
- **CEIR-1c ✅ CLOSED (2026-08-08).** Interned typed attribute VALUES (`AttrValue`/`AttrId`, dedup) + a per-op
  AttrDict (`op->attr(name)` / `Context::set_attr`) + the source map (`register_file`→`file_id`, `file_path`) so
  every op's `SourceLoc` provenance is real (§111, no retrofit). Dissolved the 1b interim: `func.call`'s callee is
  now a `SymbolRef` attribute. `tests/ceir` 18/18. Gated all 4 configs (scoped-complete).
- **CEIR-1d ✅ CLOSED (2026-08-08).** Open-world **dialect registry** + op **traits/interfaces** + **verifier**
  dispatch — analyses query traits/interfaces, the core NEVER switches on op.kind (new **I6** grep-gate proves it,
  bites on `switch(op.kind())`). Unknown-dialect ops survive opaquely; the `func` dialect self-registers.
  `tests/ceir` 22/22. Gated all 4 configs (scoped-complete).
- **CEIR-1e ✅ CLOSED (2026-08-08).** Deterministic textual **printer** (IR→canonical MLIR-flavored text; pre-order SSA
  numbering + name-sorted attrs → byte-identical; floats keep a `.`/`e` marker; unknown-dialect opaque; **no layout**,
  §10) + recursive-descent **parser** (`parse→ParseResult`; use-before-def fixup pass, strings unescaped-before-intern,
  balanced-brace region count skipping string literals, malformed input rejected w/ byte offset). **`print(parse(x))==x`
  byte-exact.** MLIR-faithful symbol identity (advisor): func name/visibility now ride ON the op as `sym_name`/
  `sym_visibility` attrs (SymbolTable = an INDEX over `sym_name`), so identity round-trips through the generic attr
  machinery and the parser rebuilds the module table. `tests/ceir` **31/31**. Gated across the 5-config contract.
- **CEIR-1f ✅ CLOSED (2026-08-08).** **Binary serial form** (`binary.hpp`/`binary.cpp`: `serialize`/`deserialize`) —
  CRDR-shaped (ADR-0038): magic `'CEIR'` + version + FourCC/length chunks a reader **skips by length** when unknown
  (`STRP`/`SRCM`/`ATTR`/`BODY`). ⛔⛔ field-by-field LE (self-contained `put_u*` + `.ok` `Cursor`; can't link crd-kir).
  **⭐ Content-pure:** pools built from the module WALK, BODY holds pool INDICES not Context ids → the blob is a pure
  function of module content (dirty-context byte-equality proven). Carries `Region::kind` (closes the 1e divergence, via
  new `Context::set_region_kind`); `SourceLoc` survives by PATH; symbol identity via the shared `detail::register_symbol`
  (extracted from the parser). `serialize∘deserialize∘serialize` byte-exact; agrees with the text form. Malformed input
  rejected w/ byte offset (bad magic/version/truncation/trailing-junk/oob index). `tests/ceir` **37/37** (`build_rich`
  now a shared `rich_graph.hpp`). Gated across the 5-config contract; invariants I3/I5/I6 green both OSes.
- **CEIR-1g ✅ CLOSED (2026-08-08).** **`ModuleBuilder` fluent API** (`builder.hpp`/`builder.cpp`: `ModuleBuilder` +
  `OpBuilder` proxy + `InsertionGuard`). ⛔⛔ NO privileged bypass — every op routes through `Context::create_operation`
  + shared `detail::register_symbol`, so a builder module is **byte-identical to the hand-built one** (proven).
  `verify(&failing)` dispatches the REAL per-kind `Context::verify` (rejection test proves it, no stub); `build()`
  returns nullptr on a duplicate `sym_name` (op erased, no silent overwrite). `tests/ceir` **41/41**. Gated across the
  5-config contract.
- **CEIR-1h ✅ CLOSED (2026-08-08).** **The permanent harness, seeded** (§119/§167): round-trip fuzz (random valid
  modules via `ModuleBuilder`, fixed xorshift64 seeds — text+binary byte-exact), a `stable_hash` (FNV-1a over the 1f
  content-pure blob — NEW surface, deterministic + content-derived), and a malformed corpus + single-byte-corruption
  SWEEP (no crash; ASan is the proof). ⛔⛔ **The fuzz caught 2 real OOM crashes on day one** in code that had passed 4
  slices of gates — a huge textual def-id and a corrupt binary count; BOTH loaders hardened (text bound by input
  length; binary counts bounded by chunk length / `kMaxDecodeCount`). `tests/ceir` **46/46**. Gated across the 5-config
  contract.
- **CEIR-1z ✅ CLOSED (2026-08-08) — BAND-1 GATE.** A typed hello-world (func + const + call + return) round-trips
  **text ⇄ binary ⇄ builder byte-identically** and its callee symbol resolves after all three forms
  (`tests/ceir/test_hello.cpp`). `tests/ceir` **49/49**. Gated across the 5-config contract. ⭐⭐ **BAND 1 CLOSED
  (1a..1z).** Already committed this session: 1a core (`5f81ce8`) + the 7 pre-existing fixes & 1a docs (`6e6f183 "CEIR-1a
  finished"`). ⛔ NOW: (A) the USER commits+pushes the remaining **CEIR-1b..1z** batch (~38 files: `engine/ceir/**` +
  `tests/ceir/**` + `scripts/check_ceir_invariants.{ps1,sh}` + docs) — ONE commit
  `feat(ceir): band 1 core IR substrate (CEIR-1b..1z)` with the per-slice breakdown in the body (slices overlap in
  files → not per-slice stageable). (B) then make **GitHub CI GREEN** (whole-repo net; fix reds gold-standard, user
  commits fix batches). (C) MEMORY.md compaction to <17.1KB during the wait. Only after CI green → **CEIR-2**.
- **RAH (parallel track) — front = RAH-1a.2.** ✅ RAH-1a.1 (visbuffer fold) DONE + gated (REN-38-F6, 97 asserts, both
  backends). **NEXT = RAH-1a.2 (DELETE, user-chosen):** retire `IGBufferTarget`+`draw_gbuffer`+`create_gbuffer_target`
  (both backends) + `RenderingDesc.gbuffer`; migrate ~8 test sites to the `color`-span MRT path; needs a
  plain-vertex-MRT-color-span path + regular-target readback first. Then RAH-1a-close → RAH-2 (unblocks CEIR-11/B).
- **PAUSED (parked, not dropped):** B/I2D+SPR (ADR-0107 review pending) · C/CGP selector + HGP/MLR · D/MED codecs
  (animated GIF→TIFF→JPEG; real-GIF external-oracle corpus owed) · main roadmap (hesap v18, eylem v1c+). Resume paths:
  the CEIR tracker's "Paused" table.

## Recently landed

- **2026-08-08** — **CEIR-1a CLOSED** (4-config per-slice sweep PASS). The global close peeled 7 pre-existing
  cross-band blockers (RAF/REN/CKIR left them: shipping-LTCG/asan-complete/tidy had never run to completion) — all
  fixed gold-standard, incl. two real engine bugs (DX12+Vulkan RT pipeline caches keyed by pointer/handle →
  fnv1a_64 content hash; DX12 anyhit flake 200/200 after), the RAF-10 catch_discover_tests ENVIRONMENT split, the
  AS-4 ASan timing guard, the C4743 stale-obj wipe, and 37 clang-tidy errors across 12 files. Uncommitted (19 files;
  user commits — proposed message in the log). Log: `docs/sessions/2026-08-08-ceir-1a-and-preexisting-fixes.md`.
- **2026-08-07 (later)** — repository-wide **documentation hygiene pass** (uncommitted): context.md → dashboard
  (history archived), ROADMAP/systems/debt/AGENTS/READMEs refreshed to honest state, retired-module overviews
  DELETED (user direction; git history keeps them), research outcome stamps, ADR index + link fixes. Full report:
  `docs/sessions/2026-08-07-doc-hygiene-pass.md`.
- **2026-08-07/08** — **CEIR pivot + CEIR-0 design phase COMPLETE:** CEIR becomes the master spine; the live tracker
  `docs/detours/D-007-ceir-tracker.md` created; CEIR-0a inventory + ADRs 0108/0109/0110 + the 0e/0g/0h/0z design notes
  all accepted; D-007 restructured (CEIR-0f). (uncommitted at time of writing — user commits.)
- **2026-08-07** — post-RAF 4-track kickoff: RAH-1a.1 + CGP-0/CUDA + MED-1 (`c116e98`); D-007 §POST-RAF + §UI/2D
  programmes + four-track tracker (`e3f8e5e`). Log: `docs/sessions/2026-08-07-post-raf-tracks-rah1-cuda.md`.
- **2026-08-06** — **RAF band COMPLETE** (`af3e04c`): `FramePassKind` retired, ADR-0106 closed.
  Log: `docs/sessions/2026-08-06-raf12-3-retire-framepasskind.md`.
- **2026-08-03…05** — RAF-0…12: substrate → one-submission frame graph → executors → engine-default assets →
  app-custom renderer → hot reload → legacy deletion. Logs: `docs/sessions/2026-08-0{3,4,5}-raf*.md`.

## Open questions / risks

- **Per-slice gate — RATIFIED (2026-08-08):** each slice closes on **2 Windows + 2 Linux configs + tidy**, all
  clean (win-debug + win-asan + linux-debug + linux-asan + tidy; Linux via WSL), **scoped to the changed module**
  (crd-ceir has zero downstream, grep-proven → crd-ceir-tests across those configs is complete). **No whole-repo
  suite per slice** (too slow). **GitHub CI is the whole-repo safety net and must stay GREEN.** See
  `project_ceir_autonomous_loop_grant`. ⛔ **Between CEIR-1 and CEIR-2: fix CI green** (it has real build/test reds).
  ⛔ **At CEIR-14: expand its subslices explicitly in the tracker.** GOAL = all bands 1→32 closed.
- **Pending user review:** RAH-0 audit (`docs/systems/rah-0-canonical-model-audit.md`) + ADR-0107
  (`docs/decisions/0107-ui-2d-architecture.md`). Track B code is blocked on the ADR-0107 review.
- `MEMORY.md` ≈ 19.9 KB (hard read limit 24.4 KB) — deeper cull deferred, entries must be MERGED/DROPPED not just
  hook-trimmed.
- The integrated CUDA fork worktree `.claude/worktrees/agent-af34b487c5544c8fa` can be removed.

## Gates that matter

Per-slice DoD: `scripts/per-slice-check.ps1` (+ `-IncludeRelease` for GPU/LTCG slices); cluster close =
`scripts/full-sweep.ps1` (18-config). **Run `ctest`, never the bare test binary** (guards are ctest-only). GPU slices:
`ValidationCapture` + both backends. Tidy per touched file via `scripts/tidy-files.ps1`, never accumulated.

## Active detour

**D-007 (merged with D-008 on 2026-07-11) — the GPU program system.** ACTIVE; grew out of hesap v17 (2026-07-07).
CKIR IR + gpu-context convergence + the full visual frontier + RAF (all ✅) → now **CEIR is the master spine** (2026-08-07;
the live tracker is `docs/detours/D-007-ceir-tracker.md`, the landed-history ledger is `D-007-gpu-program-system.md`).
Everything above is D-007 state. Queue rules: `docs/detours/README.md`.

## Recent milestones (one line each; details in session logs + `docs/bench/`)

- **2026-08-06 — RAF complete:** engine renderers are ordinary assets; one backend-neutral command model; executor
  registry; hot reload; legacy paths deleted (ADR-0106).
- **2026-07-21…08-03 — REN-36…41:** authored frame graphs/techniques/materials (`.crdm/.crdt/.crdv/.crdl/.frame.toml`),
  bindless+multi-draw (38-G1 119 fps), indexed-pull reuse, O(chunks) extract, soft shadows (PCSS/EVSM/MSM), velocity +
  TAA, Nanite-class cluster LOD start.
- **2026-07-13…16 — the GPU compute crush campaigns:** 2D FFT 1.16–1.20× cuFFT bit-exact; reduction beats CUB; radix
  sort 0.73× CUB (bit-exact, 8.4× session gain); NRC fused MLP 2.37× cuBLAS; B14 SVGF/DDGI/ReSTIR/NRC + B15
  atmosphere/clouds + B16 FFT ocean — all gold-standard CKIR. (Narrative: the context-history archive; boards:
  `docs/bench/`.)
- **2026-07-10…12 — D-007 device+IR convergence:** one `VkDevice`, I1/I2 leak gates closed, oracle rounds per-op, CUDA
  fan-out bit-exact.
- **2026-07-23 — RET band: crd-rhi/rhi-vulkan/renderer/shader DELETED** (ADR-0105); gpu-context IS the graphics layer.
- **2026-07-02 — hesap v13 close:** interpolation/quadrature/differentiation/motion — full peer-board crush (scipy/
  MATLAB/Boost/GSL/Ruckig).
- **Earlier (hesap v0→v12, geometry, units, scene/ECS):** see `docs/phases/` + the archive.

## Paused main-roadmap work

- **Phase 3.1.6 hesap:** paused mid-v17 (GPU compute) — v17's substrate is being built AS D-007; hesap-GPU is the
  detour's last stop. v14 tensors ✅ (2026-07-05) · v15 forward AD ✅ · v16 reverse AD ✅ (2026-07-07, ADR-0097).
  `docs/phases/phase-3.1.6-hesap.md`.
- **Phase 3.1 eylem:** ⏸ paused at v1b close (ADR-0076 §12 sequencing); resumes v1c+ after the detour + hesap.
  `docs/phases/phase-3.1-eylem.md`.

For the full doc map: `docs/README.md`. ADR index: `docs/decisions/README.md`. Open debt: `docs/debt.md`.
