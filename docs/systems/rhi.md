# crd-rhi

Low-level Render Hardware Interface. `crd-rhi` owns the API-agnostic GPU
surface only: device, swapchain, queue, buffers, images, command buffers,
shader modules, and pipelines. It is the layer that future backends (starting
with Vulkan) implement and that higher-level rendering code builds on.

## Status

| Slice | Ships | Status |
| --- | --- | --- |
| v1a | types, descriptors, abstract interfaces, fake-backend tests, smoke | ✅ |
| v1b | Vulkan backend bootstrap in `crd-rhi-vulkan` | ✅ |
| v1c | command buffers + frame sync | ✅ |
| v1d | pipeline + first triangle | ⏳ |

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
- no GPU allocator policy beyond the minimal host-visible vertex-buffer path
- no material system or renderer policy yet

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

The first-triangle path is now real:

```cpp
auto device = instance->create_device(device_desc);
auto swapchain = device->create_swapchain(swapchain_desc);
auto vertex_buffer = device->create_buffer(buffer_desc);
auto vs = device->create_shader_module(vertex_shader_desc);
auto fs = device->create_shader_module(fragment_shader_desc);
auto pipeline = device->create_graphics_pipeline(pipeline_desc);
auto command_buffer = device->create_command_buffer();

command_buffer->begin();
command_buffer->begin_rendering(rendering_info);
command_buffer->bind_pipeline(*pipeline);
command_buffer->bind_vertex_buffer(*vertex_buffer, 0);
command_buffer->draw(3, 0);
command_buffer->end_rendering();
command_buffer->end();

device->graphics_queue().submit(*command_buffer, *swapchain);
device->graphics_queue().present(*swapchain);
```

## Long-term direction

- Vulkan bootstrap, frame execution, and first triangle are now in place.
- Next immediate slices are ImGui debug overlay, GPU allocation policy, and
  richer shader/pipeline growth that will feed `crd-renderer`.
- Only after that path is proven do we climb further into higher-level
  renderer concerns.
- High-level rendering (`crd-renderer`) stays above this layer.
