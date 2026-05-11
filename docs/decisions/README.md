# Architecture Decision Records — Index

Each ADR is one file: `NNNN-short-slug.md`.
Status: Accepted / Superseded / Deprecated / Reserved.

> When adding a new ADR, give it the next free number, drop it in this
> folder, and add it to BOTH the tag index and the chronological table
> below. Reference it from the relevant phase file.

## By tag

### `[arch]`
- ADR-0008 — Graphics architecture
- ADR-0009 — RHI v1a scaffold
- ADR-0012 — Config substrate
- ADR-0013 — Asset pipeline
- ADR-0015 — Job system shape
- ADR-0016 — Render path strategy
- ~~ADR-0018 — Physics architecture~~ — **superseded by ADR-0062**
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0021 — Animation architecture
- ADR-0023 — UI architecture
- ADR-0058 — Öbek system
- ADR-0059 — Preset system
- ADR-0060 — Profile system
- ADR-0061 — Async GPU upload contract
- ADR-0062 — **Eylem: Cerid-native physics architecture** (supersedes ADR-0018)
- ADR-0063 — Eylem determinism contract
- ADR-0064 — **`crd-sdf` substrate architecture**
- ADR-0065 — **`crd-hesap` numerical computing substrate (MATLAB-class)**
- ADR-0066 — **`crd-draw` substrate architecture**

### `[draw]`
- ADR-0066 — `crd-draw` substrate architecture (peer module; retained `RenderBuffer` + immediate-mode API; vertex-shader quad-expanded AA lines + sort-by-centroid translucent solids; 3 depth modes; per-component visualizer plug-in registry; ImGui projection day-one text + reserved SDF text; consumed by eylem / sdf / audio / nav / editor / renderer / sandbox)

### `[sdf]`
- ADR-0064 — `crd-sdf` substrate architecture (analytic + dense + narrow-band + CSG; mesh→SDF baker via Jacobson 2013 generalised winding number; CPU first, GPU 3D-texture path; consumed by eylem / font / renderer / audio / editor)

### `[hesap]` `[math]` `[solvers]` `[autodiff]` `[opt]` `[ode]` `[fft]` `[dsp]`
- ADR-0065 — `crd-hesap` numerical computing substrate (MATLAB-class; dense + sparse + iterative + direct + eig + opt + ODE + FFT + DSP + stats + tensor + autodiff + GPU + REPL; consumed by eylem / audio / robotics / medical / cinematic / DAW / scientific tool)

### `[build]` `[lang]`
- ADR-0001 — Build & language

### `[log]`
- ADR-0002 — Logging

### `[memory]`
- ADR-0003 — Memory v1
- ADR-0014 — Reference counting split
- ADR-0022 — Streaming pipeline

### `[containers]`
- ADR-0004 — Containers v1

### `[math]`
- ADR-0005 — Math v1 (`crd-math` lean primitive layer — Vec/Mat/Quat/Transform + SIMD wrappers + deterministic stdlib)
- ADR-0065 — `crd-hesap` numerical computing substrate (heavy LA + solvers + autodiff + DSP + stats; peer module, NOT inside `crd-math`)

### `[platform]`
- ADR-0006 — Platform v1
- ADR-0041 — `crd-platform` async filesystem I/O

### `[app]` `[event]`
- ADR-0007 — `crd-app` shape

### `[rhi]` `[vulkan]`
- ADR-0008 — Graphics architecture
- ADR-0009 — RHI v1a scaffold
- ADR-0010 — Vulkan bootstrap
- ADR-0011 — First triangle
- ADR-0061 — Async GPU upload contract (adds `crd::rhi::Fence` + non-waiting `Queue::submit(cmd, fence)`)

### `[config]`
- ADR-0012 — Config substrate

### `[resources]`
- ADR-0013 — Asset pipeline
- ADR-0014 — Reference counting split
- ADR-0022 — Streaming pipeline
- ADR-0036 — `crd-resources` module placement + loader-registry pattern
- ADR-0037 — ResourceId hybrid UUID scheme
- ADR-0038 — Cooked binary container format
- ADR-0039 — `ResourceHandle<T>` semantics
- ADR-0040 — Cooker CLI + CMake integration
- ADR-0042 — Texture cooked format + GPU upload strategy
- ADR-0043 — MeshResource vertex layout + glTF import scope

### `[cooker]`
- ADR-0040 — Cooker CLI + CMake integration
- ADR-0042 — Texture cooked format + GPU upload strategy
- ADR-0043 — MeshResource vertex layout + glTF import scope
- ADR-0055 — Scene serialization: TOML authoring + SCEN CRDR cooked
- ADR-0058 — Öbek system
- ADR-0059 — Preset system
- ADR-0060 — Profile system

