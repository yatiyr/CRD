# crd-rhi

Low-level Render Hardware Interface. `crd-rhi` owns the API-agnostic GPU
surface only: device, swapchain, queue, buffers, images, command buffers,
shader modules, and pipelines. It is the layer that future backends (starting
with Vulkan) implement and that higher-level rendering code builds on.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | types, abstract interfaces, fake-backend tests, smoke | ✅ |
| v1b | Vulkan backend bootstrap (`crd-rhi-vulkan`) | ✅ |
| v1c | command buffers + frame sync | ✅ |
| v1d | pipeline + first triangle | ✅ |
| v1e | GPU memory + streaming foundation | ✅ |
| v1f | descriptor system — set layouts, pipeline layouts, ring allocator, descriptor sets | ✅ |
| v1g | caller-managed image transitions; optional color attachment; null fragment shader | ✅ |
| v1h | `IndexType` enum; `bind_index_buffer()`; `draw_indexed()` | ✅ |
| v1i | `blit_image()` with linear filter; swapchain `TransferDst` usage | ✅ |

## Core decisions

- `crd-rhi` is intentionally **minimal**. It does not own materials,
  scene/ECS, lighting, render graphs, or other high-level rendering policy.
- The public API is **backend-agnostic**. Vulkan is the first backend, but
  Vulkan types must not leak into `crd-rhi` headers.
- The module is **first-triangle oriented**. The early design only includes
  the concepts needed to describe the path from window surface to submitted
  draw call.
- v1a uses **fake backend tests** rather than real GPU work. This locks the
  interface shape before `crd-rhi-vulkan` lands.
- `crd-rhi-vulkan` is a separate backend module. Its public entrypoint returns
  `crd::rhi::Instance` without exposing any `Vk*` type in the header surface.

## What ships today (v1a)

- `types.hpp`
  - backend-neutral enums: `BackendApi`, `Format`, `PresentMode`,
    `MemoryUsage`, `BufferUsage`, `ImageUsage`, `ShaderStage`,
    `PrimitiveTopology`, `LoadOp`, `StoreOp`
  - core structs: `Extent2D`, `Rect2D`, `ClearColorValue`,
    `ClearDepthStencilValue`, `AdapterInfo`
  - descriptors: `DeviceDesc`, `SwapchainDesc`, `BufferDesc`, `ImageDesc`,
    `ShaderModuleDesc`, `GraphicsPipelineDesc`, `RenderingInfo`
- Abstract interfaces
  - `Instance`
  - `Device`
  - `Queue`
  - `Swapchain`
  - `Buffer`
  - `Image`
  - `CommandBuffer`
  - `ShaderModule`
  - `Pipeline`
- `smoke_rhi_api`
  - exercises the intended first-triangle flow through a local fake device:
    create shader modules, buffer, pipeline, command buffer, begin/bind/draw/
    submit
- fake-backend tests
  - prove descriptor defaults, flag composition, adapter enumeration, and the
    intended first-triangle resource flow without needing Vulkan yet

## What ships today (v1b — Vulkan bootstrap)

- `crd-rhi-vulkan`
  - `create_vulkan_instance(InstanceDesc)` factory
  - real Vulkan instance creation
  - optional validation-layer enablement when available
  - debug-utils messenger hookup for warning/error reporting
  - physical-device enumeration through the RHI-facing `AdapterInfo` surface
  - logical-device creation with graphics queue selection
  - surface creation from `Window::native_handle()`
  - swapchain creation and swapchain-image wrapping into backend-neutral
    `Image` objects
- `smoke_rhi_vulkan_bootstrap`
  - opens a real GLFW window
  - creates a Vulkan instance, enumerates adapters, creates a device,
    creates a swapchain, and reports success
- real backend tests
  - instance enumerates at least one adapter
  - device bootstrap creates a swapchain for an invisible window

## What ships today (v1c — frame execution path)

- real Vulkan command-pool creation on the device
- real command-buffer allocation / reset / begin / end
- swapchain-owned per-frame synchronization objects
  - image-available semaphore
  - render-finished semaphore
  - in-flight fence
- acquire / submit / present loop through the real Vulkan queue path
- clear-only dynamic-rendering frame path
- real first-triangle path with shader modules, vertex buffer, pipeline,
  bind, and draw
- backend tests now prove:
  - bootstrap + swapchain creation
  - command buffer + acquire + submit + wait-idle path
  - real triangle frame path in debug/ASan
- `smoke_rhi_vulkan_bootstrap` now exercises a bounded real per-frame loop

Important nuance:

- sync2 support is detected and kept in mind architecturally, but the shipped
  execution path uses the classic barrier / submit flow in this slice because
  it proved more stable across the current release-path testing.

The current RHI/Vulkan slices still intentionally stop short of higher-level
GPU work:

- no descriptor sets / resource binding model yet
- no push constants / camera path yet
- no full allocator policy beyond the current centralized buffer/image
  allocation foundation
- no material system or renderer policy yet

## What ships today (v1e — GPU memory + streaming foundation)

- centralized backend-owned Vulkan allocator helper
- centralized memory-type selection
- backend-owned buffer allocation path
- backend-owned image allocation path
- image memory bind + image-view creation path
- allocator-backed device-local resources are now possible, even though the
  more advanced streaming/suballocation strategies are still future work

This is still intentionally not the final allocator architecture:

- no TLSF/Buddy/Streaming allocator policy yet
- no residency/eviction model yet
- no broad deferred-destruction system yet
- no cross-resource suballocation yet

