# 2026-09-11 — CEIR-34 R2: overlay device-verb retirement + the DX12 first_vertex offset fix

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The last CEIR-34 legacy-deletion-ledger residual (0h row **R2**): retire the `draw_overlay` /
`draw_overlay_range` / `record_overlay` **device verb** and fold the overlay compose into the generic command
model — one execution-program architecture, no dedicated overlay verb. Driving R2 to close surfaced a real,
**pre-existing** DX12 defect (a non-indexed draw's `first_vertex` not reaching `SV_VertexID`), which — under the
SOLVE / no-gaps mandate — was root-caused and fixed in the same slice rather than parked.

## Slices

| slice | what | record |
|-------|------|--------|
| R2 verb-retirement | the overlay rides generic `draw_storage` / `draw_storage_depth_load` (blend → PSO/dynamic-state, LOAD, `first_vertex`, `depth_write=false` via `set_pass_state`); `draw_overlay*` / `record_overlay` DELETED both backends | 4 configs + tidy (prior ticks) |
| R2 fv-fix | DX12 non-indexed `first_vertex` → `SV_VertexID` restored via a backend-internal identity index buffer; a permanent both-backend offset-contract gate | 4 configs + tidy (this session) |

## Mechanism — the verb retirement (one execution-program architecture)

The overlay was already a PASS-level `RasterDrawPacket` (`overlay_pass.cpp` `submit_overlay`: StoragePull +
`LoadOp::Load` + `BlendMode::Alpha` + read-only depth). The residual was a shape-recognition arm in
`command_lowering.hpp` that routed the single-Alpha-color packet to a backend-private `draw_overlay*`. R2 removed
that arm: the overlay now flows through the generic StoragePull verbs, which gained three defaulted parameters so
scene callers stay bit-identical:

- **blend** — Vulkan sets `set_color_blend_enable`/`set_color_blend_equation` AFTER `set_draw_state` (the A15
  rule: blend paths overwrite the blend-off default); DX12 folds `BlendMode` into the PSO key via `pass_pso(…,
  &blend)`.
- **load** — `draw_storage` gained `LoadOp load=Clear`; `LoadOp::Load` binds the attachment LOAD (Vulkan
  `loadOp=LOAD`; DX12 skips `ClearRenderTargetView`). `draw_storage_depth_load` delegates to it when the target is
  depthless (symmetric with `draw_storage_depth`).
- **depth_write=false** — the sanctioned DIRECT-ENCODER discipline: `submit_overlay` (and the `enc_draw_overlay*`
  test helpers) call `set_pass_state({depth_write=false})` before the draws and restore `{}` after. (The encoder
  never mirrors `packet.state.raster` into `set_pass_state` — that would clobber cull/bias/stencil on every
  frame-graph pass, whose state is set at the pass level by `frame_graph.cpp`.)

Verified through the generic path with REN-1's sync-vs-frame-graph **bit-match** as the strongest identity proof,
plus new both-backend overlay pixel gates (the DX12 side closed a prior coverage gap — DX12 had no overlay pixel
gate).

## Mechanism — the DX12 first_vertex offset fix

Driving the ranged-overlay gate exposed that a non-indexed `DrawInstanced(count, 1, first_vertex, 0)`'s
`StartVertexLocation` does **not** reach `SV_VertexID` on this DX12 path. Confirmed empirically (not inferred):
instrumentation proved the draw *issues* with a *valid PSO* and `first_vertex=4`, yet the `{4,5,6}`-keyed probe
stayed dark → `SV_VertexID` was `{0,1,2}`. A new permanent contract gate (`build_vid_offset_probe_vs/_fs`,
flat-int-varying idiom) pins the *value*: geometry covers the centre for `vid∈{0,1,2}` OR `{4,5,6}`, the FS writes
`R = vid·(40/255)`; drawn `first_vertex=4` the provoking vertex sees `vid=4` iff the offset arrived ⇒ centre
`R=160`, else `R=0`. DX12 read **R=0**, Vulkan read **R=160**.

This is **pre-existing, not an R2 regression**: `git show HEAD` shows the retired `draw_overlay_range` used the
identical `DrawInstanced(…, first_vertex, 0)` under a comment falsely asserting "D3D12 folds StartVertexLocation
into SV_VertexID" — there was simply no DX12 ranged-overlay gate to catch it. It matches the codebase's own
REN-39-B1 doctrine ("the only portable offset channel is the draw table, never the draw call").

**Fix (at the DX12 backend seam, robust to the root cause):** the `first_vertex → VertexIndex` command-model
contract is unchanged for the encoder / `submit_overlay` / the authored expand-VS; only DX12's *honoring* of it
moved. `draw_instanced_ranged(vertex_count, first_vertex)` replaces the four ranged-draw sites (in `draw_storage`,
`draw_storage_depth_load`, `record_scene`, `record_offscreen`): `first_vertex==0` keeps the plain
`DrawInstanced` (scene fast-path, byte-identical); `first_vertex>0` binds a context-owned **identity index
buffer** `[0,1,2,…]` (UPLOAD heap, `GENERIC_READ`, lazily created/regrown + Map-filled by
`ensure_identity_index_buffer`) and issues `DrawIndexedInstanced(count, 1, first_vertex, 0, 0)` ⇒
`SV_VertexID = identity[first_vertex+i] = first_vertex+i`. REN-39-A1 already proves indexed draws deliver the
index value as `SV_VertexID` on this adapter. After the fix, both backends' contract gate reads **R=160** — the
same value — and the ranged overlay composites correctly; the `[!mayfail]` tag was removed.

## Platform-matrix note (PQP-3)

The DX12 default adapter for this run: **NVIDIA GeForce RTX 4070 Ti SUPER**, driver **32.0.15.9579** — a real
hardware adapter, not WARP. So the `StartVertexLocation`-not-reaching-`SV_VertexID` observation is a genuine
hardware/driver platform fact (worth a re-check on WARP / Intel / AMD adapters as a PQP-3 platform-matrix item —
the identity-index-buffer fix is portable regardless, since it rides the index-value channel both backends honor).

## Verification (4 configs + tidy)

- **win-debug**: CEIR-34 R2 7/7 (3 Vulkan + 4 DX12; contract gate DX12 R=0→R=160, Vulkan R=160) · REN- 108/108
  (incl. REN-1 bit-match, REN-2 RTT, REN-3 scene/shadow, REN-39 indexed, REN-40 GPU-draw) · RET-6 2/2.
- **win-asan**: R2 7/7 · REN- 166/166 · RET-6 2/2 — ASan-clean.
- **linux-gcc-debug**: R2 3/3 (Vulkan; DX12 not built on Linux) · REN- 261/261 · RET-6 2/2 — gcc 13.3.0 -Werror
  build of the Vulkan test (the changed shared test headers compile clean); contract twin R=160 on llvmpipe.
- **linux-gcc-asan**: R2 3/3 · REN- 261/261 · RET-6 2/2 — ASan-clean, no lavapipe flake.
- **LLVM-20.1.8 tidy CLEAN**: `dx12_raster_context.cpp`, `ckir_raster_triangle.hpp`, `verb_packet_helpers.hpp`,
  `test_dx12_raster.cpp`, `test_vulkan_context.cpp`.

## Files

- `engine/gpu-context/include/crd/gpu/detail/command_lowering.hpp` — Alpha arm removed; overlay rides the generic arms.
- `engine/gpu-context-vulkan/src/vulkan_raster_context.cpp` — `apply_draw_blend` + blend/load/first_vertex params; `draw_overlay*`/`record_overlay` deleted.
- `engine/gpu-context-dx12/src/dx12_raster_context.cpp` — blend/load/first_vertex params; `draw_overlay*`/`record_overlay` deleted; `draw_instanced_ranged` + `ensure_identity_index_buffer` + the identity-IB member.
- `engine/draw/src/overlay_pass.cpp` — `submit_overlay` routes the depth bucket + sets `depth_write=false`.
- `engine/gpu-context/include/crd/gpu/raster_context.hpp` — the de-virtualization comment.
- `tests/gpu-shared/ckir_raster_triangle.hpp` — `build_solid_alpha_fs`, `build_vid_offset_probe_vs/_fs`.
- `tests/gpu-shared/verb_packet_helpers.hpp` — `enc_draw_storage_ranged`; `enc_draw_overlay*` set `depth_write=false`.
- `tests/gpu-context-vulkan/test_vulkan_context.cpp`, `tests/gpu-context-dx12/test_dx12_raster.cpp` — the R2 overlay + offset-contract gates.

## Proposed commit (user commits — NO AI co-author trailer)

```
refactor(gpu-context): retire the overlay device verb — the overlay rides the generic StoragePull command model; fix DX12 non-indexed first_vertex via an identity index buffer

CEIR-34 R2: draw_overlay/draw_overlay_range/record_overlay deleted on both backends;
the overlay composes through draw_storage / draw_storage_depth_load (blend, LOAD,
first_vertex, depth_write=false). Restores the first_vertex->SV_VertexID contract on
DX12 (StartVertexLocation does not reach SV_VertexID on the tested NVIDIA adapter) via
a backend-internal identity index buffer; adds a both-backend offset-contract gate.
```
