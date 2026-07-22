# Session — 2026-07-22 · D4 runtime load + persistent pipeline cache (both backends)

The payoff of the D1–D5 chain: ship cooked bytecode, load it with **zero runtime shader compilation**, and persist the driver's
pipeline compile across runs so a warm start is instant.

## (1) Zero-compile runtime load

A cooked `.crdr` bundle is read from disk and turned into a dispatchable pipeline straight from its cooked bytecode + the
IR-derived reflection — no compiler touched at load:
- **Vulkan** `create_pipeline_from_spirv(bundle.bytecode(SpirV), reflection.n_bindings, 0)`.
- **DX12** `create_pipeline_from_dxil(bundle.bytecode(Dxil), reflection.n_bindings, 0)` (the D2 load primitive).

The binding count comes from `ShaderReflection` (D1, IR-derived) — no SPIRV-Cross. `[d4]` (Vulkan): cook → write `.crdr` → read
back fresh → load → dispatch **32/32 reversed**, zero compile. (DX12's zero-compile load is already covered by the D2 `[cook]`
DXIL gate.)

## (2) Persistent driver pipeline cache — both backends warm-restart

- **Vulkan** — a `VkPipelineCache` created on the compute context and passed to `vkCreateComputePipelines` (was `VK_NULL_HANDLE`).
  `pipeline_cache_data(out)` serializes it (`vkGetPipelineCacheData`); `warm_pipeline_cache(blob)` reseeds a fresh context from a
  persisted blob (a foreign-driver blob is safely ignored by header UUID). `[d4]`: after creating a pipeline the cache is **111 KB**,
  header version 1 (`VK_PIPELINE_CACHE_HEADER_VERSION_ONE`); a FRESH `VulkanComputeContext` warm-starts from the blob and recreates
  the pipeline.
- **DX12** — an `ID3D12PipelineLibrary` (obtained via `ID3D12Device1`; if unsupported, `pipe_lib` stays null and PSOs are created
  directly — graceful fallback). `build_dxil_pipeline` names each PSO by an FNV-1a hash of `(DXIL · n_bindings · push)` and does
  `LoadComputePipeline(name,…)` (a warm hit) → miss → `CreateComputePipelineState` + `StorePipeline`. `pipeline_cache_data` uses
  `GetSerializedSize`/`Serialize`; `warm_pipeline_cache` COPIES the blob into an owned `Array` (because `CreatePipelineLibrary` does
  NOT copy — it references the blob for the library's lifetime) and falls back to an empty library on a version/adapter mismatch.
  `[d4]`: library **1824 B** → a fresh `Dx12ComputeContext` warm-starts from it and runs the cooked DXIL **32/32**.

## Verification

- Vulkan `[d4]` 15/1; DX12 `[d4]` 11/1 — both green.
- No regression: Vulkan smoke ([d4]/[cook]/[variant]/[program]/[indirect]) 113/10; **DX12 full suite 968/104** (the refactored
  `build_dxil_pipeline` is on EVERY DX12 kernel's path — the whole suite exercises the library path).
- clang-tidy (LLVM-20.1.8, warnings-as-errors) clean on both compute contexts + their headers + both `[d4]` tests.

## Honest scope

- **Delivered:** zero-compile load (both backends) + a persistent driver pipeline cache with warm restart (both backends).
- **Follow-tier (with D5):** async warmup (background pipeline creation on a worker), spec-constant binding at load, and a single
  multi-variant container that the loader indexes by variant key (the D3 manifest → bundle wiring).
- **Next: D5** — hot-reload (IR edit → recook affected variants → atomic pipeline swap), the last link, then the OFF-* offline
  path tracer.

## Proposed commit (user commits — no AI co-author trailer)

```
feat(d4): zero-compile runtime load + persistent pipeline cache (both backends)

Load a cooked .crdr into a dispatchable pipeline from the cooked bytecode +
IR-reflection, no runtime compiler. Add a persistent driver pipeline cache:
Vulkan VkPipelineCache (wired into vkCreateComputePipelines) and DX12
ID3D12PipelineLibrary (PSOs keyed by a DXIL-hash name; blob owned for the
library lifetime), each with pipeline_cache_data()/warm_pipeline_cache().

[d4]: Vulkan loads a .crdr zero-compile and dispatches 32/32; its VkPipelineCache
(111 KB, header v1) warm-starts a fresh context. DX12 pipeline library (1824 B)
warm-starts a fresh context and runs the cooked DXIL 32/32. DX12 full 968/104.
```
