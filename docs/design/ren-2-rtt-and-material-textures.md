# REN-2 design — render-to-texture transients + sampled material textures (D-007 row 99)

**Status**: spec written 2026-07-25 (during the REN-1 close sweep); implementation follows against this doc.
Builds directly on REN-1's frame graph and the ALREADY-PROVEN RTT sequence in `draw_wboit`.

REN-2 has two halves:
- **A — RTT transients**: a frame-graph pass renders into a transient color(/depth) image; a *later* pass
  samples it as a texture. Makes REN-1's transients (aliased but drawn-through-stubbed) first-class.
- **B — material textures in the forward pass**: the SceneRenderer samples real PBR maps (base-color / normal /
  metallic-roughness, cooked at GEO-3/4) per fragment instead of the flat material color.

## The load-bearing precedent: `draw_wboit` already does RTT

`vulkan_raster_context.cpp` `draw_wboit` (≈line 2985) is a complete render-into-then-sample sequence:
1. `create_image_bundle(w,h, R16F/RGBA16F, usage = COLOR_ATTACHMENT | SAMPLED, bundle)` — a color image that is
   both drawn into and sampled.
2. barrier `UNDEFINED → COLOR_ATTACHMENT_OPTIMAL`, `vkCmdBeginRendering`, draw, `vkCmdEndRendering`.
3. barrier `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` (src COLOR_ATTACHMENT_WRITE @ COLOR_OUTPUT →
   dst SHADER_READ @ FRAGMENT_SHADER).
4. bind the view as a `SAMPLED_IMAGE` (set-0 binding 3, the bindless array) + the default sampler (binding 2),
   composite pass samples it.

REN-2 generalizes exactly this into the frame graph: the transient IS the accum/reveal image; the graph owns the
barrier schedule and the sample-time descriptor binding.

## A. RTT transients

### A.1 The transient is drawable — a BORROWED render target

`VulkanRasterTarget` and `VulkanTexture` both `destroy_image_bundle` in their dtor. A transient's image lives in
the frame-graph `ImageNode` and its memory in an aliased `Slot` (freed by `free_transients`). So a transient
wrapper must NOT free the bundle. Two edits:

- **`VulkanRasterTarget` gains a `bool m_borrowed` (default false)**; ctor overload / setter marks it. When
  borrowed, the dtor SKIPS every `destroy_image_bundle`. `record_scene` / `record_overlay` already
  `static_cast<VulkanRasterTarget&>(target)` — so a borrowed target drops in with ZERO draw-path changes.
- **`VulkanFrameGraph::build`** (transient image branch): after `vkBindImageMemory` + view creation, construct the
  drawable target as `new VulkanRasterTarget(device, borrowedColorBundle, {}, borrowedDepthBundle, {}/*no readback*/,
  1, w, h)` with `m_borrowed = true`, where `borrowedColorBundle = {image = n.image, mem = null, view = n.view}`.
  Store it in `ImageNode::target` (replacing the `VulkanTransientTarget` stub). `image(FgImage)` returns it.

Depth RTT: a transient created with a depth format (`D32Float`) is a depth attachment; a color transient that a
raster pass writes may pair with a graph-created transient depth (REN-3 territory — REN-2 ships color RTT + an
optional depth transient in the same slot family; the gate exercises color RTT).

### A.2 The transient is sampleable — `texture(FgImage)` returns a real texture

- Replace the `texture()` stub (`return nullptr`) with: if the handle is a transient with `desc.sampled`, return a
  BORROWED `VulkanTexture` over `{image=n.image, view=n.view}` with `m_borrowed`-equivalent (add the same
  no-free guard to `VulkanTexture`, or a thin `VulkanBorrowedTexture : ITexture` exposing `view()`), created in
  `build()` alongside the target and stored in `ImageNode`.
- The sampling draw is the existing `draw_textured` (set-0 binding 1 = sampled image, binding 2 = sampler). In
  frame-recording mode, `draw_textured` needs a record path too (like `record_scene`): `record_textured` that
  allocates a frame descriptor set binding the transient's view as the sampled image + the default sampler, into
  the shared command buffer. (REN-1 added `record_scene`/`record_overlay`; REN-2 adds `record_textured`.)

