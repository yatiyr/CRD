# ADR-0104 — IR-as-`crdr`: the shader cook & deploy pipeline (D1–D5)

**Status:** Accepted — **D1–D12 ALL LANDED 2026-07-22.** CKIR is a full shipping pipeline (IR-as-crdr → cook → variants →
zero-compile load + persistent pipeline cache → hot-reload → joint VS+FS specialize (D6) → full raster reflection (D7) →
multi-variant container (D8) → neural-material completeness (D9) → parallel cook on `crd-jobs` fibers (D10) → async pipeline
warmup (D11) → spec-constant binding at load (D12)). See `docs/sessions/2026-07-22-d6-d12-shader-polish.md`.
**Extends:** ADR-0101 (the IR is the source of truth for all shaders) · ADR-0103 (gpu-context owns every GPU program) · ADR-0037 (CRDR container: FourCC, little-endian)

## Context

CKIR builds every shader in C++ (a `KGraph` builder), emits GLSL/HLSL, and compiles to SPIR-V/DXIL **at runtime** via shaderc/DXC.
That ships the shader compiler in the runtime, stalls on first-use compiles, and means a shader can't be stored, versioned, patched,
or hot-reloaded as data. ADR-0101 declared the IR the single source of truth; this ADR makes that real at the resource/deploy level.

## Decision

A shader is a **`crdr` resource carrying the CKIR graph**, cooked offline to per-backend bytecode, loaded at runtime with zero
compile. Five slices:

- **D1 — IR-as-`crdr` + reflection.** Serialize `(KGraph, KEntry)` to a versioned blob and deserialize it into a byte-identical
  graph, so re-emitting from the loaded graph yields **bit-identical** backend source. Reflection (descriptor-set layout, vertex
  layout, push-constant + workgroup size) is derived **straight from the IR** — no SPIRV-Cross (own-format mandate).
- **D2 — cook.** ✅ **LANDED 2026-07-22.** IR → SPIR-V / DXIL / MSL / WGSL bytecode/source at build time (module
  `engine/shader-cook/` = `crd-shader-cook` lib + `shader_cook` CLI). `cook_compute_shader` serializes the IR (D1), derives the
  reflection (D1), emits+compiles **SPIR-V** (shaderc) + **DXIL** (dxc) + **real PTX** (CUDA→NVRTC, guarded on CUDAToolkit) to REAL bytecode, emits
  **MSL/WGSL** source, and packs a **CRDR bundle** (ADR-0038, chunks `KIR0·REFL·SPVC·DXIC·PTX·CUDA·MSLC·WGSL`) with a
  **content-hash cache**. The DX12 context grew `create_pipeline_from_dxil` (the zero-runtime-compile load primitive). **Both
  production backends load+run their cooked bytecode 32/32** (`[cook]`), the cooked SPIR-V is byte-identical to the runtime compile,
  real PTX is verified (`.version`/`.target`), and the CLI cooks a `.kgph` → `.crdr`. Parallel cook is a follow-tier enhancement.
  Session `docs/sessions/2026-07-22-d2-shader-cook.md`.
- **D3 — variants.** ✅ **LANDED 2026-07-22.** `engine/shader-cook/variant.{hpp,cpp}`. A shader is an übershader with feature
  toggles; a variant is a `key` bitmask. `cook_variant_matrix(build_fn, keys, opts)` cooks the requested keys via a per-key
  `VariantBuildFn` (emit the live path — how real material compilers cook per-permutation; the honest fit given `KGraph::stmt` is
  const-only), **content-hash dedups** identical specialized IR (cooked once), and returns the manifest (key → bundle hash) +
  **telemetry** (requested vs unique). `cook_one_variant` is the **on-demand** single. `[variant]`: 4 keys → 2 unique (50%
  reduction), dedup proven, each variant's cooked SPIR-V runs GPU-correct. A single multi-variant container + CLI variant flow fold
  in with D4. Session `docs/sessions/2026-07-22-d3-variants.md`.