### `[jobs]`
- ADR-0015 — Job system shape
- ADR-0033 — crd-jobs implementation architecture (fibers, asm switch, Chase-Lev, SBO, ABA-safe counters)

### `[scripting]`
- ADR-0034 — C++ hot-reload DLL scripting as primary scripting mechanism
- ADR-0056 — Scene/ECS L6–L8: Reserved API slots (ScriptComponent slot)

### `[networking]` `[determinism]`
- ADR-0035 — Networking architecture principles (layered, determinism-first)
- ADR-0056 — Scene/ECS L6–L8: Reserved API slots (Replication slot)

### `[renderer]` `[render-path]`
- ADR-0016 — Render path strategy
- ADR-0032 — Frame graph v1
- ADR-0042 — Texture cooked format + GPU upload strategy
- ADR-0043 — MeshResource vertex layout + glTF import scope
- ADR-0044 — Phase ordering: material PSO/variant completion precedes scene/ECS
- ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy
- ADR-0047 — Font rendering system (MTSDF shader, billboard text, Surface domain)
- ADR-0048 — Material system architecture foundation (two-tier Template/Instance, surface function, MATR format, ShaderOptions)
- ADR-0061 — Async GPU upload contract (`UploadHandle` + per-module polling system)

### `[culling]`
- ADR-0017 — Culling strategy

### `[physics]` `[eylem]`
- ~~ADR-0018 — Physics architecture~~ — **superseded by ADR-0062**
- ADR-0062 — Eylem: Cerid-native physics architecture
- ADR-0063 — Eylem determinism contract
- ADR-0064 — `crd-sdf` substrate (eylem consumes for mesh colliders + closest-point; v3 XPBD uses SDF environment colliders)
- ADR-0065 — `crd-hesap` substrate (eylem v7 FEM refactors to consume sparse PCG + sparse Cholesky once `crd-hesap` ships; eylem v9 differentiable refactors to consume reverse-mode autodiff)

### `[scene]` `[ecs]`
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0044 — Phase ordering: material PSO/variant completion precedes scene/ECS
- ADR-0049 — Scene/ECS L1: Entity identity & SlotMap
- ADR-0050 — Scene/ECS L2: Storage backends (Archetype + SparseSet hybrid)
- ADR-0051 — Scene/ECS L3: Relations as first-class
- ADR-0052 — Scene/ECS L4: Query · System · Schedule
- ADR-0053 — Scene/ECS L5: Component index slot framework
- ADR-0054 — Scene/ECS: Transform hierarchy update model
- ADR-0055 — Scene serialization: TOML authoring + SCEN CRDR cooked
- ADR-0056 — Scene/ECS L6–L8: Reserved API slots (Replication, Scripts, Reflection)
- ADR-0057 — Scene/ECS: UI nodes in scene tree (boundary declaration)
- ADR-0058 — Öbek system: cooked entity-graph templates with composition, variation, AAAA-tier future-proofing
- ADR-0059 — Preset system: typed system-config bags with five-layer resolution
- ADR-0060 — Profile system: typed predicate selectors with additive composition

### `[obek]` `[prefab]`
- ADR-0058 — Öbek system: cooked entity-graph templates with composition, variation, AAAA-tier future-proofing

### `[preset]` `[profile]`
- ADR-0059 — Preset system: typed system-config bags with five-layer resolution
- ADR-0060 — Profile system: typed predicate selectors with additive composition

### `[async]` `[upload]`
- ADR-0014 — Reference counting split (resource handle async-load substrate)
- ADR-0022 — Open-world streaming pipeline (forward-looking)
- ADR-0039 — `ResourceHandle<T>` semantics (CPU-side async load)
- ADR-0053 — Component index slot framework (`AsyncAwareIndex` consumer-facing surface)
- ADR-0061 — Async GPU upload contract (closes the design half of the GPU-side polling protocol)

### `[sandbox]` `[build]`
- ADR-0045 — Sandbox executable, asset layout, cook workflow, crd-meshgen

### `[meshgen]`
- ADR-0045 — Sandbox executable, asset layout, cook workflow, crd-meshgen

### `[post-fx]` `[rt]`
- ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy

### `[font]` `[text]`
- ADR-0047 — Font rendering system (MTSDF, FreeType+msdfgen, HarfBuzz, offline+dynamic atlas, extruded text)
- ADR-0064 — `crd-sdf` substrate (font consumes for MTSDF baker + sampler patterns)

### `[animation]`
- ADR-0021 — Animation architecture

### `[ui]` `[node-editor]`
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0023 — UI architecture
- ADR-0047 — Font rendering system (crd-font, crd-ui dependency)
- ADR-0057 — Scene/ECS: UI nodes in scene tree (boundary declaration)

### `[imgui]` `[vulkan]`
- ADR-0024 — ImGui single-viewport default

