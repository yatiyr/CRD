# Session: `crd-renderer` v1e+f — Push Constants + Descriptor System + Material Binding

**Date:** 2026-05-01
**Presets at close:** win-debug 248/248, win-release 247/247, win-asan 248/248, win-clang-cl 248/248, win-tidy 248/248

---

## What was shipped

v1e and v1f were merged into one slice. Everything from push constants through the
material API shipped together because the dependencies flow one-way and the combined
slice is still narrow.

### RHI surface additions (`crd-rhi`)

`types.hpp`:
- `ShaderStage` promoted from sequential enum (Vertex=0, Fragment=1, Compute=2) to
  **bitmask** (Vertex=1<<0, Fragment=1<<1, Compute=1<<2). `operator|` and `has_stage()`
  added. This is the correct representation because push constant ranges and descriptor
  binding visibility are OR-composed sets of stages, not single choices.
- `DescriptorType` enum — six values mirroring the Vulkan taxonomy.
- `DescriptorBinding` struct — binding slot, type, array count, stage mask.
- `DescriptorSetLayoutDesc`, `PipelineLayoutDesc`, `DescriptorAllocatorDesc` structs.
- `PushConstantRange` struct.
- `GraphicsPipelineDesc` gains `pipeline_layout*` at the **end** of the struct so
  existing positional aggregate initializations in smoke tests and integration tests
  do not break (trailing default = nullptr → empty synthesised layout).

`descriptor.hpp` (new file):
- `DescriptorSetLayout` abstract base — `desc()` accessor.
- `PipelineLayout` abstract base — `desc()` accessor.
- `DescriptorSet` abstract base — `update_buffer()`.
- `DescriptorAllocator` abstract base — `begin_frame()` + `allocate()`.

`device.hpp`:
- `create_descriptor_set_layout()`, `create_pipeline_layout()`, `create_descriptor_allocator()`.

`command_buffer.hpp`:
- `push_constants(layout, stages, offset, size, data)`.
- `bind_descriptor_sets(layout, first_set, sets)`.

### Vulkan backend additions (`crd-rhi-vulkan`)

Free helper functions (in the anonymous namespace, before the class definitions):
- `to_vk_descriptor_type()` — maps `DescriptorType` enum to `VkDescriptorType`.
- `to_vk_shader_stage_flags()` — maps bitmask `ShaderStage` to `VkShaderStageFlags`
  by testing each bit individually with `has_stage()`.

New classes (defined before `VulkanCommandBuffer` so they can be used there):
- `VulkanDescriptorSetLayout` — owns `VkDescriptorSetLayout`, exposes `handle()`.
- `VulkanPipelineLayout` — owns `VkPipelineLayout`, exposes `handle()`.
- `VulkanDescriptorSet` — thin wrapper around `VkDescriptorSet` + a copy of the
  layout's `DescriptorBinding` array for type-aware `update_buffer()`.
  **Not individually freed** — pool reset handles reclamation.
- `VulkanDescriptorAllocator` — ring-buffer pool allocator (see design note below).

`VulkanCommandBuffer` additions:
- `push_constants()` → `vkCmdPushConstants`.
- `bind_descriptor_sets()` → collects raw `VkDescriptorSet` handles, calls
  `vkCmdBindDescriptorSets`.

`VulkanPipeline` changes:
- Now has `m_owned_layout` (non-null only when `create_graphics_pipeline` synthesised
  an empty layout because `desc.pipeline_layout == nullptr`). User-provided layouts
  are owned by `VulkanPipelineLayout` objects, not by the pipeline.

`VulkanDevice` additions:
- `create_descriptor_set_layout()` — calls `vkCreateDescriptorSetLayout`.
- `create_pipeline_layout()` — calls `vkCreatePipelineLayout`.
- `create_descriptor_allocator()` — constructs `VulkanDescriptorAllocator`.

### Renderer material system (`crd-renderer`)

`material.hpp` + `material.cpp` (new files):
- `MaterialLayout` — wraps a `DescriptorSetLayout` for set 1 (per-material).
  Created once at startup, shared across frames and material instances.
  `create_instance(allocator)` allocates one `MaterialInstance` from the ring.
- `MaterialInstance` — wraps a single-frame `DescriptorSet`.
  `update_buffer(binding, buffer)` forwards to the underlying set.
  **Lifetime: one frame.** Do not hold across `begin_frame()`.

### Tests

10 new RHI-layer unit tests (in `tests/rhi/test_rhi.cpp`):
- ShaderStage bitmask composition and `has_stage()`.
- `DescriptorSetLayout` creation + binding count.
- `PipelineLayout` creation with set layouts and push constant ranges.
- `DescriptorAllocator` ring-buffer lifecycle (2 frames × begin + allocate).
- `DescriptorSet::update_buffer()` recording.
- `CommandBuffer::push_constants()` + `bind_descriptor_sets()` recording.

