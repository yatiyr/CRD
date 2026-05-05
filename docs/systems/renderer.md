# crd-renderer

High-level rendering orchestration above `crd-rhi`, `crd-rhi-vulkan`, and
`crd-shader`. This is where renderables, camera data, draw-item preparation,
the frame graph, render paths, and material binding live.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | explicit renderable list + camera + draw-item preparation | ✅ shipped 2026-05-01 |
| v1b | real draw execution / pass orchestration | ✅ shipped 2026-05-01 |
| v1c | frame graph v1 — typed handles, pass DAG, automatic barriers | ✅ shipped 2026-05-01 |
| v1d | `IRenderPath` on top of frame graph + `FrameContext` rework | ✅ shipped 2026-05-01 |
| v1e | push constants + descriptor set RHI surface | ✅ shipped 2026-05-01 |
| v1f | material system v1 (`MaterialBindLayout`, `MaterialBindGroup`) | ✅ shipped 2026-05-01 (renamed in v1c) |
| v1g | `ForwardRenderPath` — depth prepass + main color as frame graph passes | ✅ shipped 2026-05-01 |
| v1h | index buffer + `draw_indexed` | ✅ shipped 2026-05-01 |
| v1i | swapchain blit + output (first full frame loop) | ✅ shipped 2026-05-01 |
| v1j | GPU instancing | ⏳ Phase 3.2 dep — see `docs/debt.md` |

**Phase 2.7 additions**

| Slice | Ships | Status |
| --- | --- | --- |
| v1c | `MaterialTemplate` + `MaterialInstance` + `MaterialDomain` + `PassType` + `RasterState` (ADR-0048) | ✅ shipped 2026-05-05 |
| v1d | `GpuUploader` + `GpuTexture` + `GpuMesh`; RHI `copy_buffer` / `copy_buffer_to_image` / `submit_and_wait` | ✅ shipped 2026-05-05 |

**Phase 2.7 v1c — Material system foundation (resource side)**

| Slice | Ships | Status |
| --- | --- | --- |
| v1c | `MaterialTemplate` + `MaterialInstance` + `MaterialDomain` + `PassType` + `RasterState` (ADR-0048) | ✅ shipped 2026-05-05 |

## Core decisions

- Starts from an **explicit renderable list**, not ECS and not a scene graph.
  Scene/world systems layer on top in Phase 3.
- Consumes `crd-shader`'s backend-neutral `VariantPipelineDesc` handoff
  instead of owning shader compilation or pipeline objects directly.
- **Frame graph** is the foundation: all render paths declare passes into it;
  the compiler inserts Vulkan barriers and aliases transient images.
- **`IRenderPath`** makes Forward, Deferred, Visibility Buffer pluggable. Each
  implementation owns its render targets and declares frame graph passes.
- **`PipelineResolver`** is injectable for testability; concrete render paths
  own their variant → pipeline cache.

## Architecture layers

```
Layer 0  RHI          API-agnostic GPU surface
Layer 1  Frame graph  typed resource handles, pass DAG, automatic barriers
Layer 2  IRenderPath  Forward / Deferred / Forward+ / … implement this
Layer 3  Material     render-path-agnostic parameter binding
Layer 4  crd-ui       Phase 5 — UI canvas as a frame graph pass
```

## What ships today

### Scene-side types

**`Camera`**
- `view`, `projection` (Mat4f)
- derived `view_projection()`

**`Renderable`** — what the caller submits per frame
- `transform` (Transformf)
- `vertex_buffer*`, `vertex_count`
- `index_buffer*`, `index_count`, `index_type` (`Uint16` / `Uint32`) — null = non-indexed
- `material_instance_id`
- `variant` (VariantHandle)
- `bucket` (Opaque / Masked / Translucent)

**`DrawBucket`** — sort policy per bucket
- Opaque + Masked: front-to-back (minimises overdraw, feeds early-Z)
- Translucent: back-to-front (correct alpha compositing)

### Renderer

**`Renderer`**
- `submit(Renderable)` — queue a renderable for the current frame
- `clear()` — drop all queued renderables
- `build_frame(ctx, shader_runtime, out)` — classify, validate, and depth-sort
  into a `DrawList`; returns false on invalid input (null vertex buffer, zero
  vertex count, index buffer present but index count zero, invalid variant)

**`FrameContext`** — per-frame data passed to `build_frame` and `IRenderPath::build`
- `camera`, `camera_position`, `viewport`, `frame_index`

**`DrawList`** — bucketed, depth-sorted output
- `opaque`, `masked`, `translucent` — each an `Array<DrawItem>`

**`DrawItem`** — prepared draw item consumed by render paths
- `model` (Mat4f), `view_projection` (Mat4f)
- `vertex_buffer*`, `vertex_count`
- `index_buffer*`, `index_count`, `index_type`
- `material_instance_id`, `variant`, `handoff` (VariantPipelineDesc), `depth`

