# Session: `crd-renderer` v1h — Index Buffer Support

**Date:** 2026-05-01
**Slice:** Phase 2.4h
**Status:** shipped

## What was built

Full indexed draw support from the RHI surface down to `ForwardRenderPath` dispatch.
Backward compatible: existing non-indexed `Renderable` submissions are unaffected.

## RHI changes

### `engine/rhi/include/crd/rhi/types.hpp`

Added `IndexType` enum after `PrimitiveTopology`:

```cpp
enum class IndexType : crd::u8 { Uint16, Uint32 };
```

### `engine/rhi/include/crd/rhi/command_buffer.hpp`

Two new pure virtuals:

```cpp
virtual void bind_index_buffer(Buffer& buffer, crd::u64 offset_bytes, IndexType type) = 0;
virtual void draw_indexed(crd::u32 index_count, crd::u32 first_index, crd::i32 vertex_offset) = 0;
```

`draw_indexed` is non-instanced (`instance_count` hardwired to 1 at the Vulkan call site).
Instanced variants arrive in Phase 3.2 — see `docs/debt.md` §GPU instancing.

### `engine/rhi-vulkan/src/vulkan_backend.cpp`

`VulkanCommandBuffer` implements both:

- `bind_index_buffer` — maps `IndexType::Uint16` → `VK_INDEX_TYPE_UINT16`, `Uint32` → `VK_INDEX_TYPE_UINT32`; calls `vkCmdBindIndexBuffer`.
- `draw_indexed` — calls `vkCmdDrawIndexed(cmd, index_count, 1, first_index, vertex_offset, 0)`.

## Renderer changes

### `engine/renderer/include/crd/renderer/renderer.hpp`

`Renderable` gains three fields (all defaulted for backward compat):

```cpp
crd::rhi::Buffer*   index_buffer  = nullptr; // null → non-indexed draw
crd::u32            index_count   = 0;
crd::rhi::IndexType index_type    = crd::rhi::IndexType::Uint32;
```

`DrawItem` mirrors the same three fields.

### `engine/renderer/src/renderer.cpp`

`build_frame` additions:
- Validation: `index_buffer != nullptr && index_count == 0` → return false.
- Copy: `index_buffer`, `index_count`, `index_type` forwarded from `Renderable` to `DrawItem`.

### `engine/renderer/src/forward_render_path.cpp`

Both the depth-prepass loop and the color-pass `draw_items` lambda now dispatch:

```cpp
if (item.index_buffer)
{
    cmd.bind_index_buffer(*item.index_buffer, 0, item.index_type);
    cmd.draw_indexed(item.index_count, 0, 0);
}
else
{
    cmd.draw(item.vertex_count, 0);
}
```

## Fake CommandBuffer updates

All four `CommandBuffer` fakes updated with `bind_index_buffer` + `draw_indexed` overrides:

- `tests/renderer/test_renderer.cpp` — counters: `bind_index_buffer_count`, `draw_indexed_count`, `last_index_count`, `last_index_type`
- `tests/rhi/test_rhi.cpp` — counters + `last_first_index`, `last_vertex_offset`
- `runtime/examples/smoke_renderer.cpp` — logs the calls
- `runtime/examples/smoke_rhi_api.cpp` — logs index_count, first_index, vertex_offset

## Tests

4 new unit tests in `tests/renderer/test_renderer.cpp`:

| Test | Tags |
|------|------|
| `build_frame copies index fields from Renderable into DrawItem` | `[renderer][index]` |
| `build_frame rejects indexed renderable with zero index_count` | `[renderer][index]` |
| `ForwardRenderPath dispatches draw_indexed for indexed items` | `[renderer][forward][index]` |
| `ForwardRenderPath mixes indexed and non-indexed items in color pass` | `[renderer][forward][index]` |

## Instancing documentation

`docs/debt.md` — new section "GPU instancing (planned Phase 3.2)" documents:
- Exact RHI additions needed (`draw_instanced`, `draw_indexed_instanced`)
- Renderer changes (`instance_count` field, dispatch logic)
- Phase 3 dependency (stable GPU scene buffer / instance data layout)
- Hard gate: do not add `instance_count` to `Renderable` before Phase 3.1 ships

`docs/phases/phase-2-graphics.md` — added v1j slice entry pointing to the debt doc.

## Quality pass

Three-flavour green: debug 257/257, release 256/256, asan 257/257.