4 new material system tests (in `tests/renderer/test_renderer.cpp`):
- `MaterialLayout` creates `DescriptorSetLayout` on the device.
- `MaterialInstance` allocates from the ring allocator.
- `MaterialInstance::update_buffer()` forwards to the underlying set.
- Ring allocator `begin_frame()` advances the frame index correctly.

---

## Design: ring-buffer descriptor allocator

### Why not `vkFreeDescriptorSets` per set?

Individually freeing descriptor sets requires the pool to be created with
`VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`. This flag forces the driver to
maintain a free-list, which adds lock contention and fragmentation. Engines like Frostbite
and Unreal Engine avoid individual frees entirely for per-frame transient sets.

### Why not one pool with reset each frame?

A single pool reset means GPU frame N and CPU frame N+1 cannot overlap — you can only
reset the pool after the GPU has finished every submit that touched any set from it.
This forces a `vkDeviceWaitIdle`-style stall, killing pipelining.

### Ring-buffer pool (chosen approach)

`frames_in_flight` pools (default: 2). On `begin_frame(frame_index)`:

```
pool_index = frame_index % frames_in_flight
vkResetDescriptorPool(device, pools[pool_index], 0)
current_pool = pool_index
```

All `allocate()` calls in the current frame go to `pools[current_pool]`.

**Why is this safe?** When `begin_frame(N)` resets `pool[N % K]`, the GPU has finished
processing all submits from frame `(N - K)` — those are the submits that used sets from
that same pool slot. The fence-wait in `acquire_next_image()` guarantees this: you cannot
enter frame N until the fence for frame `(N - K)` has fired.

**Why zero pool exhaustion risk?** Pool capacity is pre-declared based on the maximum
number of sets/descriptors you expect in a single frame. The ring reset reclaims the
entire pool at once — you can never accumulate garbage from previous frames.

**What happens on reset?** All `VkDescriptorSet` handles allocated from that pool become
invalid. The `VulkanDescriptorSet` wrapper does not free them in its destructor (the
destructor is a no-op). Consumers must not hold `MaterialInstance` objects across
`begin_frame()` calls.

### Frequency layout

```
Set 0 = per-frame   (camera matrices, light buffer — lowest rebind frequency)
Set 1 = per-material (textures, material params — per visible material per frame)
Push  = per-draw     (model matrix, draw index — per draw call)
```

Low set index = lowest rebind frequency. Vulkan's `vkCmdBindDescriptorSets` can rebind
from set N without disturbing sets 0..N-1, so keeping long-lived bindings at low indices
minimises driver overhead. This matches the convention used in id Tech 7, Lumen, and
AMD's GDC presentations on bindless-forward rendering.

---

## Files changed

| File | Change |
|---|---|
| `engine/rhi/include/crd/rhi/types.hpp` | ShaderStage bitmask, descriptor types/structs, pipeline layout at end |
| `engine/rhi/include/crd/rhi/descriptor.hpp` | New — four abstract classes |
| `engine/rhi/include/crd/rhi/device.hpp` | Three new factory virtuals |
| `engine/rhi/include/crd/rhi/command_buffer.hpp` | `push_constants` + `bind_descriptor_sets` |
| `engine/rhi/include/crd/rhi/rhi.hpp` | Include `descriptor.hpp` |
| `engine/rhi-vulkan/src/vulkan_backend.cpp` | Four new Vulkan classes + pipeline changes |
| `engine/renderer/include/crd/renderer/material.hpp` | New — MaterialLayout + MaterialInstance |
| `engine/renderer/src/material.cpp` | New — material implementations |
| `engine/renderer/CMakeLists.txt` | Linked material.cpp |
| `tests/rhi/test_rhi.cpp` | 10 new tests + updated fakes |
| `tests/renderer/test_renderer.cpp` | 4 new material tests + updated fakes |
| `runtime/examples/smoke_renderer.cpp` | Updated fakes for new virtuals |
| `runtime/examples/smoke_rhi_api.cpp` | Updated fakes for new virtuals |

---

## Proposed commit message

```
feat(renderer): v1e+f push constants + descriptor system + material binding

Merge v1e (push constants + descriptor set RHI surface) and v1f (material
system) into one slice. ShaderStage promoted to bitmask; DescriptorSetLayout,
PipelineLayout, DescriptorSet, and ring-buffer DescriptorAllocator added to
RHI + Vulkan backend. MaterialLayout + MaterialInstance wrap the per-frame
allocation lifecycle. 14 new tests.
```
