## RAF-12.4 relocation design — move the 49 encoder-used verbs off IRasterContext into per-backend encoders

### 0. Ground truth (verified in source)
- `TranslatingCommandEncoder` (engine/gpu-context/src/command_encoder.cpp:115-602) is `final`, in an anonymous namespace, holds `IRasterContext& m_ctx`, and lowers begin_rendering/draw/dispatch/transfer/trace_rays by calling the 49 VIRTUAL verbs. Selection uses file-local pure helpers: `first_storage`, `find_kind`, `shadow_atlas_from`, `plain_sampled_texture`, `map_texture`, `to_blit_filter`, `has_color`, `wants_clear`, and the slot constants `kSceneMapSlot=1`/`kSceneAtlasSlot=4`.
- `IRasterContext::create_command_encoder()` (command_encoder.cpp:607-610) is a NON-pure virtual returning `make_unique<TranslatingCommandEncoder>(*this)`. Its own comment (605-606) already reserves the RAF-12 per-backend override.
- Backends: `VulkanRasterContext final : public IRasterContext` (vulkan_raster_context.cpp:1033) and `Dx12RasterContext final : public IRasterContext` (dx12_raster_context.cpp:989), both in anonymous namespaces, reached only via the factories `create_vulkan_raster_context(vkc)` / `create_dx12_raster_context()` which return `unique_ptr<IRasterContext>`. **Tests and the concrete verb bodies are therefore only reachable today through the IRasterContext vtable** — that is precisely why deleting a verb decl breaks every test call site.
- `clear()` and `set_next_draw_load_depth()` are ALSO called by the encoder (lines 526, 153) but are NOT in the 49 — they stay virtual.

### 1. Shared-lowering-detail header (do this FIRST, before touching any verb)
Extract the file-local helpers + slot constants from command_encoder.cpp:18-113,585-596 into an INTERNAL header `engine/gpu-context/include/crd/gpu/detail/command_lowering.hpp` (or a private src header both backend TUs can include). This is the byte-identical guarantor: both per-backend encoders must select verbs with the SAME source, not two hand-copies. The helpers are pure functions over `ResourceBindingTable`/`RenderingDesc` with no backend types, so they move verbatim.

### 2. Per-backend encoder classes (Phase A — additive, zero deletions)
Add `engine/gpu-context-vulkan/src/vulkan_command_encoder.cpp` defining `VulkanCommandEncoder final : public ICommandEncoder` in that TU's anonymous namespace, holding `VulkanRasterContext& m_ctx` (CONCRETE type, not the interface). Its begin/end/draw/dispatch/transfer/trace_rays bodies are a literal copy of TranslatingCommandEncoder (command_encoder.cpp:120-582), including the depth-prepass `set_next_draw_load_depth` path and the `m_first`/`m_in_scope`/`m_rendering` state. Add the DX12 mirror `engine/gpu-context-dx12/src/dx12_command_encoder.cpp` with `Dx12CommandEncoder`/`Dx12RasterContext&`.

Override the factory in each backend:
- In vulkan_raster_context.cpp add `std::unique_ptr<ICommandEncoder> VulkanRasterContext::create_command_encoder() override { return std::make_unique<VulkanCommandEncoder>(*this); }` (declare it in the class near line 1033).
- DX12 mirror.

At this point the base `TranslatingCommandEncoder` is dead for real backends but still compiles. **Gate A (byte-identical by construction — same lowering, same still-virtual verbs, just concrete-typed m_ctx):** run tests/gpu-context/test_command_encoder_gpu.cpp (all gate_* GPU parity gates), both frame-graph suites (test_vulkan_frame_graph.cpp / test_dx12_frame_graph.cpp), scene-render GPU suite, and the sandbox smoke on BOTH backends against pre-change read_pixel/RenderDoc goldens. Must match exactly.

### 3. De-virtualize + delete, FAMILY BY FAMILY (Phase B)
For each verb in a family, three edits + a test migration:
1. **IRasterContext (raster_context.hpp):** delete the verb decl (pure-virtual `= 0` line OR defaulted `{}`/delegating body). ⚠ Deleting a defaulted body that CONTAINS the multi-draw loop (draw_storage_multi_* default bodies loop the single-draw verbs; draw_storage_multi_depth_only and draw_bindless_depth have dx=0/0 so DX12 RELIES on that header loop) means the loop must be MOVED into the per-backend encoder or the concrete method — never lost. Also DO NOT delete the shared `struct IndexedDraw` (hpp 1103-1108, used by 3 verbs) nor re-home the interleaved REN-38-A1b comment (hpp 603-614 sits above dispatch_kernel, describes draw_storage_mrt).
2. **Each backend .cpp:** turn the `override` verb into a plain PRIVATE non-virtual method on the concrete context (drop `override`, keep the identical body + the frame-recording/synchronous branch). The per-backend encoder already holds the concrete type, so `m_ctx.draw_xxx(...)` resolves statically to this method with no source change in the encoder.
3. **Migrate that family's test call sites** (§4).
Then run the family gate (§5) before starting the next family.

