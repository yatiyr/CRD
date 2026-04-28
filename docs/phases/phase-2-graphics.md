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
| 2.3   | `crd-shader`                     |   ⏳   | GLSL→SPIR-V (glslang), reflection (SPIRV-Cross), on-disk cache, hot-reload           |
| 2.4   | `crd-renderer` v1                |   ⏳   | `IRenderPath` interface + Clustered Forward+ implementation; frustum culling; skybox |
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

## Open questions

- Runtime scene binary format — FlatBuffers vs Cap'n Proto? Decided in
  Phase 3.1c, not here. Robotics RPC story leans Cap'n Proto; game ecosystem
  leans FlatBuffers.
