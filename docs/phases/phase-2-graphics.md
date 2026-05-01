# Phase 2 — Graphics foundation

**Status:** 🚧 partially shipped (2.0 done, 2.1+ planned)

Vulkan-first, layered:

- **`crd-rhi`** — minimal API-agnostic GPU interface
- **`crd-rhi-vulkan`** — Vulkan backend
- **`crd-renderer`** — high-level rendering on top of RHI

## Slices

| Slice | Topic                            | Status | Notes                                                                                |
| :---: | -------------------------------- | :----: | ------------------------------------------------------------------------------------ |
| 2.0a  | `crd-rhi` interfaces             |   ✅   | scaffold; backend-agnostic, no Vulkan leak                                            |
| 2.0b  | `crd-rhi-vulkan` bootstrap       |   ✅   | instance / device / surface / swapchain                                               |
| 2.0c  | command buffers + frame sync     |   ✅   | pools, frames-in-flight, present/acquire, resize stability                            |
| 2.0d  | pipeline + first triangle        |   ✅   | minimal shader, pipeline, vertex buffer, draw — milestone gate passed                 |
| 2.1   | ImGui debug overlay              |   ✅   | docking branch; debug-only; consumes `crd-config` for theme/style                    |
| 2.2   | GPU memory + streaming           |   ✅   | centralized allocator-backed buffer/image path shipped; broader policy still ahead   |
| 2.3   | `crd-shader`                     |   ✅   | detailed design packet: `docs/phases/phase-2.3-shader.md`                            |
| 2.4   | `crd-renderer` v1                |   🚧   | v1a–b shipped; v1c–h planned (frame graph → IRenderPath → materials → depth → index) |
| 2.5   | `crd-jobs`                       |   ⏳   | thread pool + fiber tasks, work-stealing, per-frame allocator, async I/O ready       |
| 2.6   | `crd-resources` + `asset_cooker` |   ⏳   | async load, LRU, refcounted handles, runtime binary; cooker is a separate exe        |

## Near-term execution order

The concrete next-session sequence:

1. **`crd-shader` (2.3)** — grow from a proven triangle path
2. **`crd-renderer` v1 (2.4)**
3. **`crd-jobs` (2.5)** — pulled in once renderer needs async upload (may swap with 2.4)
4. **`crd-resources` + `asset_cooker` (2.6)**

## Decisions

- ADR-0008 — Graphics architecture (RHI / backend / renderer split)
- ADR-0009 — RHI v1a scaffold
- ADR-0010 — Vulkan bootstrap
- ADR-0011 — First triangle (dynamic rendering, classic submit/barrier)
- ADR-0013 — Asset pipeline (separate executable)
- ADR-0014 — Reference counting split
- ADR-0015 — Job system shape (thread pool + fibers)
- ADR-0016 — Render path strategy (Clustered Forward+ first)
- ADR-0017 — Culling strategy
- ADR-0024 — ImGui single-viewport default

## 2.3 detail

`crd-shader` now has its own dedicated design packet:

- `docs/phases/phase-2.3-shader.md`
- `docs/phases/phase-2.3-shader/survey.md`
- `docs/phases/phase-2.3-shader/api-envelope.md`

Treat this file as the broad graphics-phase umbrella and the 2.3 shader docs
as the detailed implementation-planning surface.

## 2.4 detail

`crd-renderer` system overview: `docs/systems/renderer.md`
Session history: `docs/sessions/2026-05-01-renderer-v1b-draw-execution.md`

### Renderer v1 slice table

