# 2026-08-03 — REN-41 Visual Frontier: velocity CORRECTNESS gate + 1M fps board (the two "also open" items)

> Live state: `context.md` (READ THIS FIRST block). Continuation dossier: `docs/research/2026-08-02-visual-frontier-plan.md`.
> REN-band row: D-007 REN-41. Prior session (runtime integration): `docs/sessions/2026-08-03-ren41-velocity-runtime-integration.md`.

Per-object velocity motion vectors WRITE + are CONSUMED end-to-end (prior session). This session closed the two small
items that stood between velocity and REN-41's remaining Stage 4: a rigorous **correctness gate** that reads the
velocity buffer's VALUES, and the **1M-with-velocity fps board**. Both done, both backends. Stage 4 (Nanite
cluster-LOD renderer integration) is now the sole remaining REN-41 item, opened next as a fresh session.

## (1) Velocity correctness gate — a decoded motion-vector debug view

The dossier asked for "read the velocity buffer: static ≈ 0, mover ≈ expected screen delta." The engine had no
float-readback of a named transient (readback is RGBA8 on the final target only), so the gate reads velocity through
a **shipped motion-vector DEBUG VIEW**, which is also a genuinely useful engine feature:

- **`crd://scene/velocity_debug`** (`ensure_velocity_debug_program`) — a fullscreen pass that samples `velocity`
  and encodes `out.rg = velocity.xy · kVelocityDebugScale + 0.5`, `out.b = 0.5` into RGBA8 (0 motion → mid-grey;
  either sign visible/decodable). Shares the clip-Y flip with the TAA resolve so the resample is an identity on both
  backends. `kVelocityDebugScale` is a public constant in `scene_renderer.hpp` — one home for the shader and the gate.
- **`assets/frame/velocity_debug.frame.toml`** — the SHIPPING MRT velocity prepass (same `crd://scene/velocity`
  program, same dither) → the encode pass. `velocity` is cleared to a **SENTINEL (0.25, 0.25)**, not zero, so a DRAWN
  static instance (writes velocity 0 → encodes 0.5) is distinguishable from BACKGROUND (never drawn → sentinel → 0.75).
  Added to the disk cook gate (`check_frame`).
- **The gate** (`test_scene_render_gpu.cpp`, Vulkan + DX12): two unit cubes of one mesh (static left, mover right).
  Frame 1 seeds both instances' `prev_world` (a fresh slot self-shadows → the gate verifies **zero velocity on
  spawn**). Then the mover's Transform is pushed +3 world-units in Y via `get_component_mut` (bumps the chunk
  Transform version → the incremental extract snapshots `prev_world = A`, sets `world = B`), and frame 2 carries:
  BACKGROUND = sentinel, STATIC = (0.5, 0.5), MOVER = (0.5, enc(exp_v)). The move is pure-Y and this camera's view
  row0 has no Y term, so `u_delta = 0` for every mover fragment — "drawn" is classified by R ≈ 0.5 and the signed
  motion lives entirely in G. `exp_v` is computed from the SAME projection the velocity FS uses. Asserts: mover
  u ≈ 0, v ≈ exp_v (within 0.06 UV), |v| > 0.10 (present + distinct from static), static ≈ 0, background = sentinel.

### ⛔ Scar this cost (memory entry): a 1-read fullscreen pass binds via the SINGLE-TEXTURE path, not the bindless heap
First cut sampled the bindless heap at set 0 / binding 16 (copying the TAA resolve, which is a MULTI-read fullscreen
pass using `draw_bindless_storage`). The output came out fully black. The frame runtime's `RasterFullscreen` executor
routes a pass with **exactly one read** through the single-texture path (`draw_textured`), which binds the texture at
**set 0 / binding 1** and the sampler at binding 2 — the same bindings `body_hzb_build` (the other 1-read fullscreen)
uses. `n_sampled > 1` is the branch that binds the bindless heap. Fixed by sampling binding 1 with `tex_sample`
(not `tex_sample_at` at index). Memory: `feedback_one_read_fullscreen_binds_single_texture_not_bindless`.

## (2) 1M-with-velocity fps board — `docs/bench/2026-08-03-ren41-velocity-1m-fps.md`

Median-of-5, win-release, RTX 4070 Ti SUPER, `--lod --gpu-cull --smoke-test 4.0 --present immediate`:

| Instances | VK fps | DX12 fps | 40-J baseline (both, pre-TAA/velocity) |
|----------:|-------:|---------:|---------------------------------------:|
| 100,000   |  65.6  |   19.8   |                                   90.7 |
| 1,000,000 |  20.8  |    7.5   |                                   12.4 |

- **Velocity is cheap:** `palette_snapshot` 0.03 ms + an RG16F write folded into the `depth_prepass` (which REPLACED
  the depth-only prepass TAA already required) + a ~free `taa_resolve` velocity tap. The frame is geometry-dominated.
- **VK 1M improved (12.4 → 20.8)** — Stage 1's LOD/impostor retune (landed after 40-J) cuts far-field triangles
  enough to outweigh the added TAA + velocity passes. **100k slower (90.7 → 65.6)** — TAA's fixed per-frame cost
  dominates when the geometry is light; the price of "no aliased pixels" by default. Recorded, not chased.
