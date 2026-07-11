# D-008 — the gpu-context convergence: one surface for every GPU program

- **Status:** 🔨 OPEN · **C0 ✅ · C1 ✅ · C2-a…c ✅ · C2-d1…d4 ✅ (I2 FULLY CLOSED — `ShaderModule` retired) · C2-e ✅ (I1 FULLY CLOSED — Effect frontend off shaderc via injected `ISpirvCompiler` + new `crd-shader-vulkan` bridge; allowlist EMPTY)** · **remaining:** C2-f (retire rhi's own device creation) · **blocks** D-007 Phase-B from B3-c onward.
- **Decision record:** [ADR-0103](../decisions/0103-gpu-context-owns-every-gpu-program.md) (supersedes ADR-0099 §6).
- **Why a separate detour:** D-007's own rule — *"If a slice grows past its contract, promote it to its own phase — don't
  let it quietly take over the roadmap."* This touches `rhi`, `rhi-vulkan`, `renderer`, `draw`, `imgui`, `perf`, `shader`.
  That is past "CKIR becomes the universal shader IR".

## The one rule

> **No module outside a backend names a shading language or a bytecode.**
>
> **I1** — GLSL/HLSL/WGSL/MSL/CUDA source lives only inside one backend TU, between our emitter and the vendor compiler.
> **I2** — SPIR-V/DXIL/PTX bytes never appear in a public header. A consumer holds an opaque `IGpuProgram`, nothing else.

Currency **in** = the IR (`KGraph` + `KEntry`). Currency **out** = `IGpuProgram`. Both invariants are **grep-gates**, not
prose: a violation fails the build. (A rule with no check is a wish — `docs/SANITY.md`.)

## The three leaks this closes (measured on 95251f3, not estimated)

| # | leak | evidence |
|---|---|---|
| 1 | **language** — `crd-shader` exports `compile_glsl`/`compile_hlsl`, links `shaderc` + `dxc`; `crd-kir-vulkan` **depends on it** | 9 call sites: `kir-vulkan/src/backend_vulkan.cpp`, `tools/asset_cooker/src/cook_handlers/glsl.cpp`, 5 test TUs, 2 test CMakes |
| 2 | **bytecode** — `crd::rhi::ShaderModuleDesc::code` is a `ConstSpan<u8>` of raw SPIR-V, in a public header | 38 `create_shader_module` sites / 9 files · 18 `create_graphics_pipeline` sites |
| 3 | **two device layers** — `rhi::Device` (graphics) and `gpu::IGpuContext`+`IComputeContext` (compute) over separate `VkDevice`s | ADR-0099 named this; ADR-0100 half-closed it |

> **How leak 1 survived review:** ADR-0099 §6 *mandated* it, in writing, as an Accepted decision — while ADR-0101 (also
> Accepted) forbade it. Two accepted ADRs contradicted each other for two days, and the D-007 B3 plan followed the wrong
> one. **Scar → rule:** when an ADR supersedes part of another, strike the superseded clause **in place**, in the old
> document. A reader who lands on ADR-0099 §6 must not be able to follow it. (Done: 0099 §6 is struck through.)

## C0 landed (2026-07-10) — what shipped, and the deeper leak it surfaced

**Shipped, all green + tidy:** the two SPIR-V compilers moved verbatim from `crd-shader` to `gpu-context-vulkan`
(`compile_glsl_to_spirv` / `compile_hlsl_to_spirv`, keyed on `crd::gpu::ShaderStage`); `crd/shader/compile.hpp` +
`compile.cpp` + `compile_hlsl.cpp` **deleted**; `crd-kir-vulkan` no longer links `crd-shader`; the asset cooker + the
kir-vulkan / geometry-shader-helpers / resources tests migrated. `program.hpp` lays `ShaderStage` (14) + `IGpuProgram`;
`IGpuContext::create_program(cooked SPIR-V) → IGpuProgram` is implemented in Vulkan (a `VkShaderModule` wrapper) and
**tested end-to-end** (compile a trivial kernel → `create_program` → `valid()` + correct `stage()`; malformed bytecode
rejected, not crashed). The **I1 grep-gate** (`crd-no-shader-language-leak`, PS + bash, ctest-registered) is green.

**The relocated `test_ckir_glsl` moved to `tests/gpu-context-vulkan`.** It compiles emitted GLSL to SPIR-V, so it must
link the compiler — which now lives in the Vulkan backend (→ `Vulkan::Vulkan`). Leaving it in `crd-kir-tests` would have
broken *that suite's own* link-isolation smoke ("crd-kir drags no GPU API"). Counts conserved exactly: kir **400/43 →
390/40**, gpu-context-vulkan **8/1 → 24/5** (the moved 3 cases/10 asserts + the new seam case).

**⚠ Deeper leak the gate surfaced (the honest scoreboard) — ✅ RESOLVED in C2-e:** `crd-shader/src/runtime.cpp` — the
**Effect/Module RENDERING frontend** — had its *own* shaderc loader and compiled hand-written GLSL. Rather than drag
Vulkan into `crd-shader` (which `renderer`/`draw` link), **C2-e injected the compiler**: `runtime.cpp` takes a
`crd::shader::ISpirvCompiler`, and the new **`crd-shader-vulkan`** bridge implements it over
`crd::gpu::compile_glsl_to_spirv` (linking both crd-shader + gpu-context-vulkan so neither depends on the other). The I1
gate's allowlist is now **EMPTY** — C0 closed the standalone-compiler half; **C2-e closed the whole of I1.**

## Sizing (honest)

`rhi` 1138 · `rhi-vulkan` 4781 · `renderer` 3099 · `draw` 3071 · `shader` 2123 · `gpu-context*` 1332 lines.
The migration is bounded by the **call-site** counts above (38 + 18 + 9), not by the line counts — the interfaces are
narrow. Every step below ships green on its own.

## Slices

| slice | contract | gate | status |
|---|---|---|---|
| **C0** | **the program seam.** `crd/gpu/program.hpp`: `ShaderStage` (all 14 SPIR-V execution models) · opaque `IGpuProgram` · `IGpuContext::create_program(cooked SPIR-V) → IGpuProgram`. `gpu-context-vulkan` absorbs `shaderc` + `dxc` (GLSL/HLSL → SPIR-V, PRIVATE). **`crd/shader/compile.hpp` DELETED**; the standalone `compile_glsl`/`compile_hlsl` API relocated as `crd::gpu::compile_*_to_spirv`; 7 consumer call sites + 4 CMakes migrated. | ✅ **DONE** — kir-vulkan **33010** · kir-dx12 **30821** · geometry-bvh-gpu **851016** · rhi_vulkan **4819** all EXACT; **I1 grep-gate green**; seam tested end-to-end (compile → `create_program` → valid program). All tidy-clean |
| **C1** | **`IRasterContext` + graph `create_program` — designed on the modern pipeline/binding model, not legacy PSOs** — raster pipelines + draw in `gpu-context`; `create_program(KGraph, KEntry)` overload (compute now; raster once D-007 **B3-c** emits VS/FS) → adds `gpu-context → crd-kir`. **Pipeline model = SHADER OBJECTS** (`VK_EXT_shader_object`) with a graphics-pipeline-library (`VK_EXT_graphics_pipeline_library`) fallback — the frontier answer to the **variant/permutation explosion ADR-0101 explicitly warns about**, a C1 *design* decision, not a later bolt-on. **Rendering = DYNAMIC RENDERING** (Vulkan 1.3/1.4 core — no `VkRenderPass`). **Binding = BINDLESS-FIRST** (descriptor indexing + `VK_EXT_descriptor_buffer`/`VK_EXT_descriptor_heap`). **Raster state flags:** VRS · fragment-interlock/ROV · conservative raster · multiview. | 🔨 **C1-a ✅ · C1-b ✅ · C1-c ✅** (see sub-slices); C1-d next |

### C1 sub-slices (started 2026-07-10)

| slice | contract | status |
|---|---|---|
| **C1-a** | **graphics-capable context + `IRasterContext` foundation** — `VulkanGpuContext` now creates a GRAPHICS queue (distinct from the async-compute queue) + enables dynamic-rendering & **`VK_EXT_shader_object`** (guarded); `graphics_capable()`/`graphics_queue()`/`graphics_family()`/`shader_object()`. Backend-agnostic `IRasterContext` + `IRasterTarget` (shader-object-shaped, no Vulkan in the interface). Vulkan impl: offscreen RGBA8 target + **dynamic-rendering CLEAR + pixel readback** (raw Vulkan, NO crd-rhi edge). | ✅ **green on RTX 4070 Ti Super** (graphics_family=0, shader_object=YES; clear reads back correct RGBA8); kir-vulkan **33010** unchanged; gpu-context-vulkan 24/5→**36/6**; tidy-clean |
| **C1-b** | **the shader-object DRAW** ✅ — `IGpuProgram` retains its cooked SPIR-V (`VulkanGpuProgram::vk_spirv()`); `IRasterProgram` = a linked VS+FS pair as **`VkShaderEXT`** (one `vkCreateShadersEXT`, `LINK_STAGE`); `IRasterContext::create_raster_program` + `draw` (all ~18 shader-object dynamic-state setters — no PSO — + `vkCmdDraw`) on dynamic rendering. Attributeless triangle from trivial cooked VS/FS via the relocated compiler (no B3-c). | ✅ **green on RTX 4070 Ti Super** — centre pixel RED (triangle), corner BLUE (clear); gpu-context-vulkan 36/6→**48/7**; kir-vulkan **33010** unchanged; tidy + gates green |
| **C1-c** | **`create_program(KGraph, KEntry)`** graph→program overload ✅ — `IGpuContext` gains the IR on-ramp (kir types forward-declared in the base header; only `gpu-context-vulkan` links **crd-kir** — the acyclic edge). Compute path: emit GLSL via crd-kir's `emit_(vec\|elementwise)_glsl` → SPIR-V via the relocated compiler → `IGpuProgram`. Raster entry refused (D-007 B3-c). `entry.out[0].node` names the compute output. | ✅ **green** — a compute KGraph `(x+y)*exp(x)` → valid program (stage Compute, real SPIR-V); a vertex entry → nullptr; gpu-context-vulkan 48/7→**53/8**; kir-vulkan **33010** unchanged; tidy + gates green. (Dispatch-through-the-seam = the kir-vulkan convergence, later; C1-c proves the currency + emit + compile.) |
| **C1-d** | ⛔ **RECLASSIFIED into C2** (2026-07-10, after scouting). renderer/draw/sandbox all run on `crd::rhi::Device` — a **separate `VkDevice`** from `VulkanGpuContext`'s; a `VkShaderModule`/`IGpuProgram` can't cross devices, so "renderer consumes `IRasterContext`" is inseparable from C2's device unification. **C1's raster surface is complete at a/b/c.** | ⛔ → C2 |
| **C2** | **absorb the device — ONE `VkDevice`** (see sub-slices). The endpoint: `VulkanGpuContext` owns the single device (ADR-0099); `rhi-vulkan` adopts it instead of creating its own; `renderer`/`draw`/`imgui`/`perf` share it; `crd-rhi` retires `shader_module.hpp` + the raw-SPIR-V surface (I2); the Effect frontend (`crd-shader/src/runtime.cpp`) stops compiling GLSL via shaderc (empties the C0 I1 allowlist). | 🔨 in progress (see sub-slices) |

### C2 sub-slices (the device unification — started 2026-07-10, incremental so the working sandbox never breaks)

| slice | contract | status |
|---|---|---|
| **C2-a** | **`VulkanGpuContext` becomes render-capable** ✅ — `GpuContextConfig::headless == false` enables the surface (instance: `VK_KHR_surface` + platform) + `VK_KHR_swapchain` (device) extensions when available; `render_capable()` = surface + swapchain + a graphics queue. Guarded + ADDITIVE — the headless/compute path is byte-for-byte unchanged. | ✅ **green** — a windowed context comes up `render_capable()` (still async-compute-capable: ONE device, both concerns); headless stays render-incapable; kir-vulkan **33010** unchanged; gpu-context-vulkan 53/8→**59/9**; tidy + gates green |
| **C2-b** | **`rhi-vulkan` ADOPTS an external device** ✅ — `create_vulkan_device_adopting(crd::gpu::IGpuContext&)` builds a `VulkanDevice` over the context's `VkInstance`/`VkPhysicalDevice`/`VkDevice`/queues (downcast in the `.cpp`; the header stays abstract via a forward-decl). `VulkanDevice` gained `owns_device` — an adopted device frees ITS pools/allocations but **never destroys the shared `VkDevice`**. Edge `rhi-vulkan → gpu-context-vulkan` (PRIVATE, acyclic — verified by build). The renderer keeps its rhi API. | ✅ **green** — an adopted rhi Device stands up on the shared device; destroying it leaves the device alive (re-adoption succeeds); rhi_vulkan 4819/25→**4823/26**; kir-vulkan **33010** unchanged; sandbox links; tidy + gates green |
| **C2-c1** | **the adopted device is FEATURE-MATCHED** ✅ — scouting found rhi's own device enables **synchronization2 + fillModeNonSolid** (the render path needs them) that the windowed context lacked. A `windowed` `VulkanGpuContext` now enables both (guarded — headless/compute unchanged); `create_vulkan_device_adopting` passes `sync2 = render_capable()`. So the renderer can run on the adopted device unchanged. | ✅ **green** — a windowed context adopts into a feature-complete rhi Device; gpu-context-vulkan **59/9**, kir-vulkan **33010**, both unchanged; tidy + gates green |
| **C2-c2** | **the sandbox runs on the shared device** ✅ — (1) **decoupled ImGui**: added `crd::rhi::vulkan_instance(Device&)` (VkInstance from the device via `VulkanDevice::instance()`); `ImGuiLayer` dropped its `crd::rhi::Instance&` param and gets the instance from the Device — so it works on the adopted path. (2) **swapped the sandbox bring-up**: `create_vulkan_instance`+`create_device` → `create_vulkan_gpu_context({headless=false})` + `create_vulkan_device_adopting` (the `gpu_context` declared first so it outlives `device`). Renderer + swapchain + ImGui + frame graph all on the ONE device. | ✅ **sandbox smoke PASS — 436 frames presented over 3.0s @ 145 fps, validation layers ON, no VUID/errors.** rhi_vulkan **4824/26** · kir-vulkan **33010** unchanged. (Tidy: all `.cpp` clean; `vulkan_native.hpp` standalone-UNGATED by a PRE-EXISTING `<GLFW/glfw3.h>` include, but its content is gated via `imgui_layer.cpp` + `vulkan_backend.cpp`, both clean.) |
| **C2-d1** | **the opaque-program pipeline path** ✅ — `GraphicsPipelineDesc` gains `vertex_program`/`fragment_program` (`crd::gpu::IGpuProgram*`, forward-declared — rhi header stays abstract), APPENDED at the struct end (positional inits unaffected). `VulkanGpuProgram::vk_module()` exposes the compiled module; `create_graphics_pipeline` resolves a stage from the program (wins) OR the legacy `ShaderModule`. Strangler-fig: existing `ShaderModule` consumers untouched; new consumers hold opaque programs. | ✅ **green** — a pipeline builds from opaque VS+FS programs on the shared device; **sandbox still renders** (292 frames, ShaderModule path unchanged); rhi_vulkan **4830/27**, kir-vulkan **33010**; tidy clean (+3 justified NOLINTs on pre-existing flag/format enums the direct-tidy surfaced) |
| **C2-d2** | **the migration facade + first consumer** ✅ — `rhi::Device::create_program(ShaderStage, cooked SPIR-V) → IGpuProgram` (NON-pure default nullptr; the ADOPTED `VulkanDevice` delegates to its owning `VulkanGpuContext`, mapping `rhi::ShaderStage`→`crd::gpu::ShaderStage`). So any consumer holding a `Device` mints opaque programs with no gpu-context threading. Edge `crd-rhi → crd-gpu-context` (header-only, acyclic). **First consumer migrated: renderer `ForwardRenderPath`** — all 3 `create_shader_module` sites → `create_program` + `vertex_program`/`fragment_program`. | ✅ **sandbox renders — 529 frames @ 176 fps, no VUID.** renderer 192/40 · rhi 152/32 · rhi_vulkan 4830/27 · kir-vulkan 33010, all green; tidy clean |
| **C2-d3** | **migrate the remaining PRODUCTION consumers** ✅ — **draw** (`renderer.cpp` + `gpu_types.hpp`: 6 module members `ShaderModule`→`IGpuProgram` + the loader + 6 desc sites) and **sandbox_layer** (5 sites, local). After this **NO production code calls `create_shader_module`** — the renderer, debug-draw, and the sandbox all hold opaque programs. | ✅ **sandbox renders — 529 frames @ 176 fps, no VUID**; draw 167/15 · kir-vulkan 33010 green; tidy clean (renamed a pre-existing `_pad_camera` the direct-tidy surfaced) |
| **C2-d4** | **RETIRE the ShaderModule surface** ✅ — deleted `crd/rhi/shader_module.hpp` (`ShaderModule`), `ShaderModuleDesc`, `Device::create_shader_module`, the graphics `*_shader` fields, and `VulkanShaderModule`. **Compute migrated too**: `ComputePipelineDesc.compute_shader`→`compute_program` (opaque `IGpuProgram`). The standalone rhi device mints programs via the new `crd::gpu::make_vulkan_program(VkDevice,…)` factory — the SAME constructor `IGpuContext::create_program` uses (ADR-0103: gpu-context-vulkan still owns authoring), so `test_rhi_vulkan` kept its device + queue-selection untouched. HLSL side: the Vulkan compiler now normalizes the SPIR-V entry to `main` (`-fspv-entrypoint-name=main`), so every minted program entry-points at `main` and the pipeline needs no source function name. | ✅ **I2 FULLY CLOSED** — `crd-no-shader-language-leak` gate green; no raw SPIR-V in any public rhi header. rhi 152/32 · renderer 192/40 · rhi_vulkan **4830/27** (compute+graphics on the standalone device) · geometry-shader-helpers **910/21** (21 HLSL manifests dispatch with the normalized entry) · kir-vulkan 33010 · sandbox smoke PASS. 14 files tidy-clean; MSVC + clang-cl both build clean. |
| **C2-e** | **migrate the Effect frontend** ✅ — `crd-shader` now names NO shading language: `runtime.cpp` takes an INJECTED `crd::shader::ISpirvCompiler` (dropped the shaderc loader entirely — struct, dynamic-load, `to_shaderc_kind`, `#include <shaderc/…>`, the CMake shaderc find/link). A new bridge module **`crd-shader-vulkan`** (`create_vulkan_spirv_compiler`) wraps `crd::gpu::compile_glsl_to_spirv` — it links both crd-shader + crd-gpu-context-vulkan so neither depends on the other (gpu-context stays rhi-free). The compiler is called with a new `optimize=false` flag so `OpName`s + dead bindings survive for spirv-reflect. Callers (sandbox, test_renderer, the crd-shader test suite) inject the compiler (declared before the runtime it borrows). **I1 allowlist EMPTIED.** | ✅ **I1 FULLY CLOSED — `crd-no-shader-language-leak` gate green with an EMPTY allowlist.** crd-shader-tests 139/21 · renderer 192/40 · resources 12301/78 · kir-vulkan 33010 · geometry 910 · sandbox smoke PASS. MSVC + clang-cl clean; 10 files tidy-clean. |
| **C2-f** | **retire rhi's own device creation** — `rhi-vulkan` no longer creates a `VkDevice`; one pipeline cache; full sweep. | full sweep + sandbox |
| **C3** | **`IRayTracingContext` — including the DXR-1.2 / RTX-Mega-Geometry frontier** — RT pipelines · SBT · AS binding · inline `rayQuery` · **Shader Execution Reordering** enable (`VK_EXT_ray_tracing_invocation_reorder`, the pipeline half of D-007 B9's `HitObject`/`ReorderThread`) · **opacity-micromap build** (`VK_EXT_opacity_micromap`) · **CLUSTER acceleration structures** (`VK_NV_cluster_acceleration_structure` — RTX Mega Geometry: cluster templates, streaming LoD, dynamic tessellation+displacement) · motion AS | an RT pipeline builds from IR-authored raygen/miss/closest-hit (feeds D-007 B9); a cluster-AS path builds + traces |
| **C4** | **DX12 raster** — `gpu-context-dx12` gains `IRasterContext` (mirroring C1's shader-object / dynamic-rendering / bindless model on D3D12: shader objects ≈ `ID3D12Device`/PSO-library, descriptor heaps, enhanced barriers) | the DX12 half of every render gate stops being "unreachable" | ⬜ |
| **C5** | **GPU-DRIVEN execution — the dispatch surface grows a GPU-generated path** — the `ComputeRecorder`/raster path gains **indirect-count** → **device-generated commands** (`VK_EXT_device_generated_commands`, Vulkan 1.3.296 / D3D12 `ExecuteIndirect`) → **WORK GRAPHS** (`VK_AMDX_shader_enqueue` + mesh nodes / D3D12 Work Graphs SM 6.8) so the GPU schedules its own work (the device half of D-007 B11's node shaders). Keep the recorder abstraction wide enough that this is an add-on, not a fork | a device-generated-command dispatch + a minimal work-graph run, both from IR-authored node shaders |
| **C6** | **Cooperative-vector device capability — neural shading enablement** — `VK_NV_cooperative_vector` feature/property query + the program path for coop-vector shaders (the device half of D-007 B10); cross-vendor mapping (DX12 cooperative vectors / CUDA tensor / Metal simdgroup-matrix) | a coop-vector MLP program builds + dispatches, result bit-matches the CPU MLP reference |

**Then** D-007 resumes at B3-c (GLSL VS+FS emitters) behind the finished seam.

## Frontier device/dispatch coverage (2025–26 research, 2026-07-10) — the context side

The C-slices were, as first written, a *correctness* convergence (get raster + RT + DX12 working behind the seam). This
table pulls the 2024–26 device/dispatch frontier out of the ADR-0077 "vision fog" and pins each item to a C-slice, so
**gpu-context handles ALL the shaders — including the cutting-edge ones — by construction, not aspiration.** The
shader-language half lives in **[D-007](D-007-ckir-universal-shader-ir.md)**'s frontier table.

| capability | real extension(s) / status | our slice |
|---|---|---|
| Shader objects (kill PSO permutation explosion — ADR-0101's stated fear) | `VK_EXT_shader_object` | **C1** design |
| Graphics pipeline library (link-time, anti-hitch fallback) | `VK_EXT_graphics_pipeline_library` | **C1** |
| Dynamic rendering (no `VkRenderPass`) | Vulkan 1.3 core, mandated in **1.4** | **C1** |
| Bindless / descriptor buffers + heaps | descriptor indexing (core 1.2) · `VK_EXT_descriptor_buffer` → `VK_EXT_descriptor_heap` | **C1/C2** |
| Variable rate shading | `VK_KHR_fragment_shading_rate` | **C1** state |
| Fragment-shader interlock / ROV · conservative raster · multiview | `VK_EXT_fragment_shader_interlock` · `VK_EXT_conservative_rasterization` · `VK_KHR_multiview` | **C1** state |
| Device-generated commands (= D3D12 ExecuteIndirect) | `VK_EXT_device_generated_commands` (Vulkan 1.3.296, Sep 2024) — "biggest addition since ray tracing" | **C5** |
| Work graphs (GPU self-scheduling) + mesh nodes | `VK_AMDX_shader_enqueue` (experimental) · D3D12 Work Graphs (SM 6.8 retail) | **C5** |
| Shader Execution Reordering (pipeline enable) | `VK_EXT_ray_tracing_invocation_reorder` · DXR 1.2 | **C3** |
| Opacity micromap build | `VK_EXT_opacity_micromap` | **C3** |
| Cluster AS (RTX Mega Geometry) | `VK_NV_cluster_acceleration_structure` (driver ≥ 572.16) | **C3** |
| Cooperative vectors (neural shading device enable) | `VK_NV_cooperative_vector` · DX12 cooperative vectors (SM 6.9 retail) | **C6** |

**Sources:** [VK_EXT_shader_object](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_EXT_shader_object.adoc) · [graphics pipeline library](https://www.khronos.org/blog/reducing-draw-time-hitching-with-vk-ext-graphics-pipeline-library) · [VK_EXT_descriptor_buffer](https://www.khronos.org/blog/vk-ext-descriptor-buffer) · [Vulkan 1.4](https://www.khronos.org/news/press/khronos-streamlines-development-and-deployment-of-gpu-accelerated-applications-with-vulkan-1.4) · [VK_EXT_device_generated_commands](https://developer.nvidia.com/blog/new-vulkan-device-generated-commands/) · [Work Graphs mesh nodes (Vulkan)](https://gpuopen.com/learn/gpu-workgraphs-mesh-nodes-vulkan/) · [D3D12 Work Graphs](https://developer.nvidia.com/blog/advancing-gpu-driven-rendering-with-work-graphs-in-direct3d-12/) · [SER (Vulkan)](https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder) · [RTX Mega Geometry](https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples/) · [VK_NV_cooperative_vector](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_NV_cooperative_vector.html).

## Stage model — complete from day one

`ShaderStage` = the 14 SPIR-V execution models, matching `docs/systems/shader-ir-corpus-and-stages.md` §2:

```
Compute · Vertex · TessControl · TessEval · Geometry · Fragment · Task · Mesh
       · RayGen · Intersection · AnyHit · ClosestHit · Miss · Callable
```

Mesh/task is the amplification path; **geometry is supported-but-discouraged** (single-thread bottleneck, low occupancy —
industry consensus). A backend that cannot yet emit a stage **refuses loudly**; it never silently falls back to compute.

> **Why the enum lands complete before the emitters do.** D-007 B3-a shipped `KStage {Compute, Vertex, Fragment}`. A
> 3-value enum bakes a 3-stage assumption into every `switch` that consumes it, and each such switch is a silent
> `default:` waiting to mis-lower a mesh shader. Same class as `MatFromCols` hardcoding three columns (B0-2) and
> `KOp::Cast` hitting `default: return false` in three emitters (B0-4). **Declare the whole domain; refuse the
> unimplemented part loudly.**

## Escape hatch (ADR-0101 §4) — preserved, and boxed

Hand-written per-backend kernels stay legal: `geometry-bvh-gpu`'s LBVH `.comp`, imgui/draw overlay GLSL, third-party
HLSL. They cook to blobs and load through `create_program(cooked_name)` — **explicitly non-portable, never re-entering
the engine as source.** The asset cooker keeps its GLSL→SPIR-V path; it just calls the *Vulkan backend's* compiler
instead of `crd-shader`'s.
