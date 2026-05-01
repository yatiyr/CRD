# Phase 2 — Graphics foundation

**Status:** 🚧 active — 2.0–2.3 + renderer v1a–h shipped; v1i (swapchain blit) next

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
| 2.4   | `crd-renderer` v1                |   🚧   | v1a–h shipped; v1i next (swapchain blit + first full frame loop)                     |
| 2.5   | `crd-jobs`                       |   ⏳   | thread pool + fiber tasks, work-stealing, per-frame allocator, async I/O ready       |
| 2.6   | `crd-resources` + `asset_cooker` |   ⏳   | async load, LRU, refcounted handles, runtime binary; cooker is a separate exe        |

## Near-term execution order

1. **`crd-renderer` v1i** — `IRenderPath::output_image()` feeds swapchain blit; first renderable on screen
2. **`crd-jobs` (2.5)** — thread pool + fiber tasks; async pipeline compile, async upload
3. **`crd-resources` + `asset_cooker` (2.6)** — async load, LRU, refcounted handles, runtime binary

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
Session history: `docs/sessions/` (2026-05-01 entries; most recent: renderer-v1h-index-buffer)

### Renderer v1 slice table

| Slice | Topic                                               | Status | Notes |
| :---: | --------------------------------------------------- | :----: | ----- |
| v1a   | explicit renderables + camera + draw-item prep      |   ✅   | shipped 2026-05-01 |
| v1b   | real draw execution / pass orchestration            |   ✅   | shipped 2026-05-01 |
| v1c   | Frame graph v1 — typed handles, pass DAG, automatic barriers | ✅ | shipped 2026-05-01; ADR-0032 |
| v1d   | `IRenderPath` on top of frame graph + `FrameContext` rework | ✅ | shipped 2026-05-01 |
| v1e   | push constants + descriptor set RHI surface         |   ✅   | shipped 2026-05-01; `ShaderStage` bitmask, descriptor factory, ring-buffer allocator |
| v1f   | material system v1                                  |   ✅   | shipped 2026-05-01; `MaterialLayout` + `MaterialInstance` over ring allocator |
| v1g   | `ForwardRenderPath` v1 — depth prepass + main color as frame graph passes | ✅ | shipped 2026-05-01; `ForwardRenderPath` owns depth + color images; `PerFrameUbo` set 0 |
| v1h   | index buffer + real mesh support                    |   🚧   | `IndexType` enum; `bind_index_buffer()` + `draw_indexed()`; `Renderable::index_buffer` |
| v1i   | swapchain blit + output                             |   ⏳   | `IRenderPath::output_image()` feeds swapchain blit; first full frame loop |
| v1j   | GPU instancing                                      |   ⏳   | Phase 3.2 dep; see `docs/debt.md` §GPU instancing for exact change list |

### Architecture layers

```
Layer 0  RHI          API-agnostic GPU surface — backend swap point (Vulkan → DX12 / Metal)
Layer 1  Frame graph  typed resource handles, pass DAG, automatic barrier insertion, transient aliasing
Layer 2  IRenderPath  a collection of frame graph passes (Forward, Deferred, Forward+, ...)
Layer 3  Material     render-path-agnostic parameter binding (set 1 = per-material, push = per-draw)
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

**Descriptor set frequency convention (fixed from v1g onward):**
- Push constants → per-draw (model matrix; 64 bytes, fits Vulkan 128-byte minimum)
- Set 0 → per-frame (camera UBO: `PerFrameUbo`; Vertex|Fragment; lowest rebind frequency)
- Set 1 → per-material (textures, material params; per visible material per frame)

Note: Vulkan `vkCmdBindDescriptorSets` can rebind from set N without disturbing 0..N-1,
so keeping long-lived bindings at set 0 minimises driver overhead.

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
