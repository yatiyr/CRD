# RAF-8a increment 3 — the `scene.raster` executor records the full live draw vocabulary (2026-08-03)

**Detour:** D-007 (RAF band). **Row:** RAF-8a. **ADR:** 0106. **Prior:** `2026-08-03-raf7-one-submission-close.md`.

## What shipped

The render-graph's `scene.raster` executor was grown from a single hard-coded triangle into a faithful port of the
live `FramePassKind::RasterGeometry` selection (`frame_runtime.cpp:286`) — **the "scar-dense monster"** named in the
RAF-8a row. It now records the FULL vocabulary from a resolved per-pass draw list, proven on both backends in one
submission.

### 1. The per-pass draw-list contract (render-graph, host pre-resolves)
`engine/render-graph/include/crd/rendergraph/frame_graph.hpp`:
- `RenderDrawItem` — one draw: `storage` (vertex-pull buffer) · per-item `program`/`texture` twins · `vertex_count` ·
  `indexed`/`index_count`/`instance_count`/`first_index` · GPU-driven `args`/`args_offset`.
- `DrawList` — `items[]` + a pass sampled read (`pass_texture` + `pass_texture_is_depth`, e.g. the shadow atlas).
- `DrawListTable` — bound by pass `name_hash`; `RecordContext::draws()` hands the pass its list.
- Threaded through `execute()` and `execute_frame()` (trailing `const DrawListTable* = nullptr`, so every existing
  call site is unchanged). **The HOST pre-resolves ECS → `DrawList`; the render-graph sees only gpu-context handles —
  zero `FrameDrawListDesc`/ECS leak.**

### 2. `record_scene_raster` — the exact live selection (`frame_graph.cpp`)
Per item, emitting a canonical `RasterDrawPacket` the encoder lowers:
- **GPU-driven indexed-indirect** (`args` + `index_count`) → `Indirect` + map/atlas + DrawIndex row `i`.
- **indexed-SAMPLED** (`index_count` + a texture/atlas) → `Indexed` + map/atlas + row.
- **combined textured+shadowed** (item map + pass depth atlas) → map@1/2 + atlas@4/5, one draw.
- **shadowed** / **textured** (a pass depth read / a base-colour map).
- **plain run-coalesce** — consecutive items with the same program+storage, no texture, same indexed-ness merge into
  ONE `MultiStoragePull`/`MultiIndexed` (the batching perf contract: one descriptor reset per run, not per draw);
  single plain items take the plain `StoragePull` arm.

Clear-vs-load across draws is handled by the encoder's `m_first` (first packet clears, rest load) — the executor just
emits packets in order.

### 3. Encoder completed to match (`command_encoder.cpp`)
- `Indexed` case now routes to `draw_storage_indexed_sampled_depth` (with the DrawIndex row) when a map/atlas is bound;
  plain indexed keeps `draw_storage_indexed_depth`.
