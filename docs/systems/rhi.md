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
| v1c | command buffers + frame sync | ⏳ |
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

Bootstrap intentionally stops short of higher-level GPU work:

- no command-buffer recording path yet
- no real queue submit/present path yet
- no buffers/images allocation policy yet
- no shader-module or graphics-pipeline implementation yet

## How to use it (today)

At v1a/v1b, the low-level flow now has a real backend entrypoint:

```cpp
auto instance = crd::rhi::create_vulkan_instance({});
auto device = instance->create_device(device_desc);
auto swapchain = device->create_swapchain(swapchain_desc);
```

The full first-triangle path is still the intended end-state:

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

device->graphics_queue().submit(*command_buffer);
device->graphics_queue().present(*swapchain);
```

## Long-term direction

- Vulkan bootstrap is now in place. Next slice is command-buffer and frame-
  synchronization work on top of a real backend.
- Only after that path is proven do we climb into pipelines, shader modules,
  and the first triangle on a real GPU.
- High-level rendering (`crd-renderer`) stays above this layer.