### Frame graph (v1c)

**`FrameGraph`** — the compile-and-execute surface
- `import(image*, initial_access)` → `ImageHandle`
- `add_pass(name)` → `PassBuilder`
  - `builder.read(handle, access)` — declare a read dependency
  - `builder.write(handle, access)` — declare a write + ownership
  - `builder.set_execute(lambda)` — the GPU recording callback
- `build()` — topological sort + barrier schedule computation
- `execute(device, command_buffer)` — run lambdas, inserting barriers between passes
- `reset()` — clear for the next frame

Barriers are inserted between passes only when the access state changes.
No barrier is inserted if two consecutive passes use the same access (e.g.
two depth-write passes using the same depth target).

### IRenderPath (v1d)

```cpp
class IRenderPath
{
public:
    virtual void build(FrameGraph&, const DrawList&, const FrameContext&) = 0;
    virtual ImageHandle output_image() const noexcept = 0;
    virtual void resize(Extent2D) = 0;
};
```

### Material system (v1f, renamed in v1c)

**`MaterialBindLayout`** (was `MaterialLayout`)
- wraps a `DescriptorSetLayout` (set 1 = per-material by convention)
- `create(device, desc)` — factory; returns `nullptr` on allocation failure
- `create_instance(allocator)` — allocates a `MaterialBindGroup` from the ring

**`MaterialBindGroup`** (was `MaterialInstance`)
- wraps a `DescriptorSet`
- `update_buffer(binding, buffer)` — wire a UBO/SSBO into the descriptor
- `descriptor_set()` — access for `CommandBuffer::bind_descriptor_sets`

### Material resource types (Phase 2.7 v1c / ADR-0048)

**`MaterialDomain`** — enum class u8: `Surface=0`, `PostProcess=1`, `Compute=2`, `Decal=3`, `UI=4`

**`PassType`** — enum class u8: `DepthPrepass=0`, `Shadow=1`, `Forward=2` (`kPassTypeCount=3`)

**`RasterState`** — 8 bytes: `alpha_mode`, `cull_mode`, `fill_mode`, `depth_test`, `depth_write`, `src_blend`, `dst_blend`, `pad`

**`CookedParameter`** — 24 bytes: `name_hash u64`, `enables_option_hash u64`, `ubo_offset u16`, `type ParameterType`, `binding_slot u8`, `pad[4]`

**`ShaderOptionDecl`** — 16 bytes: `name_hash u64`, `default_enabled u8`, `pad[7]`

**`PassShaderPair`** — `{ResourceHandle<ShaderResource> vert; ResourceHandle<ShaderResource> frag;}`

**`MaterialTemplate`** — loaded asset type. Constructor takes `IAllocator*`. Fields: `MaterialDomain domain`, `Array<CookedParameter> parameters`, `Array<u8> defaults_blob`, `PassShaderPair pass_shaders[3]`, `RasterState pso_states[3]`, `Array<ShaderOptionDecl> options`.

**`MaterialInstance`** — caller-owned transient instance. `MaterialInstance(ResourceHandle<MaterialTemplate>, IAllocator*)` copies defaults into `values_blob`. `set_float(name_hash, value)` / `set_vec4(name_hash, Vec4f)` binary-search by `name_hash` and write to `values_blob` at `ubo_offset`. `variant_for_pass(PassType)` returns the `PassShaderPair` for that pass, falling back to `PassType::Forward` if the requested pass has no shader.

### Descriptor frequency convention (fixed from v1g)

| Binding | Frequency | Content |
|---------|-----------|---------|
| Push constants | per-draw | model matrix (64 bytes, Vertex) |
| Set 0 | per-frame | `PerFrameUbo` (camera matrices, 288 bytes) |
| Set 1 | per-material | textures, material params |

### ForwardRenderPath (v1g–h)

First concrete `IRenderPath`. Two passes declared into the frame graph each frame:

| Pass | Action | Draws |
|------|--------|-------|
| `depth-prepass` | clears + fills depth buffer | `draw_list.opaque` |
| `main-color` | clears color, loads depth | `draw_list.opaque` + `draw_list.masked` |

Draw dispatch per item:
- `index_buffer != nullptr` → `bind_index_buffer` + `draw_indexed`
- otherwise → `bind_vertex_buffer` + `draw`

Owns: `B8G8R8A8Unorm` color image + `D32Sfloat` depth image (recreated on `resize()`).
Ring of `PerFrameUbo` UBOs + descriptor sets, indexed by `frame_index % frames_in_flight`.

### Push / UBO data types (`per_frame_data.hpp`)

