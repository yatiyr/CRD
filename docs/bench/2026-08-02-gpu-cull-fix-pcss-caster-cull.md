# GPU-cull regression fix + PCSS + shadow-caster screen-size cull (median-of-5, both backends)

The `--gpu-cull` path's "regression" (flagged on the 40-H board as a sync/warmup suspicion) was a
**device loss**: the depth prepass recorded its draws with the items' FORWARD programs — the executor
never honored `material_pass = "Shadow"` for a non-`for_each` pass — so a texture-sampling FS ran in a
pass that binds no textures. The driver dead-coded that FS for years (no color attachments), until the
REN-40-C4 dither DISCARD gave it a depth-affecting side effect: the FS then executed, sampled
never-written descriptors (GPU-AV: `VUID-...-08114`, binding 4 = the shadow atlas), and the undefined
behaviour intermittently hung the GPU for seconds → Windows TDR → `VK_ERROR_DEVICE_LOST` → every later
present refused. The wildly-varying run-to-run fps was how far each run got before the device died.

## Defects fixed (all found via GPU-assisted validation + flag/asset/frame-graph bisection)

1. **Depth prepass ran forward programs** (root cause of the device loss). New `DrawItem::program_depth`:
   the renderer cooks a real depth-only camera program (indexed scene VS + PassType::Shadow FS) and the
   depth-only executor arm prefers it. When dither is on, the prepass FS carries the SAME Bayer discard,
   so prepass depth exists exactly where the forward draw keeps pixels (no cross-dissolve holes).
2. **Cull-kernel padding threads read past the buffer end** (GPU-AV: OOB reads at stride 8 = the LOD
   override records, which END a non-skinned/non-impostor group's buffer). The range guard now clamps
   the READ index, not just the verdict.
3. **Cascade shadow VS variants missed the draw-table stamps** (`rebase_table`/`rebase_stride`) in BOTH
   the pull and indexed cooks — every cascade draw of every LOD slot read slot 0's visible list with its
   own slot's count (stale entries past slot 0's valid length).

## New in this slice

- **PCSS on by default** in the sandbox (`--hard-shadows` keeps fixed-radius PCF for the A/B).
- **Shadow-caster screen-size cull** (`set_shadow_caster_min_px`, default 16 px): an instance whose
  CAMERA-projected height is below the threshold never enters a cascade's caster list — UE5's "Min
  Screen Radius For Shadows". Applied identically by the CPU cascade loop and the device cull kernels
  from one number (camera row-norm metric — the SCAR-5 rule — behind-camera casters always pass).
- (Earlier today, same session: shadow distance fade `shadow_fade_pct` = 30, cascade cross-fade
  `blend_pct` default 15, `depth_bias_slope = 1.5` on every csm_cascade pass.)

## Machine / config

Same rig as the 40-H board (RTX 4070 Ti SUPER / i9-14900K / win-release, `--present immediate`).
Harness: `crd-sandbox.exe [--dx12] --lod --gpu-cull --instances N --smoke-test 4.0 --present immediate`
— now with PCSS + caster cull + shadow fade + cascade blend active (the shipping look).

## The board (GPU cull + LOD + PCSS, median of 5)

| Instances | VK fps | DX12 fps | 40-H CPU-cull (PCF) | 40-H GPU-cull | speedup vs CPU-cull |
|----------:|-------:|---------:|--------------------:|--------------:|--------------------:|
| 10,000    |  ~141† |    144.0† |               151.3 |  broken (TDR) | 0.95× (PCSS costs)  |
| 100,000   |   90.7 |     90.7 |                44.4 |  17.1 (dying) | **2.04×**           |
| 1,000,000 |   12.4 |     12.4 |                 3.3 |   3.2 (dying) | **3.8×**            |

† single-run 6-8s soaks, not medians (the 10k configs are present-bound at these rates).

All runs: **0 validation errors** (including a full GPU-assisted-validation soak, exit 0), no device
loss, backends within noise of each other. 1M is now GPU-bound (~65 ms GPU, ~51 ms CPU) — the CPU-bound
279 ms wall from the 40-H board is gone.

## What the numbers say

- The 1M target (≤16.6 ms) is still not met (80 ms frame), but the frame is now GPU-geometry-bound with
  the cull on-device; the next levers are the wave-scalarized compaction (documented in the cull asset)
  and the shadow-map churn (VSM / REN-5).
- PCSS costs ~7% at 10k against fixed-radius PCF (151 → 141) and is invisible at 100k+ where the frame
  is geometry-bound.
- The caster cull pays for itself: CPU-cull 10k went 151 → 202 fps (single run) with casters below
  16 px skipped in all four cascade lists.

## Known residuals (stated, not hidden)

- `--gpu-cull-verify`'s per-view count comparison predates the occlusion re-cull: view 0 compares
  post-occlusion GPU counts against a frustum-only CPU arm, and cascade GPU counts read back as 0
  because the mid-frame occlusion reset consumes them. The comparison needs re-homing, separately.
- The scene-render test binary does not LINK in the current working tree (unresolved
  `builtin_asset_text`) — pre-existing incomplete migration from the previous session's uncommitted
  embedded-pack removal, untouched here. vertex-cook (445/445), kir (52,917/52,917) pass; frame-cook
  405/406 with one pre-existing parser-cycle failure in previously-modified `frame_asset.cpp`.