| Slice | Topic                                               | Status | Notes |
| :---: | --------------------------------------------------- | :----: | ----- |
| v1a   | explicit renderables + camera + draw-item prep      |   ✅   | shipped 2026-05-01 |
| v1b   | real draw execution / pass orchestration            |   ✅   | shipped 2026-05-01 |
| v1c   | Frame graph v1 — typed handles, pass DAG, automatic barriers | ⏳ | ADR-0032; foundation for all render paths |
| v1d   | `IRenderPath` on top of frame graph + `FrameContext` rework | ⏳ | render path = a declared set of graph passes |
| v1e   | push constants + descriptor set RHI surface         |   ⏳   | `push_constants()`, `DescriptorSetLayout`, `DescriptorSet`, `bind_descriptor_sets()` |
| v1f   | material system v1                                  |   ⏳   | `MaterialLayout`, `Material`, `MaterialInstance`; render-path-agnostic |
| v1g   | `ForwardRenderPath` v1 — depth prepass + main color as frame graph passes | ⏳ | first real multi-pass path; ForwardRenderPath owns depth image |
| v1h   | index buffer + real mesh support                    |   ⏳   | `draw_indexed()`; `Renderable::index_buffer` |

### Architecture layers

```
Layer 0  RHI          API-agnostic GPU surface — backend swap point (Vulkan → DX12 / Metal)
Layer 1  Frame graph  typed resource handles, pass DAG, automatic barrier insertion, transient aliasing
Layer 2  IRenderPath  a collection of frame graph passes (Forward, Deferred, Forward+, ...)
Layer 3  Material     render-path-agnostic parameter binding (set 0 = per-material, push = per-draw)
Layer 4  crd-ui       retained-mode widget tree; rendered as a frame graph UI canvas pass (Phase 5)
```

### Architecture notes for v1c+

**Frame graph (v1c) — why it comes first:**
Without a frame graph, every render path manually manages resource lifetimes, image layout transitions,
and Vulkan barriers — that code would be duplicated in every `IRenderPath`. The frame graph centralises
all of that. Adding a new technique (SSAO, bloom, shadow maps, TAA) becomes declaring a new pass node
with typed resource inputs/outputs; the compiler inserts barriers and aliases transient images for free.

**`IRenderPath` contract (ADR-0032):**
- `IRenderPath` declares a set of frame graph passes each frame
- It owns its render targets (color, depth, G-buffer textures, etc.)
- `IRenderPath::resize(Extent2D)` recreates owned transient resources
- `IRenderPath::output_image()` returns the composited result (used for swapchain blit / present)
- `Renderer` builds and categorises the `DrawList`; `IRenderPath` owns everything after

**`DrawList` buckets and sort order:**
- `opaque` — front-to-back (Z ascending); minimises overdraw, feeds early-Z
- `masked` — front-to-back (same)
- `translucent` — back-to-front (Z descending); correct alpha compositing

**Descriptor set frequency convention:**
- Push constants → per-draw (model matrix + VP; max ~128 bytes)
- Set 0 → per-material (textures, material constant buffer)
- Set 1 → per-frame (camera, lights) — deferred until lights land

**`PipelineResolver` after v1d:**
Moves inside `IRenderPath` implementations. Each path owns its pipeline cache (map from `VariantKey`
→ `Pipeline*`). `PipelineResolver` as an injectable interface stays available for unit-test fakes.

**Render paths planned (all implement `IRenderPath` via frame graph passes):**
- `ForwardRenderPath` — depth prepass + main color pass (v1g)
- `ForwardPlusRenderPath` — + clustered light compute pass (v1i / Phase 2.5)
- `DeferredRenderPath` — G-buffer pass + lighting pass + composite (Phase 5.3a)
- `VisibilityBufferRenderPath` — triangle-ID pass + material evaluation (Phase 5.3b)

No frame graph in v1b. No frame graph means no transient aliasing, no automatic barriers — correct, but
not optimal. The frame graph replaces this from v1c onward.

Treat this file as the broad graphics umbrella and the renderer system/session
docs as the more detailed near-term renderer planning surface.

## Open questions

- Runtime scene binary format — FlatBuffers vs Cap'n Proto? Decided in
  Phase 3.1c, not here. Robotics RPC story leans Cap'n Proto; game ecosystem
  leans FlatBuffers.