### A.3 The graph inserts the RTT barrier

`VulkanFrameGraph::execute`, per pass, currently transitions imported color targets `COMMON→COLOR_ATTACHMENT`.
Extend for transients:
- When a pass WRITES a transient color image: `UNDEFINED/last → COLOR_ATTACHMENT_OPTIMAL` (it's rendered into).
- When a LATER pass READS that transient (samples it): `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL`
  (the RTT barrier, exactly step 3 above), tracked via `ImageNode::layout`.
- `last_barrier_count()` counts these — the gate asserts the RTT barrier landed.

Transients are still ALIASED (REN-1): an RTT transient's lifetime `[write pass, last sample pass]` participates in
the interval-coloring; a disjoint transient reuses its slot. (An aliasing barrier at slot reuse is a REN-2 nicety;
the gate uses non-overlapping single-use RTT so plain aliasing suffices.)

### A.4 DX12 mirror (`Dx12FrameGraph`) — NO deferral

Same shape on D3D12 (the RET/REN-1 discipline — both backends):
- `Dx12RasterTarget` gains a borrowed mode (skip resource release when borrowed); the transient placed-resource
  (`CreatePlacedResource`, REN-1) already exists — give it an RTV + SRV and wrap borrowed.
- `texture(FgImage)` returns a borrowed `Dx12Texture` (SRV over the placed resource).
- Barriers: `COMMON→RENDER_TARGET` (write) then `RENDER_TARGET→PIXEL_SHADER_RESOURCE` (sample) — the D3D12 analog
  of the Vulkan RTT barrier — recorded on the shared command list.
- `record_textured` binds the SRV in the per-draw descriptor-heap ring (REN-1's ring, extended with an SRV slot).

## B. Material textures in the SceneRenderer forward pass

- Mesh groups already carry cooked material data (GEO-3/4). Add the group's base-color / normal / metallic-
  roughness textures (uploaded once via `create_texture_from_mips`, the RET-3 path) to `SceneDraw`.
- The forward pass records `draw_textured` (or a bindless material variant) sampling those maps; the material
  shader (CKIR, B6/B7 lowering) samples albedo + normal instead of emitting the flat color.
- Falls out of A.2's `record_textured` — the same sample-binding machinery, fed by material textures rather than
  an RTT transient.

## Gate (`test_vulkan_frame_graph.cpp` + `test_dx12_frame_graph.cpp`, both backends)

1. **RTT round-trip**: a 2-pass graph — pass "offscreen" `writes(rtt)` renders a known pattern (e.g. left-red /
   right-green via a FragCoord split) into a `sampled` transient; pass "compose" `reads(rtt)` samples it full-screen
   and `writes(final)`. Readback of `final` == the pattern pass 1 rendered (a sampled RTT round-trip, not a flat
   clear). `last_barrier_count()` includes the `COLOR_ATTACHMENT→SHADER_READ_ONLY` transition. ONE submission.
2. **Material sampling** (SceneRenderer / GEO gate): a textured mesh renders with its sampled albedo + normal
   (a two-texel base-color map → left/right pixel split), NOT the flat color.
3. Validation-SILENT by counter (0/0); transient still ALIASES (physical < logical) when RTT lifetimes are disjoint.

## Files

- `frame_graph.hpp` — no new interface (uses existing `create_transient_image(sampled)`, `texture()`, `image()`).
- `vulkan_raster_context.cpp` — borrowed `VulkanRasterTarget`/`VulkanTexture`; `record_textured`; transient
  drawable+sampleable wiring in `build()`; RTT barrier in `execute()`.
- `dx12_raster_context.cpp` — the mirror (borrowed target/texture, SRV, RTT barrier, `record_textured`).
- `scene_renderer.cpp` — material textures on `SceneDraw` + the textured forward record.
- tests — the RTT round-trip on both backends + the material-sampling gate.
- benchmark at close (per the mandate) if a perf delta is measurable (RTT vs read-back-and-reupload).
