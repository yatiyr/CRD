# 2026-08-14 — CEIR-16d/16z: scene.raster migration + visbuffer dissolution (§128 CLOSED)

**Band:** D-007 CEIR detour, CEIR-16 (executor migration → §127/§128). This session drove CEIR-16d's live path to
completion and dissolved the last composite raster executor (visbuffer.raster) into scene.raster, **closing CEIR-16**:
every composite raster executor is now a CEIR-authored program recorded through the generic `record_ceir_render`, and
every imperative recorder is deleted. **14 built-in executors → 13.**

## What shipped

### 16d-live-4 — the scene.raster migration + `record_scene_raster` deletion
- **mrt≥2 as an op-mode (4a):** the full 7-mode `BlendMode` became the closed CEIR `blend` vocab (WBOIT
  revealage_multiply/multiply/reveal_composite no longer fold to Opaque); `SceneBuildDesc.mrt_n`/`blend[4]`;
  `build_scene_ceir` emits N `render.color_attachment` ops with a per-attachment `blend` + a `color_slot` index attr;
  `execute_render_lowered` DEFERS a ≥2-colour scope's `begin_rendering` and `emit_scene_list_mrt` opens a scope PER ITEM
  (the legacy MRT arm); `build_frame_plans` un-skips mrt≥2.
- **On-device A/B (4b)** caught a real bug: `fs_target` resolved every colour attachment to the "color" slot, leaving MRT
  color1..3 unwritten — fixed with the `color_slot` attr `fs_target` maps to color/color1..3.
- **THE DELETION (4c):** `record_scene_raster` + its scar helpers (bind_map/bind_atlas/attach_textures) + the
  `CRD_SCENE_VIA_CEIR` flag deleted; `register_builtin_records` → `record_ceir_render` unconditionally. The 4 device-free +
  2 on-device A/B parity tests were converted **single-path with ABSOLUTE asserts** — post-deletion a "legacy" arm is a
  vacuous CEIR-vs-CEIR (see the scar [`feedback_deleting_reference_in_ab_parity_test_degrades_to_can_t_fail`]).

### 16z — visbuffer.raster §41 dissolution (fork-b)
A visibility buffer is not a canonical concept; it dissolves INTO scene.raster. Sub-sliced:
- **16z-1 — uint clear:** `SceneBuildDesc.clear_is_uint`/`clear_uint`; `build_scene_ceir` emits a **u32-format** attachment
  image + `clear_kind="uint"` + `clear_uint` (the materializer already read them, RAH-1a.1; the verifier's
  `ClearKindFormatMismatch` scar REQUIRES the uint image type).
- **16z-2 — procedural scope-mode:** a closed-vocab `geometry="storage"|"procedural"` attr on `render.scene_draw_list`
  (absent = storage), wired the 4a-1 way (toml + opgen regen + `kGeometryVocab` + `RenderMisuseKind::GeometryModeInvalid`).
  `emit_scene_list`'s procedural branch is a byte-exact port of the deleted `record_visbuffer_raster` loop:
  `GeometryKind::None`, vertex_count only, **zero bindings**, no coalescing; the skip is on `vertex_count==0` **not**
  storage (the storage-null skip is the resolve-failure guard — advisor's key ruling: procedural is *declared*, never
  inferred from a null storage buffer).
- **16z-3 — the re-author + delete + close-out:**
  - **A-wiring / the flip:** the `KindRow` remaps the kept kind string `raster.visbuffer` → `kExecSceneRaster` + a new
    `procedural` role bit (+ `pp::kProcedural`), exactly like `raster.depth_only`/`raster.mrt` — zero asset churn.
    `add_draws_scene(procedural)` carries no-storage draws; `build_frame_plans` sets `sbd.procedural` / `clear_is_uint`
    (DERIVED from colour-0's R32Uint format via a new `fg_format_is_uint`, RAH-1 "visibility is a typed attachment", not an
    author boolean) / `clear_uint`; scene.raster's schema gained an optional `clear_id`; `map_raster` + the scene
    `to_authored_pass` arm carry it.
  - **Deletion:** `record_visbuffer_raster` + its record registration, the `visbuffer.raster` **schema** registration
    (`register_builtin_executors` 14→13 too), `map_visbuffer`, `pass_is_visbuffer`, `kExecVisbufferRaster`, the visbuffer
    `to_authored_pass` arm, and the frame_ceir name line — all deleted. Repo-wide `== 14U` → `== 13U` sweep (both
    registries) + `reg.size() == 15U` → `14U` (builtins+custom). The gpu-context `draw_visbuffer`/`create_visbuffer_target`
    verbs are KEPT (B4-vis-4 raster-context consumers — 16z deletes the *executor*, not the verb).
  - **The 15c `VisbufferNeedsUintTarget` contract re-keyed to a UNION trigger** (advisor Q3 corrected against REN-38-A11):
    `pass_is_scene_raster && (pp::kProcedural ‖ pass_has("clear_id")) ⇒ the colour write must be R32Uint` (depth writes
    skipped). The **procedural arm** catches "authored a visbuffer, forgot the uint format" (REN-38-A11); the **clear_id
    arm** catches "any scene pass declared id semantics on a target that can't hold them". `procedural ⟺ visbuffer` (it's the
    only procedural kind), so procedural-only would drop the clear_id hole and clear_id-only would drop the procedural arm.

## Gate (2 Win + 2 Linux + tidy)
render-pass 46/5 · render-graph device-free 309/15 + gpu [raf7] 363/2 (Vk+DX12) / 181/1 (llvmpipe) · frame-cook 2751/96 ·
scene-render 1539/72 (win) / 1254/55 (linux) **incl. the device visbuffer id read-back** (test_scene_render_gpu §1483) ·
ceir-gpu 528/35 · ceir 128/13 + opgen-drift · `crd-sandbox --smoke-test 2` PASS · **gpu-context vulkan + dx12 REN-38-A11
visbuffer cook + device gates** (the last unrun consumer of the remapped scene+CEIR path). win-debug + win-asan +
linux-gcc-debug + linux-gcc-asan; clang-tidy clean. Count verified 13.

## Scars / lessons
- [`feedback_deleting_reference_in_ab_parity_test_degrades_to_can_t_fail`] — deleting the reference impl an A/B compares
  against silently degrades the test to A==A; convert to ABSOLUTE asserts in the same change.
- The advisor's clear_id-only re-key (Q3) had a hole exposed by REN-38-A11 (a `raster.visbuffer` with no clear_id writing
  RGBA must reject); reconciled to the union trigger. Primary-source evidence (a test) correctly overrode a general
  advisor principle — surfaced in one reconcile call before switching.

## Next
CEIR-17 (master spine `project_ceir_master_spine_locked`; expand its sub-slices explicitly in the tracker at band-open,
sized by CEIR-0z). Broad feature development now resumes as CEIR program assets.

## Commit
All CEIR-16 work is UNCOMMITTED (user commits). Proposed messages: 16d-live-4 (scene.raster migration + record_scene_raster
deletion) and 16z (visbuffer dissolution) — see the session hand-off.
