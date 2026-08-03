# REN-41 — VISUAL FRONTIER: TAA + prefiltered impostors + Nanite cluster-LOD

> Execute-ready campaign dossier. User-directed 2026-08-02: "fully gold standard, frontier 2026
> level, no artifacts — seamless LODs, no aliased pixels, impostors far away, full speed." Scope
> approved: full frontier bundle (deterministic + TAA) **and** Nanite cluster-LOD integration.

## ⏩ NEXT-SESSION START HERE (2026-08-03, later) — STAGE 4

REN-41 Stages 1–3 **and per-object velocity (steps 0→6) are DONE + verified** — velocity motion vectors WRITE
(depth prepass → MRT) and are CONSUMED (TAA resolve `prev_uv = R_reproject + velocity`) on both backends and all
four TAA frames. Two new GPU verbs (`draw_storage_multi_indexed_mrt_indirect` for GPU-cull, `draw_storage_indexed_mrt`
for CPU-cull), the velocity FS/programs/DrawItem, `prev_palette` (CPU snapshot + `--gpu-skin` device pass), and the
resolve flip all shipped; a pre-existing cook cycle-detection regression was root-caused and fixed. scene-render
53/53 both backends; sandbox renders correct (Vulkan CPU/GPU-cull, DX12 GPU-cull, `--gpu-skin`), 0 validation
errors; all touched files tidy-clean. Session: `docs/sessions/2026-08-03-ren41-velocity-runtime-integration.md`.

**NEXT: STAGE 4 — Nanite cluster-LOD renderer integration** (see "Stage 4" below). The 40-I algorithm/data pipeline
is CLOSED; this wires it into the renderer: a CKIR mesh+task shader graph over the packed clusters, a GPU
`cluster_select` compute pass, a `raster.mesh` draw path, a `MeshRenderer` cluster-vs-discrete route flag, and
authored `cluster_select`→`mesh_draw` frame passes. A large new render path — begin with a plan + `advisor`.

**Also open before REN-41 close** (small): a velocity CORRECTNESS gate (render → read the velocity buffer:
static≈0, mover≈expected screen delta) + a 1M-with-velocity fps bench board. Live state: `context.md`; row: D-007 REN-41.

## The diagnosis (verified, not guessed)

Five root causes, in order of how much they hurt:

1. **No antialiasing anywhere.** MSAA sample count = 1 across both backends; no TAA, no velocity
   buffer, no history. At 1M instances the far field carries spatial detail far above Nyquist (many
   objects per pixel) — a hard undersampling floor no LOD/impostor trick can cross. This is the deepest
   cause of "aliased pixels" and "black dots".
2. **Impostor atlas is hard binary coverage, unfiltered, no mips.** `impostor_atlas.cpp` writes
   alpha ∈ {0,255} (point-in-triangle at pixel centres), one mip; the draw FS discards at `alpha<0.5`.
   Tiny billboards with jagged silhouettes and point-sampled texels ⇒ the far-field carpet.
3. **LOD thresholds compressed into the visible range.** Metric `px = r·|row1|·H/w` is a correct
   projected DIAMETER in pixels (`|row1| = 1/tan(vfov/2) = 1.732` for the 60° camera). But the policy
   thresholds `[512,256,128,64,24]` put the first drop at 71% of a 720px screen — the torus (5u, 17u
   from origin, camera r=26–55) sits at ~65–130px, straddling the 64/128 boundaries and dithering as
   the camera orbits.
