# 2026-08-03 — REN-41 Visual Frontier: per-object velocity RUNTIME INTEGRATION (steps 0–6)

> Live state: `context.md` (READ THIS FIRST block). Continuation dossier: `docs/research/2026-08-02-visual-frontier-plan.md`.
> REN-band row: D-007 REN-41. Prior half (cook + assets): `docs/sessions/2026-08-03-ren41-velocity-cook-and-assets.md`.

The velocity (motion-vector) COOK + ASSETS had shipped the prior session; this session wired the RUNTIME
INTEGRATION end-to-end — the dossier's steps 0→6 — so the depth prepass WRITES motion vectors (MRT) and the TAA
resolve CONSUMES them, on **both backends** and **all four TAA frames**, plus the two follow-on gaps the user
asked to close in the same pass. Everything renders and is verified.

## What shipped (8 source files + 4 frame assets, all both backends)

**Step 0 — the gating GPU verbs.** The 1M scene draws GPU-cull *indexed-indirect* and the executor's only MRT path
was CPU-driven, so a new verb was needed. Authored **`draw_storage_multi_indexed_mrt_indirect`** on Vulkan
(`vkCmdDrawIndexedIndirectCount` + dynamic-rendering N colour attachments) AND DX12 (`ExecuteIndirect` + N RTVs) —
the fusion of `draw_storage_mrt`'s attachment shape with the depth-only-indirect command path. **Discovered mid-step
that the non-`gpu` (CPU-cull) frames draw indexed items with `args == null`**, which neither the indirect verb nor
the non-indexed `draw_storage_mrt` covers → added a second verb **`draw_storage_indexed_mrt`** (CPU-driven single
indexed draw into N colour + depth, `first_draw_index` pushed) so velocity works on all four frames. Executor
`RasterMrt` routes: GPU-driven → indirect verb, CPU indexed → indexed verb, else `draw_storage_mrt`. A depth-format
write on an MRT pass now routes to the depth attachment (kept a graph WRITE/producer) via a record-building change.

**Step 1 — velocity FS** (`build_velocity_fs_cooked`): reads `prev_clip`@5 + `cur_clip`@6, writes the object-motion
UV delta `(prev.xy/prev.w − cur.xy/cur.w)·0.5` with the backend NDC-Y sign (`flip_clip_y ? −0.5 : +0.5`, matching
the resolve's `ndc_y_points_down ? +1 : −1`), and carries the SAME IGN dither discard as the forward FS (so prepass
depth survives exactly the pixels the forward keeps — the DEPTH-ONLY≠forward scar in MRT form).

**Step 2 — programs.** `cook_velocity_fs` + `program_velocity` / `program_skinned_velocity` cooked `indexed=true`
with the same LOD/dither prefix the scene VS carries, so the cook auto-emits `fade`@4 and the FS reads the exact
same fade. Registered `crd://scene/velocity`.

**Step 3 — DrawItem.** `SceneDraw::program_velocity` + `DrawItem::program_velocity` resolved per group (rigid vs
skinned twin, mirroring `program_depth`) and threaded to the executor.

**Step 4 — frame graph.** `velocity` RG16F transient + `depth_prepass` → `raster.mrt` (`writes=["velocity",
scene_depth]`, `clear_color=[0,0,0,0]`) in all four `forward_csm_{agx,srgb,gpu,gpu_srgb}` frames.

**Step 5 — previous bone palette (both paths).** CPU-skin path: `MeshGroup::prev_palette` CPU snapshot — upload last
frame's palette to `prev_palette_off` before the current one overwrites `palette_off`. GPU-skin path: a device
`palette_snapshot` compute kernel (copies palette→prev_palette per skinned instance) authored BEFORE `gpu_skin` in
the two GPU frames, gated by a new **`kHdrGpuSkinActive`** header word so it no-ops under CPU skinning (an
unconditional device copy would clobber the CPU snapshot with the current pose → zero motion).

