# 2026-08-03 — REN-41 Visual Frontier: Stage 3 dither + per-object velocity (cook + assets)

> Live state: `context.md` (READ THIS FIRST block). Turnkey continuation with file:line: the dossier
> `docs/research/2026-08-02-visual-frontier-plan.md`. REN-band row: D-007 REN-41.

## What shipped (all both backends, scene-render suite green throughout)

**Stage 3 — temporal-stochastic LOD dither.** The cross-dissolve dither was a fixed 4×4 Bayer — spatially uniform
but TEMPORALLY FROZEN, so TAA saw one static checkerboard and the LOD pop merely CRAWLED across it. Replaced with
interleaved-gradient noise (Jimenez) shifted every frame by an R2 (Roberts) low-discrepancy offset, seeded from a
new monotonic `kHdrFrameIndex` header word (masked to 1024 so `float(frame)` stays exact). Now each frame paints a
different half of the two levels and TAA averages them into a seamless dissolve. Forward FS + dither depth-prepass
share the identical noise → prepass depth still matches the forward pixels.

**DX12 brought fully online** (from prior session, verified): varying-contract fix (fragment emits ALL StageIns so
the DXIL PS input signature packs identically to the VS output), NDC±Y orientation + TAA reprojection sign baked
per backend, `taa_history` `resizable` → follows the window.

**Per-object velocity (motion vectors) — the COOK + ASSET half, DONE + VERIFIED.** The two hardest, riskiest
pieces of the whole feature:
- **Cook `prev:clip` + `clip` source terms** (`engine/vertex-cook/src/vertex_asset.cpp`): `prev:clip` re-skins the
  object position with the PREVIOUS bone palette (full LBS blend against `prev_palette_off`), transforms by the
  PREVIOUS per-instance matrix (`prev_world_off`, per-instance ×16 section), and projects by the CURRENT
  `view_proj` — so the TAA jitter (baked into view_proj) cancels against the current clip and a static instance
  yields a zero motion vector. `clip` = the current clip interpolant (so the velocity FS needs no render
  resolution). Additive + inert: emitted only when a varying declares them (`VaryingSourceKind::{PrevClip,Clip}`
  + parse + round-trip emit + width + validation skip). `.crdv` header gains `prev_world_off`/`prev_palette_off`
  words (appended at struct end + POD-walk name table grown 13→15).
- **`assets/vertex/velocity.crdv` + `velocity_skinned.crdv`** — matched motion-vector VS programs, CLIP-IDENTICAL
  to scene/scene_skinned (only `position` declared; the clip depends on nothing else → same depth, shares the
  prepass), emitting `prev_clip`@loc5 + `cur_clip`@loc6.
- **Foundation** (`scene_renderer`): `MeshGroup::prev_world` snapshotted in `write_slot` BEFORE overwrite (fresh
  slot shadows itself → zero velocity on spawn); `prev_world` (all groups) + `prev_palette` (skinned) GPU sections
  threaded through the allocation chain + per-run incremental upload; header words 107/108 published per group.
- **The REN-41 gate COOKS both velocity assets** (not just parses) — the real `prev:clip` graph builds for
  non-skinned AND skinned-LBS. Suite **53/53, 1211 assertions**; 1M renders full pipeline on DX12 + Vulkan.

## Scars captured
- ⛔ **A per-instance CPU shadow OOM-regresses at 1M though it compiles + the small case renders.** velocity's
  `prev_world` is 64 B/instance = 64 MB at 1M; the sandbox arena (`192 MB + 512 B/inst`) was already near-full and
  the addition asserted `TlsfAllocator: out of memory` — while `--lod-showcase` (small) rendered fine. Fix: arena
  → 768 B/inst. Lesson: a "safe dead-code foundation" is NOT safe until proven at the TARGET scale (memory
  regressions are invisible to the compiler + any small smoke). Memory:
  `feedback_velocity_prev_transform_64b_per_instance_oom_at_1m`.
- ⛔ **Build toolchain moved to VS 18** — the old `...\2022\Community\...` vcvarsall path silently fails (exit 1,
  ZERO output). Memory: `reference_build_toolchain_vs18_vcvarsall_path`.

## Banked to next session (the runtime integration) — gated on a NEW backend verb
The 1M scene draws GPU-cull **indexed-indirect** (counts in device memory; depth-prepass uses
`draw_storage_multi_indexed_depth_only_indirect`). The frame-cook executor's only MRT path
(`frame_runtime.cpp` `RasterMrt`) is CPU-driven (`draw_storage_mrt(..., vertex_count)`) — it CANNOT draw
GPU-written commands. So writing velocity+depth from the culled draws requires authoring a NEW raster-context verb
`draw_storage_multi_indexed_mrt_indirect` on **DX12 + Vulkan** (mirror the depth-only-indirect verb + N color
attachments). THEN: DrawItem velocity role → `program_(skinned_)velocity` variants (init_programs, mirror the
depth twins at scene_renderer.cpp ~L5188) → velocity FS (`build_velocity_fs_cooked`, motion = ndc2uv(prev) −
ndc2uv(cur), replicate the IGN dither discard so prepass depth matches) → frame-graph velocity RG16F RT + prepass
`raster.mrt` → `palette_snapshot` compute pass (palette→prev_palette before gpu_skin) → resolve flip LAST
(`prev_uv = R_reproject + velocity`). All steps with file:line in the dossier's REMAINING section.

**Nothing is half-wired** — the tree is green at the cook/asset boundary (all the above is additive/inert until
the runtime reads the velocity RT).

## Proposed commit (user commits; NO AI-coauthor trailer)
`feat(vertex-cook,scene-render): REN-41 velocity cook + matched VS assets + Stage 3 dither` — see context.md
handoff for the body.