- `Indirect` case now carries map/atlas + the DrawIndex row (velocity binds neither → both null, byte-unchanged).
- Added `GeometrySource::first_index` (the indexed-sampled scene draw's start-in-indices).

### 4. `IRasterTarget::has_depth()` generalized
Was a concrete non-virtual method on each backend's color-depth target; now a base virtual (default false) the
color-depth targets `override`. `record_scene_raster` auto-uses a bundled-depth colour target as its own depth — the
live scene shape, and the reason the multi-item load-vs-clear across draws takes the depth-aware storage verbs (not the
always-clearing plain `draw_storage`).

### 5. `scene.raster` "geometry" slot → OPTIONAL
A draw-list-driven scene pass supplies geometry PER-ITEM, so the single "geometry" slot is now optional (used only by
the legacy single-draw branch).

## Gate
`crd-render-graph-gpu-tests` — new `run_scene_drawlist_gpu`: a `scene.raster` pass driven by a resolved 2-item
`DrawList` (each item its OWN storage buffer, a triangle in a distinct screen third) renders BOTH thirds with a
background centre gap — two SEPARATE draws, the second LOADS not clears. **Vulkan AND DX12 green, `submit_count == 1`.**

## Regression fixed (latent)
Last session renamed the fullscreen.raster read slot `input` → `input0..7` and rewrote `record_fullscreen_raster` to
abort on an unresolved declared read. The device-free record gates (`test_frame_graph.cpp`) still bound `"input"` and a
null texture — they passed only on a STALE binary. Fixed: `input0` + a new minimal `FakeTexture`. Also updated the
render-pass "missing required slot" test (geometry is now optional → it omits `color` instead).

## Verification
- `crd-render-graph-gpu-tests` (raf7): 7/7 both backends. `crd-render-graph-tests` (device-free): 7/7.
  `crd-render-pass-tests` (raf6): 5/5. `crd-gpu-context-encoder-gpu-tests` byte-parity (#23/#24): green.
- LLVM-20 tidy-clean across every changed engine lib + test target (`kPassHash`→`pass_hash`: local constexpr is a
  LocalConstant, lowercase under the gate).
- **Sandbox smoke both backends: PASS** — Vulkan 62fps / DX12 46fps, the real 11-pass / 5376-instance forward_csm frame
  (the gpu-context vtable change for `has_depth` did not disturb the legacy live path).

## Next (RAF-8a, in the D-007 row + ADR-0106)
`FrameGraphDesc→FrameGraphTemplate` **load bridge** (new acyclic frame-cook→render-graph edge) → for_each cascade
expand (0 = `UnresolvedForEach`) → overlay weave → flip `record_pass` case-by-case (byte-exact — each packet is the
SAME verb the RAF-7 encoder==verb gates prove) + `--smoke-test 2` both backends per increment → RAF-8b (scene-render →
pure orchestration).

---

# RAF-8a increment 4 — the FrameGraphDesc → FrameGraphTemplate LOAD BRIDGE (same session)

## What shipped
`crd::framecook::build_frame_graph_template` (`frame-cook/src/frame_template_bridge.cpp` + header) — the ADR-0106 load
bridge, over a NEW acyclic frame-cook→render-graph edge (verified: render-graph has zero frame-cook references). A
cooked frame TOPOLOGY (`FrameGraphDesc`) becomes a render-graph `FrameGraphTemplate` the single live runtime compiles.

- **Resources** → `GraphResource`: kind (image→ColorTarget · buffer→StorageBuffer · accel→AccelStructure) · lifetime
  (Transient · Persistent · History for ping-pong · Persistent for host imports) · a `(shape,format,samples,layers)` /
  `(bytes)` size-class for the aliaser. Any referenced-but-undeclared name (`@output`) is auto-declared external.
- **15/19 pass kinds** → the 9 built-in executors as DATA: RasterGeometry/DepthOnly/**Mrt**/**Visbuffer**→scene.raster ·
  Fullscreen/**Composite**→fullscreen.raster · Compute/ComputeIndirect→compute.dispatch · Clear/Copy/Blit/Resolve→
  transfer.* · RayTrace→raytrace.dispatch · Present. Reads/writes → the right slots (writes→color/color1..3/depth ·
  sampled reads→input0..N as the shadow-atlas SCHEDULING edge · compute buffers→storage0..3 · src/dst · source ·
  output/accel); params folded (clear_color/depth/compare · VRS/conservative · groups · filter).
- **for_each** expanded into N ordinary passes via a `ForEachCountFn` host resolver. 0 → a NAMED `UnresolvedForEach`.
- The 4 amplification/RT-pipeline kinds (Tess/Mesh/MeshIndirect/RayTracePipeline) → a NAMED `UnsupportedPassKind`
  (LOUD, not a silent gap — they get executors with their flip).

## Schema + executor changes (render-pass / render-graph)
- scene.raster: `color` now OPTIONAL (a DEPTH-ONLY cascade/prepass writes only depth); added MRT `color1..3`, sampled
  `input0..3`, and a `load_depth` Bool param. `record_scene_raster` renders depth-only when `color` is absent
  (extent from the depth target; no colour attachment → the encoder's depth-only verbs).
- compute.dispatch: added `storage1..3`.
- Two new `DiagCode`s: `UnsupportedPassKind`, `UnresolvedForEach`.
- **Real bug fixed:** `record_scene_raster` read `load`/`load_depth` via `u32_param`, but the schema declares them
  **Bool** — so a cooked payload's depth-prepass load flag NEVER fired. Added `bool_param`.

## Gate
`crd-frame-cook-tests` (device-free): a forward_csm-shaped frame (4 for_each cascades + depth prepass + shadowed
forward + post + present) bridges → 8 passes → `rg::compile` SCHEDULES it correctly — every cascade (atlas write) before
the forward pass (atlas read), forward→post→present. Plus two negative gates: an unmapped kind and an unresolved
for_each each surface a NAMED diagnostic.

## Verification
raf6 5/5 · raf7 7/7 both backends · RAF-8 bridge 3/3 · LLVM-20 tidy-clean (bridge + all changed libs/tests) · sandbox
smoke PASS both backends (Vulkan 55fps / DX12 39fps, the real 11-pass frame — the live FrameRecorder path is unchanged;
frame-cook now links render-graph, an additive edge).

## Next (RAF-8a)
overlay weave · a depth-only + MRT device gate (`draw_storage_multi_depth_only` encoder arm for depth-only batching) →
flip the live `record_pass` case-by-case + `--smoke-test 2` both backends per increment → RAF-8b (orchestration).

## Bridge files
- `engine/frame-cook/include/crd/framecook/frame_template_bridge.hpp` + `src/frame_template_bridge.cpp` (NEW).
- `engine/frame-cook/CMakeLists.txt` — the acyclic render-graph/render-pass/render-asset-core edge + the new source.
- `engine/render-pass/src/executor_registry.cpp` — scene.raster/compute schema growth.
- `engine/render-asset-core/{include/crd/renderasset/diagnostic.hpp,src/diagnostic.cpp}` — 2 diag codes.
- `engine/render-graph/src/frame_graph.cpp` — `bool_param` + depth-only `record_scene_raster`.
- `tests/frame-cook/test_frame_template_bridge.cpp` (NEW) + `CMakeLists.txt`.
- `tests/render-pass/test_executor_registry.cpp` — missing-required-slot test repointed to `present`.

---

---

# Rendering bug triage (same session, user-directed)

The user reported the sandbox rendering wrong (TAA haze, "DX12/legacy background-only"). Captured deterministic
screenshots (`--screenshot --screenshot-at --fixed-dt --seed`) + distinct-colour stats on BOTH backends. Findings:

- **Default + `--gpu-cull`: correct on both backends and MATCH** (63k–70k distinct colours, mean RGB 173.3 vs 173.4).
  So DX12 is not globally broken.
- **`--pull-draws` (classic non-indexed pull, the A/B baseline): background-only** — only the overlay grid/gizmos.
  This is the "legacy one". **Deletion-bound at RAF-12 → NOT fixed** (user: don't implement soon-deleted code; the
  flip's `scene.raster` executor supersedes it).
- **`--gpu-skin`: background-only → FIXED (both backends, 822 → 70,199/70,299 colours).** Root cause: `--gpu-skin`
  selects the GPU-driven graph (`forward_csm_gpu.toml`, whose forward pass draws INDIRECT from device-written cull
  commands) but only called `set_gpu_skinning(true)`, never `set_gpu_cull(true)` — so the draw fed CPU counts against
  device commands and nothing landed. Fix: any flag selecting the GPU graph enables the device-command draw path
  (`if (want_gpu_cull || want_gpu_skin) set_gpu_cull(true)`). Scar:
  `feedback_gpu_frame_graph_requires_device_command_draw_path`.
- **Forward-shading NaN: FALSE ALARM.** My first TAA attempt (a variance clamp replacing the loose min/max clamp)
  turned Vulkan black, which I read as a latent NaN. A clean NaN detector (`step(x,x)`==0 ⇒ NaN, output RED) over the
  forward colour at 4 poses found **0 NaN pixels** on Vulkan. The black was my own change's untested Vulkan CKIR path
  (`Max(vec4, scalar)` / vec4 `Sqrt`), not a forward defect. **Reverted** the variance clamp per never-ship-worse; the
  forward output is clean and both backends match. The min/max TAA clamp works — the residual softness (contrast
  −21% during camera motion at a fixed pose) is inherent to this ultra-high-frequency scene (fine grid + thousands of
  sub-pixel instances), a tuning question, not a defect.

Net: 1 bug fixed (`--gpu-skin`), 1 correctly deferred as deletion-bound (`--pull-draws`), 1 non-bug cleared honestly
(forward NaN). `scene_renderer.cpp` is back to baseline (both TAA experiments reverted); only the 6-line gpu-skin fix
lands in `sandbox/src/main.cpp`.

---

## Increment 3 files
- `engine/render-graph/include/crd/rendergraph/frame_graph.hpp` — `RenderDrawItem`/`DrawList`/`DrawListTable`; threading.
- `engine/render-graph/src/frame_graph.cpp` — `record_scene_raster` port + `bind_map`/`bind_atlas`/`attach_textures`.
- `engine/gpu-context/src/command_encoder.cpp` — indexed-sampled + indirect map/atlas/row.
- `engine/gpu-context/include/crd/gpu/command_model.hpp` — `GeometrySource::first_index`.
- `engine/gpu-context/include/crd/gpu/raster_context.hpp` — `IRasterTarget::has_depth()` virtual.
- `engine/gpu-context-vulkan/src/vulkan_raster_context.cpp`, `engine/gpu-context-dx12/src/dx12_raster_context.cpp` — `override`.
- `engine/render-pass/src/executor_registry.cpp` — `scene.raster` geometry slot optional.
- `tests/render-graph/test_frame_graph_gpu.cpp` — `run_scene_drawlist_gpu`.
- `tests/render-graph/test_frame_graph.cpp` — `input0` + `FakeTexture` fix.
- `tests/render-pass/test_executor_registry.cpp` — missing-required-slot test updated.
