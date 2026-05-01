# ADR-0032 — Frame graph v1

**Date:** 2026-05
**Status:** Accepted
**Tags:** [renderer] [arch] [render-path]

## Context

`crd-renderer` v1a–b ships a flat single-pass execution loop. Every resource
lifetime, image layout transition, and Vulkan barrier is managed manually at
the call site. As render paths grow (depth prepass, shadow maps, SSAO, bloom,
deferred G-buffer, TAA), this manual management duplicates per technique and
per render path — it becomes unmaintainable and error-prone.

Professional engines solve this with a **frame graph** (Frostbite), **Render
Dependency Graph** (Unreal), or **pass system** (O3DE): passes declare typed
resource inputs and outputs; a compiler resolves execution order, inserts
GPU barriers, and aliases transient resource memory.

## Decision

Cerid introduces a frame graph layer between the RHI and `IRenderPath`:

### Pass declaration

Each pass is declared with typed resource handles:

```cpp
// Example — not final API, illustrates intent
PassBuilder& builder = frame_graph.add_pass("depth-prepass");
auto depth_handle  = builder.write(depth_tex, ImageAccess::DepthWrite);
auto gbuf_handle   = builder.write(gbuf_tex,  ImageAccess::ColorWrite);
builder.set_execute([=](FrameResources& res, CommandBuffer& cmd) {
    // record draw calls here; barriers already inserted by compiler
});
```

### Compiler responsibilities

1. Topological sort of passes by declared resource dependencies.
2. Barrier/layout-transition insertion at pass boundaries (Vulkan: image layout
   transitions + pipeline stage + access masks).
3. Transient resource aliasing: resources whose lifetimes don't overlap share
   the same backing allocation (reduces VRAM).
4. Culling of passes whose outputs are never read (dead-pass elimination).

### Transient vs external resources

- **Transient** — created by the frame graph, valid only within one frame
  (depth targets, G-buffer textures, intermediate ping-pong buffers).
- **External** — imported into the graph (swapchain image, persistent shadow
  map atlas, persistent GPU buffers). The graph tracks their usage but does
  not own their memory.

### `IRenderPath` contract on top of the frame graph

- `IRenderPath::build(FrameGraph&, const DrawList&)` — declares all passes
  for this frame; does not record commands yet.
- `IRenderPath::output_image()` — returns the external handle of the
  composited output (used for swapchain blit).
- `IRenderPath::resize(Extent2D)` — clears cached transient descriptors;
  frame graph re-creates transient allocations next build.

The `Renderer` calls `build_frame()` to produce a `DrawList`, then calls
`render_path.build(frame_graph, draw_list)`, then `frame_graph.execute()`.

### v1 scope

The v1 frame graph is intentionally minimal:

- Typed image and buffer resource handles.
- Linear pass ordering (no multi-queue parallelism yet).
- Automatic Vulkan image layout transitions at pass boundaries.
- Transient image aliasing (buffer aliasing is a v2 concern).
- Dead-pass culling.

Not in v1: multi-queue submission, async compute overlap, split barriers,
subpass merging. These are Phase 5 concerns when real workloads demand them.

## Alternatives considered

**Manual barriers per render path** — rejected. Every render path and every
new technique (SSAO, TAA, bloom) repeats the same barrier logic. The barrier
book-keeping grows with the feature set; it is a correctness liability.

**Full frame graph from day one (multi-queue, async compute)** — rejected.
Over-engineering for current scope. The v1 frame graph's API is designed to
accommodate async compute later without breaking callers.

## References

- `docs/phases/phase-2-graphics.md` (2.4 detail — v1c)
- ADR-0016 — Render path strategy
- ADR-0030 — Shader / PSO boundary
- Frostbite "FrameGraph: Extensible Rendering Architecture" (GDC 2017)
- O3DE RPI pass system documentation
