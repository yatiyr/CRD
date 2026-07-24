# 2026-07-24 — REN-1: the FRAME GRAPH on gpu-context (D-007 row 98)

**Slice**: the scheduled render surface — passes declare typed resource reads/writes, the graph orders them +
inserts barriers automatically, transient images/buffers get created + owned + memory-aliased by lifetime, and
the whole frame submits ONCE instead of the synchronous submit+wait+readback-per-draw substrate. Landed
**Vulkan-complete and gated**; DX12 renders through the synchronous fallback and its one-submission batching is
REN-1 pt-2 (the RET Vulkan-first precedent).

User direction this slice: **"full transient-resource graph now"** (the graph CREATES and OWNS transients with
lifetime analysis + memory aliasing, not just import-based — REN-2's RTT and REN-3's shadow-map-written-then-
sampled need it) and **"run the whole slice, report at the end."**

## What shipped

1. **`engine/gpu-context/include/crd/gpu/frame_graph.hpp` (NEW, backend-neutral)** — the REN-1 interface:
   - `struct FgImage`/`FgBuffer` — u32-id struct handles (not an enum: dodges the `performance-enum-size` lint
     while staying a typed, comparable, `valid()`-checkable handle).
   - `enum class FgAccess {Read,Write,ReadWrite}` · `FgPassKind {Raster,Compute,Present}` ·
     `FgImageFormat {RGBA8Unorm,RGBA8Srgb,RGBA16F,R16F,R32F,R32Uint,D32Float}` · `struct FgImageDesc`.
   - `IFrameContext` (raster()/image()/texture()/buffer() — resolves a handle to the live resource inside a
     pass callback) · `IFramePassBuilder` (reads/writes/read_writes/execute/present) · `IFrameGraph`
     (import_target/import_storage/create_transient_image/create_transient_buffer/add_pass/build/execute/reset
     + last_barrier_count/last_submit_count/transient_memory_bytes/transient_logical_bytes for the gate).
   - `using FgExecuteFn = void(*)(IFrameContext&, void*)` — a plain function pointer + user data (no std::function).

2. **`create_frame_graph()` appended at END of `IRasterContext`** (`raster_context.hpp`) — vtable-stability rule;
   default body `return nullptr` (the DX12 path today). Needed `#include <crd/gpu/frame_graph.hpp>` so the
   `unique_ptr<IFrameGraph>` return type is complete at the default-body site.

