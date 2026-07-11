# ADR-0103 — `crd-gpu-context` owns every GPU program and pipeline; **no module outside a backend names a shading language or a bytecode**

- **Status:** Accepted (2026-07-10) — user decision ("I want gpu context to handle all the shaders… `crd-shader` must not know any glsl or hlsl, gpu context must handle all of that… we must be able to handle all kinds of shaders from compute to lightings to materials to ray tracings to mesh shaders to geometry shaders to everything").
- **Phase:** detour **D-008** (the gpu-context convergence), sequenced *before* D-007 Phase-B's raster emitters.
- **Tags:** `gpu-context` `kir` `shader` `rhi` `renderer` `architecture` `ir` `ray-tracing` `mesh-shaders` `substrate` `north-star`
- **Supersedes:** **ADR-0099 §6** (`crd-shader` stays the single shared GLSL/HLSL→SPIR-V/DXIL compiler) — reversed, see *Context*.
- **Amends:** ADR-0100 (the dispatch surface generalizes from compute to **all** program domains) · ADR-0030 (shader/PSO boundary) · ADR-0027 (reflection consumption) · ADR-0080 (`crd-rhi-compute`).
- **Realizes:** ADR-0101 (the IR is the single source of truth) · ADR-0099 §2 (`RenderDevice` as an interface over `IGpuContext`).

## Context — an accepted decision that contradicts another accepted decision

ADR-0101 (Accepted) says backend languages and bytecodes are **outputs only**, "never authored or stored". ADR-0099 §6 (Accepted) says `crd-shader` **is** the shared GLSL/HLSL→SPIR-V/DXIL compiler that both authored *and* CKIR-generated shaders route through. Those cannot both hold: §6 makes a portable, mid-level module the owner of two backend languages, and forces `crd-kir-vulkan` to **depend on** it. The D-007 B3 plan (2026-07-10) faithfully followed §6 and proposed gating the raster emitters on `crd::shader::compile_glsl(Stage::Vertex)` — i.e. CKIR depending on a GLSL compiler. **The user rejected that plan; §6 is the decision that gives way.**

Three leaks, measured on the tree at 95251f3:

1. **Language leak.** `crd/shader/compile.hpp` exports `compile_glsl` / `compile_hlsl`. **9 call sites** (`kir-vulkan/src/backend_vulkan.cpp`, `tools/asset_cooker/src/cook_handlers/glsl.cpp`, 5 test TUs, 2 test CMakes). `crd-shader` links `shaderc` and `dxc`.
2. **Bytecode leak.** `crd::rhi::ShaderModuleDesc::code` is a `ConstSpan<crd::u8>` of **raw SPIR-V**, in a public header. **38 `create_shader_module` sites across 9 files**; 18 `create_graphics_pipeline` sites. So a *renderer* handles SPIR-V bytes today.
3. **Two device layers.** `crd::rhi::Device` (graphics: shader module / graphics pipeline / swapchain) and `crd::gpu::IGpuContext` + `IComputeContext` (compute) are separate stacks over separate `VkDevice`s — the very duplication ADR-0099 opened by naming, and only half-closed.

Meanwhile `docs/systems/shader-ir-corpus-and-stages.md` §2 already specifies the stage set we intend to express — task/mesh (primary), vertex/fragment (universal fallback), tessellation, geometry (legacy-but-supported), and the six ray-tracing stages — while D-007's B3-a shipped `KStage { Compute, Vertex, Fragment }`. A 3-value enum silently bakes a 3-stage assumption into every emitter that switches on it.

## Decision

**`crd-gpu-context` is the one surface that turns a program into something the GPU can run — for every stage.** Two invariants, both mechanically checkable:

> **I1 — No shading language crosses a module boundary.** GLSL / HLSL / WGSL / MSL / CUDA-C source exists only *inside* one backend, between our emitter and the vendor compiler. No public header, no non-backend module, no consumer ever holds shader source.
>
> **I2 — No bytecode crosses a module boundary.** SPIR-V / DXIL / PTX / metallib bytes never appear in a public header or a consumer. The only handle a consumer holds is an **opaque `IGpuProgram`**.