4. **Dithered cross-dissolve is spatial-only.** The 4×4 Bayer dither hides the synchronized 1M LOD
   pop, but a spatial dither is only seamless when a temporal accumulator averages it across frames
   (UE5's "dithered LOD + TAA"). No TAA ⇒ crawling checkerboard on the near torus.
5. **Far shadows flicker.** Caster cull works, but far-half casters still enter cascade 3 (~0.18 u/texel)
   and throw single-texel flicker; the far cascade needs a hard distance stop, not a faded dot.

## The end-state architecture

- **High-poly hero meshes** → Nanite-style continuous cluster LOD (mesh shaders). Seamless by
  construction, no dither.
- **Far field** → prefiltered mipped octahedral impostors, smooth coverage.
- **Instanced discrete-LOD meshes** (the grid) → corrected ladder + temporal-stochastic dither.
- **Antialiasing** → TAA over everything (jitter + motion vectors + history reproject). Resolves the
  dither, the impostor edges, and sub-pixel geometry.
- **Shadows** → hard far-distance stop + caster cull + cross-fade + PCSS (last three already shipped).

Build order is dependency-driven: deterministic wins first (validate against a moving target is
impossible otherwise), then TAA (the AA foundation the rest leans on), then temporalize the dither
(needs TAA), then Nanite (biggest; benefits from TAA for sub-pixel AA).

---

## Stage 1 — Deterministic LOD + impostor + shadow correctness  *(no new subsystem)*

**1a. LOD ladder.** Re-author `assets/lod/scene_default.crdlod` thresholds to hold detail for
clearly-visible objects and only reduce where a triangle is ≲1px (density-target reasoning in the
doc). Impostor threshold drops to genuinely-tiny (~10–12px). Keep the metric — it is correct.

**1b. Prefiltered mipped impostors** — the big far-field win.
- Bake the octahedral atlas as a MIP PYRAMID: supersample each tile then box-downsample so coverage
  becomes FRACTIONAL alpha (prefiltered), and color is averaged (no shimmer).
- Store mips in the group buffer; the impostor FS selects a mip from the projected billboard size
  (screen-space derivative of the atlas coords, or the size passed from the VS).
- Replace the binary `alpha<0.5` discard with the fractional alpha (blend / alpha-to-coverage). A
  prefiltered impostor at 5px samples an averaged mip ⇒ no aliasing even before TAA.

**1c. Hard shadow-distance stop.** The outermost cascade stops casting past a world distance — a tiny
far caster contributes nothing rather than a fading dot. Compose with the existing `shadow_fade_pct`.

**DoD:** torus holds detail at visible sizes and any transition is confined to small screen sizes;
far-field impostor carpet is smooth (mipped) not speckled; far shadows hard-stop. Screenshots at
10k/1M both backends, 0 validation errors.

## Stage 2 — TAA (the antialiasing foundation)

**2a. Camera jitter.** Halton(2,3) subpixel offset added to the raster projection each frame; the
cull/LOD/shadow-fit matrices stay UNJITTERED (jitter must not move the cull metric). Jitter tracked
per-frame for the reprojection to undo.

**2b. Velocity buffer** (RG16F RT). The scene VS emits current (jittered) AND previous clip position:
- header gains `prev_view_proj` (camera motion);
- the instance record gains `prev_world` (the animated grid bob + skinned ring move — static instances
  get prev=curr ⇒ zero velocity);
- velocity = curr_ndc − prev_ndc, both un-jittered.

**2c. History buffer** — persistent ping-pong RT (`PersistentNeedsSize`, already supported).

**2d. TAA resolve pass** — fullscreen in the frame graph: reproject history by velocity, neighborhood
min/max (variance) clamp against the current 3×3, blend feedback ≈0.9, write display + new history.
Graph: forward → velocity → taa_resolve(scene_hdr, velocity, history) → post.

**DoD:** static scene converges to a supersampled-looking image (edges clean); a panning camera shows
no ghosting past a few frames; both backends match. Sub-pixel far-field geometry stops shimmering.

## Per-object velocity (motion vectors) — IN PROGRESS (2026-08-02) — implementation dossier

User-directed maximal scope: **fold velocity into the depth-prepass (MRT)** + **full skinned velocity** (previous
bone palette, not just rigid). Purpose: continuously-moving geometry (spinning monuments, moving+deforming Fox,
the per-frame bobbing grid row) currently has its history rejected by the neighborhood clamp → reduced temporal
AA (crisp but shimmery). Motion vectors fetch the correct history texel → movers get full temporal AA. Same
buffer is the mandatory input for temporal upscaling (DLSS/FSR2/XeSS/TSR) and motion blur — foundational, not a
patch. No ghosting exists today (clamp prevents it); this closes the AA-quality gap on movers.

### The clean design (chosen — avoids header growth AND a prev_vp constants buffer)
Store only the **object-motion delta in current-camera space**, keep the exact R-matrix camera reproject:
`obj_delta = proj_uv(cur_vp · prev_world · skin_prev(pos)) − cur_uv`. Because BOTH samples use the current
jittered `cur_vp` (header word 6), the TAA jitter cancels in the delta, and a static instance (prev_world ==
world) stores exactly **zero** → the resolve is `prev_uv = R_reproject(uv,depth) + obj_delta`. Consequences:
- No `prev_vp` needed in the vertex stage (would be 16 words — the header has no 16-word gap and growing
  `kHeaderWords` is corruption-prone). No separate velocity_constants buffer.
- The R matrix stays the camera authority (exact); velocity only carries object motion. Safe incremental: with
  the velocity RT cleared to 0 and the resolve not yet reading it, the scene renders identically.

### DONE (built + compiles clean, non-regressing — velocity data uploaded but not yet read)
- `MeshGroup::prev_world` (16 floats/slot) + `prev_world_off`, `prev_palette_off` fields (scene_renderer.hpp).
- `write_slot` snapshots last frame's `rec.world` → `prev_world[slot]` BEFORE overwrite; fresh slot
  (rec.world[15]==0) shadows itself → zero velocity on spawn. Array pushed/cleared with the instance table.
- Section allocation: `prev_world_off = anim_state_off + anim_state_words` (16 w/inst, ALL groups);
  `prev_palette_off` after it (skinned only, `joint_count*16` w/inst). Buffer sized to include both.
- Upload on the SAME grain as instances: full payload (rebuild path) + per-run dirty (incremental path).
- Header words: `kHdrPrevWorldOff=107`, `kHdrPrevPaletteOff=108` (free band; kHeaderWords unchanged, nothing
  moves). Published per-group: `header[107]=prev_world_off`, `header[108]= skinned ? prev_palette_off : 0`.
- ⛔ **SCALE-DEPENDENT REGRESSION CAUGHT AT 1M (not by compiling, not by the showcase).** The CPU `prev_world`
  shadow is 64 B/instance = 64 MB at 1M, and an Array grows by doubling (transient old+new peak). The sandbox
  arena was `192 MB + 512 B/inst` = 704 MB and already near-full, so the foundation **OOM-asserted the TLSF
  allocator at 1M** while the `--lod-showcase` (small) path rendered fine. Fix: arena → `768 B/inst` (~924 MB at
  1M). The frontier alternative that avoids the CPU shadow entirely: a GPU compute pass copying `instances[].world
  → prev_world` at END of frame (for next frame) — no CPU array, no arena cost; deferred as a memory optimization.
- ✅ Verified after the fix: 1M field renders through the full 20-pass pipeline on DX12 (all passes execute,
  depth_prepass 22.7 ms / forward 22.7 ms / taa_resolve) AND Vulkan (no OOM, no crash); scene-render suite 53/53.

### DONE — cook `prev:clip` (2026-08-03, suite-green, ADDITIVE/inert)
`VaryingSourceKind::PrevClip` + parse `prev:clip` + round-trip emit + width; `.crdv` header gains `prev_world_off`
/`prev_palette_off` words (appended at struct end + POD-walk name table grown 13→15). In `cook_vertex_program_unchecked`:
`needs_prev` scan → capture pre-skin object position → after the current clip, re-skin it with `prev_palette_off`
(LBS; DQS refused as no-consumer), transform by `prev_world_off` (per-instance ×16), project by CURRENT `view_proj`
→ `prev_clip` varying. Inert unless a varying declares `prev:clip`, so **scene-render suite stays 53/53**. This was
the riskiest edit (shared vertex cook); it is landed and safe.

### ⛔ ARCHITECTURE FORK (decide before wiring the velocity pass)
The depth-prepass binds a **per-group** VS (skinned vs non-skinned) via `material_pass="Shadow"`. A velocity pass
needs the same per-group selection AND its clip must equal the forward's (shared_depth GreaterEqual). Two paths:
- **(A) Separate velocity program + a "Velocity" material pass.** velocity.crdv / velocity_skinned.crdv emit ONLY
  position + prev_clip + clip (clip-identical to scene → depth matches). Matched VS↔FS pair → NO DXIL ripple to the
  forward. Cost: a new material pass that resolves per-group like Shadow (material-system touch).
- **(B) Add prev:clip+clip to the SHARED scene VS.** Reuses the existing per-group Shadow resolution, but the
  forward + shadow FS must absorb the extra varyings (DXIL packing — the `ckir_hlsl` StageIn-injection from earlier
  this session must cover loc5/loc6), and it adds cost to every pass using the scene VS. Riskier (ripples widely).
- Recommend **(A)** — isolated, matched pair, no ripple. Velocity is inherently a separate pass.

### DONE — path A cook + assets (2026-08-03, suite-green + cook-verified)
- `clip` source term added (mirrors `prev:clip`) — the velocity FS forms the motion vector from two interpolated
  clips (prev + cur), so it needs no render-resolution input.
- `VaryingSourceKind::{PrevClip,Clip}` skipped in the varying-source validation (they're cook-derived, no name to
  resolve — that was the `UnknownSource`=10 failure the gate caught).
- **`assets/vertex/velocity.crdv` + `velocity_skinned.crdv`** — matched motion-vector VS programs, CLIP-IDENTICAL to
  scene/scene_skinned (only `position` declared; clip depends on nothing else → same depth, shares the prepass),
  emitting `prev_clip`@loc5 + `cur_clip`@loc6. Skinned one declares `prev_palette_off=108` for the previous pose.
- **Gate strengthened**: the two assets not only PARSE+validate, they **COOK** to valid CKIR entries in the suite
  (`cook_ok` exercises the actual prev:clip graph build — non-skinned AND skinned-LBS). Suite **53/53, 1211 asserts**.

### ⛔ RUNTIME NEEDS A NEW BACKEND VERB (discovered 2026-08-03, traced to the metal)
The 1M scene is GPU-cull **indexed-indirect** (visible counts in device memory; depth-prepass uses
`draw_storage_multi_indexed_depth_only_indirect`, frame_runtime.cpp:188). The existing `RasterMrt` executor path
(frame_runtime.cpp:250) is **CPU-driven** (`draw_storage_mrt(..., it.vertex_count)`) — it CANNOT draw GPU-written
commands. So writing velocity+depth from the culled draws requires a NEW raster-context verb
`draw_storage_multi_indexed_mrt_indirect` (N color attachments + depth + the indexed-indirect command) authored on
**BOTH** `dx12_raster_context.cpp` (RTV formats + ExecuteIndirect) AND `vulkan_raster_context.cpp` (dynamic-rendering
color attachments + vkCmdDrawIndexedIndirect) — mirror the depth-only-indirect verb + add the color attachments.
This turns the "runtime integration" from plumbing into a real multi-backend GPU addition. It is the gating item.

### REMAINING (renderer runtime integration — the last chunk; test BOTH backends)
0. ⛔ **NEW VERB (gating):** `draw_storage_multi_indexed_mrt_indirect` on DX12 + Vulkan (see above) + the frame-cook
   `RasterMrt` executor path routing indexed-indirect items to it (mirror the depth-only path at L188/L216).
1. **Velocity FS** (`build_velocity_fs_cooked`): read `prev_clip`@5 + `cur_clip`@6; `motion = ndc2uv(prev.xy/prev.w)
   − ndc2uv(cur.xy/cur.w)` (NDC±Y aware); replicate the IGN dither discard (fade@4, gated on dither_active, same as
   `build_scene_fs_cooked` L392-417 so prepass depth still matches the forward); `fe.out[0]={motion_vec, 0}`; lower.
2. **Program variants**: cook velocity.crdv → `program_velocity`, velocity_skinned.crdv → `program_skinned_velocity`
   in `init_programs` (mirror the depth/skinned twins); FS = the velocity FS. Register `crd://scene/velocity`.
3. **Per-group resolution**: the prepass binds `program_(skinned_)velocity` per group (mirror how `program_depth`/
   the skinned depth twin resolve for `material_pass="Shadow"`) — add a `DrawItem::program_velocity` (see L520-521).
4. **Frame graph**: `velocity` transient RG16F (scale 1.0, cleared [0,0]) + depth_prepass → `raster.mrt` writing
   [velocity]+depth in the 4 forward_csm*/gpu* frames. Verify `draw_storage_mrt`/`image_with_depth` on DX12+Vulkan.
5. **Palette snapshot**: compute/copy pass BEFORE gpu_skin copying `palette_off`→`prev_palette_off` per skinned
   group (the previous pose; first frame prev==cur is correct).
6. **Resolve flip (LAST)**: `ensure_taa_program` samples `velocity` (bindless), `prev_uv = R_reproject(uv,depth) +
   velocity.xy`. Keep the neighborhood clamp. Velocity RT populated but unread until here → no regression before it.
1. **`.crdv` cook: `prev_clip` varying — EXACT LOCATIONS TRACED (`engine/vertex-cook/src/vertex_asset.cpp`).**
   In `cook_vertex_program_unchecked` (L2696), the regular VS path builds: skinning blend (LBS **L3137–3180** =
   `Σ wₖ·(Mₖ·p)` reading `pbase = header[palette_off] + slot·joint_count·stride`; DQS **L3181–3230**), then the
   instance transform (**L2856–2859** `m[16]` from `ibase + instance.transform`, `mul_mat4 → wrld`), then clip
   (**L2860–2863** `vp[16]` from `header[view_proj]`, `mul_mat4 → clip`). To emit `prev_clip`, mirror these with
   prev sources — a **new source term `prev:clip`** parsed in `source_term` (L240) + emitted additively (gated on a
   varying declaring it, so every existing `.crdv` is untouched; the scene-render suite is the regression gate).
   Add to `scene_skinned.crdv`: `[[varying]] name="prev_clip" location=4 source=["prev:clip"]`.
   - ⛔ **SCOPE CLIFF (full skinned):** `prev:clip` at full-skinned fidelity means **duplicating the entire ~120-line
     LBS+DQS blend** with `header[kHdrPrevPaletteOff]` + `header[kHdrPrevWorldOff]` — major surgery on the SHARED
     cook. **Cheaper alternative (rigid-for-skinned):** reuse the CURRENT skinned object-space position for both
     clips, varying only the transform (`prev_world`) + keeping `cur_vp` — deformation cancels (velocity=0 for
     limbs, correct rigid+camera for the whole mesh). ~10 lines, no skinning duplication, captures the ring's
     dominant circling motion; limb-deformation velocity is the only thing deferred. Decide before building:
     full-skinned (user's stated choice, ~120 lines duplicated) vs rigid-for-skinned (~10 lines, 90% of benefit).
2. **Velocity FS.** New `build_velocity_fs_cooked` (mirror `build_scene_fs_cooked`'s dither discard EXACTLY so
   prepass depth still matches the forward — the DEPTH-ONLY≠forward scar). Reads `prev_clip` (loc 4) + FragCoord;
   writes `velocity = prev_clip.xy/prev_clip.w*0.5 − (FragCoord.xy*inv_res)` (obj_delta, NDC±Y aware). MRT color 0.
3. **Prepass → `raster.mrt`.** In the 4 forward_csm*/gpu* frames: depth_prepass becomes `raster.mrt` writing
   `["velocity"]` + creating `scene_depth`; add a `velocity` transient RG16F (scale 1.0) cleared to `[0,0]`;
   `shader="crd://scene/velocity"` (VS=scene_skinned+prev_clip, FS=velocity). Verify `draw_storage_mrt` /
   `image_with_depth` path on DX12 + Vulkan (MRT format match — the PSO-format scar).
4. **Skinned prev-palette.** `palette_snapshot` compute/copy pass BEFORE `gpu_skin`: copy `palette_off` →
   `prev_palette_off` (group buffer, per skinned group). Watch the RAW inline-load scar — separate pass, not
   folded into gpu_skin. First frame prev==cur (no motion) is correct.
5. **Resolve rewire.** `ensure_taa_program`: sample `velocity` (bindless), `prev_uv = R_reproject(uv,depth) +
   velocity.xy`. Keep the neighborhood clamp. Flip this LAST, after 1-4 are tested (velocity RT populated but
   unread until here → no regression at any checkpoint).
6. Consolidated `scene_buf` path (non-shadowed frames) `region_words` (scene_renderer.cpp ~4699) must extend to
   `prev_world_off + capacity*16` — defensive; our shadowed frames use per-group buffers so it's off the critical
   path, but a non-shadowed frame would truncate the region without it.

Integration map: VS asset `assets/vertex/scene_skinned.crdv`; gpu_skin kernel ~L1900-2050 (C++ KGraph, writes
palette); header[25]=skin_off, [26]=palette_off, [27]=joint_count; MRT verbs in frame_runtime.cpp (`draw_storage_mrt`,
`image_with_depth`, `RasterMrt`); dither FS `build_scene_fs_cooked` ~L315.

## Stage 3 — SHIPPED (2026-08-02)

The LOD cross-dissolve dither was a fixed 4×4 Bayer threshold — spatially uniform but TEMPORALLY FROZEN, so TAA
saw one static checkerboard and the LOD pop merely CRAWLED across it. Replaced it with a TEMPORAL-STOCHASTIC
dither: interleaved-gradient noise (Jimenez) whose sample point is shifted every frame by an R2 (Roberts)
low-discrepancy offset, seeded from a new monotonic `kHdrFrameIndex` header word (uploaded per frame, masked to
1024 so `float(frame)` stays exact). Now each frame paints a different half of the two levels and TAA AVERAGES
them into a seamless cross-dissolve (the UE5 "dithered LOD + TAA" result). The forward FS and the dither depth
prepass share the identical noise (same discard set → the prepass depth still matches the forward pixels).
Verified on DX12 (1M) and Vulkan; full scene-render suite 53/53. **The remaining Stage-2 polish — per-object
velocity for the FEW animated meshes (a velocity buffer + a `prev_world` on the instance record) — is a distinct
addition; the static 1M majority already reprojects exactly from the camera matrix.**

## Stage 3 (original plan) — Temporal-stochastic LOD dither

Swap the fixed spatial Bayer threshold for interleaved-gradient-noise + per-frame offset, so TAA
averages the two LODs into a smooth crossfade (the UE5 result). Impostor smooth-coverage likewise
becomes a stochastic alpha TAA resolves. **DoD:** LOD/impostor transitions are invisible in motion.

## Stage 4 — Nanite cluster-LOD integration

The REN-40-I cluster-DAG (cook + selection + BVH + unpack) is built; this wires it into the renderer.
- **CKIR mesh-shader graph** consuming the packed clusters (10 u32/cluster, 8 u32/BVH node). CKIR
  already emits mesh+task shaders (`GL_EXT_mesh_shader` / `DispatchMesh`).
- **GPU per-cluster selection** — a compute pass running `select_clusters_bvh` on device (traverse the
  BVH, take the cut where `parent_error > τ ≥ error`), writing the selected cluster list + count.
- **Mesh-shader draw** — task shader reads the selected list, mesh shader unpacks each cluster's ≤128
  tris and emits them.
- **Scene renderer** — packed-cluster buffer layout; a `MeshRenderer` route flag (cluster path vs
  discrete-LOD path).
- **Frame graph** — `cluster_select` (compute) → `mesh_draw` (raster.mesh) passes, authored.
- Seamless continuous LOD by construction — no dither for cluster meshes. Virtualized STREAMING is out
  of scope (in-memory clusters only, as REN-40-I stated).

**DoD:** a high-poly mesh routed through the cluster path shows continuous, pop-free LOD from any
distance; triangle count scales with screen size; both backends; TAA resolves residual sub-pixel AA.

---

## Progress

- **Stage 1 — SHIPPED** (LOD ladder retune + prefiltered mipped impostors). 1M ~23 fps, 0 errors.
- **Stage 2 — TAA WORKING ON VULKAN** (2026-08-02). Depth-reprojection TAA: one reproject matrix
  `R = prev_vp·inv(cur_vp)` (the w-factor cancels), 3×3 neighborhood variance clamp, feedback blend.
  Delivered the matrix to the fullscreen resolve via a NEW raster verb `draw_bindless_storage` (binds
  the bindless texture heap + a constants buffer at the already-fragment-visible binding 0 — the engine
  had NO fullscreen-uniform path before). History is `persistent_image` + a copy pass (a ping-pong can't
  be read-after-write in one frame). Halton(2,3) camera jitter, subpixel, gated to the TAA frame.
  **Result:** the far-field aliased carpet is gone — clean converged image at 1M, ~free (resolve
  0.006 ms, 1M still ~22.5 fps), 0 validation errors on Vulkan.
  **Stage 2 update (2026-08-02, later):** the subtle L-R shimmer was root-caused — the reproject matrix
  used the JITTERED previous view-proj, so motion vectors carried the per-frame jitter delta and sampled
  history a fraction of a pixel off every frame. Fixed: `R = prev_UNJITTERED_vp · inv(cur_JITTERED_vp)`,
  computed in the sandbox (which owns both projections; un-jittering a composed vp needs the separate
  view). The blur: a quick unsharp fringed (mixed history-blend against current-frame mean), so it was
  removed — the image is clean and slightly soft; Catmull-Rom history sampling is the proper sharpness
  polish. 1M ~22 fps, 0 errors, far field clean. Also learned: a standalone fullscreen program must NOT
  buffer_decl binding 0 (a reachable storage_load auto-declares `sbuf`), must lower_entry before
  create_program, and the KGraph builder types a binary op from its FIRST operand (put the vec before the
  scalar in `vec*scalar`).
  **Stage 2 update (2026-08-02, DX12 + DEFAULT):** (a) Catmull-Rom history sampling landed (Karis 5-tap,
  sharper than bilinear) and the overlay was moved AFTER the resolve so the grid stays crisp and out of the
  history. (b) DX12 mirror of `draw_bindless_storage` landed — `record_bindless_storage` binds the constants
  UAV at root param 0 (via `frame_alloc_storage_slot`), the sampler at param 2, and the bindless SRV run at
  param 3 (`frame_alloc_bindless_run`); the two allocators share the frame-heap cursor so their slots never
  collide. DX12 `--gpu-cull` now renders the resolved image (was black), 0 validation errors, matching Vulkan.
  (c) **TAA IS NOW THE DEFAULT.** `forward_csm_agx` + `forward_csm_srgb` gained a depth prepass → sampled
  `scene_depth` + the three TAA resources + resolve/store passes, so the flagship CPU-path forward frames
  carry temporal AA out of the box; a new `forward_csm_gpu_srgb` twin gives the device-cull path an sRGB
  arm. The sandbox jitters the whole default path (not just `--gpu-cull`) and the tonemap radio now switches
  between DISTINCT srgb/agx frames in every mode. This also FIXED the "sRGB checkbox does nothing" bug —
  under `--gpu-cull` the radio had mapped both arms to the single gpu frame. Validated on Vulkan AND DX12,
  CPU-path and device-cull, both tonemaps: 0 validation errors everywhere; TAA cost at 1M is resolve 0.139 ms
  + store 0.006 ms (essentially free); AgX vs sRGB mean-luma 511 vs 291 confirms the display transform genuinely
  switches. **Remaining for Stage 2:** per-object velocity for the animated meshes (camera-motion reprojection
  is exact; only the bobbing grid + moving ring lack a velocity vector, so they fall back to neighbourhood
  clamp — acceptable but not gold for fast object motion).
  **Disk-only asset migration COMPLETED (2026-08-02):** the prior disk-only refactor (−1345/+562 in
  `scene_renderer.cpp`) had removed the embedded `builtin_asset_text` pack but left the header declaration, the
  `test_scene_render.cpp` DRIFT GATE, and ~40 GPU-test `init_programs` sites referencing it — so the whole
  `crd-scene-render-tests` binary would not LINK. Finished the migration (user-directed "everything is a disk
  asset; ours ship as defaults, apps author their own"): (a) removed the `builtin_asset_text` declaration; (b)
  `SceneRenderer::init` now honours the `CRD_ASSETS_DIR` convention to LOCATE the shipped default assets when the
  host has not pinned a root (an app authoring its own pipelines calls `set_asset_root`; that wins) — this fixes
  every test that inits programs without ceremony; (c) rewrote the drift gate as a disk-only COOK gate (every
  shipped default parses + every frame validates — 83 assertions, PASS, now covers the two new gpu frames too);
  (d) redesigned the F15 "an edited disk asset is live" gate around a COMPLETE temp-root mirror (`copy_tree`) since
  there is no embedded pack to shadow (26 assertions, PASS); (e) the 12 GPU-test asset reads now come from disk
  (`read_shipped_asset` / the renderer's root). Suite LINKS and RUNS: 50/53 GPU cases pass.
  **⚠ 3 pre-existing prior-WIP REN-40 failures SURFACED (not caused) by making the suite runnable** — the
  failing assertions are byte-identical to HEAD (they passed at HEAD; behaviour shifted under the prior WIP's
  `scene_renderer.cpp` rewrite, none of which my TAA/migration/env work touches): (1) `GEO-7` `drawn_instances >
  5000` → 2330, because the prior WIP added a `min_draw_px = 3.0` SUB-PIXEL CULL (`draw_px_ok`) to the default
  cull path — the 100×100 grid from distance now culls its far <3px cubes (the frontier "no aliased pixels" goal),
  so the >5000 threshold is stale; (2)/(3) `REN-40-D` soft-shadow metrics `soft_step < hard_step` (59<59) and
  `evsm.partial > hard.partial+1` (2>2), borderline metrics from the prior WIP's PCSS/moment changes. NEEDS a
  REN-40 decision: expected behaviour (→ update the stale thresholds) vs regression (→ fix the cull/shadow code).

  **Stage 2 update (2026-08-02, DX12 brought fully online):** three DX12-specific defects, all pre-existing and
  exposed only once DX12 actually ran the scene path (the sandbox flag is `--backend dx12`, not `--dx12`, so DX12
  scene rendering had never executed):
  (1) **DX12 `draw_bindless_storage` mirror** — `record_bindless_storage` binds the TAA constants UAV at root
  param 0, sampler at 2, bindless SRV run at 3; DX12 `--gpu-cull` renders the resolve (was black).
  (2) **Meshes upside-down** — the `flip_clip_y` / `ndc_y_points_down()` convention was applied only to the shadow
  FS, never to the FULLSCREEN RESAMPLE, so on DX12's y-up NDC every RTT resample of the scene came out mirrored
  while the direct-drawn overlay stayed upright. Fix: the taa_resolve VS (the resample that reads `scene_hdr`)
  negates its clip-Y on a y-up backend, making the resample an identity on both backends. Verified upright + fully
  populated at 1M; Vulkan untouched.
  (3) **`--lod` = "background color"** — the DXIL varying-packing wall. The flat forward FS reads a NON-CONTIGUOUS
  subset of the VS outputs (normal/worldpos + the LOD-dither fade, skipping uv); DXIL packs the PS input signature
  by register, so the subset desynced from the VS and `CreateGraphicsPipelineState` failed → whole scene renderer
  fell back to overlay-only. **General fix (varying contract):** the scene VS's full interpolant layout is captured
  once (`scene_varyings`), and `cook_fs` injects an unreachable StageIn for every varying a scene fragment does not
  read, so the DXIL PS input signature packs IDENTICALLY to the VS output. The HLSL emitter emits every fragment
  StageIn (reachable or not, one per location); GLSL/Vulkan and the reach-based varying requirements ignore the
  dead nodes, so Vulkan is unchanged. This makes ANY VS/FS varying pairing portable to DX12 by construction —
  future scene shaders inherit it for free. Verified: DX12 `--gpu-cull --lod` and CPU-path `--lod` render correctly
  at 1M (upright, LOD/impostors, TAA, shadows, 0 errors); Vulkan matches; scene-render suite 50/53 (only the 3
  pre-existing REN-40 `min_draw_px`/soft-shadow threshold failures, no new).
  **Remaining for Stage 2:** `taa_history` resize-recreate (fixed-size persistent image needs a recreate path so
  TAA stays correct across window resizes); per-object velocity for the animated meshes.

  **Stage 2 update (2026-08-02, TAA reprojection + gate sweep):**
  - **TAA reprojection on DX12** — the taa_resolve clip-Y flip (orientation fix) made the resample an identity but
    left the FS reconstructing `ndc.y = 2v−1` (y-down convention) while a texel's TRUE ndc.y on a y-up backend is
    `1−2v`. History reprojected MIRRORED in Y → samples slid against the current frame on any pan/resize ("textures
    piling on top of each other"). Fix: bake the backend clip-Y sign into BOTH the `uv→ndc.y` reconstruction and
    the `ndc.y→prev_uv.v` read (`sgn = ndc_y_points_down ? +1 : −1`). Vulkan unchanged; DX12 history now aligns.
  - **The 3 long-standing REN-40 gate failures — all fixed at the ROOT, no loosened thresholds:** (1) `GEO-7` —
    the `min_draw_px` cull is real, so the frustum-count assertion turns it OFF (LOD-off, nothing catches the small
    ones); (2) cascade cross-fade — the cross-fade works, the metric measured the shadow's own edge on touched
    ROWS, now measures the step at the changed PIXELS (the seam) and correctly shows soft<hard; (3) EVSM — isolated
    by experiment: the moment frame's `depth_bias_slope` narrows EVSM's EXPONENTIAL moments (MSM's polynomial ones
    barely move). Moment shadows are filterable → acne-resistant → don't need a slope bias; removed it, EVSM's 3px
    penumbra restored, floor stays acne-free. Full scene-render suite: **53/53, 1207 assertions, 0 failures.**
  - **`taa_history` resize (gold-standard close):** persistent images demand an absolute size by rule (a
    scale-relative extent would silently discard history on resize). Added an explicit `resizable = true` opt-in
    (frame_asset struct + parser + emitter + cook validation + binary serialization): a `resizable` persistent may
    be sized by `scale` alone, the runtime sizes it from the output every build, and the device's
    `create_persistent_image` already destroys+recreates on a desc-size change — so `taa_history` now FOLLOWS THE
    WINDOW. The sandbox resets `taa_has_prev` on resize so the one blank frame is never blended (no flash). The
    four TAA frames' history is now `scale = 1.0, resizable = true`. Verified rendering correct + TAA clean at BOTH
    1280×720 and 1920×1080 on DX12, 0 errors; the taa_store copy now matches sizes (both follow the window).
  **Stage 2 is COMPLETE on both backends** — TAA (jitter + depth reproject + neighbourhood clamp + Catmull-Rom
  history), correct orientation, correct reprojection under motion, window-following history, LOD on DX12, and the
  full REN-40 gate suite green (53/53). Remaining polish: per-object velocity for the animated meshes (camera
  reprojection is exact; the bobbing grid + moving ring fall back to neighbourhood clamp).

## Checkpoints

Each stage ends with: screenshots (10k + 1M, both backends), 0 validation errors incl. a GPU-assisted
soak, a bench board in `docs/bench/`, and a proposed commit (the user commits). Stages land in order;
the campaign spans multiple sessions.