3. **⭐ THE KEY INSIGHT — the frame graph is a RECORDING MODE of the raster context** (`vulkan_raster_context.cpp`):
   while a graph executes, the existing `draw_storage_depth`/`draw_storage_depth_load`/`draw_overlay` RECORD into
   ONE shared `VkCommandBuffer` instead of each doing begin_cmd → record → end_and_wait → readback. **Zero new
   draw vocabulary** — the same CKIR draws the sandbox already issues. A `FrameRec` struct + `frame_rec_begin/
   _new_pass/_end/frame_recording/frame_readback` on `VulkanRasterContext`, and the three draws branch:
   `if (frame_recording()) { record_scene(...); return; }`.
   - **The blocker solved**: the per-draw `vkResetDescriptorPool` is what stops back-to-back recording (it would
     free the previous draw's live sets). The graph owns a FRAME descriptor pool — 256 sets, sized for all 3
     set-0 types (STORAGE_BUFFER + SAMPLED_IMAGE×(kBindlessMax+1) + SAMPLER), reset ONCE per `execute()` — so N
     draws' sets coexist in one command buffer.

4. **Transient memory ALIASING** (`VulkanFrameGraph::build()`) — greedy interval-coloring: each transient's
   lifetime is `[first_pass, last_pass]`; transients whose intervals are DISJOINT share one `VkDeviceMemory`
   slot (images with `VK_IMAGE_CREATE_ALIAS_BIT`, then buffers). `vkAllocateMemory` per slot; `vkBind*Memory`
   binds every resource in the slot at offset 0. `transient_memory_bytes()` (post-aliasing) < `transient_
   logical_bytes()` (sum of sizes) is the observable proof.

5. **Automatic barriers** (`VulkanFrameGraph::execute()`) — cross-pass transitions (COLOR/DEPTH attachment
   layout changes, WRITE→READ|WRITE ordering via a `layout_src` access/stage map) inserted by the graph;
   intra-pass self-barriers between consecutive draws to the same target by `frame_self_barrier_if_needed`.
   ONE `vkQueueSubmit` + fence wait; final readback loop copies each imported target that was read back.

6. **SceneRenderer MIGRATED** (`engine/scene-render/src/scene_renderer.cpp`) — `render()` now collects the
   visible mesh groups into a draw list during culling, then dispatches through `impl.frame_graph`
   (create/reuse → import target + each group's storage buffer → one "scene" pass writing the target image +
   reading every group buffer → execute). A synchronous per-draw fallback loop runs when `create_frame_graph()`
   returns nullptr (DX12). The frame_graph member lives on `Impl` so it destructs before the raster context.

## Gates (all green)

- **`tests/gpu-context-vulkan/test_vulkan_frame_graph.cpp` (NEW) — 33 assertions / 2 cases**:
  - *one submission + bit-match*: a scene pass + overlay pass compose into a single `vkQueueSubmit`
    (`last_submit_count()==1`), the graph inserts the cross-pass barrier (`last_barrier_count()>=1`), and the
    readback is BIT-IDENTICAL to the synchronous submit+wait-per-draw reference on a second target; ValidationCapture
    0 errors / 0 warnings across the whole lifecycle; reset+rebuild+re-execute still submits once.
  - *transient aliasing*: 2 equal-size disjoint-lifetime transients collapse to 1 slot (`memory*2==logical`),
    same-pass (overlapping) transients do NOT alias (`memory==logical`), an orphan transient no pass writes
    fails `build()`.
- **scene-render GPU gate — 58 assertions / 5 cases**: the 10k-instance field composes through the frame graph
  in one submission; GEO-7's draws==1 / drawn-instances / lighting-readback / cull-agreement all hold.
- **sandbox smoke — 65.2 fps** (261 frames / 4 s), up from ~58 fps on the sync substrate; 4705 instances last frame.
- **DX12 raster suite — 993 assertions / 106 cases** green via the synchronous fallback (the shared-header
  change is backward-compatible).
- All touched files **tidy-clean** (LLVM 20.1.8 gate): frame_graph.hpp, raster_context.hpp,
  vulkan_raster_context.cpp, scene_renderer.cpp, test_vulkan_frame_graph.cpp.

## Scars / decisions

- **Struct handles over an enum** — an `enum class FgImage` would trip `performance-enum-size`; a 1-field struct
  with `operator==` + `valid()` is the same ergonomics, lint-clean.
- **The frame descriptor pool must cover every set-0 type**, not just STORAGE_BUFFER — the shared layout has
  storage(0) + sampled-image(1) + sampler(2) + the bindless array(3); a pool missing types earns a
  `vkAllocateDescriptorSets` validation warning. Sized for all three (see #3).
- **crd Array `push_back` needs an lvalue** — `push_back(SceneDraw{...})` brace-init failed to overload-resolve;
  fill a named local then push it. And `group.buffer` is a `unique_ptr` — `.get()` for the raw `IStorageBuffer*`.
- **DX12 batching is genuinely a second implementation, not a config flag**: DX12's single storage-UAV heap slot
  (slot 0, overwritten per draw; descriptors are consumed at execute time, not record time) means multi-draw
  recording needs a per-draw descriptor-heap RING — the DX12 analog of the Vulkan frame pool — plus placed-
  resource transient aliasing and D3D12 cross-pass barriers. Deferred to REN-1 pt-2 to keep this window's Vulkan
  landing verified-green rather than ship an unverifiable half-backend (the "never claim done when partial" rule).

## Status

REN-1 is **◧** — Vulkan-complete + fully gated + SceneRenderer migrated + sandbox faster; DX12 correct via the
synchronous fallback with the one-submission batching as pt-2. D-007 row 98 + context.md updated. Full per-slice
sweep (win-debug/asan/shipping/tidy) run at close.

**Next**: REN-1 pt-2 (DX12 frame graph) or REN-2 (RTT + sampled textures — makes transients fully drawn-through),
per the user's command.
