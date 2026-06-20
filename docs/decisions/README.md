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
- ADR-0079 — **`crd-perf` profiler substrate + `crd-perf-ui` ImGui frontend** (region timing + jobs auto-instrument + GPU timestamps + memory tracking + CPROF v1 capture format + 7-panel ImGui UI)
- ADR-0080 — **`crd-rhi-compute` substrate** (Proposed; Phase 3.1.7.6 prerequisite for v9 GPU geometry; additive RHI extension: IComputePipeline + IStorageBuffer + dispatch + compute↔graphics sync + opt-in async compute queue + shaderc compute pipeline)

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
- ADR-0080 — **`crd-rhi-compute` substrate** (Phase 3.1.7.6 prerequisite for v9; additive RHI extension: IComputePipeline + IStorageBuffer + dispatch + compute↔graphics sync + opt-in async compute + shaderc compute pipeline)

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
| 0065  | `crd-hesap` numerical computing substrate (MATLAB-class); §13-§16 v0/v1/v2 locks; §17-§25 v3 dense SVD+eig+lstsq close; **§26 amendment 2026-05-27 — v4 iterative-solvers + preconditioners + AMG cluster CLOSED** (D(iter)-1..10 determinism moat + reorder default-ON + graceful-degrade + O(1) AMD bucket-head + the β=0.3 quantified result) | arch, hesap, math, solvers, autodiff, opt, ode, fft, dsp, scripting | Accepted |
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
| 0076  | `crd-geometry` substrate (BVH + GJK/EPA + mesh queries + polygon ops + Delaunay + decomposition + GPU-LBVH + shader-helpers); §19-§21 amendments closed `-mesh` / `-spatial` / `-polygon` clusters 2026-05-16; §22 amendment 2026-05-17 — `-mesh-processing` v7 cluster CLOSED (8 algorithm slices); §23 amendment 2026-05-17 — `-delaunay` v8 cluster CLOSED (11 algorithm slices incl. cospherical Stage D insphere_exact paydown); §24 amendment 2026-05-18 — `-decomposition` v9c cluster CLOSED (V-HACD voxelize + decompose); §25 amendment 2026-05-18 — **`-gpu` v9a LBVH cluster CLOSED** (10 algorithm slices: 30-bit Morton CPU+GPU + 60-bit Morton CPU+GPU + typed wrappers + async-compute pool + CPU radix + GPU radix + scalar+prefetch + parallel-via-jobs + LBVH tree+upsweep elite-combine; locks D132-D164 / 33 decisions); §26 amendment 2026-05-19 — **`-shader-helpers` v9e cluster CLOSED** (4 algorithm slices + close: formula-IR flat 3-array storage + GLSL backend + ULP-conformance GPU dispatch + HLSL backend + dxc → SPIR-V GPU verification + cooker library-API; locks D166-D181 / 16 decisions; substrate-side `crd::shader::compile_hlsl` shipped); §27 amendment 2026-05-19 — **`-curves` v10 cluster CLOSED** (6 slices: substrate + sampling + arc-length + queries + frames-viz-sandbox + typed-boundary; locks D182-D216 / 35 decisions; Wang 2008 RMF + uniform closure-twist; typed `queries_typed.hpp` covers WHOLE v10 surface per ADR-0078 §5 D34); §28 amendment 2026-05-19 — **v11 transform-aware + PHASE 3.1.7 FULLY CLOSED** (TransformedShape composition wrapper with trait-based scalar deduction + 14 3D + 7 2D shape transforms + `transform_*_typed` boundary covering FULL primitive catalog; D217-D233 / 17 decisions; 5 advisor-pinned discriminators); **🎉 Phase 3.1.7 substrate FULLY CLOSED 2026-05-19 — 12 of 11 sub-modules complete** | arch, substrate, computational-geometry, bvh, gjk-epa, mesh-processing, spatial-acceleration, polygon-ops, cdt, decomposition, gpu-lbvh, shader-helpers, sdf-cooker, determinism | Accepted  |
| 0077  | Multi-domain substrate expansion (9 new peer modules + Phase 3.5 prologue + Phase 6 platform expansion)  | arch, strategy, multi-domain, manufacturing, cad, cfd, aerospace, ml, scientific-computing | Accepted  |
| 0078  | `crd-units` substrate (dimensional types + 6-layer conversion system); §2 v0b adoption A; §3 v0c adoption B; §4 v0d adoption C + Phase 3.1.7.5 CLOSE; §5 amendment 2026-05-16 — **two-layer typed architecture** (D32-D36: units at API surface, raw scalars in inner loop; boundary is the API surface and only there; bridges = `.value` / `to_raw_vec` / `from_raw_vec` / strip-compute-retag wrappers) | arch, substrate, units, dimensional-analysis, type-safety, determinism, physics, eylem, geometry, format-parse, ui, architecture-principle | Accepted  |
| 0079  | `crd-perf` profiler substrate + `crd-perf-ui` ImGui frontend (D-003 v0a-v0h)       | arch, substrate, perf, profiler, instrumentation, gpu-timing, capture-format, ui            | Accepted  |
| 0080  | `crd-rhi-compute` substrate (Phase 3.1.7.6 v0a-v0e+close); additive RHI extension for compute pipelines, storage buffers, dispatch, sync, opt-in async compute | arch, substrate, rhi, gpu, compute, async-compute, descriptors, shader-pipeline, prerequisite | Proposed  |
| 0081  | Agent-native engine: CLI + JSON-RPC + Anthropic MCP substrate (`crd-cli` + `crd-rpc` + `crd-script`); CLI is the source of truth (GUI is a visualization layer that emits commands); supersedes ADR-0034 (folded in as the C++ hot-reload sub-aspect); MCP compatibility = instant Claude Code / OpenAI Function Calling / Gemini Function Calling integration; capability-based security + transactional sessions + sandbox isolation + deterministic replay; Phase 4.0 substrate work + per-DoD CLI surface requirement going forward; first concrete consumer = `crd-hesap` v0 (Phase 3.1.6 immediate next slice ships with CLI surface from day 1) | arch, cli, rpc, mcp, agent, scripting, substrate, vision, supersedes-0034 | Proposed |
| 0082  | Hesap GEMM microkernel: intrinsics-via-Vec8f/Vec16f, ASM deferred. Locked hot-swap signature `gemm_microkernel<T>(k, a_packed, b_packed, c_tile, ldc)` + `CRD_HESAP_MICROKERNEL_BACKEND` compile-time switch (Intrinsics default; Asm reserved). Target 80-85% peak via crd-math::simd; final 5-10% gap to MKL deferred. Three-condition revisit gate: GEMM >50% of solve time AND intrinsics <70% peak AND no better alternative (GPU/sparse). Same call Eigen/Faer/Highway/xtensor/Stan-math/Armadillo/mlpack made. | arch, hesap, blas3, microkernel, perf, simd, intrinsics, hot-swap | Accepted |
| 0083  | hesap-dense row-major storage (with per-factor escape hatch). Keep row-major public default (ML/array-ecosystem alignment: NumPy/PyTorch/JAX are row-major; GEMM layout-neutral via packing; GEMV naturally row-major; sparse independent). Accepted bounded cost: small-N (≤256) dense factorizations trail column-major Eigen/LAPACK ~1.4× (column-oriented elimination fits column-major; proven layout-fit gap not kernel quality via 3 experiments). Escape hatch: opaque factor objects may store internal buffer column-major if a hot-loop consumer proves it; batched/fixed-size kernels preferred for tiny-solve hot loops. Revisit only on measured system-level bottleneck. | arch, hesap, dense, storage, layout, perf, rowmajor | Accepted |
| 0084  | Sparse matrices as first-class cooked engine resources (`crd-hesap-resources` bridge module, depends `crd-resources`+`crd-hesap-sparse`; one-way). `'HMTX'` CRDR (MXHD/MXOP/MXII/MXVL chunks) + 40-byte pinned `MatrixFileInfo` (u64 nnz + topology_hash + frame_stamp + format byte; loader asserts-on-hash-mismatch) + append-only `variant` enum (0=f32/1=f64/2=c32/3=c64). Single loader, variant-in-header; type-erased `SparseMatrixResource` payload + `build_csr<T>()`. In-memory cook-time cooker (reuses v1g `read_matrix_market`); no filesystem dep in the module. Corpus delivery reuses `manifest_write`+`mount_manifest`+`load_sync` (no new ResourceManager API). CLI `hesap.matrix.{info,cook,load}` stateless on inline `.mtx`; `fetch` dev-time (no HTTP client). Cooked binary loads 6–7× faster than re-parsing `.mtx` on real SuiteSparse. Solver vs-reference benches through this path land at v4a (ship-at-consumer). | arch, hesap, sparse, resources, cooker, crdr, corpus, agent-native | Accepted |
| 0085  | Virtual-memory + streaming allocator cluster (Phase 2.2, before hesap v5). Six allocators + a `crd::platform::vm` reserve/commit layer, composing with the shipped streaming pipeline (jobs + async I/O + ResourceManager) rather than replacing it: (1) platform VM API; (2) `VirtualMemoryAllocator` — reserve-big / commit-on-demand / decommit, **stable addresses, no relocating CPU heap**; (3) re-parent `GrowableTlsfAllocator` onto VM → **removes malloc-at-the-root**; (4) thread-safe **fence/epoch-gated RingAllocator** (staging); (5) `StreamingAllocator` policy layer + ResourceManager budgets/eviction; (6) `GpuAllocator` VkDeviceMemory suballocation; (7) GPU defrag (handle relocation) + residency, **pluggable `IResidencyPolicy`/`IDefragPolicy`**. GPU built now per explicit eyes-open user override (design-without-consumer risk confined to policy objects). D(mem-stream)-1..7. Extends ADR-0003 Phase B + ADR-0022. | memory, platform, rhi, streaming, resources, virtual-memory, gpu-allocator | Accepted |
| 0086  | Eylem unified motion model (design from a 2026-05-30 session; ratify/reconcile per-slice when eylem resumes after hesap). One solver / one state — animation proposes TARGETS, physics is the sole arbiter of the rendered pose (kills the Unity-style animation-vs-physics fragmentation). Constraints as the universal primitive (collision/joint/IK/animation-motor all one solve). Powered/active ragdoll (animation = per-joint motor targets + a CONTINUOUS gain; no discrete kinematic↔ragdoll switch — smooth hit reactions + death). Reduced-coords Featherstone end-state for characters/robots (FK = forward pass, IK = constraints on the same Jacobians). Pluggable solver over the constraint graph (SI/TGS → XPBD → reduced-coords+hesap → the v9 unify). LOD = a fidelity continuum scaling the whole pipeline per agent (NOT decoupled animation/physics clocks). Authoritative-coarse / cosmetic-fine split = the key LOD + networking enabler (only the cheap authoritative layer is deterministic+networked; cosmetic detail is local). Crowd = detail-follows-attention (thousands cheap/GPU + ~dozens focused full reactions). Networking = deterministic LOCKSTEP for the shared authoritative sim (inputs-not-state ⇒ scales to thousands) + rollback for local players; needs cross-PLATFORM FP determinism (deterministic transcendentals). REFINES ADR-0021 + ADR-0074 (powered ragdoll vs cinematic-kinematic — reconcile), EXTENDS ADR-0035 + ADR-0062 §5 + ADR-0063; builds on ADR-0065 (hesap = the per-step constrained-solve backbone). | arch, physics, eylem, animation, networking, determinism, lod | Proposed |
| 0087  | Large-scale deterministic simulation (worked example: a 1700s naval-battle MMO — sea + colliding ships + thousands of soldiers, ~half real players; rules: incredible physics, deterministic networking, >60fps). Six transferable principles: (1) replace state with DETERMINISTIC FIELDS (ocean/wind/atmosphere = procedural functions, never simulated or networked; physics-field cheap+deterministic vs render-field rich+cosmetic); (2) couple via SURFACE INTEGRAL over a low-poly proxy (Kerner per-face hydrostatic+hydrodynamic → float/righting/wave-ride/drag), NOT point-probe floaters; (3) authoritative-coarse/cosmetic-fine everywhere; (4) detail-follows-attention; (5) SCALE THE NETWORK MODEL to human-player count; (6) solve agents on carriers in the carrier's LOCAL non-inertial frame + balance controller (inverted-pendulum CoM, ankle/hip/step) + powered ragdoll. KEY networking decision (REFINES ADR-0086 D10): few humans + AI crowd → deterministic LOCKSTEP; MANY humans (MMO) → server-authoritative spatially-partitioned DETERMINISTIC regions (server meshing) + interest management + client prediction/rollback (tight via identical deterministic code) + authoritative/cosmetic split; determinism stays the bedrock (replay + anti-cheat + tight prediction), delivery is NOT lockstep. Generalizes ADR-0073 aero (atmosphere = field, aero = surface integral); extends ADR-0035; builds on ADR-0065 (FFT ocean). Honest frontier: server meshing is bleeding-edge (~100-200 humans/region realistic), cross-platform FP determinism is the deep dependency, balance robustness is research-grade. | arch, physics, eylem, networking, determinism, lod, environment, mmo | Proposed |
| 0088  | **GEMM hand-tuned asm — INVESTIGATED → REVERTED (intrinsics vindicated).** Re-opened ADR-0082's asm question, built the whole thing (dual-syntax MASM/GAS f64 6×8 kernel + runtime CPUID dispatch + build integration, all bit-identical to the intrinsic, green on MSVC/clang-cl/gcc), and MEASURED it. **Decisive same-process A/B (asm vs intrinsic, identical clock): asm is ~1–2% SLOWER.** Structural, not tunable — the intrinsic INLINES into the panel loop (no call overhead, ~98% peak) while the asm is a hard function call. The bmwcra premise also failed separately (factor is memory/scaling-bound at 8T, not kernel-bound — the kernel win washes out). Earlier "+10–16%" was a turbo-clock artifact. **All asm code + the v0d-asm-0 direct-to-C framework change REVERTED; tree back to v5a-5. ADR-0082 (intrinsics-first) STANDS, vindicated.** Hot-swap point reserved for a genuine future asm-only-ISA need (AMX etc.), not a generic perf lever. bmwcra's crush = the SCALING lever (within-front parallelism), not the kernel — **EXECUTED in v5a-7 (2026-05-31): bmwcra flipped 0.65→1.04–1.05× WIN, every matrix now beats CHOLMOD.** Record: `docs/phases/hesap-v0d-asm-microkernel-plan.md`. | arch, hesap, math, simd, asm, perf, reverted | **Reverted** |
| 0089  | **crd-hesap-eigen — sparse eigensolvers + module edges (Phase 3.1.6 v6).** Matrix-free over `LinearOp<T>`; Rayleigh-Ritz reuses `crd-hesap-dense`. The crush axis is ALGORITHMIC not kernel: plain Krylov (Lanczos/Arnoldi/IRLBA) = parity + the determinism moat (the honest ceiling, shares ARPACK/PRIMME's BLAS/LAPACK ceilings); the crush is fewer-matvecs via preconditioned methods (shift-invert/LOBPCG/JD/FEAST). **v6-z bench proven (3D Poisson, matched acc): AMG-LOBPCG CRUSHES direct shift-invert ARPACK (9.5× wall + 40× mem, growing) + PARITY vs state-of-art PRIMME at f64 — crush-vs-direct + parity-vs-same-class + the moat is the win, NOT a speed-win over PRIMME+AMG.** Determinism = the differentiator: deterministic counter-RNG start + fixed-order reorthog + pinned sign/coupled-SVD-sign + deterministic dense RR ⇒ eigenpairs bit-identical {1..16} workers (`[moat]` per method); none of ARPACK/PRIMME/SLEPc/PROPACK carries it. Restart SUBSTITUTION (determinism): thick-restart Lanczos≡IRLM, Krylov-Schur≡IRAM, augmented thick-restart≡IRLBA (deterministic, vs the ordering-sensitive implicit bulge-chase). Real-symmetric focus; complex where inherent (Arnoldi/KS emit complex). New acyclic edges hesap-eigen→{hesap-direct (shift-invert/FEAST: v5 LU factor), hesap-iterative (JD: FGMRES correction eqn)} (mirror v5f-c2). Extends ADR-0065 §v6. | arch, hesap, eigen, eigensolver, svd, determinism, module-edges | Accepted |
| 0090  | **crd-hesap-opt — the optimization domain (substrate + contracts + edges, Phase 3.1.6 v7).** Matrix-free over `Objective<T>` (value/n pure + gradient/hessian_vector virtual; `has_gradient()` capability flag agrees with `gradient()->bool`; vtable append-at-end + a RESERVED fused `value_and_gradient` slot for the L-BFGS/LM hot path). v7 ABSORBS the old v8 constrained cluster (user direction — opt is ONE domain); the universal "find the best X" substrate (eylem/control/FEA-CFD/CAD/ML/rendering/games/robotics/DAW). **Determinism moat (the differentiator none carry):** serial optimizer + bit-exact objective eval ⇒ bit-identical TRAJECTORY+result across {1..16} ⇒ certifiable MPC/control (DO-178C/ISO26262) + reproducible training + replay; `[moat]` per slice asserts convergence too (vacuous-maxed-out guard). Contracts pinned up front: line-search g_out cost contract (Wolfe fills it, Armijo recomputes); workspace = caller's `IAllocator*`; stochastic moat SPLITS (minibatch reproducibility needs v12 counter-RNG). Gold standards (honest, crush-where-the-algorithm-allows + parity+moat else; wall-clock-vs-Python-overhead caveat): Ceres/liblbfgs/scipy/NLopt/CMA-ES/PyTorch/OSQP/IPOPT/SCS/HiGHS (probe Ceres v7-e + IPOPT/cyipopt v7-n early). Edges: hesap-opt→hesap-dense/sparse/jobs now; →hesap-direct/iterative (Newton/KKT) + hesap-eig (exact-TR) later; reverse-mode autodiff plugs the same gradient interface. Extends ADR-0065 D14. | arch, hesap, opt, optimization, determinism, module-edges, substrate | Accepted |
| 0091  | **crd-hesap-ode — the ODE/DAE module: two API layers + deterministic controllers + the work-precision contract (Phase 3.1.6 v9).** TWO API LAYERS (ADR-0078 two-layer applied to integration): raw-span allocation-free stepper KERNELS (caller scratch, inlined RHS, in-place safe, fixed per-element FP order — what eylem's fused SoA sweep / animation / DAW inline; never a virtual f per body per substep) + the general DRIVER over `OdeFunction<T>` (the v7 Objective capability contract; vtable LOCKED append-at-end — mass matrix appends at v9-h, sparse Jacobian at v9-j; events = integration options, NOT virtuals). Deterministic `OdeWork` counters (CVODE semantics) = the work-precision scoreboard currency (error-vs-nfev-vs-wall, the domain's native honest format). Step controllers = pure deterministic FP fns, TWO types matching their references exactly (scipy elementary accepts err<1 strictly; Hairer PI accepts err≤1 + facold floor — one parametrization would be dishonest). Dense-output contract pinned day 1 (fixed-width coeff blocks, caller-owned storage; cubic-Hermite fallback shipped); recomputed t (no accumulation). Edges v9-a: core/containers/memory ONLY; hesap-dense at v9-d/e, hesap-direct/iterative at v9-j; NO edge to hesap-opt (acyclic — opt consumes ode for shooting later). Named OUT: LSODA auto-switch, f32 stiff. Gold standards: SUNDIALS 6.4.1 + scipy (trajectory-exact) + Hairer Fortran + odeint. Extends ADR-0065; consumes ADR-0063 (symplectic = eylem replay, v9-g). | arch, hesap, ode, dae, determinism, module-edges, substrate | Accepted |
| 0092  | **crd-hesap-fft — the FFT cluster: deterministic plan-from-factorization + portable-C++ MKL-adjacent + the full transform suite (Phase 3.1.6 v10).** Plan chosen from the size factorization with NO runtime measurement (vs FFTW MEASURE/PATIENT, whose wisdom varies run-to-run + isn't reproducible across builds); one read-only twiddle table/plan ⇒ fixed per-element FP order ⇒ output bit-identical across runs+threads (each thread its own plan; {1..16} nearly free — an FFT has no thread-dependent reduction ⇒ determinism is OFF the usual moat axis, so lead with it + zero-planning-overhead + typed zero-dep integration, NOT a beat-MKL claim). SUITE: complex FFT (`FftPlan`, Stockham radix-2/4/8 + straight-line SIMD codelets N≤32 + Bailey four-step >2¹⁹) · real (`RealFftPlan`) · DCT/DST (`DctPlan`, Makhoul — beats scipy/PocketFFT) · NUFFT (`NufftPlan` — wins FINUFFT small/mid @3× acc) · **Bluestein ANY-size** (`BluesteinPlan`, chirp-z over one pow-2 plan) · **N-D** (`NdFftPlan`, row-column; pow-2 axis→FftPlan else Bluestein; forward-only-per-axis + N-D forward-trick inverse so unnormalized-vs-normalized inverses never clash) · **Sparse FFT** (`SparseFftPlan`, HIKP 2012, END-TO-END SUB-LINEAR + NOISE-ROBUST: multi-scale binary location + voting + median, no O(n) step; coeff-under-noise bounded √(n/B)σ/√R info-theoretically). Lower-layer raw `Complex<T>` (ADR-0078 §5); unnormalized both ways (`ifft_normalized`=1/n). Correctness oracle = brute-force O(N²) DFT (NEVER round-trip — the odeint-d4 trap). **MKL global parity DEFERRED — production-grade portable, not MKL clone:** MKL-adjacent on AVX2 (256K fixed 0.25→0.85×=3.6× default-on, 4M/8M ~0.92-0.95× host-noise-caveated), beats PocketFFT everywhere; residual ~15% = the four-step inter-stage twiddle, measured-exhausted (8 attempts) as the asm-integrated-butterfly gap, OUT OF SCOPE per ADR-0082 (portable-C++/WASM-no-asm). CLI `hesap.fft.*` (forward + sparse). Edges: core/containers/memory + math (simd) + hesap (Complex, CLI). Extends ADR-0065; consumes ADR-0078 + ADR-0082. | arch, hesap, fft, determinism, module-edges, substrate | Accepted |
| 0093  | **crd-hesap-dsp (+ -wavelet + -comms) — the DSP cluster: the design/application honest-gate split + SOS-by-default + the two-layer streaming contract (Phase 3.1.6 v11).** MAXIMAL subject (user: "every DSP functionality") = full Signal-Proc-Toolbox + adaptive + wavelets + comms, as 3 ISOLATED modules (a DAW links dsp not comms) + crd-units add (DecibelRatio/DecibelPower nonlinear lenses + NormalizedFrequency). ⭐⭐ THE HONEST GATE (the v10 scoreboard scar): filter DESIGN (ellip/remez/firls = transcendental+iterative — std::sin not bit-identical cross-libm, ULP drift compounds) gates on SPEC-COMPLIANCE (ripple≤Rp·atten≥Rs·alternation-count) + coeffs-to-**10-sig-digits**, NOT bit-match (physically unachievable + meaningless — spec-compliance is what MATLAB validates against, the STRONGER gate); filter APPLICATION (lfilter/sosfilt/biquad = pure mul-add) gates BIT-EXACT + {1..16} determinism MOAT (streaming-only — DAW replay/DO-178C; design is one-time, "deterministic ellip()" is not a differentiator). ⭐ DATA-FLOW RULE (locked v11-a): design in zpk → convert zpk→sos DIRECTLY; tf is OUTPUT-only NEVER a design intermediate (roots-of-tf = Wilkinson-ill-conditioned >order~8, proven by order-12 gate); zpk_freqz = well-conditioned factored eval; zpk→sos = nearest-to-unit-circle pairing (the reason SOS exists); SOS = default high-order IIR. Two-layer (ADR-0078): every filter ships typed whole-array batch + allocation-free stateful streaming kernels (Direct-II-T biquads/FIR/polyphase = the DAW/SDR hot loop). Gold standards: scipy.signal (free primary) + MATLAB-SP-Toolbox-R2026a (industry authority, installed) + Intel-IPP + liquid-dsp + CMSIS-DSP + PyWavelets. Edges: dsp→fft (spectral/conv/Hilbert/CZT) + eigen (DPSS-multitaper/MUSIC-ESPRIT/AR) + math + units + core; wavelet→fft; comms→dsp; NO edge to opt. Extends ADR-0065; consumes ADR-0078 + ADR-0092. | arch, hesap, dsp, filters, determinism, module-edges, substrate | Accepted |