- **D4 — runtime load + pipeline cache.** ✅ **LANDED 2026-07-22.** Load a cooked `.crdr` into a dispatchable pipeline straight
  from its bytecode + IR-reflection (`n_bindings`), **zero runtime shader compile** (Vulkan `create_pipeline_from_spirv`, DX12
  `create_pipeline_from_dxil`). Persistent driver cache: **Vulkan** `VkPipelineCache` (wired into `vkCreateComputePipelines`) and
  **DX12** `ID3D12PipelineLibrary` (PSOs keyed by a DXIL-hash name; the warm blob is owned since `CreatePipelineLibrary` doesn't
  copy), each with `pipeline_cache_data()`/`warm_pipeline_cache()`. `[d4]`: Vulkan loads a `.crdr` zero-compile and dispatches
  32/32, its 111 KB cache warm-starts a fresh context; DX12's 1824 B library warm-starts a fresh context running cooked DXIL 32/32.
  Async warmup + spec-const binding fold in with D5. Session `docs/sessions/2026-07-22-d4-runtime-load-pipeline-cache.md`.
- **D5 — hot-reload.** ✅ **LANDED 2026-07-22.** `engine/shader-cook/reload.{hpp,cpp}`. `ReloadableCompute::reload(g, e, name,
  backend, create_fn, user)` recooks + content-hashes the IR: an unchanged graph is a no-op, a changed one builds the new pipeline
  from the cooked bytecode + IR-reflection binding count (via a backend-agnostic create-callback) and **atomically swaps** it in,
  retiring the previous pipeline one generation for in-flight safety. `[d5]`: an IR edit (kernel ×1 → ×2) hot-swaps the live
  pipeline in the same context, no restart; a same-graph re-cook is a no-op. Session `docs/sessions/2026-07-22-d5-hot-reload.md`.

## The D1 format (little-endian, ADR-0037)

```
[FourCC 'KGPH' u32][version u32]
[sizeof manifest: sizeof(KNode)·sizeof(KStmt)·sizeof(KType)·sizeof(KEntry) — 4× u32]
[n_inputs u32]
5× POD pool, each [count u32][raw bytes]:  nodes · ext · struct-fields · struct-begins · stmts
[KEntry raw bytes]
```

**Why a POD-pool container, not a field-by-field format:** a cooked shader is **regenerable from source**. So the format that is
*correct and fast* is a raw copy of the (trivially-copyable) IR pools, guarded by a version + a struct-layout (`sizeof`) manifest: a
layout drift is **detected** and cleanly rejected (⇒ recook), never silently mis-read. Field-by-field serialization earns its cost
only for long-lived, hand-edited files (save games, scenes) — not for ephemeral cook artifacts. This is why the whole CKIR enum
discipline is *append-at-END / cook-stable*: the op/type/stmt enum values are frozen, so the pools mean the same thing across builds
at a given format version. `KNode`/`KStmt`/`KType`/`KEntry` are `static_assert`-ed trivially copyable.

**Reflection from the IR:** a graph walk maps `BufferDecl`/`UniformBlock`/`Texture`/`Sampler`/`AccelStructDecl` → descriptor
bindings (deduped by set·binding·kind, `writable` from the decl), `StageIn` (vertex) → vertex attributes, and the entry → workgroup
size / stage. The renderer's binding layer wires straight from `ShaderReflection` — no third-party reflector.

## Consequences

- **Ship without a shader compiler** (D4); **zero first-use hitches**; **shaders are data** (ship/version/patch/hot-reload); **one
  IR → every backend** from one resource; **reflection for free**; **UE5-class variants** with dedup; **deterministic validated
  incremental builds** (D2).
- Format-version churn is a non-issue: a struct change bumps the version / trips the `sizeof` manifest → the cook regenerates.
- MSL/WGSL final *bytecode* is platform-gated (Metal needs macOS; WebGPU needs a naga/tint validator) — the cook emits + stores the
  text and validates where the local toolchain allows, honestly documented (SPIR-V/DXIL/PTX get real bytecode on the dev box).

## Verification (D1)

`[kir][serialize][d1]`: build a compute kernel (bindings + builtin + wave op + store) → serialize → deserialize into a fresh graph
→ **re-emit is byte-identical GLSL AND HLSL**; reflection = 2 storage bindings (set 0, binding 1 writable) + workgroup 64; junk /
truncated / wrong-magic blobs cleanly rejected. Full kir regression green.
