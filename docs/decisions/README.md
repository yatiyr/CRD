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
- ADR-0018 — Physics architecture
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0021 — Animation architecture
- ADR-0023 — UI architecture

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
- ADR-0005 — Math v1

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

### `[cooker]`
- ADR-0040 — Cooker CLI + CMake integration

### `[jobs]`
- ADR-0015 — Job system shape
- ADR-0033 — crd-jobs implementation architecture (fibers, asm switch, Chase-Lev, SBO, ABA-safe counters)

### `[scripting]`
- ADR-0034 — C++ hot-reload DLL scripting as primary scripting mechanism

### `[networking]` `[determinism]`
- ADR-0035 — Networking architecture principles (layered, determinism-first)

### `[renderer]` `[render-path]`
- ADR-0016 — Render path strategy
- ADR-0032 — Frame graph v1

### `[culling]`
- ADR-0017 — Culling strategy

### `[physics]`
- ADR-0018 — Physics architecture

### `[scene]` `[ecs]`
- ADR-0020 — Scene & ECS hybrid + UI in scene tree

### `[animation]`
- ADR-0021 — Animation architecture

### `[ui]` `[node-editor]`
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0023 — UI architecture

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
| 0018  | Physics architecture                           | physics, arch                     | Accepted |
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