**Step 6 — resolve flip (LAST).** `ensure_taa_program` samples `velocity` (bindless idx 3, array count 3→4) and
`prev_uv = R_reproject(uv,depth) + velocity.xy`. A static instance stores 0 → byte-identical to the camera-only
reproject (the 1M majority unchanged); a mover fetches its real history texel. Added `velocity` to the four frames'
`taa_resolve` reads.

## Bonus root-cause: the cycle-detection regression (frame-cook 1/407, was pre-existing red)

The prior WIP had rewritten the cook's cycle detector to add WAR/WAW edges (needed so the TAA history ping-pong
`taa_resolve`(reads old)→`taa_store`(writes new) is not a false cycle). But the WAR branch — "writer declared after
reader ⇒ reader-before-writer" — was applied to **every** resource, which **masks a genuine mutual-RAW cycle on
transients** (the `DependencyCycle` gate's test: p1 reads b/writes a, p2 reads a/writes b). Fixed at the root: a
read-before-later-write is a legitimate WAR only when the resource **has a value at that point** — it is
host-provided (external), cross-frame (persistent/ping-pong), OR has an earlier writer this frame (the two-phase
occlusion re-cull of `instances`/`cull_args`); otherwise it is a forward-reference RAW that surfaces the cycle.
frame-cook back to **407/407**.

## Scars captured (memory entries)
- ⛔⛔ **A frame-graph WAR heuristic that ignores resource lifetime masks genuine cycles.** Declaration order alone
  cannot tell "reader wants the old value" (valid WAR) from "reader wants a value written later that does not exist
  yet" (a cycle). The disambiguator is whether the resource has a frame-start value or an earlier producer. Memory:
  `feedback_frame_graph_war_needs_resource_lifetime_not_declaration_order`.
- ⛔ **Velocity's previous bone palette needs population on BOTH skinning paths, and the device copy must be gated.**
  Under CPU skinning the renderer CPU-uploads `prev_palette`; under `--gpu-skin` the palette is device-computed so a
  device `palette_snapshot` pass copies it — but that pass must be gated (a header flag) or it clobbers the CPU
  snapshot with the current pose whenever the GPU frame runs without GPU skinning. Memory:
  `feedback_velocity_prev_palette_two_paths_and_device_gate`.

## Verification
- Tidy: all 5 changed `.cpp` clean (`--warnings-as-errors=*`), INCLUDING a cleanup of ~26 pre-existing committed
  tidy issues the touched files surfaced (gpu_skin `T/R/S` naming, multi-declaration splits, barrier branch-clones,
  raw-`new` → `nothrow`+null-check in both raster contexts).
- Tests: frame-cook **407/407**; scene-render **53/53 both backends** (cook gate covers all 4 velocity MRT frames;
  REN-40-G1 depth-prepass gate pixel-identical → the MRT prepass change does not regress depth).
- Sandbox "it draws": CPU-path Vulkan, GPU-cull Vulkan, GPU-cull DX12, `--gpu-cull --gpu-skin` — all render the full
  frontier scene correctly (mean-luma 169–170, ~99% non-black), **0 validation errors**, Vulkan/DX12 byte-consistent.

## Not done / next
- **Velocity CORRECTNESS gate** (render → read the velocity buffer: static≈0, mover≈expected screen delta) — the
  current proof is "renders correctly + no regression + no validation errors", not a pixel-level motion assertion.
  A rigorous gate is worth adding.
- **Bench board** — fps at 1M with velocity on, for the perf record at REN-41 close.
- **Stage 4 — Nanite cluster-LOD integration** (the REN-41 remainder): CKIR mesh+task shader graph over the packed
  clusters, GPU `cluster_select` compute pass, `raster.mesh` draw path, `MeshRenderer` cluster route flag, authored
  frame passes. The 40-I algorithm/data pipeline is CLOSED; this is the renderer layer. **Opened as a fresh session.**

## Proposed commit (user commits; NO AI-coauthor trailer)
`feat(scene-render,gpu-context,frame-cook): REN-41 per-object velocity motion vectors` — see context.md handoff for the body.