```cpp
// PerFrameUbo — set 0, binding 0, CpuToGpu, 288 bytes
struct PerFrameUbo {
    Mat4f view, proj, view_proj, inv_view_proj;
    Vec4f camera_pos_ws;
    f32   viewport_width, viewport_height, time_seconds, _pad;
};

// PerDrawPush — push constants, 64 bytes, Vertex stage
struct PerDrawPush { Mat4f model; };
```

### Swapchain blit (v1i)

Free function in `crd/renderer/swapchain_blit.hpp`:

```cpp
ImageHandle add_swapchain_blit_pass(FrameGraph& fg,
                                    ImageHandle render_output,
                                    rhi::Image& sc_image,
                                    rhi::Extent2D render_extent,
                                    rhi::Extent2D display_extent);
```

Adds two passes to the frame graph:
- **`swapchain-blit`** — reads `render_output` as `TransferSrc`, writes `sc_image` as `TransferDst`, calls `blit_image` (linear filter)
- **`present-barrier`** — empty execute; triggers `TransferDst → Present` barrier so the swapchain image is layout-ready for `vkQueuePresentKHR`

After `fg.execute()` the full frame loop becomes:
```cpp
fg.reset();
render_path->build(fg, draw_list, ctx);
add_swapchain_blit_pass(fg, render_path->output_image(),
                        swapchain->current_image(), render_extent, display_extent);
fg.build();
fg.execute(*device, *cmd);
queue.submit(*cmd, *swapchain);
queue.present(*swapchain);
```

`blit_image` uses `VK_FILTER_LINEAR` — renders correctly at any render-to-display scale ratio (dynamic resolution ready).

`ForwardRenderPath` color image carries `ColorAttachment | TransferSrc` usage. Swapchain images carry `ColorAttachment | TransferDst` usage (set at swapchain creation).

### GPU upload helpers (Phase 2.7 v1d)

**`GpuTexture`** — `{ std::unique_ptr<rhi::Image> image }`. Wraps a device-local image with `Sampled | TransferDst` usage. No sampler or image view yet — those land in Phase 2.8 with per-material descriptor binding.

**`GpuMesh`** — `{ std::unique_ptr<rhi::Buffer> vertex_buffer; std::unique_ptr<rhi::Buffer> index_buffer; }`. Both buffers are device-local (`GpuOnly`) with `Vertex/Index | TransferDst` usage.

**`GpuUploader`** — static helper class:
```cpp
[[nodiscard]] static GpuTexture upload_texture(TextureResource& cpu, rhi::Device& device);
[[nodiscard]] static GpuMesh    upload_mesh(MeshResource& cpu, rhi::Device& device);
```
Both methods use the staging-buffer pattern: allocate a host-visible (`CpuToGpu`) staging buffer, `memcpy` the CPU data in, create the device-local resource, record a one-shot command buffer (transition + copy), call `queue.submit_and_wait()`, then let the staging buffer destruct. Uploads are **synchronous** — the call returns only after the GPU has finished consuming the staging buffer.

`upload_texture` inserts layout transitions: `Undefined → TransferDst` before the copy regions, `TransferDst → ShaderRead` after. Only `RGBA8Unorm` format is supported in v1d; `BC7` asserts false (deferred to Phase 2.8).

**RHI additions shipped with v1d:**
- `CommandBuffer::copy_buffer(src, dst, src_off, dst_off, size)` — maps to `vkCmdCopyBuffer`
- `CommandBuffer::copy_buffer_to_image(src, dst, ConstSpan<BufferImageCopy>)` — maps to `vkCmdCopyBufferToImage`; `BufferImageCopy` struct in `rhi/types.hpp`: `{buffer_offset u64, mip_level u32, extent Extent2D}`
- `Queue::submit_and_wait(CommandBuffer&)` — headless submit + `vkQueueWaitIdle`
- `transition_image` subresource range fix: `VK_REMAINING_MIP_LEVELS` (was hardcoded to 1)

## What it does not do yet

- No translucent pass in `ForwardRenderPath` (draws opaque + masked only)
- No scene graph or ECS (Phase 3)
- No per-material set 1 binding in `ForwardRenderPath` (Phase 2.8 — `GpuTexture` has no sampler/imageview yet)
- No GPU instancing (Phase 3.2 — see `docs/debt.md`)
- No async GPU upload (Phase 2.8+ — v1d uploads are synchronous fence+wait)

## Long-term direction

- v1i completes the first end-to-end frame loop (renderable on screen)
- Phase 2.5 (`crd-jobs`) shipped — async pipeline compilation and async upload are now possible
- Phase 2.6 (`crd-resources`) brings real mesh loading and GPU streaming
- Phase 3 brings scene/ECS systems; renderer becomes a scene consumer
- `ForwardPlusRenderPath` follows `ForwardRenderPath` once the clustered
  light grid is ready; `DeferredRenderPath` and `VisibilityBufferRenderPath`
  follow in Phase 5