**Deletion order (leaf/isolated → most-entangled):**
- **F1 Transfer** — copy_image, blit_image, resolve_image (encoder-only, zero test callers). Warm-up: proves the mechanics on the smallest surface.
- **F2 Compute** — dispatch_kernel, dispatch_kernel_sampled, dispatch_kernel_indirect, dispatch_kernel_rt (only dispatch_kernel has test callers, in test_vulkan_frame_graph.cpp).
- **F3 Ray-trace** — trace_rays, trace_rays_anyhit, trace_rays_full (encoder-only; note the CommandEncoder::trace_rays(TraceDesc) wrapper at command_encoder.cpp:550 that frame_graph.cpp:774 calls stays — it lowers INTO these).
- **F4 Fullscreen** — draw_textured, draw_shadow, draw_bindless, draw_bindless_storage, draw_bindless_blend_load, draw_vrs, draw_conservative, draw_visbuffer, draw_visbuffer_load. Heavy: draw_textured/draw_shadow/draw_bindless/draw_vrs/draw_conservative are pure-virtual core verbs with the largest verb-test footprint.
- **F5 Amplification** — draw_mesh, draw_mesh_load, draw_mesh_storage_load, draw_mesh_indirect, draw_mesh_indirect_buffer, draw_tess, draw_tess_load, draw_tess_storage, draw_tess_storage_load. ⚠ draw_mesh_storage is BLOCKED (see §6) — do it last in F5 only after the scene_renderer migration.
- **F6 Storage scene** — draw_storage, draw_storage_depth(_load), draw_storage_depth_only(_load), draw_storage_textured_depth(_load), draw_storage_shadowed_depth(_load), draw_storage_textured_shadowed_depth(_load), draw_storage_indexed_depth, draw_storage_indexed_sampled_depth, draw_storage_mrt.
- **F7 Multi-draw batches** — draw_storage_multi_depth(_only), draw_storage_multi_indexed_depth(_only), draw_storage_multi_indexed_depth_only_indirect, draw_storage_multi_indexed_indirect. Last because of the header-default loop hazard above.

### 4. Test-call-site migration (~150 sites) — the dominant work
The verb tests hold an `IRasterContext&` (factory returns unique_ptr<IRasterContext>) and call `raster.draw_xxx(...)`. Once the decl is gone AND the concrete class is in an anonymous namespace, tests CANNOT reach the method. The gold migration is to drive the SAME capability through the command encoder, which is exactly what tests/gpu-context/test_command_encoder_gpu.cpp already does. Mechanics:
- Add a shared test header `tests/gpu-context/verb_packet_helpers.hpp` with one builder per verb-shape that constructs the `RenderingDesc` + `RasterDrawPacket` (or DispatchDesc/TransferDesc/TraceDesc) whose bindings/state make the encoder select THAT verb. The selection recipe is authoritative and already documented inline in command_encoder.cpp:161-467 — e.g. draw_textured ⇐ GeometryKind::None + one plain SampledTexture (no ComparisonSampler); draw_shadow ⇐ None + a SampledTexture at slot 4 (atlas); draw_storage_mrt ⇐ StoragePull + r.color.size()>=2; draw_storage_indexed_sampled_depth ⇐ Indexed + (map@1 or atlas@4); the multi verbs ⇐ MultiStoragePull/MultiIndexed with multi_counts/multi_indexed set; transfer verbs ⇐ TransferDesc kind Copy/Blit/Resolve; trace verbs ⇐ TraceDesc with/without any_hit/intersection/callable.
- Add `record_single(raster, RenderingDesc, RasterDrawPacket)` = create_command_encoder → begin_rendering → draw → end_rendering, so each test site collapses to a builder call + the SAME read_pixel assertion it already had.
- Do the rewrite family-by-family so each batch is validated by that family's gate. This also folds the "two homes" (bespoke verb suites + encoder suite) into ONE encoder-packet home — the stated RAF-12 endgame.
- **Untouched:** the six refuted test_only verbs (draw_depth, draw_gbuffer, draw_bindless_depth, draw_mesh_bindless_depth, draw_mesh_vrs, draw_wboit) are NOT in the 49 — they stay pure-virtual on IRasterContext, their tests keep calling them directly. Likewise draw_overlay/draw_overlay_range (engine_used, encoder_calls=0) stay virtual. So the migration touches only the 49's sites, not all raster-suite draws.

### 5. Byte-identical gate PER family
Before deleting a family, capture goldens (read_pixel dumps for the gate tests + RenderDoc capture of the sandbox frame, both backends). After the family's three edits + test migration:
1. tests/gpu-context/test_command_encoder_gpu.cpp — the family's gate_* (gate_copy/blit/resolve, gate_compute, gate_mesh, gate_storage/indexed/depth-loadstore, etc.).
2. Both frame-graph suites (they record through create_command_encoder → now the per-backend encoder) — the family's gates.
3. scene-render GPU suite.
4. Sandbox smoke both backends → pixel-identical to golden.
5. scripts/per-slice-check.ps1 (sequential, Ninja-capped) for cross-config SIMD/layout + LLVM-20 clang-tidy clean.
Byte-identical is expected BY CONSTRUCTION (the per-backend encoder is a literal copy of the lowering and the de-virtualized method bodies are unchanged); the gate proves no accidental drift.

### 6. Final cleanup (Phase C)
After all 49 are de-virtualized: delete the base `TranslatingCommandEncoder` and make `IRasterContext::create_command_encoder()` PURE virtual (`= 0`) since both backends now override it. engine/gpu-context/src/command_encoder.cpp reduces to nothing device-specific (the shared helpers already live in the detail header from §1) and can be removed from the gpu-context target. Full rebuild (wipe build dir — removing 49 virtuals reorders the vtable, a stale-.obj/LTCG cross-config hazard) + full sweep.

### Blocking prerequisite for F5 (draw_mesh_storage)
draw_mesh_storage has a LIVE non-test engine caller: SceneRenderer::draw_clusters (scene_renderer.cpp:4103). It cannot be removed from IRasterContext until that call is migrated to record a GeometryKind::Meshlet packet (storage-bound) through create_command_encoder — the encoder already lowers exactly that to draw_mesh_storage (command_encoder.cpp:389). Sequence: migrate draw_clusters → gate scene-render suite → then de-virtualize draw_mesh_storage as the last verb of F5.