## What ships today (v1f — descriptor system)

- `DescriptorBinding` — one slot (binding index, type, array count, stage mask)
- `DescriptorSetLayoutDesc` + `Device::create_descriptor_set_layout()`
- `PushConstantRange` + `PipelineLayoutDesc` + `Device::create_pipeline_layout()`
- `DescriptorAllocatorDesc` + `Device::create_descriptor_allocator()`
  - ring-buffer allocator; holds N pools (one per frame-in-flight)
  - `begin_frame(frame_index)` resets the oldest pool
  - `allocate(layout)` returns a `DescriptorSet` for the current frame
- `DescriptorSet::update_buffer(binding, buffer, offset, size)`
- `CommandBuffer::push_constants(layout, stages, offset, size, data)`
- `CommandBuffer::bind_descriptor_sets(layout, first_set, sets)`
- `ShaderStage` bitmask: `Vertex=1`, `Fragment=2`, `Compute=4`; combinable with `|`

## What ships today (v1g — caller-managed transitions + depth-only pipeline)

Changes that harden `crd-rhi-vulkan` for multi-pass rendering:

- **`begin_rendering` no longer inserts implicit image transitions.**
  The old implementation called `transition_to_color_attachment()` at
  `begin_rendering` and `transition_to_present()` at `end_rendering`. Both
  helpers and the `m_render_target` member were removed. Callers (frame graph,
  tests, smokes) are now responsible for explicit `transition_image()` calls.
  This is the correct design: the frame graph's barrier schedule covers it.
- **Color attachment is optional.** Pass `info.color_attachment.image == nullptr`
  for a depth-only pass. `colorAttachmentCount = 0` in `VkRenderingInfo`.
- **Null fragment shader** accepted by `create_graphics_pipeline`. When
  `desc.fragment_shader == nullptr`, the pipeline is vertex-only (no fragment
  stage, `colorAttachmentCount = 0`). Required for depth-only pipeline variants.

## What ships today (v1h — index buffer)

- `IndexType` enum: `Uint16`, `Uint32`
- `CommandBuffer::bind_index_buffer(buffer, offset_bytes, type)` — maps to
  `vkCmdBindIndexBuffer`
- `CommandBuffer::draw_indexed(index_count, first_index, vertex_offset)` —
  maps to `vkCmdDrawIndexed` with `instance_count=1`

## What ships today (v1i — swapchain blit)

- `CommandBuffer::blit_image(src, dst, src_extent, dst_extent)` — maps to
  `vkCmdBlitImage` with `VK_FILTER_LINEAR`; both images must already be in
  `TransferSrc` / `TransferDst` layout (frame graph handles transitions)
- Swapchain image usage now includes `VK_IMAGE_USAGE_TRANSFER_DST_BIT` so
  swapchain images can be blit targets

## How to use it (today)

At v1a/v1b/v1c, the low-level flow now has a real backend entrypoint and a
real frame path:

```cpp
auto instance = crd::rhi::create_vulkan_instance({});
auto device = instance->create_device(device_desc);
auto swapchain = device->create_swapchain(swapchain_desc);
auto command_buffer = device->create_command_buffer();

swapchain->acquire_next_image();
command_buffer->begin();
command_buffer->begin_rendering(rendering_info);
command_buffer->end_rendering();
command_buffer->end();

device->graphics_queue().submit(*command_buffer, *swapchain);
device->graphics_queue().present(*swapchain);
```

Non-indexed draw:

```cpp
// explicit transition required before recording (frame graph handles this automatically)
command_buffer->transition_image(*render_target, ImageAccess::Undefined, ImageAccess::ColorWrite);
command_buffer->begin_rendering(rendering_info);
command_buffer->bind_pipeline(*pipeline);
command_buffer->bind_vertex_buffer(*vertex_buffer, 0);
command_buffer->draw(vertex_count, 0);
command_buffer->end_rendering();
command_buffer->transition_image(*render_target, ImageAccess::ColorWrite, ImageAccess::Present);
```

Indexed draw:

```cpp
command_buffer->bind_pipeline(*pipeline);
command_buffer->bind_vertex_buffer(*vertex_buffer, 0);
command_buffer->bind_index_buffer(*index_buffer, 0, IndexType::Uint32);
command_buffer->draw_indexed(index_count, 0, 0);
```

Descriptor binding:

```cpp
auto layout    = device->create_pipeline_layout(pipeline_layout_desc);
auto set_layout = device->create_descriptor_set_layout(set_layout_desc);
auto allocator = device->create_descriptor_allocator(alloc_desc);

allocator->begin_frame(frame_index);
auto set = allocator->allocate(*set_layout);
set->update_buffer(0, *ubo_buffer, 0, sizeof(PerFrameUbo));

command_buffer->push_constants(*layout, ShaderStage::Vertex, 0, sizeof(model_matrix), &model_matrix);
rhi::DescriptorSet* sets[] = { set.get() };
command_buffer->bind_descriptor_sets(*layout, 0, crd::containers::make_span(sets, 1u));
```

## Long-term direction

- Vulkan bootstrap, frame execution, and first triangle are now in place.
- Next immediate slices are shader/descriptor growth and renderer-facing
  resource binding work on top of the now-stable allocation path.
- Only after that path is proven do we climb further into higher-level
  renderer concerns.
- High-level rendering (`crd-renderer`) stays above this layer.