The public currency **in** is the IR (`KGraph` + `KEntry`); the public currency **out** is `IGpuProgram`.

```
crd-kir            KGraph + KEntry                      (the IR — the ONLY authored form)
                        │
crd-gpu-context    IGpuContext
                     ├ create_program(graph, entry) → IGpuProgram      ← the ONE shader seam
                     ├ create_program(cooked_name)  → IGpuProgram      ← ship path + escape hatch
                     ├ compute()      → IComputeContext*     pipelines + dispatch
                     ├ raster()       → IRasterContext*      VS/FS · task/mesh · draw   (null if unsupported)
                     └ raytracing()   → IRayTracingContext*  RT pipelines + SBT         (null if unsupported)
                        │  backends implement; each owns its language AND its compiler, privately:
                     ├ -vulkan  IR→GLSL→shaderc→SPIR-V     ├ -cuda    IR→CUDA→NVRTC→PTX
                     ├ -dx12    IR→HLSL→DXC→DXIL           ├ -webgpu  IR→WGSL
                     └ -metal   IR→MSL→metallib            └ -hip     IR→HIP

crd-shader         Effect · reflection · runtime · material payload.  KNOWS NO LANGUAGE, NO BYTECODE.
```

**Rules:**

1. **`ShaderStage` is complete from day one** — the 14 SPIR-V execution models: `Compute` · `Vertex` · `TessControl` · `TessEval` · `Geometry` · `Fragment` · `Task` · `Mesh` · `RayGen` · `Intersection` · `AnyHit` · `ClosestHit` · `Miss` · `Callable`. Backends **refuse loudly** for stages they do not yet implement; they never silently fall back to compute. (Geometry is supported-but-discouraged per the corpus doc; mesh/task is the amplification path.)
2. **Two on-ramps, neither naming a language** (keeps ADR-0100's kernel-source-agnostic rule): **(a)** from an IR graph — runtime compile, hot-reload, autotuning; **(b)** from a cooked, content-addressed blob by name — the ship path (Phase D), zero runtime compile.
3. **The escape hatch (ADR-0101 §4) enters through (b) only.** Hand-written per-backend kernels (`geometry-bvh-gpu`'s LBVH `.comp`, imgui/draw overlay GLSL, third-party HLSL) cook to blobs and load by name, explicitly **non-portable**. They never re-enter the engine as *source*.
4. **`crd-shader` keeps** `Effect`/`Module`/reflection/runtime/material payload and the resource loaders. It **loses** `compile.hpp`, `compile.cpp`, `compile_hlsl.cpp`, and its `shaderc`/`dxc` dependencies.
5. **`crd-rhi`'s device converges onto `IGpuContext`.** `ShaderModule`/`GraphicsPipelineDesc::vertex_shader` give way to `IGpuProgram`; `rhi-vulkan`'s device/pipeline/swapchain/command-buffer implementation is absorbed by `gpu-context-vulkan`. One `VkDevice`, one pipeline cache, one place that knows Vulkan.
6. **The dependency edge is `crd-gpu-context → crd-kir`, and it is acyclic** — verified: `crd-kir` links only `crd-core`/`-containers`/`-memory`/`-math`, and `crd-gpu-context` links only `crd-core`/`-containers`. A `KGraph` is a *program representation*, not compute-or-rendering semantics, so ADR-0099 §1's "no compute or rendering semantics in `IGpuContext`" is upheld.

## Consequences

- **+** ADR-0101 becomes *enforceable*, not aspirational: I1/I2 are grep-gates in CI, so the next hand-written `.glsl` cannot quietly become a source of truth.
- **+** One device, one pipeline cache, one Vulkan-aware module. The two-stack duplication ADR-0099 named and ADR-0100 half-closed is finally closed.
- **+** Mesh/task and ray-tracing stages get a home the day they are emitted — no second seam, no `rhi` extension per stage.
- **+** A DX12 raster backend becomes a `gpu-context-dx12` slice rather than a whole new `rhi-dx12` module — which is what made D-007 B3's original "renders a lit triangle on Vulkan + DX12" gate unreachable.
- **−** A real migration: `rhi` (1138 lines) · `rhi-vulkan` (4781) · `renderer` (3099) · `draw` (3071) · `shader` (2123) + `imgui`/`perf`. Contained by the measured call-site counts (38 + 18) and staged below so **every step ships green**.
- **−** `crd-gpu-context` gains a `crd-kir` dependency, so a consumer that only wants a device now compiles the IR headers. Accepted: the IR *is* the program currency; a device with no way to make a program is not useful.
- **−** Until C4, DX12 has no raster path; render gates stay Vulkan-only. Stated, not glossed.

## Rollout (detour D-008 — each step independently green, ADR-0099/0100 discipline)

- **C0 — the program seam.** `crd/gpu/program.hpp`: `ShaderStage` (14) + `IGpuProgram` + `create_program(graph, entry)`. `gpu-context-vulkan` absorbs `shaderc` and emits GLSL internally; `gpu-context-dx12` absorbs DXC. **`crd/shader/compile.hpp` is deleted**; the 9 call sites migrate. **Gate:** every suite lands on its exact prior count (kir-vulkan 33010 · kir-dx12 30821 · geometry-bvh-gpu · rhi_vulkan) **+ the I1/I2 grep-gate goes green**.
- **C1 — `IRasterContext`.** Raster pipelines + draw in `gpu-context`, implemented over `rhi-vulkan`'s existing internals; `GraphicsPipelineDesc` takes `IGpuProgram*`. **Gate:** rhi_vulkan + renderer + draw suites green; sandbox still renders.
- **C2 — absorb the device.** `rhi-vulkan`'s device/swapchain/command-buffer move into `gpu-context-vulkan`; `crd-rhi` retires `shader_module.hpp` + the bytecode surface; `renderer`/`draw`/`imgui`/`perf` consume `crd::gpu`. **Gate:** full sweep + sandbox.
- **C3 — `IRayTracingContext`** (RT pipelines, SBT, AS binding) + the DXR-1.2 / RTX-Mega-Geometry frontier (SER enable, opacity micromaps, cluster AS) — feeds D-007 B9.
- **C4 — DX12 raster** — unlocks the DX12 half of every render gate.
- **C5 — GPU-driven execution** — indirect-count → device-generated commands (`VK_EXT_device_generated_commands`) → work graphs (`VK_AMDX_shader_enqueue` / D3D12 Work Graphs); the GPU schedules its own work.
- **C6 — cooperative-vector device capability** — `VK_NV_cooperative_vector` / DX12 cooperative vectors, the device half of neural shading (D-007 B10).

> **Frontier commitment (2026-07-10, user direction):** the C-slices were first written as a *correctness* convergence; a
> web-grounded frontier audit pulled the 2024–26 device/dispatch state of the art (shader objects · bindless/descriptor
> buffers · dynamic rendering · VRS/ROV · device-generated commands · work graphs · SER · opacity micromaps · cluster AS ·
> cooperative vectors) into explicit slices. **Full extension list + citations + slice mapping: `docs/detours/D-008` +
> `docs/detours/D-007` frontier tables.** C1's pipeline/binding model is designed for shader objects + bindless from the
> start — retrofitting them onto monolithic PSOs would be a rewrite.

**Then, and only then**, D-007 resumes at B3-c (GLSL VS+FS emitters) behind the finished seam.

**Prior art:** [Slang](https://shader-slang.org/) (IR → every target; the runtime loads IR modules or precompiled blobs, never source) · [Dawn/wgpu](https://dawn.googlesource.com/dawn) (`ShaderModule` from WGSL/SPIR-V behind one device) · [Vulkan `VK_KHR_ray_tracing_pipeline`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_ray_tracing_pipeline.html) · [Mesh shading (Khronos)](https://www.khronos.org/blog/mesh-shading-for-vulkan).
