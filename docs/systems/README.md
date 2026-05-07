# Systems

One short overview per shipped engine module. Plain English. "What is it,
what does it do, how do I use it." Read this folder to remember what the
engine *is*; read `docs/sessions/` to remember how it got that way.

| System | Status | Overview |
| ------ | ------ | -------- |
| `crd-core`       | ✅        | [core.md](core.md) |
| `crd-log`        | ✅        | [log.md](log.md) — deep-dive: [`docs/log/LOG_FILE.md`](../log/LOG_FILE.md) |
| `crd-memory`     | ✅        | [memory.md](memory.md) — deep-dive: [`docs/memory/MEMORY_FILE.md`](../memory/MEMORY_FILE.md) |
| `crd-containers` | ✅        | [containers.md](containers.md) — deep-dive: [`docs/containers/CONTAINERS_FILE.md`](../containers/CONTAINERS_FILE.md) |
| `crd-math`       | ✅        | [math.md](math.md) |
| `crd-platform`   | ✅        | [platform.md](platform.md) |
| `crd-app`        | ✅        | [app.md](app.md) |
| `crd-config`     | ✅ (1.6a) | [config.md](config.md) — 1.6b reload hook planned |
| `crd-imgui`      | ✅        | [imgui.md](imgui.md) — debug-only overlay, docking branch |
| `crd-rhi`        | ✅        | [rhi.md](rhi.md) — Vulkan backend; descriptor system; index buffer |
| `crd-shader`     | ✅ (2.3g) | [shader.md](shader.md) — GLSL ingest, SPIR-V, reflection, hot-reload, pipeline handoff |
| `crd-renderer`   | 🚧 (v1i) | [renderer.md](renderer.md) — frame graph, IRenderPath, material system, ForwardRenderPath, swapchain blit; per-material PSO cache + depth prepass added in Phase 2.8 |
| `crd-jobs`       | ✅ (v1k) | [jobs.md](jobs.md) — fiber job system; run/wait/parallel_for; SBO lambdas; frame allocator |
| `crd-resources`  | ✅ (v1g) | [resources.md](resources.md) — handle table, sync/async/streamed loading, 2Q LRU eviction, hot-reload, asset cooker (CRDR), texture + mesh + material loaders |
| `crd-meshgen`    | ✅        | [meshgen.md](meshgen.md) — procedural geometry primitives (cube, sphere, cylinder, cone, plane, capsule, torus, icosphere) producing standard 48B vertex layout |
| `crd-sandbox`    | ✅        | [sandbox.md](sandbox.md) — interactive desktop app: orbit camera, ForwardRenderPath, unified Asset Browser (procedural shapes + cooked glTF imports), ImGui overlay |
| `crd-scene`      | 🚧 (v1b) | [scene.md](scene.md) — Phase 3.0 foundation: `EntityId`/`SlotMap`/`World`, `ComponentRegistry` + variadic trait grammar, `IStorageBackend` interface (declared). 8-layer slot architecture; v1c–v1n in flight |

When a new module ships, add a row here and link to its overview. If a module
gets a long-form deep-dive document (like `LOG_FILE.md`), link to that too —
the overview here should stay short and stable.
