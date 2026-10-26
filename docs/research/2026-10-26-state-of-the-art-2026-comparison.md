# Cerid and the 2026 state of the art: area-by-area comparison

<!-- doc-role: reference -->
> Technical reference and proposals; verify dated claims against current contracts/source. Live slice state exists only in
> [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md). Agent entry: [START_HERE](../../START_HERE.md).

**Date:** 2026-10-26. **Status:** Recorded research; every proposed row below is a proposal for the user, not an
accepted change. Nothing in [ROADMAP](../ROADMAP.md) was edited by this research.

## Question, method and evidence boundary

What do the best engines, APIs and libraries ship or announce in 2026, how does Cerid compare in each area, and which
missing pieces should become roadmap rows? The bar is the user's standing order: the proper, full-feature,
gold-standard implementation, never the "modern only" or easy option.

- **Cerid side.** Read, not run: the 904 rows of [ROADMAP](../ROADMAP.md#master-table), the
  [capability registry](../capabilities/gpu-platform-capabilities.toml), [systems](../systems/README.md), the
  [execution contract](../design/renderer-ui-execution-contract.md), the
  [quality contract](../design/system-quality-contract.md), the [DIAG contract](../design/runtime-diagnostics.md),
  the [whole-system review](2026-09-12-cerid-whole-system-review.md), the
  [QUAL.0 brief](../sessions/2026-10-26-qual-0-review-brief.md), the [RAH-0 brief](../sessions/2026-10-26-rah-0-review-brief.md)
  and source greps of `engine/` on 2026-10-26. A grep hit is a source fact, not a runtime or quality claim. No build,
  test, benchmark or CI ran for this document.
- **Outside side.** Web pages read on 2026-10-26 (search snippets or fetched pages). Sources are listed at the end as
  W-numbers with their publication dates. **(K)** marks a link given from prior knowledge and not re-read today; its
  current version or status is uncertain. "Uncertain" in a cell means the sources disagree or were not conclusive.
- **No downloads** were made. Items that can only be judged with a download are in
  [Needs a download](#needs-a-download).

## The user's 2026-10-26 decisions that shape this comparison

Recorded in the [user decisions session](../sessions/2026-10-26-user-decisions-platform-floors-and-research.md); they
answer the open questions in the two briefs. Where a brief recommended otherwise, the decision wins. That session also
asks for three serial research parts; this document is the first (engine, GPU, rendering, computational geometry and
navigation) and also covers the other engine areas the coordinator listed.

1. **x64 SSE2 floor stays supported** (QUAL.0 question 1, option b). CMake picks one SIMD level at compile time
   (`cmake/CrdSimd.cmake`, `CRD_SIMD_LEVEL` = auto/scalar/sse2/avx2/neon/native, default AVX2), and outside
   `engine/foundation/time` there is no CPUID/XGETBV feature detection. Runtime dispatch with per-ISA kernels is
   required and not implemented; the new [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) row (Order 093, added
   2026-10-26) owns it.
2. **Older-GPU fallbacks** (RAH-0 question 2, option b, against the brief's recommendation a). Every RAH row must
   carry an explicitly authored fallback path, not only a modern floor.
3. **Peer-relative budgets** (QUAL.0 question 5, option c): beat the best peer; parity only when no lever remains, with
   that evidence recorded. Approved peers: Tracy, Eigen, cuBLAS, MKL/OpenBLAS, Mitsuba 3/pbrt-v4, Filament, Jolt and
   PhysX; each download is approved separately.
4. **All nine RAH breaking changes** (RAH-0 question 1) are accepted; every asset is re-cooked under new format
   versions; RAH-1 first; the refreshed no-loss census is the gate.
5. **Runtime changeability** of frame graphs, pipelines, shaders, materials and every GPU program, and **capability
   ownership** (one owning module per capability, [OWN.1](../ROADMAP.md#slice-own.1)), both recorded the same day.

## Highest-impact gaps first

Ranked by how many later rows they block, how far Cerid is from the 2026 reference, and the user decisions above.
Details and placements are in [Gaps and proposed roadmap changes](#gaps-and-proposed-roadmap-changes).

| Rank | Gap | Cerid today | Proposed owner |
|---:|---|---|---|
| 1 | Runtime CPU ISA detection and per-ISA kernel dispatch (SSE2 to AVX-512/AVX10.2, NEON/SVE2) | Missing in source; planned by SIMD-DISPATCH (Order 093, added 2026-10-26) | Amend SIMD-DISPATCH |
| 2 | Authored GPU feature tiers with older-GPU fallbacks in every RAH row | Partial: capability probes and registry, no tier ladder | New GPU-TIER.0; amend RAH-1 to RAH-8 |
| 2a | Fine-grained live replacement of every GPU program and frame graph with no visible hitch (fallback while compiling, staged pipeline parts, generation-safe retirement) | Partial: frame reload shipped, program reload coarse (retire-all), async compute warmup; RAH-7 Open | Amend RAH-7.a to 7.d; new RAH-7.e, RAH-7.f |
| 3 | One sync model over DX12 enhanced barriers and Vulkan synchronization2, legacy fallback | Missing on DX12 (legacy `ResourceBarrier` in all three DX12 contexts) | New RAH-SYNC |
| 4 | Heap-style binding: SM 6.6 resource heaps, `VK_EXT_descriptor_heap`, bounded descriptor-set fallback | Planned in RAH-2 without these models | Amend RAH-2.a/2.c |
| 5 | DX12 Agility SDK deployment and a shader-model ladder to SM 6.9 (6.10 preview) | Missing: SM 6.0/6.1/6.5 profiles, `lib_6_8` only via the Windows SDK DXC | New DX12-AGILITY |
| 6 | Vulkan 1.4 baseline and Roadmap 2026 profile as the top tier, through Khronos profiles | Missing: device created at 1.3 | New VK-PROFILE; amend RAH-8.a |
| 7 | DirectStorage-class direct I/O and GPU decompression (GDeflate, Zstd) | Missing (no hits in source or docs) | New RAH-5.d |
| 8 | GPU crash forensics: Aftermath, RGD, DirectX Dump Files beside DRED/`VK_EXT_device_fault` | Partial: DRED and device-fault evidence (DIAG.7b/7c) | New GPU-FORENSICS |
| 9 | Cross-vendor neural-shading provider (DX LinAlg, Vulkan cooperative matrix) and NTC/neural materials/NRC | Planned (MLR rows); probes target NV cooperative vector | Amend MLR-render.1/.2, MLR-2 |
| 10 | Frame generation, latency reduction and frame pacing | Seam only in TPR-1 | Amend TPR-1, PRESENT.2 |
| 11 | Ray-tracing 2026 set: SER/OMM on EXT and DXR 1.2, cluster AS, partitioned TLAS, swept spheres | Mechanic probes on NV extensions only | Amend RAH-4.c, RTX-0, RTX-4 |
| 12 | Pipeline delivery: pipeline binaries, Advanced Shader Delivery, partial graphics programs | Missing | Amend RAH-7.a, PQP-4.renderer |
| 13 | Number-type-generic exact geometry kernel (filtered float, exact integer, rational, fixed point) | Missing: Shewchuk predicates for double only | New GEOM-K.0 to GEOM-K.4 |
| 14 | Navigation: navmesh, grid/graph search, crowds, dynamic tiles, 3D, routing, geodesy | Missing: no module, no row | New NAV.0 to NAV.7 |
| 15 | Exact Booleans, arrangements, offsets, skeletons and a geometry qualification board | Partial: float polygon Boolean, CDT, repair | New GEOM-EXACT.1/.2, GEOM-QUAL |

Rank 2a is inserted beside rank 2 because it is the engine's defining principle (see
[Runtime changeability](#runtime-changeability)); it does not displace the other fifteen.

Structural note, not ranked: physics (eylem) and animation are almost entirely Later rows, while Jolt, PhysX, Godot,
Unity and Unreal ship mature rigid, character, vehicle, GPU and learned-motion systems. The order is the user's
explicit choice ([ADR-0129](../decisions/0129-renderer-ui-editor-delivery-order.md)); it is stated here so that
"full-feature engine" is not read as already true for those domains.

<a id="runtime-changeability"></a>
## Runtime changeability of the GPU/frame-graph stack

Cerid's principle: frame graphs, render pipelines, passes, shaders, materials and every GPU program are authored in
CEIR/CHIR/CKIR and can be edited, recompiled and swapped while the application runs, without restarting or rebuilding
the engine. The [execution contract](../design/renderer-ui-execution-contract.md) states the replacement test
(change or shadow an `engine://` asset, cook and install it while running, observe the new behaviour) and assigns
[RAH-7](../ROADMAP.md#slice-rah-7): a dependency registry, staged atomic commit, last-good generation on late failure,
and GPU retirement after real queue completion rather than an assumed frame count. This section judges the 2026 state
of the art on three questions: how live it is, what a change costs, and how it stays correct.

| Axis | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| Live shader and material edits | Unreal material editor and changed-shader recompile, Unity and Godot editor shader edits (K): all live inside the editor | Shipped: frame-asset reload (`hot_reload_frame`, raf 5 / ceir 5) and program reload marked "shipped-coarse" in the registry; RAH-7 granular pending |
| Live frame-graph / pipeline structure | Unreal RDG passes are C++ (engine rebuild or Live Coding); Unity render graph in C# scripts (script recompile and domain reload); Godot compositor effects in scripts (K) | Ahead in design: frame graphs are `.frame.toml` CEIR programs that already reload (registry `hot_reload_frame`); structural edits (add/remove passes, resource re-realization) through the shared registry are RAH-7 work |
| Live behaviour logic | Unreal Blueprints and Live Coding; Unity C# hot reload; Godot GDScript (K) | CHIR is a five-construct prototype; [LANG](../ROADMAP.md#slice-lang) and [SCRIPT.3](../ROADMAP.md#slice-script.3) own real live behaviour and native reload |
| Hitch on first use | Unreal PSO precaching with async background compiles and skip-draw for late PSOs, "never able to guarantee" readiness [W77](#w77); Unity 6 `GraphicsStateCollection` tracing and progressive warm-up (experimental), cache-miss tracing in 6.6 alpha [W78](#w78); Godot 4.4 ubershaders with specialization constants compiled in the background, 4.5 export-time shader baker (20x load-time cut on one demo, vendor figure) [W79](#w79) | Partial: async compute pipeline warm-up on the fiber pool after a warm `VkPipelineCache` (`shadercook/warmup.hpp`); no draw-time fallback while a variant compiles |
| Pipeline granularity | `VK_EXT_shader_object` (no monolithic pipeline) and `VK_EXT_graphics_pipeline_library` (four separately compiled parts, linked) [W80](#w80); DX12 Partial Graphics Programs preview [W15](#w15); DX12 state objects with incremental additions for RT and work graphs (K) | Vulkan raster on shader objects (`raster_context.hpp`); DX12 builds whole PSOs and whole RT state objects; no pipeline library or incremental state-object path |
| Compiled-pipeline delivery | Advanced Shader Delivery (SODB to per-driver PSDB, recompiled on driver update) [W16](#w16); `VK_KHR_pipeline_binary` (K) | Local cache hierarchy ([ADR-0028](../decisions/0028-shader-cache-hierarchy.md)); no distributable driver-level cache |
| Variant explosion | UE 5.8 cut shader permutations across Lumen, VSM, Substrate, MegaLights [W29](#w29); Unity shader build settings strip keywords [W31](#w31) | Variant keys ([ADR-0026](../decisions/0026-shader-variant-key.md)); joint VS+FS specialization [D6](../ROADMAP.md#slice-d6) Open |
| Correctness during replacement | Engine-internal deferred deletion by frame count (K); few engines publish a staged, all-or-nothing replacement contract | Ahead in contract: staged atomic commit, last-good generation, completion-driven retirement, unaffected-generation conservation tests ([RAH-7.b](../ROADMAP.md#slice-rah-7.b), [RAH-7.d](../ROADMAP.md#slice-rah-7.d)); generation-checked handles exist (DIAG.3e) |

### Where Cerid's design must go further

- **No-hitch replacement.** The contract guarantees correctness but not smoothness. Every program needs an authored
  fallback that draws while its new variant compiles: the last-good generation for an edit, a generic (ubershader or
  specialization-constant) variant for a first use, or an explicit skip with a diagnostic. The choice is per asset and
  visible, never a silent stall.
- **Compile cost is a first-class budget.** Time from save to visible change, the worst frame-time spike during a swap
  and compile CPU time are measured per backend with peer-relative budgets where a peer exists, through the DIAG
  latency and flight-recorder routes.
- **Use the finest pipeline unit each API offers**: shader objects and graphics pipeline libraries on Vulkan, partial
  graphics programs and pipeline libraries on DX12, incremental state-object additions for ray-tracing hit groups and
  work-graph nodes, so one material edit relinks one part instead of rebuilding a whole pipeline family.
- **Structural frame-graph edits** (adding, removing or reordering passes, changing resource formats or sizes) re-plan
  resource lifetimes and aliasing for the next generation while frames in flight keep the old plan until their fences
  complete. Multi-window and multi-queue cases are covered by RAH-7.d.
- **Delivery for players**: authored edits stay live in development, and the same pipeline identities feed a
  shipping-time pipeline collection (Advanced Shader Delivery database, Vulkan pipeline binaries) so a shipped game
  does not compile on first use.
- **Every later area inherits this axis**: neural weights, tiers and fallbacks, streaming formats, UI styles, physics
  parameters and navigation data are reloadable assets with the same staged commit and generation rules. Each area
  below notes its part.

## 1. GPU APIs and features

| Feature | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| Vulkan 1.4 core | 1.4 (Dec 2024) mandates push descriptors, dynamic rendering local read, scalar layouts, streaming transfers, maintenance up to 6 [W04](#w04) | Missing: device at Vulkan 1.3; dynamic rendering always on, shader objects used (`raster_context.hpp`); maintenance5 probed for DGC |
| Vulkan Roadmap 2026 milestone | Requires VRS, shader clock, host image copy, compute derivatives, swapchain improvements, higher limits; conforming devices by end of 2026 [W01](#w01) [W05](#w05) | Missing; no profile use (`VK_KHR_profiles` absent); limits are fixed caps (RAH-0 brief) |
| Descriptor heaps | `VK_EXT_descriptor_heap` (Jan 2026) replaces descriptor sets, on a path to core; NVIDIA 610+ drivers, SDK 1.4.341 [W01](#w01) [W02](#w02) [W03](#w03); DX12 SM 6.6 `ResourceDescriptorHeap` (K) | Planned without heap models: [RAH-2](../ROADMAP.md#slice-rah-2) (resource tables, BDA); eight-texture cap still in source |
| Device generated commands | `VK_EXT_device_generated_commands` cross-vendor; DX12 ExecuteIndirect (K) | Mechanic: NV and EXT DGC contexts (`vulkan_dgc_context.hpp`, CEIR-20c-2); production owner [RAH-3](../ROADMAP.md#slice-rah-3) |
| Mesh/task shaders | `VK_EXT_mesh_shader`, DX12 SM 6.5; UE Nanite uses them | Shipped asset `mesh_shader_frame` (registry raf 5 / ceir 6); cluster mesh shader mechanic ([VGE-3.runtime](../ROADMAP.md#slice-vge-3.runtime)) |
| SER | `VK_EXT_ray_tracing_invocation_reorder` ratified Nov 2025 (no-op where hardware cannot reorder) [W06](#w06); DXR 1.2 announced Mar 2025 [W10](#w10), SER retail in SM 6.9, Feb 2026 [W12](#w12) | Mechanic on `VK_NV_ray_tracing_invocation_reorder` only (`rt_capabilities.hpp`); DX12 missing |
| Opacity micromaps | `VK_EXT_opacity_micromap`; DXR 1.2 OMM, HLSL part in SM 6.9; Alan Wake 2 cut RT cost by a third with OMM+SER [W11](#w11) | Mechanic on Vulkan (FA-1); DX12 missing; owner [RAH-4.c](../ROADMAP.md#slice-rah-4.c) |
| Cluster AS and partitioned TLAS | `VK_NV_cluster_acceleration_structure`, `VK_NV_partitioned_acceleration_structure` (not ratified), RTX Mega Geometry, OptiX 9 [W07](#w07) [W08](#w08) [W76](#w76); RTX Kit 2026.3 adds Mega Geometry 2.0 [W40](#w40) | Cluster AS mechanic (FA-3); partitioned TLAS missing; owners [RTX-0.runtime](../ROADMAP.md#slice-rtx-0.runtime), [VGE-6.runtime](../ROADMAP.md#slice-vge-6.runtime) |
| Cooperative matrix / vector | Vulkan `VK_KHR_cooperative_matrix`, `VK_EXT_cooperative_matrix_maintenance1` (Aug 2026), NV matrix2/decode-vector [W24](#w24); `VK_NV_cooperative_vector` not ratified; DX12 Cooperative Vector superseded by LinAlg matrix in SM 6.10 preview (Apr 2026) [W12](#w12) [W13](#w13) [W14](#w14) | Cooperative matrix GEMM proven (v17-g research); cooperative vector probe is NV only; owners [MLR-1](../ROADMAP.md#slice-mlr-1), [MLR-4](../ROADMAP.md#slice-mlr-4) |
| Work graphs | DX12 work graphs (SM 6.8); mesh nodes preview (2024); Vulkan only `VK_AMDX_shader_enqueue`, provisional [W23](#w23); 2026 status of mesh nodes uncertain | Mechanic: `work_graph.hpp` topology plan and `lib_6_8` node library (CEIR-20c-1); owner [CGP-0](../ROADMAP.md#slice-cgp-0), [MLR-4](../ROADMAP.md#slice-mlr-4) |
| Agility SDK and shader models | SM 6.9 retail with Agility 1.619 (long vectors to 1024, 16/64-bit ops mandatory, SER/OMM); 1.719 preview: fence barriers; SM 6.10 preview: LinAlg [W12](#w12) [W13](#w13) | Missing: no `D3D12SDKVersion` export; profiles `vs_6_0`, `ps_6_0/6_1`, `ms/as_6_5`, `cs_6_5`, `lib_6_3`, `lib_6_8` (`dx12_context.cpp`) |
| Enhanced barriers | DX12 enhanced barriers, extended by fence barriers (preview, Feb 2026) [W12](#w12) | Missing: legacy `ResourceBarrier` in compute, raster and RT contexts |
| GPU upload heaps | DX12 `D3D12_HEAP_TYPE_GPU_UPLOAD` (ReBAR) (K); Vulkan host-visible device-local memory and host image copy (Roadmap 2026) [W01](#w01) | Missing; owner by content [RAH-5](../ROADMAP.md#slice-rah-5) |
| Direct I/O and GPU decompression | DirectStorage 1.1 GDeflate (K); DirectStorage 1.4 preview adds Zstd and open-source decompression shader, Mar 2026 [W17](#w17) | Missing (no `DirectStorage`, `GDeflate` in source or docs); streaming rows [RAH-5.b](../ROADMAP.md#slice-rah-5.b), [VGE-1.runtime](../ROADMAP.md#slice-vge-1.runtime) |
| Sampler feedback | DX12 Ultimate sampler feedback (K); no Khronos equivalent found (uncertain) | Abstraction planned in [TXS-2.runtime](../ROADMAP.md#slice-txs-2.runtime) |
| VRS | Required by Vulkan Roadmap 2026 [W01](#w01); DX12 VRS tier 2 (K) | Seam planned in [RAH-1.d](../ROADMAP.md#slice-rah-1.d); `ShadingRate` appears in 18 source files (not reviewed) |
| HDR/WCG output | Windows Advanced Color, HDR10/scRGB, `VK_EXT_hdr_metadata` (K); Godot 4.7 added HDR output, Jun 2026 [W35](#w35) | Planned in [PRESENT.2](../ROADMAP.md#slice-present.2) and [TPR-2.runtime](../ROADMAP.md#slice-tpr-2.runtime); no `hdr_metadata` in source |
| Pipeline/shader delivery | Advanced Shader Delivery (SODB to PSDB per driver, Avowed launch time down 85%), general availability Oct 2026 [W16](#w16) [W13](#w13); Partial Graphics Programs preview May 2026 [W15](#w15); `VK_KHR_pipeline_binary` (K) | Shader objects avoid monolithic PSOs on Vulkan; DX12 PSO caching and pipeline binaries missing; cache hierarchy [ADR-0028](../decisions/0028-shader-cache-hierarchy.md) |
| Metal 4 | MTL4 command buffers from the device, argument tables, residency sets, ML encoder, MetalFX frame interpolation (WWDC 2025) [W26](#w26) | `kir-metal` emitter only; provider owner [MAC.1](../ROADMAP.md#slice-mac.1) |
| WebGPU | In Chrome, Edge, Firefox (Windows, ARM Mac) and Safari 26 since Nov 2025; W3C Candidate Recommendation draft (uncertain) [W27](#w27) | `kir-webgpu` WGSL emitter; runtime owner [WEB.1](../ROADMAP.md#slice-web.1) |
| Shader languages | Slang under Khronos since Nov 2024, autodiff, SPIR-V/HLSL/GLSL/WGSL/MSL targets, MaterialX backend [W21](#w21) [W22](#w22); HLSL in Clang and SPIR-V as the DX interchange from SM 7 [W20](#w20) | CKIR is the source of truth ([ADR-0101](../decisions/0101-ir-is-source-of-truth-for-all-shaders.md)); emits GLSL (glslang), HLSL (DXC), MSL, WGSL, CUDA, HIP; no Slang import or peer |
| Older-GPU support pattern | UE 5.8: Nanite/VSM need SM 6.6 atomics or `VK_KHR_shader_atomic_int64`; Lumen/MegaLights need RX 6000, Arc A, RTX 2000+; TSR runs on SM 5; Lumen Lite (beta) targets low-end and Switch 2 [W28](#w28) [W29](#w29) [W30](#w30); Godot keeps Forward+, Mobile and Compatibility renderers (K) | Explicit fallback rule in the [execution contract](../design/renderer-ui-execution-contract.md); no tier ladder; [RAH-8](../ROADMAP.md#slice-rah-8) unifies capabilities but does not define tiers |

### Gaps in area 1

- **No tier ladder.** The 2026 pattern is one asset per feature with declared tiers: a modern path (heaps, mesh
  shaders, hardware RT, Roadmap 2026), a mid path (descriptor indexing, compute culling, software RT) and a legacy path
  (bounded descriptor sets, vertex pipeline, raster-only lighting), as Unreal does with Nanite fallback meshes, TSR on
  SM 5 and Lumen Lite. The user chose fallbacks, so the tiers must be authored assets with tests, not caps.
- **DX12 is behind the Vulkan side.** No Agility SDK, no enhanced barriers, no SM 6.6+ dynamic resources, no GPU upload
  heaps, no DXR 1.2. Vulkan has more mechanic probes but still sits at 1.3 and on NV-only extensions where EXT
  equivalents now exist (SER).
- **No direct I/O or GPU decompression.** Virtual geometry and virtual texturing (VGE-1, TXS-2/3) depend on it.
- **No delivery story for compiled pipelines** beyond the local cache.
- **Shader front ends.** Keeping CKIR as the source of truth is right for Cerid's authoring model, but Slang is now
  the shared cross-vendor neural-shading language; Cerid needs Slang as a measured peer and an import route for
  third-party shader code, and must track the SM 7 SPIR-V-for-D3D change in its emitters.
- **Runtime changeability.** Tier selection and fallback must themselves be live: switching backend, adapter or tier
  is a transactional replacement under RAH-7 (planned for [EDITOR.5](../ROADMAP.md#slice-editor.5)), and binding
  models (heaps, descriptor sets) must let one program's layout change without rebuilding unrelated tables.

## 2. Rendering techniques

| Technique | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| Dynamic GI | Lumen (screen, software and hardware traces, surface and radiance caches) (K); Lumen Lite beta with irradiance fields in UE 5.8 [W30](#w30) | Open: [ARG-6.runtime](../ROADMAP.md#slice-arg-6.runtime) (Lumen-class), [ARG-2.runtime](../ROADMAP.md#slice-arg-2.runtime) (DDGI), [ARG-3.runtime](../ROADMAP.md#slice-arg-3.runtime), [ARG-4.runtime](../ROADMAP.md#slice-arg-4.runtime) |
| ReSTIR family | ReSTIR DI (Bitterli 2020) and GRIS/ReSTIR PT (Lin 2022) (K); MegaLights production in UE 5.8 [W29](#w29) | Open: [LSH-4.runtime](../ROADMAP.md#slice-lsh-4.runtime) (ReSTIR-DI, MegaLights-class), [ARG-5.runtime](../ROADMAP.md#slice-arg-5.runtime), [RTX-3.runtime](../ROADMAP.md#slice-rtx-3.runtime) |
| Radiance caching (neural and classic) | NRC (Müller 2021) in RTX Kit (K); AMD FSR Redstone radiance caching due to developers in 2026 [W37](#w37) | Open: ARG-3/ARG-5 radiance cache; neural part [MLR-2.runtime](../ROADMAP.md#slice-mlr-2.runtime) |
| Virtualized geometry | Nanite (cluster DAG, software raster, streaming) (K); RTX Mega Geometry 2.0 [W40](#w40); UE 5.8 fast geometry streaming (experimental) [W29](#w29) | VGE-0 to VGE-6 Open; `cluster_mesh_shader` mechanic (raf 4 / ceir 5); geometry has meshlets and cluster DAG |
| Virtual shadow maps | UE VSM (K) | Open: [LSH-3.core](../ROADMAP.md#slice-lsh-3.core), [LSH-3.runtime](../ROADMAP.md#slice-lsh-3.runtime) |
| Many lights | MegaLights production-ready, hundreds of shadowed area lights at 60 fps on consoles [W30](#w30) | Open: LSH-4.runtime; clustered/Forward+ frames exist (`forward_plus_gpu`, `forward_clustered_3d_gpu`) |
| Upscaling and frame generation | DLSS 4.5 (second transformer, up to 6x dynamic MFG) [W38](#w38); FSR Redstone ML upscaling/frame gen/ray regeneration on RDNA 4 [W37](#w37); XeSS 3 MFG up to 4x [W39](#w39); MetalFX frame interpolation [W26](#w26) | Open: [TPR-1.runtime](../ROADMAP.md#slice-tpr-1.runtime) lists vendor and frame-gen seams; TAA resolve asset exists (`taa_resolve.ckir`) |
| Ray reconstruction / ML denoise | DLSS Ray Reconstruction (transformer), FSR ray regeneration [W38](#w38) [W37](#w37) | Open: [RTX-2.runtime](../ROADMAP.md#slice-rtx-2.runtime) (NRD seam), MLR-2.runtime |
| Neural texture compression | RTX NTC 0.10 beta on the DX LinAlg preview, up to 8x memory saving claimed (vendor figure) [W40](#w40) | Missing as a named item; MLR-2.runtime "neural materials/caches" only |
| Neural materials | RTX Neural Shading 1.4 on LinAlg [W40](#w40); DX "ML era" names per-pixel inference [W14](#w14) | Open: MLR-2.runtime; tensor MLP CEIR program gate-only (registry) |
| Gaussian splatting | glTF `KHR_gaussian_splatting` release candidate Feb 2026, Cesium/Babylon/PlayCanvas support [W41](#w41) | Research [2026-07-21 3DGS](2026-07-21-3dgs-frontier.md); runtime owner [GVA-5.runtime](../ROADMAP.md#slice-gva-5.runtime) |
| Real-time path tracing | DXR/Vulkan PT with SER+OMM, ReSTIR PT, denoisers [W11](#w11) | Open: [RPL-5.runtime](../ROADMAP.md#slice-rpl-5.runtime), RTX-3.runtime; wavefront trace/shade assets exist |
| Offline transport | pbrt-v4, Mitsuba 3 (approved peers) | Open: [OFF-1](../ROADMAP.md#slice-off-1) to [OFF-9](../ROADMAP.md#slice-off-9) |
| Volumetrics, sky | UE heterogeneous volumes and sparse volume textures (K); UE 5.8 fog screen-space scattering experimental [W29](#w29) | Open: [VFX-2.runtime](../ROADMAP.md#slice-vfx-2.runtime), [VFX-3.runtime](../ROADMAP.md#slice-vfx-3.runtime) |
| Hair and strands | UE groom (K); Jolt 5.6 GPU Cosserat-rod hair (work in progress) [W49](#w49); NV linear swept spheres (K) | Research [hair dossier](2026-07-19-hair-fur-frontier-collection.md); owners [MAT-3.runtime](../ROADMAP.md#slice-mat-3.runtime), GVA-5.runtime, RTX-4.runtime |
| Skin, water | Substrate materials (UE) (K) | MAT-3.runtime; ocean recorded under [REN-OCEAN](../ROADMAP.md#slice-ren-ocean) |
| Color pipeline | ACES 2.0 output transforms built into OCIO 2.5 configs; OCIO 2.5 adds Vulkan [W42](#w42); AgX in Blender (K) | AgX and sRGB tonemap shipped (raf 5 / ceir 6); ACES 2 and OCIO open in [TPR-2.runtime](../ROADMAP.md#slice-tpr-2.runtime) |
| NPR | UE 5.8 Substrate NPR toon shading (experimental) [W29](#w29) | Open: [MAT-4.runtime](../ROADMAP.md#slice-mat-4.runtime) |

### Gaps in area 2

- **Coverage is planned, delivery is not.** The retained catalogue already names almost every 2026 technique; only the
  forward/CSM/AgX/visibility-buffer/hybrid-RT frames are shipped assets. Each family close needs runtime, reference,
  both backends, reload and peer-relative budgets; none is closed.
- **Named items missing from the catalogue:** neural texture compression (as its own asset family with a block
  compression fallback for older GPUs), ML frame generation with latency reduction, ACES 2.0 output transforms with
  their OCIO 2.5 config interop, swept-sphere hair primitives, and the glTF Gaussian splatting extension.
- **Vendor SDKs are closed binaries.** DLSS, FSR 4 ML paths and XeSS need SDK downloads; a Cerid-owned upscaler and
  frame interpolator is the fallback for every vendor and the reference for tests.
- **Runtime changeability.** Peers expose techniques as engine settings over C++ passes; Cerid's techniques are
  frame/CKIR assets, so each family close must prove a live edit of the technique itself (not only its parameters),
  with history buffers (TAA, ReSTIR reservoirs, radiance caches) invalidated or migrated along the dependency closure.

## 3. CPU and engine architecture

| Feature | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| Fiber job system | Naughty Dog fibers (GDC 2015) [W47](#w47); UE Tasks with prerequisites and pipes [W46](#w46) | Shipped fiber pool (ADR-0015, ADR-0033), repaired and measured in Oct 2026 sessions; hardening [JOBS-HARD](../ROADMAP.md#slice-jobs-hard) Later |
| Runtime ISA dispatch | Highway `HWY_DYNAMIC_DISPATCH`, targets to AVX3_SPR and AVX10_2, SVE/SVE2 (1.3.0, late 2025) [W43](#w43); GCC/Clang function multiversioning (K) | Missing in source: compile-time `CRD_SIMD_LEVEL` only; owner [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch); ADR-0088 asm runtime dispatch was reverted (asm only); no CPUID outside `time/clocks` |
| AVX-512 / AVX10.2 / APX | Intel Nova Lake (late 2026) and Diamond Rapids list AVX10.2 and APX [W44](#w44); AMD Zen 4/5 AVX-512 (K) | Missing: no AVX-512 kernels (one unrelated grep hit) |
| ARM64 SVE/SME | Highway SVE/SVE2 targets [W43](#w43); Apple M4 SME (K) | NEON level only; [JOBS-PORT.2](../ROADMAP.md#slice-jobs-port.2) native ARM64 open |
| Hybrid P/E cores and QoS | Windows `SetThreadInformation` power throttling (EcoQoS) and process QoS [W45](#w45); Thread Director (K) | Partial: P-core counting by EfficiencyClass and opt-in P-core routing (ADR-0094, `topology.cpp`); no QoS classes, no NUMA |
| ECS | Bevy relationships (0.16), observers and render graph as systems (0.19) [W36](#w36); Unity DOTS (K) | Shipped hybrid ECS (ADR-0020, 0049 to 0054); [ECS-EDIT](../ROADMAP.md#slice-ecs-edit), [ECS-HARD](../ROADMAP.md#slice-ecs-hard) |
| Deterministic simulation and rollback | Box2D v3 cross-platform float determinism across x64/ARM and three compilers [W48](#w48); GGPO-style rollback (K) | ADR-0063 determinism contract; DIAG.9 replay envelopes; rollback netcode [4.2c](../ROADMAP.md#slice-4.2c) Later |

### Gaps in area 3

- **Runtime dispatch is the largest CPU gap** (owned since 2026-10-26 by SIMD-DISPATCH, whose text already covers
  per-level kernel units, CPUID plus XCR0, a forced-level override, the LTCG inlining guard, Intel SDE and per-level
  boards). The SSE2 floor plus an AVX2 default needs per-ISA compilation units,
  a detection layer (CPUID leaves, XGETBV/OS XSAVE state, AVX-512/AVX10 version leaf, ARM64 HWCAP/ID registers),
  dispatch tables resolved once, per-tier determinism rules (ADR-0063), forced-tier test lanes and per-tier peer
  boards. Highway is the reference design and a measured peer; Cerid keeps its intrinsics-first kernels (ADR-0082).
- **Hybrid cores and NUMA** are in JOBS-HARD.2 (Later), but the renderer frame graph and audio will need QoS classes
  before then.
- **Runtime changeability.** Pipeline compiles and cooks run on the job system; they need their own QoS class and
  admission limit so a live edit does not steal frame or audio workers.

## 4. Diagnostics and tooling

| Tool or capability | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| CPU/GPU profiler | Tracy 0.14 (capture daemon, merge, section markers); 0.13 added GDK, ROCm and optional LLM integration [W52](#w52) | Own perf/flight recorder (DIAG.6), Perfetto export, Tracy 0.14.1 approved peer ([session](../sessions/2026-10-26-diag-11b-tracy-peer.md)) |
| Sampling profilers | Superluminal (K); WPR/xperf | External sampling workflows (DIAG.6c); Superluminal not covered |
| GPU capture | PIX with live shader debugging and Shader Explorer (GDC 2026) [W19](#w19); RenderDoc, Nsight Graphics 2026.3 [W53](#w53) | Debug names and markers for PIX/RenderDoc/DRED (`dx12_identity_naming.hpp`); tools supplement, never replace, the public path |
| GPU crash dumps | DirectX Dump Files preview Jun 2026, retail expected fall 2026, all four IHVs [W18](#w18); Aftermath 2026.3 embeds shader debug info (R615+) [W53](#w53); RGD 1.6.3 for DX12 and Vulkan [W54](#w54) | DRED and device-removal evidence (DIAG.7b), `VK_EXT_device_fault` (DIAG.7c); Aftermath, RGD and dump files missing |
| Crash reporting service | Sentry native/Unreal/Unity with console crash reporting GA [W55](#w55) | Local crash bundles with privacy and no automatic upload (DIAG.5, DIAG.12); service integration owner [OPS.1](../ROADMAP.md#slice-ops.1) Later |
| Hot reload / live editing | Live++ (K); UE Live Coding (K) | Shipped asset and program reload (registry), native reload in [NATIVE.2](../ROADMAP.md#slice-native.2) and [SCRIPT.3](../ROADMAP.md#slice-script.3) |
| Sanitizers in CI | ASan/TSan/UBSan/fuzz tiers | Done: tiered sanitizer, race and fuzz CI (DIAG.1b, 3, 10, 11b) |

### Gaps in area 4

Cerid's diagnostic foundation is among its strongest areas. Missing: vendor GPU crash-dump integrations and the
DirectX Dump Files format, Superluminal as a sampling peer, and a consented crash/telemetry service path (OPS.1 is
Later; desktop exports need it at release). On the runtime-changeability axis, crash and capture records must name the
asset generation of every program in flight (DIAG.8a provenance), so a fault after a live edit points at the edit.

## 5. Physics and simulation

| Item | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| Rigid bodies, characters, vehicles | Jolt 5.6: GPU compute on DX12/Vulkan/Metal, reworked friction, up to 40% faster [W49](#w49); default 3D physics in Godot 4.6 [W34](#w34) | eylem rigid3d partial; most EYLEM rows Later ([EYLEM-RESUME](../ROADMAP.md#slice-eylem-resume)) |
| GPU physics | PhysX 5.6 GPU source open (BSD-3, >500 CUDA kernels), Apr 2025 [W50](#w50) | Later: EYLEM-v8a to v8e |
| Robotics and differentiable | Newton (Linux Foundation; Warp, MuJoCo Warp backend, gradients through steps) [W51](#w51) | Later: EYLEM-v9a, HGP-4, hesap autodiff exists |
| Determinism | Box2D v3 [W48](#w48); Jolt deterministic mode (K) | ADR-0063; EYLEM-v9b replay-hash matrix Later |

### Gaps in area 5

Physics is retained but Later. When it resumes, [PHYS-QUAL.plan](../ROADMAP.md#slice-phys-qual.plan) should pin
Jolt 5.6 and PhysX 5.6 as matched peers (both approved), add MuJoCo/Newton for articulated and differentiable cases,
and include GPU and hair simulation, which Jolt now ships. Solver parameters and GPU physics kernels follow the same
live-edit rule (retained as [D-005](../ROADMAP.md#slice-d-005)), with deterministic replay across a reload.

## 6. Numerics, AI and ML in engines

| Item | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| OS inference stack | Windows ML GA Sept 2025, OS-managed ONNX Runtime and vendor execution providers; DirectML in sustained engineering [W56](#w56) | Own tensor/model stack (hesap-tensor, CEIR ML gate-only); [AI.1](../ROADMAP.md#slice-ai.1) to [AI.6](../ROADMAP.md#slice-ai.6) |
| GPU ML in the graphics queue | DX LinAlg (preview Apr 2026) and Compute Graph Compiler (private preview summer 2026) [W14](#w14); Metal 4 ML encoder [W26](#w26) | [MLR-render](../ROADMAP.md#slice-mlr-render), [MLR-1](../ROADMAP.md#slice-mlr-1) (coop matrix/vector) |
| Engine inference | Unity Inference Engine (Sentis renamed, Unity 6.2) [W33](#w33); UE NNE (K) | Planned AI rows; ONNX seam in [MLR-0](../ROADMAP.md#slice-mlr-0) |
| Learned animation | Learned Motion Matching (Ubisoft) [W57](#w57); UE motion matching and Mover (5.8, Mover leaving experimental) [W29](#w29) | Missing as named items; [ANIM.2](../ROADMAP.md#slice-anim.2) blending/state machines, [POSE.3](../ROADMAP.md#slice-pose.3) learned pose, both Later |
| Training in engine | Newton differentiable sim [W51](#w51) | Planned with inference together (AI.3, AI.4) |

### Gaps in area 6

The provider choice in MLR is dated: cross-vendor work now goes through DX LinAlg and Vulkan cooperative matrix, with
NV cooperative vector as a vendor seam. Motion matching and learned motion matching are not named anywhere. Model
weights and networks are assets: swapping a neural material or denoiser network while running must follow the RAH-7
staged commit, with the non-neural fallback drawing until the new network is resident.

## 7. Assets and content

| Item | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| OpenUSD | AOUSD Core Specification 1.0 ratified Dec 2025; Core Spec 1.1 work on materials, geometry, physics [W58](#w58) | Later: [INTEROP.1](../ROADMAP.md#slice-interop.1) |
| MaterialX / OpenPBR | MaterialX 1.39.2 with OpenPBR 1.1, Jan 2025; later 1.39.x releases [W59](#w59) | Open: [MAT-0.runtime](../ROADMAP.md#slice-mat-0.runtime), editor [D7E-3](../ROADMAP.md#slice-d7e-3) |
| glTF extensions | `KHR_gaussian_splatting` RC [W41](#w41); material extensions (K) | glTF mesh import ([ADR-0043](../decisions/0043-mesh-resource-gltf-import.md)); extension coverage not pinned |
| Streaming and virtual texturing | DirectStorage 1.4 with Zstd and GACL [W17](#w17) | Open: TXS-2/3, VGE-1, RAH-5 |
| Cooking at scale | ASD-style precompiled pipelines per driver [W16](#w16) | Cook pipeline and CRDR ([ADR-0038](../decisions/0038-cooked-binary-container-format.md)); distributed cook [OPS.3](../ROADMAP.md#slice-ops.3) Later |

### Gaps in area 7

Pin the OpenUSD Core Spec 1.0 subset, OpenPBR 1.1 and the glTF extension list in INTEROP.1 and MAT-0, and add a
compression-format decision (GDeflate/Zstd/own) for the CRDR container that matches the GPU decompression row.
Streamed pages and cooked packs carry generations, so a re-cook while running replaces resident pages without
invalidating handles held by frames in flight ([RAH-5.c](../ROADMAP.md#slice-rah-5.c), [TXS-3.runtime](../ROADMAP.md#slice-txs-3.runtime)).

## 8. UI and editor

| Item | Best-in-class 2026 reference | Cerid status |
|---|---|---|
| Accessibility | Godot 4.5 screen readers through AccessKit (experimental) [W35](#w35) | Planned: [I2D-3.d](../ROADMAP.md#slice-i2d-3.d), [I2D-8.e](../ROADMAP.md#slice-i2d-8.e) (UI Automation, AT-SPI) |
| Text shaping | HarfBuzz (K) | Owned shaping planned: [I2D-2.b](../ROADMAP.md#slice-i2d-2.b), [I2D-6.b](../ROADMAP.md#slice-i2d-6.b); HarfBuzz as peer not named |
| Live collaboration | UE multi-user (transaction server, locks on save) [W60](#w60); Omniverse Live [W60](#w60) | Review/Later: [COLLAB.1](../ROADMAP.md#slice-collab.1) |
| Editor UX | UE 5.8 sandboxes, procedural vegetation editor [W29](#w29) | CR-D007 rows ([EDITOR](../ROADMAP.md#slice-editor), [I2D-9](../ROADMAP.md#slice-i2d-9)) |

### Gaps in area 8

The plan is complete in scope. Add HarfBuzz as a matched peer for owned shaping (correctness corpus and speed) and
AccessKit-class behaviour as the accessibility reference. UI styles, materials, layout and CHIR behaviour reload live
with compatible interaction state preserved (I2D-3.d already requires this); the editor itself is the largest live-edit
consumer of RAH-7.

## 9. Platforms

| Platform | 2026 reference | Cerid status |
|---|---|---|
| Windows/Linux | UE 5.8 minimum Windows 10 22H2; Godot 4.7 [W28](#w28) [W35](#w35) | Primary; [QUAL.1](../ROADMAP.md#slice-qual.1), [PRESENT.3](../ROADMAP.md#slice-present.3) |
| SteamOS / Steam Deck / Steam Machine | Separate Verified ratings per device; Proton 10 [W62](#w62) | Not named; Linux lanes only |
| macOS | Metal 4 [W26](#w26); UE Nanite/VSM need M2+ (beta) [W28](#w28) | [MAC.1](../ROADMAP.md#slice-mac.1) |
| Web | WebGPU in all major browsers [W27](#w27) | [WEB.1](../ROADMAP.md#slice-web.1) |
| XR | OpenXR 1.1 (Apr 2024) [W61](#w61) | Later: [6.3](../ROADMAP.md#slice-6.3) |
| Consoles, mobile | Unity Platform Toolkit (PS5, Xbox, Switch 2) [W31](#w31) | Later: [6.0](../ROADMAP.md#slice-6.0), [6.1](../ROADMAP.md#slice-6.1) |

### Gaps in area 9

Add SteamOS (native Linux build and Proton run of the Windows build) as a QUAL.1 tuple; name Metal 4 with a Metal 3
fallback in MAC.1 (older Apple GPUs under the fallback decision). Live editing must work on every qualified platform,
including WebGPU, where pipeline creation is asynchronous by API design; native reload of C++ is the exception that the
web cannot have ([WEB.1](../ROADMAP.md#slice-web.1)).

## 10. Agent and AI-assisted development

| Item | 2026 reference | Cerid status |
|---|---|---|
| Editor MCP servers | UE 5.8 Unreal MCP Server plugin (experimental) [W29](#w29) [W30](#w30); Unity MCP in Unity AI beta, May 2026 [W32](#w32) | `ceridc` CLI and MCP diagnostic host exist; full surface [CMD.4](../ROADMAP.md#slice-cmd.4), [AGENT.1](../ROADMAP.md#slice-agent.1) to [AGENT.5](../ROADMAP.md#slice-agent.5) |
| Agentic editor assistant | Unity Assistant agentic actions (Unity 6.2) [W33](#w33) | Same rows; authority model in DIAG.8c and AGENT.3 |
| Protocol security | MCP 2025-11-25 security practices [W63](#w63) | Cited in the whole-system review (S03) |
| Profiler LLM hooks | Tracy 0.13 optional LLM integration [W52](#w52) | Not planned |

### Gaps in area 10

Cerid's command-first design is ahead in principle (one command surface for GUI, CLI and MCP), but the peers already
ship. Pin the MCP protocol version and its security practices in CMD.4 and AGENT.3. Agents are the strongest case
for runtime changeability: an agent edits a frame, material or CKIR asset, the change installs live, and a failed
edit leaves the last-good generation visible (AGENT.4 already requires restore through production commands).

## 11. Computational geometry and navigation

Cerid has 15 geometry modules (primitives with Shewchuk orient/incircle/insphere for double, spatial indexes,
polygon Boolean/CDT/ear clipping, Delaunay/Voronoi/refinement, convex GJK/EPA/SAT, mesh processing with QEM, remesh,
repair, meshlets and cluster DAG, curves, decomposition, CPU and GPU BVH, LOD and meshgen). Algorithms are templated on
`crd::math::MathScalar` (floating point). A source grep found no navigation, navmesh or pathfinding code; the only
roadmap mention is [DEBT-2](../ROADMAP.md#slice-debt-2) (navmesh editor UX). [BREP.2](../ROADMAP.md#slice-brep.2)
(robust predicates and tolerances) is Later. The [geometry research](cerid-geometry.md) already names CGAL and
Clipper2 as algorithm references.

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Kernel design | CGAL EPICK/EPECK: filtered exact predicates, lazy exact constructions (K); CGAL 6.1 (Oct 2025) added 3D constrained Delaunay, isosurfacing, Fréchet; 6.2 adds 2D alpha wrapping [W64](#w64) | Missing: scalar-generic templates, no kernel concept; predicates double only |
| Indirect predicates | Attene: predicates on implicit intersection points with floating filters and expansions, faster than lazy exact [W68](#w68) | Missing |
| Exact mesh Booleans | Cherchi et al. 2022 interactive robust Booleans (200K triangles interactive) [W66](#w66); EMBER plane-based integer exact Booleans [W67](#w67); Manifold guaranteed-manifold Booleans [W65](#w65); CGAL 6.1 polygon repair by snap rounding [W64](#w64) | Partial: self-intersection removal and manifold repair (float); no exact 3D Boolean |
| Integer polygon clipping and offsetting | Clipper2: integer core, 62-bit range, accuracy degrades beyond about 1e15 [W69](#w69) | Partial: float Vatti-class Boolean with Clipper2 as reference; no offsetting |
| Arrangements, Minkowski sums, straight skeletons | CGAL packages (K) | Missing |
| Volumes | OpenVDB 13 (Nov 2025) GPU topology operators in NanoVDB [W70](#w70) | SDF substrate rows Later ([SDF-v0](../ROADMAP.md#slice-sdf-v0) to v8) |
| Integer/lattice geometry | Exact EDT, supercover/DDA traversal, hex and voxel grids (K) | Partial: uniform grid and spatial hash (float) |
| Fixed point and determinism | Box2D v3 float determinism [W48](#w48); fixed-point lockstep (K) | Optional fixed-point solver `EYLEM-v9c` Later; no fixed-point geometry |
| Navmesh build and query | Recast/Detour 1.6.0 (zlib), used by Unity, Unreal, Godot, O3DE [W71](#w71); UE World Partition navmesh chunks, static/dynamic modes [W72](#w72); UE 5.8 per-mesh walkable surfaces [W29](#w29) | Missing |
| Grid and graph search | A*, JPS (Harabor and Grastien 2011), HPA* (Botea 2004), flow fields (K) | Missing |
| Crowds | ORCA/RVO2 (linear-program reciprocal avoidance) [W73](#w73) | Missing |
| 3D/flying navigation | Sparse voxel octree navigation (Warframe, GDC 2015) (K) | Missing |
| Road routing and geodesy | CRP (Bing Maps since 2012) [W75](#w75); contraction hierarchies (K); Karney geodesics in GeographicLib and PROJ [W74](#w74) | Missing; frames/epochs in [SCI-CONTRACT.1](../ROADMAP.md#slice-sci-contract.1), [WORLD.1](../ROADMAP.md#slice-world.1) Later |
| GPU geometry | NanoVDB GPU ops [W70](#w70); Mega Geometry cluster builds [W76](#w76) | GPU LBVH and radix sort; [CGP-3](../ROADMAP.md#slice-cgp-3) geometry processing Open |
| Qualification corpus | Thingi10K (K, needs a download); CGAL/Manifold test suites | Missing as a board |

### Gaps in area 11

- **A kernel, not only templates.** A floating template parameter does not give exact predicates or constructions.
  The gold standard is a kernel concept with interchangeable number types: float/double with filtered exact
  predicates (static filter, interval filter, Shewchuk expansion fallback, also for float), indirect predicates on
  constructed points, exact integer (bounded int64/int128 with proven bit budgets), owned big-integer rational, and
  Q-format fixed point for lockstep. Every existing algorithm declares which kernel it needs.
- **Integer and lattice geometry as a first-class family**: square, hex and voxel grids, exact Euclidean distance
  transforms, supercover and Amanatides-Woo traversal, Bresenham, lattice polygons and offsets. Navigation, procgen,
  tilemaps ([SPR-1](../ROADMAP.md#slice-spr-1)), CAM and PCB all reuse it.
- **Exact Booleans and constructions**: 2D integer Boolean and offsetting (Clipper2-class), 2D arrangements, Minkowski
  sums, straight skeletons, 3D exact mesh Booleans (EMBER/indirect-predicate class), snap rounding, 3D CDT and alpha
  wrapping.
- **Navigation does not exist.** It is a core game system in every peer engine.
- **Consumers, not gates.** [GVA-5.runtime](../ROADMAP.md#slice-gva-5.runtime) (terrain, foliage, splats), the
  [PROCGEN](../ROADMAP.md#slice-procgen) rows, tilemaps and CAD/CAM consume GEOM-K.4 and NAV.1 when they arrive; they do
  not wait for them, so the proposals below do not list them as Requires. [OWN.1](../ROADMAP.md#slice-own.1) already
  assigns predicates and spatial queries to the geometry modules, which is where the kernel lives.
- **No geometry qualification board** with peers (CGAL, Clipper2, Manifold, libigl, Geogram, Boost.Geometry, GEOS,
  Recast/Detour, RVO2, GeographicLib) and adversarial corpora (Thingi10K, degenerate and near-degenerate sets).
- **Runtime changeability.** Navigation build settings, area costs and agent profiles are authored assets; a change
  rebuilds only the affected tiles in the background while agents keep querying the old tiles (Unreal's dynamic
  navmesh does tile rebuilds [W72](#w72)). GPU geometry kernels (voxelize, LBVH) are CKIR assets under RAH-7.

<a id="gaps-and-proposed-roadmap-changes"></a>
## Gaps and proposed roadmap changes

Proposals only; the user approves before any [ROADMAP](../ROADMAP.md) edit. "After" names the existing row (and its
Order number after the 2026-10-26 SIMD-DISPATCH and OWN insertions) that the new row should follow; the validator's order rule means a row's Requires must sit
earlier in the table. Highest impact first, matching the ranking above.

| # | Proposed ID | Kind | Placement | Requires | Content and acceptance |
|---:|---|---|---|---|---|
| 1 | SIMD-DISPATCH | Amend | In place (093) | DIAG.11a | Name Google Highway 1.3 as a measured design peer; detect the AVX10 version leaf and APX; place ARM64 SME beside SVE/SVE2 under the ARM release rows; register kernel units and dispatch as one capability in OWN.1 |
| 2 | GPU-TIER.0 | New | After [RAH-0](../ROADMAP.md#slice-rah-0) (112) | RAH-0 | Tier ladder decision: T0 legacy (Vulkan 1.1/1.2, DX12 FL 11_x, SM 6.0, bounded descriptor sets, no mesh/RT), T1 (Vulkan 1.3 + indexing + BDA, FL 12_1 + SM 6.6), T2 (DX12 Ultimate class: mesh, RT 1.1, VRS, sampler feedback), T3 (Roadmap 2026, descriptor heap, DXR 1.2, SM 6.9); each asset declares tiers and an authored fallback |
| 2 | RAH-1 to RAH-8 | Amend | In place | GPU-TIER.0 | Every child's acceptance adds: the declared fallback tier executes on a real or software adapter and rejects unsupported features before recording |
| 2a | RAH-7.a to RAH-7.d | Amend | In place | RAH-7 | Registry covers frame-graph structure (pass add/remove/reorder, resource re-realization) and per-part pipelines (shader objects, graphics pipeline libraries, DX12 pipeline libraries and partial programs, incremental RT/work-graph state objects); in-flight frames keep the old plan until their fences complete |
| 2a | RAH-7.e | New | After [RAH-7.d](../ROADMAP.md#slice-rah-7.d) (144) | RAH-7.b | No-hitch replacement: per-asset authored fallback while compiling (last-good generation, generic specialization-constant variant, or explicit diagnosed skip), compile-job QoS and admission limits, first-use pipeline collection and warm-up for shipping builds |
| 2a | RAH-7.f | New | After RAH-7.e | RAH-7.e, DIAG | Live-edit budget board: save-to-visible latency, worst swap frame spike and compile CPU per backend and tier, peer-relative where a peer exists, otherwise baseline plus stated margin; adversarial soak of rapid successive edits |
| 3 | RAH-SYNC | New | After GPU-TIER.0 | GPU-TIER.0 | One access/layout/sync model: DX12 enhanced barriers (fence barriers when retail), legacy `ResourceBarrier` path for older drivers, Vulkan synchronization2; replace legacy DX12 barriers in compute, raster and RT contexts; hazard tests feed RAH-6.b |
| 4 | RAH-2.a/RAH-2.c | Amend | In place | RAH-SYNC | Name the realizations: SM 6.6 `ResourceDescriptorHeap`, `VK_EXT_descriptor_heap` with `VK_EXT_descriptor_buffer` and descriptor-indexing paths, and a bounded descriptor-set path for T0 |
| 5 | DX12-AGILITY | New | After RAH-SYNC | GPU-TIER.0 | Agility SDK export and redistributable deployment, pinned DXC, shader-model ladder SM 6.0 to 6.9 per tier (6.10 LinAlg when retail), GPU upload heaps, revised view APIs; work graphs move from mechanic to tiered feature |
| 6 | VK-PROFILE | New | After DX12-AGILITY | GPU-TIER.0 | Vulkan 1.4 device creation where available, Khronos profiles as machine-readable tier definitions (Roadmap 2022/2024/2026), EXT successors for NV-only probes (SER), host image copy, maintenance 6+ |
| 6 | RAH-8.a | Amend | In place | VK-PROFILE, DX12-AGILITY | Capability manifest derives from the profiles and DX feature queries; tier selection and fallback reporting are commands |
| 7 | RAH-5.d | New | After [RAH-5.c](../ROADMAP.md#slice-rah-5.c) (136) | RAH-5.b | Direct I/O (DirectStorage 1.4 on Windows; io_uring and the platform async FS on Linux), GPU decompression as authored CKIR (GDeflate and Zstd) with CPU fallback, CRDR compression decision; consumers VGE-1, TXS-3 |
| 8 | GPU-FORENSICS | New | After [RAH-8](../ROADMAP.md#slice-rah-8) (148) | RAH-8, DIAG | Aftermath, RGD and DirectX Dump Files capture and import into the existing crash bundle schema; shader debug info and redaction; one adversarial page-fault per vendor tool available |
| 9 | MLR-render.1/.2, MLR-2.runtime | Amend | In place | DX12-AGILITY, VK-PROFILE | Provider targets DX LinAlg and Vulkan cooperative matrix; NV cooperative vector as vendor seam; name NTC (with BC fallback), neural materials and NRC as assets with quality oracles against non-neural references |
| 10 | TPR-1.runtime, PRESENT.2 | Amend | In place | RAH-1 | Own frame interpolation and upscaler as fallback and reference; vendor seams (DLSS 4.5, FSR Redstone, XeSS 3, MetalFX); latency reduction and frame pacing (present wait/id, swapchain maintenance) measured in DIAG latency |
| 11 | RAH-4.c, RTX-0.runtime, RTX-4.runtime | Amend | In place | RAH-4.b | SER on `VK_EXT_ray_tracing_invocation_reorder` and DXR 1.2; OMM on both APIs; cluster AS and partitioned TLAS as T3 features with standard BLAS/TLAS fallback; swept-sphere curves for hair |
| 12 | RAH-7.a, PQP-4.renderer | Amend | In place | RAH-7 | Driver pipeline caches as cache tiers: `VK_KHR_pipeline_binary`, DX12 PSO libraries, partial graphics programs (when retail), SODB authoring for Advanced Shader Delivery |
| 13 | GEOM-K.0 | New | After [WORLD.1](../ROADMAP.md#slice-world.1) (605); alternative: right after [RENDERER-RUNTIME](../ROADMAP.md#slice-renderer-runtime) (239) if the user wants it before the editor | SCI-CONTRACT.1, OWN | ADR: kernel concept (predicates, constructions, number types), per-algorithm kernel requirement, determinism tiers, migration plan; no geometry code change before acceptance |
| 13 | GEOM-K.1 | New | After GEOM-K.0 | GEOM-K.0 | Filtered exact predicates for float and double (static, interval, expansion), indirect predicates, exact integer predicates with bit budgets |
| 13 | GEOM-K.2 | New | After GEOM-K.1 | GEOM-K.1 | Owned big integer and rational, lazy exact constructions, snap rounding, fixed-point Q formats for lockstep |
| 13 | GEOM-K.3 | New | After GEOM-K.2 | GEOM-K.2 | Retarget hull, Delaunay, CDT, polygon Boolean, BVH queries and repair to the kernel; byte-identical results where the old path was already exact |
| 13 | GEOM-K.4 | New | After GEOM-K.3 | GEOM-K.1 | Integer/lattice family: square/hex/voxel grids, exact EDT, supercover and DDA traversal, lattice polygons |
| 15 | GEOM-EXACT.1 | New | After GEOM-K.4 | GEOM-K.3 | 2D: integer Boolean and offsetting (Clipper2-class), arrangements, Minkowski sums, straight skeletons |
| 15 | GEOM-EXACT.2 | New | After GEOM-EXACT.1 | GEOM-K.3 | 3D: exact mesh Booleans (plane-based integer or indirect predicates), 3D CDT, alpha wrapping, polygon-soup repair |
| 15 | GEOM-QUAL | New | After GEOM-EXACT.2 | GEOM-EXACT.2 | Peer board (CGAL, Clipper2, Manifold, libigl, Geogram, Boost.Geometry, GEOS) and corpora (Thingi10K, degenerate sets); peer-relative budgets |
| 15 | BREP.2 | Amend | In place | GEOM-QUAL | Build on the kernel instead of defining its own predicates |
| 14 | NAV.0 | New | After GEOM-QUAL | GEOM-K.4, SCI-CONTRACT.1 | ADR: module boundaries (navigation data as authored/cooked assets, queries as jobs), agent/area/cost model, determinism tier |
| 14 | NAV.1 | New | After NAV.0 | NAV.0 | Recast-class tiled build: voxelize, regions, contours, polygon and detail meshes; cooked tiles; GPU voxelization via CGP-3 |
| 14 | NAV.2 | New | After NAV.1 | NAV.1 | Detour-class query: polygon A*, funnel path, raycasts, off-mesh links, area costs, dynamic obstacles and tile rebuilds, streaming with world cells |
| 14 | NAV.3 | New | After NAV.2 | GEOM-K.4 | Grid and graph search: A*, JPS/JPS+, HPA*, flow fields, D* Lite on the lattice family |
| 14 | NAV.5 | New | After NAV.3 | NAV.2, WORLD.1 | 3D/flying navigation (sparse voxel octree) and large-world origins |
| 14 | NAV.6 | New | After NAV.5 | SCI-CONTRACT.3, WORLD.1 | Road routing (contraction hierarchies, CRP) and geodesy (Karney geodesics, projections) for maps and digital twins |
| 14 | NAV.4 | New | After [EYLEM-v1k](../ROADMAP.md#slice-eylem-v1k) (638) | NAV.2, EYLEM-v1i | Crowds (ORCA, steering, formations) coupled to the character controller and physics queries |
| 14 | NAV.7 | New | After NAV.4 | NAV.4, NAV.6 | Qualification against Recast/Detour, RVO2 and GeographicLib; editor work stays in DEBT-2 and EDITOR |
| - | JOBS-QOS | New | After [CORE-USE](../ROADMAP.md#slice-core-use) (111); else amend JOBS-HARD.2 | CORE-USE | Thread QoS classes (Windows power throttling/QoS, Linux scheduler hints), P/E placement for frame, audio and background work, NUMA awareness |
| - | TPR-2.runtime | Amend | In place | - | ACES 2.0 output transforms and OCIO 2.5 config interop (own implementation; OCIO as peer) |
| - | INTEROP.1, MAT-0.runtime, GVA-5.runtime | Amend | In place | - | Pin OpenUSD Core Spec 1.0, OpenPBR 1.1 and `KHR_gaussian_splatting` |
| - | ANIM.2 | Amend | In place | - | Motion matching and learned motion matching with a non-learned fallback |
| - | PHYS-QUAL.plan | Amend | In place | - | Peers Jolt 5.6, PhysX 5.6, MuJoCo/Newton; GPU and hair cases |
| - | QUAL.1, MAC.1 | Amend | In place | - | SteamOS/Steam Deck tuple (native and Proton); Metal 4 with Metal 3 fallback |
| - | CMD.4, AGENT.3 | Amend | In place | - | Pin the MCP protocol version and its security practices |
| - | I2D-2.b, I2D-6.b | Amend | In place | - | HarfBuzz as matched shaping peer |
| - | OPS.1 | Amend | In place | - | Consented crash/telemetry service path with symbol upload, before the first desktop release (QUAL.2) |

<a id="needs-a-download"></a>
## Needs a download

Not downloaded. Sizes were not checked (that would need at least a HEAD request); each item needs separate user
approval before it is fetched.

| Item | Why | URL | Size |
|---|---|---|---|
| DirectX Agility SDK (Microsoft.Direct3D.D3D12 NuGet) 1.619 / 1.721-preview | DX12-AGILITY, SM 6.9/6.10 tests | https://www.nuget.org/packages/Microsoft.Direct3D.D3D12 | not checked |
| DXC 1.9.2602.17 | SM 6.9 compile | https://github.com/microsoft/DirectXShaderCompiler/releases | not checked |
| Vulkan SDK 1.4.341 or later | descriptor heap, Roadmap 2026 profile | https://vulkan.lunarg.com/sdk/home | not checked |
| DirectStorage 1.4 preview and GACL | RAH-5.d | https://github.com/microsoft/DirectStorage | not checked |
| Intel SDE | SIMD-DISPATCH emulation of AVX-512/AVX10 (already named in that row) | https://www.intel.com/content/www/us/en/developer/articles/tool/software-development-emulator.html | not checked |
| Google Highway 1.3.0 | SIMD-DISPATCH design peer | https://github.com/google/highway/releases | not checked |
| Nsight Aftermath SDK 2026.3; Radeon GPU Detective 1.6.3 | GPU-FORENSICS | https://developer.nvidia.com/nsight-aftermath ; https://gpuopen.com/rgd | not checked |
| DLSS, FSR (Redstone), XeSS 3 SDKs | TPR-1 vendor seams | vendor developer portals ([W38](#w38) [W37](#w37) [W39](#w39)) | not checked |
| RTX Kit (NTC 0.10, Neural Shading 1.4) | MLR quality peers | https://developer.nvidia.com/blog/get-started-with-neural-rendering-using-nvidia-rtx-kit/ | not checked |
| Slang release | shader-language peer | https://github.com/shader-slang/slang/releases | not checked |
| OCIO 2.5 and ACES 2 configs | TPR-2 peer | https://github.com/AcademySoftwareFoundation/OpenColorIO/releases | not checked |
| CGAL 6.1.1, Manifold, Clipper2, Recast/Detour 1.6.0, RVO2, GeographicLib | GEOM-QUAL, NAV.7 peers | [W64](#w64) [W65](#w65) [W69](#w69) [W71](#w71) [W73](#w73) [W74](#w74) | not checked |
| Thingi10K corpus | GEOM-QUAL adversarial meshes | https://ten-thousand-models.appspot.com/ (K) | not checked (thousands of meshes) |
| Jolt 5.6, PhysX 5.6, Newton/MuJoCo Warp | PHYS-QUAL peers | [W49](#w49) [W50](#w50) [W51](#w51) | not checked |

## Sources

Read 2026-10-26 unless marked (K). Dates are publication dates as shown by the source or its coverage.

<a id="w01"></a>W01. Khronos, [Vulkan Introduces Roadmap 2026 and New Descriptor Heap Extension](https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension), 2026-01-23 (fetched).

<a id="w02"></a>W02. Khronos, [Simplifying Vulkan One Subsystem at a Time](https://www.khronos.org/news/permalink/simplifying-vulkan-one-subsystem-at-a-time), early 2026 (day uncertain).

<a id="w03"></a>W03. NVIDIA, [Streamlining Resource Binding with End-to-End Support for Vulkan Descriptor Heaps](https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/), 2026 (date uncertain).

<a id="w04"></a>W04. Phoronix, [Vulkan 1.4 Released With More Extensions Mandated](https://www.phoronix.com/news/Vulkan-1.4-Released), 2024-12-03.

<a id="w05"></a>W05. Phoronix, [Vulkan Roadmap 2026 Milestone](https://www.phoronix.com/news/Vulkan-Roadmap-2026), 2026-01-23.

<a id="w06"></a>W06. Khronos, [Introducing VK_EXT_ray_tracing_invocation_reorder](https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder), Nov 2025; [registry page](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_ray_tracing_invocation_reorder.html), last modified 2025-11-12.

<a id="w07"></a>W07. Khronos registry, [VK_NV_cluster_acceleration_structure proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_NV_cluster_acceleration_structure.html), 2025.

<a id="w08"></a>W08. Khronos registry, [VK_NV_partitioned_acceleration_structure](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_partitioned_acceleration_structure.html), not ratified.

<a id="w10"></a>W10. Microsoft, [Announcing DirectX Raytracing 1.2, PIX, Neural Rendering and more at GDC 2025](https://devblogs.microsoft.com/directx/announcing-directx-raytracing-1-2-pix-neural-rendering-and-more-at-gdc-2025/), Mar 2025.

<a id="w11"></a>W11. Microsoft, [D3D12 Opacity Micromaps](https://devblogs.microsoft.com/directx/d3d12-opacity-micromaps/), 2026-02-26.

<a id="w12"></a>W12. Microsoft, [Announcing Shader Model 6.9 Retail and New D3D12 Improvements](https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/), 2026-02-26 (fetched).

<a id="w13"></a>W13. Microsoft, [DirectX developer blog, 2026 archive](https://devblogs.microsoft.com/directx/2026/) and [page 2](https://devblogs.microsoft.com/directx/2026/page/2/): SM 6.10 preview (2026-04-27), LinAlg matrix preview, Agility 1.721 preview (2026-05-28), Advanced Shader Delivery availability (2026-10-21) (fetched).

<a id="w14"></a>W14. Microsoft, [Evolving DirectX for the ML Era on Windows](https://devblogs.microsoft.com/directx/evolving-directx-for-the-ml-era-on-windows/), 2026-03-12 (fetched). It describes LinAlg as building on Cooperative Vector; W12 calls Cooperative Vector deprecated in favour of the unified design.

<a id="w15"></a>W15. Microsoft, [Partial Graphics Programs](https://devblogs.microsoft.com/directx/partial-graphics-programs/), 2026-05-27, Agility 1.721 preview (fetched).

<a id="w16"></a>W16. Microsoft, [Agility SDK 1.618 and Advanced Shader Delivery](https://devblogs.microsoft.com/directx/agility-sdk-1-618/), 2025-09-25.

<a id="w17"></a>W17. Microsoft, [DirectStorage 1.4 release adds support for Zstandard](https://devblogs.microsoft.com/directx/?p=13133), 2026-03-11; [Tom's Hardware coverage](https://tomshardware.com/video-games/pc-gaming/microsoft-debuts-directstorage-1-4-at-gdc-2026-with-zstandard-compression-and-gacl-update-promises-developers-improved-compression-ratios-faster-loading-and-more).

<a id="w18"></a>W18. Microsoft, [DirectX Dump Files Preview Now Available](https://devblogs.microsoft.com/directx/dx-dump-files-preview), 2026-06-18.

<a id="w19"></a>W19. Microsoft, [DirectX: Bringing Console-Level Developer Tools to Windows](https://devblogs.microsoft.com/directx/directx-bringing-console-level-developer-tools-to-windows/), 2026-03-12.

<a id="w20"></a>W20. Microsoft, [DirectX Adopting SPIR-V as the Interchange Format of the Future](https://devblogs.microsoft.com/directx/directx-adopting-spir-v/), 2024-09-19.

<a id="w21"></a>W21. Khronos, [Khronos Group Launches Slang Initiative](https://www.khronos.org/news/press/khronos-group-launches-slang-initiative-hosting-open-source-compiler-contributed-by-nvidia), Nov 2024.

<a id="w22"></a>W22. Khronos, [Khronos at SIGGRAPH 2026](https://www.khronos.org/blog/khronos-at-siggraph-2026), 2026.

<a id="w23"></a>W23. AMD GPUOpen, [GPU Work Graphs mesh nodes in Vulkan](https://gpuopen.com/learn/gpu-workgraphs-mesh-nodes-vulkan/), 2024; [VK_AMDX_shader_enqueue](https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMDX_shader_enqueue.html) (provisional).

<a id="w24"></a>W24. Phoronix, [Vulkan 1.4.359](https://www.phoronix.com/news/Vulkan-1.4.359), Aug 2026; [Vulkan 1.4.352](https://phoronix.com/news/vulkan-1.4.352-released), May 2026; [VK_NV_cooperative_vector](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_cooperative_vector.html).

<a id="w26"></a>W26. Apple, [Discover Metal 4 (WWDC25 session 205)](https://developer.apple.com/videos/play/wwdc2025/205), Jun 2025; [Understanding the Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api); [Metal feature set tables](https://developer.apple.com/metal/feature-sets).

<a id="w27"></a>W27. web.dev, [WebGPU is now supported in major browsers](https://web.dev/blog/webgpu-supported-major-browsers), 2025-11-25.

<a id="w28"></a>W28. Epic, [Hardware and software specifications for Unreal Engine](https://dev.epicgames.com/documentation/unreal-engine/hardware-and-software-specifications-for-unreal-engine), UE 5.8 page (fetched).

<a id="w29"></a>W29. Epic, [Unreal Engine 5.8 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes) (first 100,000 characters fetched).

<a id="w30"></a>W30. GameFromScratch, [Unreal Engine 5.8 Released](https://gamefromscratch.com/unreal-engine-5-8-released/), 2026-06-17 (fetched).

<a id="w31"></a>W31. Unity, [Unity 6.3 LTS is now available](https://unity.com/blog/unity-6-3-lts-is-now-available), Dec 2025.

<a id="w32"></a>W32. Unity, [Unity AI open beta: how to get started with MCP](https://unity.com/blog/unity-ai-mcp-how-to-get-started), 2026-05-11.

<a id="w33"></a>W33. Digital Production, [Unity 6.2 welcomes AI](https://digitalproduction.com/2025/08/22/unity-6-2-welcomes-ai-but-pace-caution-user-liability-on-copyright/), 2025-08-22.

<a id="w34"></a>W34. Godot, [Godot 4.6 release](https://godotengine.org/releases/4.6/), Jan 2026.

<a id="w35"></a>W35. Godot, [Godot 4.5 release](https://godotengine.org/releases/4.5/), Sept 2025; Godot 4.7 stable with HDR output, Jun 2026 ([release listing](https://newreleases.io/project/github/godotengine/godot/release/4.7-stable), secondary).

<a id="w36"></a>W36. Bevy, [Bevy 0.19](https://bevy.org/news/bevy-0-19/) and [0.18 to 0.19 migration guide](https://bevy.org/learn/migration-guides/0-18-to-0-19/), 2026.

<a id="w37"></a>W37. Windows Central, [AMD FSR Redstone release](https://www.windowscentral.com/hardware/amd/amd-fsr-redstone-release), Dec 2025.

<a id="w38"></a>W38. CGM, [CES 2026: DLSS 4.5](https://www.cgmagonline.com/news/ces-2026-dlss-4-5-rtx-gpus/), Jan 2026; NVIDIA [DLSS developer page](https://developer.nvidia.com/rtx/ray-tracing/dlss/get-started).

<a id="w39"></a>W39. Guru3D, [Intel ships XeSS 3.0 SDK with multi-frame generation](https://www.guru3d.com/story/intel-ships-xess-30-sdk-with-multiframe-generation-support/), early 2026 (date uncertain).

<a id="w40"></a>W40. TweakTown, [RTX Kit adds RTX Mega Geometry 2.0 and NTC](https://www.tweaktown.com/news/113740/nvidias-rtx-kit-adds-rtx-mega-geometry-2-0-and-lays-the-foundation-for-rtx-neural-texture-compression/index.html), 2026 (date uncertain; secondary); NVIDIA [RTX Kit](https://developer.nvidia.com/blog/get-started-with-neural-rendering-using-nvidia-rtx-kit/), Jan 2025.

<a id="w41"></a>W41. Radiance Fields, [Khronos introduces a glTF release candidate for Gaussian splatting](https://radiancefields.com/the-khronos-group-introduces-a-gltf-baseline-for-gaussian-splatting), 2026-02-03; [AEC Magazine](https://aecmag.com/news/khronos-announces-gltf-gaussian-splatting-extension/). Ratification status after Q2 2026 uncertain.

<a id="w42"></a>W42. OpenColorIO, [OCIO 2.4 release notes](https://opencolorio.readthedocs.io/en/latest/releases/ocio_2_4.html); OCIO 2.5 built-in ACES 2 configs and Vulkan from the [openSUSE changelog](https://src.opensuse.org/pool/OpenColorIO/src/branch/factory/OpenColorIO.changes) (secondary).

<a id="w43"></a>W43. Google Highway, [1.3.0 release notes](https://newreleases.io/project/github/google/highway/release/1.3.0) (late 2025) and [repository](https://github.com/google/highway).

<a id="w44"></a>W44. Phoronix, [Intel confirms Nova Lake supports AVX10.2 and APX](https://phoronix.com/news/Nova-Lake-Does-AVX10.2-APX), Nov 2025; Chips and Cheese, [Hot Chips 2026: Diamond Rapids](https://chipsandcheese.com/p/hot-chips-2026-intels-diamond-rapids), 2026.

<a id="w45"></a>W45. Microsoft Learn, [SetThreadInformation](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadinformation); [Quality of Service](https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/ProcThread/quality-of-service.md).

<a id="w46"></a>W46. Epic, [Tasks System](https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-systems-in-unreal-engine).

<a id="w47"></a>W47. C. Gyrling, [Parallelizing the Naughty Dog Engine Using Fibers](https://www.gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine), GDC 2015 (K).

<a id="w48"></a>W48. E. Catto, [Determinism](https://box2d.org/posts/2024/08/determinism/), Box2D blog, Aug 2024.

<a id="w49"></a>W49. Jolt Physics, [releases](https://github.com/jrouwe/JoltPhysics/releases) (5.6.0, 11 July, year shown as 2026 by coverage, uncertain); [GameFromScratch](https://gamefromscratch.com/jolt-physics-5-6-released/).

<a id="w50"></a>W50. CG Channel, [NVIDIA open-sources PhysX's GPU simulation code](https://www.cgchannel.com/2025/04/nvidia-open-sources-physxs-gpu-simulation-code), 2025-04-22.

<a id="w51"></a>W51. [Newton on PyPI](https://pypi.org/project/newton/); Linux Journal, [Linux Foundation welcomes Newton](https://www.linuxjournal.com/content/linux-foundation-welcomes-newton-next-open-physics-engine-robotics), 2025 (date uncertain).

<a id="w52"></a>W52. Tracy, [v0.14.0 release](https://newreleases.io/project/github/wolfpld/tracy/release/v0.14.0) and [v0.14.1](https://newreleases.io/project/github/wolfpld/tracy/release/v0.14.1); 0.13.0 (2025-11-11) NEWS via a mirror (secondary).

<a id="w53"></a>W53. NVIDIA, [Nsight Aftermath 2026.3 release note](https://developer.nvidia.com/nsight-aftermath/getting-started/release-note-v2026.3); [Nsight Graphics 2026.3](https://developer.nvidia.com/nsight-graphics/getting-started/release-note-v2026.3).

<a id="w54"></a>W54. AMD, [Radeon GPU Detective](https://gpuopen.com/rgd) (v1.6.3); [RGD 1.1 Vulkan support](https://gpuopen.com/learn/rgd-1-1-vulkan-support/).

<a id="w55"></a>W55. Sentry, [Crash reporting for gaming consoles](https://sentry.io/changelog/crash-reporting-gaming-consoles/); [Unreal SDK](https://docs.sentry.io/platforms/unreal).

<a id="w56"></a>W56. Microsoft, [Windows ML is generally available](https://blogs.windows.com/windowsdeveloper/2025/09/23/windows-ml-is-generally-available), 2025-09-23; [DirectML overview](https://learn.microsoft.com/windows/ai/directml) (sustained engineering).

<a id="w57"></a>W57. Ubisoft La Forge, [Learned Motion Matching](https://www.ubisoft.com/en-us/studio/laforge/news/3NWqwcMU9lMfumgfPAEfka/learned-motion-matching), 2020.

<a id="w58"></a>W58. AOUSD, [Introducing AOUSD Core Specification 1.0](https://aousd.org/uncategorized/foundations-of-open-3d-development-introducing-aousd-core-specification-1-0/), 2025-12-17; Linux Foundation, [press release](https://www.linuxfoundation.org/press/aousd_prmarch2026), 2026-03-25.

<a id="w59"></a>W59. ASWF, [MaterialX v1.39.2 now publicly available](https://www.aswf.io/blog/materialx-v1-39-2-now-publicly-available/), Jan 2025.

<a id="w60"></a>W60. Epic, [Multi-User Editing overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/multi-user-editing-overview-for-unreal-engine); NVIDIA, [Omniverse Live](https://docs.omniverse.nvidia.com/extensions/latest/ext_core/ext_live.html).

<a id="w61"></a>W61. Khronos, [Khronos Releases OpenXR 1.1](https://www.khronos.org/news/press/khronos-releases-openxr-1.1-to-further-streamline-cross-platform-xr-development), 2024-04-15.

<a id="w62"></a>W62. GamingOnLinux, [Steam Deck, Steam Machine, Steam Frame and SteamOS Verified ratings](https://www.gamingonlinux.com/guides/view/what-steam-deck-steam-machine-steam-frame-and-steamos-verified-ratings-mean/), 2025-2026.

<a id="w63"></a>W63. MCP maintainers, [Security Best Practices, protocol 2025-11-25](https://modelcontextprotocol.io/docs/2025-11-25/tutorials/security/security_best_practices).

<a id="w64"></a>W64. CGAL, [CGAL 6.1](https://www.cgal.org/2025/10/01/cgal61/), 2025-10-01; [2025 highlights](https://www.cgal.org/2026/01/01/2025_recap/); [CGAL 6.1.1](https://www.cgal.org/2026/01/26/cgal611/), 2026-01-26.

<a id="w65"></a>W65. E. Lalish, [Manifold library](https://github.com/elalish/manifold/wiki/Manifold-Library) (v3.x).

<a id="w66"></a>W66. G. Cherchi, F. Pellacini, M. Attene, M. Livesu, [Interactive and Robust Mesh Booleans](https://history.siggraph.org/?p=215726), ACM TOG 41(6), SIGGRAPH Asia 2022.

<a id="w67"></a>W67. P. Trettner, J. Nehring-Wirxel, L. Kobbelt, [EMBER: Exact Mesh Booleans via Efficient and Robust Local Arrangements](https://www.graphics.rwth-aachen.de/publication/03339), SIGGRAPH 2022.

<a id="w68"></a>W68. M. Attene, [Indirect Predicates for Geometric Constructions](https://arxiv.org/abs/2105.09772), arXiv, revised Jan 2025 (journal venue not confirmed).

<a id="w69"></a>W69. A. Johnson, [Clipper2 overview](https://angusj.com/clipper2/Docs/Overview.htm).

<a id="w70"></a>W70. [OpenVDB](https://www.openvdb.org); ASWF TAC, [meeting 2026-02-18](https://tac.aswf.io/meetings/2026-02-18/2026-02-18.html) (12.1 Aug 2025, 13.0 Nov 2025).

<a id="w71"></a>W71. [Recast Navigation](https://github.com/recastnavigation/recastnavigation) (1.6.0 per distribution packages).

<a id="w72"></a>W72. Epic, [World Partitioned Navigation Mesh](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partitioned-navigation-mesh).

<a id="w73"></a>W73. UNC GAMMA, [RVO2 Library documentation](https://gamma.cs.unc.edu/RVO2/documentation/2.0/index.html).

<a id="w74"></a>W74. C. F. F. Karney, [Algorithms for geodesics](https://link.springer.com/article/10.1007%2Fs00190-012-0578-z), J. Geodesy 87, 2013; [GeographicLib geodesics](https://geographiclib.sourceforge.io/geod.html).

<a id="w75"></a>W75. D. Delling, A. Goldberg, T. Pajor, R. Werneck, [Customizable Route Planning](https://www.microsoft.com/en-us/research/publication/customizable-route-planning/), SEA 2011; journal version in Transportation Science.

<a id="w76"></a>W76. NVIDIA, [Fast ray tracing of dynamic scenes using OptiX 9 and RTX Mega Geometry](https://developer.nvidia.com/blog/fast-ray-tracing-of-dynamic-scenes-using-nvidia-optix-9-and-nvidia-rtx-mega-geometry), 2025.

<a id="w77"></a>W77. Epic, [PSO Precaching](https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine); Epic, [Game engines and shader stuttering](https://www.unrealengine.com/tech-blog/game-engines-and-shader-stuttering-unreal-engines-solution-to-the-problem); skip-draw since UE 5.1 per [DSOGaming](https://www.dsogaming.com/news/epic-games-adds-new-pso-precaching-mechanism-to-unreal-engine-5-1-to-reduce-shader-compilation-stutters) (secondary).

<a id="w78"></a>W78. Unity, [PSO tracing (Unity 6.4 manual)](https://docs.unity3d.com/Manual/shader-pso-trace.html) and [6.6 alpha manual](https://docs.unity3d.com/6000.6/Documentation/Manual/shader-pso-trace.html); `GraphicsStateCollection` marked experimental.

<a id="w79"></a>W79. Godot, [Reducing pipeline compilation stutter](https://docs.godotengine.org/es/4.x/tutorials/performance/pipeline_compilations.html) (ubershaders, 4.4); [Godot 4.5 release](https://godotengine.org/releases/4.5/index.html) (shader baker).

<a id="w80"></a>W80. Khronos, [Reducing Draw Time Hitching with VK_EXT_graphics_pipeline_library](https://www.khronos.org/blog/reducing-draw-time-hitching-with-vk-ext-graphics-pipeline-library) (Valve, Source 2); Khronos, [You Can Use Vulkan Without Pipelines Today](https://www.khronos.org/blog/you-can-use-vulkan-without-pipelines-today) (`VK_EXT_shader_object`), 2023.

Prior-knowledge (K) references not re-read today: Lumen, Nanite, virtual shadow map and MegaLights documentation on
dev.epicgames.com; ReSTIR (Bitterli et al. 2020), GRIS/ReSTIR PT (Lin et al. 2022), neural radiance caching (Müller et al.
2021); DirectStorage 1.1 GDeflate (2022); DX12 SM 6.6 dynamic resources and GPU upload heaps; `VK_KHR_pipeline_binary`;
`VK_EXT_hdr_metadata`; Unreal RDG, Live Coding and Blueprints, Unity render graph and C# hot reload, Godot
compositor effects; DX12 incremental state-object additions; Superluminal, Live++, PIX, RenderDoc; HarfBuzz; jump point search (Harabor and Grastien 2011),
HPA* (Botea et al. 2004), contraction hierarchies (Geisberger et al. 2008), flow fields, sparse-voxel-octree navigation
(Warframe, GDC 2015); Thingi10K (Zhou and Jacobson 2016); Geogram, libigl, Boost.Geometry, GEOS. Re-pin each before use.