### `[shader]` `[cache]` `[reflection]`
- ADR-0025 — Shader mechanism policy
- ADR-0026 — Shader variant key
- ADR-0027 — Shader reflection consumption model
- ADR-0028 — Shader cache hierarchy
- ADR-0029 — Shader hot reload
- ADR-0030 — Shader / PSO boundary
- ADR-0031 — Shader frontend → IR seam
- ADR-0048 — Material system architecture foundation (ShaderOptions, inline functor, ParameterType)

## All ADRs (chronological)

| ID    | Title                                          | Tags                              | Status   |
| ----- | ---------------------------------------------- | --------------------------------- | -------- |
| 0001  | Build & language                               | build, lang                       | Accepted |
| 0002  | Logging                                        | log                               | Accepted |
| 0003  | Memory v1                                      | memory                            | Accepted |
| 0004  | Containers v1                                  | containers                        | Accepted |
| 0005  | Math v1                                        | math                              | Accepted |
| 0006  | Platform v1                                    | platform                          | Accepted |
| 0007  | `crd-app` shape                                | app, event                        | Accepted |
| 0008  | Graphics architecture                          | rhi, vulkan, arch                 | Accepted |
| 0009  | RHI v1a scaffold                               | rhi, arch                         | Accepted |
| 0010  | Vulkan bootstrap                               | vulkan, rhi                       | Accepted |
| 0011  | First triangle milestone                       | vulkan, rhi, renderer             | Accepted |
| 0012  | Configuration substrate                        | config, arch                      | Accepted |
| 0013  | Asset pipeline                                 | resources, arch                   | Accepted |
| 0014  | Reference counting split                       | memory, resources                 | Accepted |
| 0015  | Job system shape                               | jobs, arch                        | Accepted |
| 0016  | Render path strategy                           | renderer, render-path, arch       | Accepted |
| 0017  | Culling strategy                               | culling, renderer                 | Accepted |
| 0018  | Physics architecture                           | physics, arch                     | **Superseded by 0062** |
| 0019  | (reserved)                                     | —                                 | Reserved |
| 0020  | Scene & ECS hybrid + UI in scene tree          | scene, ecs, ui, arch              | Accepted |
| 0021  | Animation architecture                         | animation, arch                   | Accepted |
| 0022  | Open-world streaming pipeline                  | memory, resources                 | Accepted |
| 0023  | UI architecture                                | ui, node-editor, arch             | Accepted |
| 0024  | ImGui single-viewport default                  | imgui, ui, vulkan                 | Accepted |
| 0025  | Shader mechanism policy                        | shader, renderer, arch            | Accepted    |
| 0026  | Shader variant key                             | shader, cache, arch               | Accepted    |
| 0027  | Shader reflection consumption model            | shader, reflection, rhi           | Accepted    |
| 0028  | Shader cache hierarchy                         | shader, cache, vulkan             | Accepted    |
| 0029  | Shader hot reload                              | shader, hot-reload, runtime       | Accepted    |
| 0030  | Shader / PSO boundary                          | shader, rhi, renderer             | Accepted    |
| 0031  | Shader frontend → IR seam                      | shader, arch, ir                  | Accepted    |
| 0032  | Frame graph v1                                 | renderer, render-path, arch       | Accepted    |
| 0033  | crd-jobs implementation architecture           | jobs, arch, fibers, threading     | Accepted    |
| 0034  | C++ hot-reload DLL scripting                   | scripting, arch, extensibility    | Accepted    |
| 0035  | Networking architecture principles             | networking, arch, determinism     | Accepted    |
| 0036  | `crd-resources` module + loader registry       | resources, arch                   | Accepted    |
| 0037  | ResourceId hybrid UUID scheme                  | resources, arch                   | Accepted    |
| 0038  | Cooked binary container format                 | resources, arch, cooker           | Accepted    |
| 0039  | `ResourceHandle<T>` semantics                  | resources, arch                   | Accepted    |
| 0040  | Cooker CLI + CMake integration                 | resources, cooker, build, arch    | Accepted    |
| 0041  | `crd-platform` async filesystem I/O            | platform, resources, jobs         | Accepted    |
| 0042  | Texture cooked format + GPU upload strategy    | resources, renderer, cooker       | Accepted    |
| 0043  | MeshResource vertex layout + glTF import scope | resources, renderer, cooker       | Accepted    |
| 0044  | Phase ordering: material PSO/variant before scene/ECS | arch, renderer, scene, resources | Accepted |
| 0045  | Sandbox executable, asset layout, cook workflow, crd-meshgen | arch, sandbox, resources, cooker, build | Accepted |
| 0046  | MaterialDomain enum, node-editor future-proofing, RT hybrid strategy | arch, renderer, shader, materials, rt | Accepted |
| 0047  | Font rendering system (MTSDF, FreeType+msdfgen, HarfBuzz, offline+dynamic atlas, extruded text) | arch, font, renderer, ui, text | Accepted |
| 0048  | Material system architecture foundation (two-tier Template/Instance, surface fn, MATR chunks, ShaderOptions, ParameterType) | arch, renderer, shader, materials, resources, cooker | Accepted |
| 0049  | Scene/ECS L1: Entity identity & SlotMap                | scene, ecs, arch, layer-1               | Accepted |
| 0050  | Scene/ECS L2: Storage backends (Archetype + SparseSet hybrid) | scene, ecs, arch, layer-2, performance | Accepted |
| 0051  | Scene/ECS L3: Relations as first-class                 | scene, ecs, arch, layer-3, relations    | Accepted |
| 0052  | Scene/ECS L4: Query · System · Schedule                | scene, ecs, arch, layer-4, query, scheduler | Accepted |
| 0053  | Scene/ECS L5: Component index slot framework           | scene, ecs, arch, layer-5, indexes, extensibility | Accepted |
| 0054  | Scene/ECS: Transform hierarchy update model            | scene, ecs, math, performance           | Accepted |
| 0055  | Scene serialization: TOML authoring + SCEN CRDR cooked | scene, ecs, resources, cooker, arch     | Accepted |
| 0056  | Scene/ECS L6–L8: Reserved API slots (Replication, Scripts, Reflection) | scene, ecs, arch, layer-6, layer-7, layer-8, networking, scripting, editor | Accepted |
| 0057  | Scene/ECS: UI nodes in scene tree (boundary declaration) | scene, ecs, ui, arch                  | Accepted |
| 0058  | Öbek system: cooked entity-graph templates with composition, variation, AAAA-tier future-proofing | scene, ecs, cooker, resources, arch, renderer, networking, determinism, obek, prefab | Accepted |
| 0059  | Preset system: typed system-config bags with five-layer resolution | scene, resources, cooker, arch, renderer, audio, physics, input, config, preset | Accepted |
| 0060  | Profile system: typed predicate selectors with additive composition | scene, resources, cooker, arch, config, networking, app, profile | Accepted |
| 0061  | Async GPU upload contract: `UploadHandle` + per-module polling system | arch, renderer, rhi, scene, resources, async | Accepted |
| 0062  | **Eylem: Cerid-native physics architecture** (supersedes 0018) | arch, physics, eylem, ecs, jobs, simd, determinism | Accepted |
| 0063  | Eylem determinism contract                     | arch, physics, eylem, determinism, ci, fp | Accepted |
| 0064  | `crd-sdf` substrate architecture               | arch, sdf, eylem, renderer, font, audio, editor, resources | Accepted |
| 0065  | `crd-hesap` numerical computing substrate (MATLAB-class) | arch, hesap, math, solvers, autodiff, opt, ode, fft, dsp, scripting | Accepted |
| 0066  | `crd-draw` substrate architecture        | arch, draw, eylem, sdf, audio, renderer, editor, resources | Accepted |
| 0067  | Eylem force-field architecture (three-tier substrate)        | arch, physics, eylem, fields, sdf, draw, ecs, obek, determinism | Accepted |
| 0068  | Eylem body types + collision filtering + callbacks (3 motion types + sensor + 5-tier filter + deferred ECS events + ContactModify) | arch, physics, eylem, collision, filtering, callbacks, ecs, determinism | Accepted |
| 0069  | Eylem materials substrate (friction + restitution + surface velocity + density)  | arch, physics, eylem, materials, friction, restitution                       | Accepted  |
| 0070  | Eylem solver catalog + selection guidance (incl. Nonsmooth Newton)              | arch, physics, eylem, solvers                                                 | Planned   |
| 0071  | Robotics importers (URDF / SDF / MJCF) + actuator catalogue                       | arch, physics, eylem, robotics, importers, actuators, urdf, sdf, mjcf         | Planned   |
| 0072  | Eylem sensor substrate (IMU / LIDAR / proximity / threshold events / diagnostics) | arch, physics, eylem, sensors, robotics                                       | Planned   |
| 0073  | Eylem aerospace substrate (variable mass + aero + atm + propulsion + J2 + sep)    | arch, physics, eylem, aerospace, aero, atmosphere, propulsion, fields         | Planned   |
| 0074  | Eylem cinematic / animation-physics bridge (`crd-eylem-cine` module)              | arch, physics, eylem, cinematic, animation, film                              | Planned   |
| 0075  | Eylem testing rigor + conservation-law CI                                          | arch, physics, eylem, testing, ci, conservation, scientific-computing        | Accepted  |
| 0076  | `crd-geometry` substrate (BVH + GJK/EPA + mesh queries + polygon ops + Delaunay)  | arch, substrate, computational-geometry, bvh, gjk-epa, mesh-processing, determinism | Accepted  |