- **⛔ OPEN, flagged not buried:** DX12 ~3× behind VK. GPU work is comparable (32.9 vs 26.0 ms); the gap is CPU-side —
  the pre-existing 40-B DX12 `upload_storage` (~36 ms/frame at 1M vs VK's ~0.2) + a ~1 s first-frame 1M build skewing
  the 4 s smoke window (steady DX12 is ~34 ms CPU / ~33 ms GPU ≈ 29 fps, not 7.5). Velocity's `prev_world` adds only
  ~77 KB to the dirty upload — negligible. Filed as a separate DX12-perf item (upload batching), not REN-41.

## (3) DX12 upload-batching — the gap the board exposed, FIXED on the spot (not filed)

The board showed DX12 ~3x behind Vulkan. Decomposed: comparable GPU (32.9 vs 26.0 ms); the gap was CPU-side
`upload_storage`. DX12's synchronous path did, PER CALL, `CreateCommittedResource` + `m_cmd_alloc->Reset()` +
**`submit_and_wait()`** — a full CPU<->GPU flush. At 1M the renderer issues dozens of uploads per frame, so the
frame paid dozens of serialized GPU round-trips: **~36 ms/frame**. Vulkan was ~0.1 ms because it has an upload
BATCH (38-G1) the renderer already brackets sync with; **DX12 never implemented `begin/end_upload_batch` (no-ops)**.

Implemented the DX12 batch: a double-buffered `UploadBatch` (own allocator+list + a persistent MAPPED ring),
`begin/end_upload_batch` + `upload_batched` (ring memcpy + one recorded `CopyBufferRegion`, bracketed by the
UAV<->COPY_DEST pair), submitted ONCE with no wait; `drain_upload_batches` at teardown + a defensive flush at
`frame_rec_begin`. A single upload larger than the base ring bypasses to the synchronous path, so the ring stays
8 MB x 2 and the one-time first-frame 1M bulk upload does not balloon it. **Steady-state DX12 `upload`: ~36 ms ->
0.11 ms (~300x);** 1M is now GPU-bound like Vulkan, DX12 CPU (6.4 ms at 100k) dropped below Vulkan's. Board:
`docs/bench/2026-08-03-ren41-velocity-1m-fps.md`. Scar: `feedback_dx12_upload_needs_batch_not_per_call_submit_wait`.

## (4) Frame-graph cycle detector - a pre-existing regression the DX12 verify surfaced, ALSO fixed

Running the DX12 gates after (3) turned up **REN-1 gates red on BOTH backends** (231/372 "build() REJECTS a cycle",
230/371 "producer-declared-last runs first"). Root-caused: a prior uncommitted change had replaced the gpu-context
frame graph's edge rule ("a reader ALWAYS follows the writer") with a plain declaration-order WAR heuristic
("reader-before-writer if declared first"), to support the TAA history ping-pong -- but it **masked genuine cycles**
(the exact class the prior session fixed in frame-**cook** and left unfixed in gpu-context, on both backends).
Ported the frame-cook lifetime-aware rule to both `vulkan_raster_context.cpp` and `dx12_raster_context.cpp`: a read
matching a LATER writer is a legitimate WAR (edge reader->writer) ONLY when the resource HAS A VALUE there -- a
PERSISTENT image (TAA history), an EXTERNAL buffer, or an earlier writer this frame (two-phase occlusion re-cull);
otherwise the later writer is the only producer and must PRECEDE the reader (a forward RAW), which surfaces the
cycle. All 6 REN-1 gates green both backends; scene-render 55/55; the full 22-pass frame (TAA ping-pong + occlusion)
still builds+renders on both. Scar: `feedback_frame_graph_war_needs_resource_lifetime_gpu_context_twin`.

## Verification
- scene-render **55/55** (was 53; the 2 new velocity cases), 1264 assertions, full suite green.
- Velocity gate green on **Vulkan AND DX12**; all 6 **REN-1 frame-graph gates** green both backends.
- 41/41 DX12 GPU gates green (incl. bit-identical CPU-vs-GPU skinning -- proves the batch uploads are byte-correct).
- Sandbox both backends: the full 22-pass frame builds + renders, 0 validation errors, no device loss.
- Tidy: all touched files clean (`scene_renderer.cpp/.hpp`, both test files, `dx12_raster_context.cpp`,
  `vulkan_raster_context.cpp`) -- incl. cleaning 3 pre-existing issues the touched gpu-test surfaced.
- `crd-no-non-ascii-test-names` guard green.

## Not done / next
- **Stage 4 — Nanite cluster-LOD renderer integration** (the REN-41 remainder): CKIR mesh+task shader graph over
  the packed clusters, GPU `cluster_select` compute pass, `raster.mesh` draw path, `MeshRenderer` cluster route flag,
  authored frame passes. The 40-I algorithm/data pipeline is CLOSED; this is the renderer layer. A large new render
  path — begin with a plan + `advisor`. Opened as a fresh session.

## Proposed commit (user commits; NO AI-coauthor trailer)
`feat(scene-render,gpu-context): REN-41 velocity gate + motion-vector debug view + 1M board; DX12 upload batching; frame-graph cycle-detector fix` — see context.md handoff for the body.
