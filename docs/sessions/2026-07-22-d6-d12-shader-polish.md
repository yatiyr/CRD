# Session — 2026-07-22 · D6–D12 shader-system polish band (detour slices 37–45)

The user asked to complete **every shader-system polish task** to gold standard, added them to the D-007 slice table after D5,
and gave one standing constraint: **reuse the engine's existing fiber-based `crd-jobs` scheduler for parallel work — do not write
a bespoke thread pool** ("we are not rewriting what we already have"). This session closed D6–D12 (slices 37–45). D6–D9 landed in
the earlier part of this session; D10–D12 (and their scars) below are the new work.

## D10 — parallel cook on `crd-jobs` (NOT a bespoke pool)

`cook_variant_matrix_parallel` (`engine/shader-cook/src/variant.cpp`): pass 1 (serial, cheap) builds + content-hashes each key →
the unique set + the manifest; pass 2 cooks each **unique** variant concurrently on the existing `crd-jobs` fibers
(`jobs::parallel_for`), every job on its own `TlsfAllocator`, writing content-addressed to the cache. Content-hash dedup is
order-independent, so the parallel output is byte-identical to the serial cook.

**Two scars — both silent `0xC0000005`, no diagnostic** (isolated by proving the sibling test [d11], off-thread *pipeline*
creation, passed while [d10], the *cook*, crashed → the crash was specific to running the heavy compiler on a worker fiber):

1. **`shaderc_compiler_t` is thread-HOSTILE.** `compile_glsl_to_spirv` shared ONE compiler behind `static global_loader()`;
   two workers compiling at once race → AV. Fix: a **`thread_local` compiler** in `ShadercLoader::compile()` (each thread inits
   its own once, released at thread exit by a `thread_local` RAII holder whose dtor calls `compiler_release`; the singleton's
   `m_api` outlives every worker). No single-thread regression. `engine/gpu-context-vulkan/src/vulkan_glsl_compile.cpp`.
2. **The cook OVERFLOWS the 64 KB Small fiber.** `jobs::parallel_for` defaults to `StackSize::Small` = 64 KB (Medium 512 KB,
   Large 2 MB). The cook runs the whole glslang/shaderc front-end + the CKIR emitter + serializer, which need a real thread-sized
   stack (an OS thread gives shaderc 1 MB and it's fine). Fix: request **`StackSize::Large`** (2 MB; 16 Large fibers exist by
   default via `Config::large_fiber_count = 16`).

`[d10]` GREEN (Vulkan, 4 workers): 8 keys → 2 unique, manifest matches serial, all cache files byte-identical (23 assertions).
Memory: [[feedback_parallel_cook_shaderc_threadhostile_and_fiber_stack]].

## D11 — async pipeline warmup

`AsyncPipelineWarmer` (`engine/shader-cook/include/crd/shadercook/warmup.hpp`, header-only). Building a live pipeline is the
driver's SPIR-V→ISA compile — done lazily on the render thread it stalls the frame. `add()` queues cooked-SPIR-V blobs (from the
D8 container / the D4-warmed cache); `submit()` kicks **ONE** background `crd-jobs` job that builds every pipeline via a
`PipelineCreateFn` off the render thread (serialized ⇒ the shared driver pipeline cache needs no extra sync); the caller keeps
working, then `wait()` joins. A `VkPipeline` is not thread-affine, so a worker-built one binds on the render thread.
`pipeline_for_key` maps a variant key → its ready pipeline. `[d11]` GREEN (Vulkan): `submit()` non-blocking (main-thread work
overlaps the background compile), 2 pipelines built on a worker, both dispatch-correct on the main thread (15 assertions).

## D12 — spec-constant binding at load

`KGraph::spec_constant(constant_id, default, dt)` is a **`KOp::Const` tagged `kSpecConstFlag`** (in `axes`) + the SPIR-V
constant_id (in `iidx`). This reuses `Const` so **zero new-KOp switch surface** — the key insight is that node identity
(`node_equal` / `key_hash`) already keys on both `axes` and `iidx`, so CSE can never fuse a spec constant with a plain literal.
Two guards make it a pipeline-time value: `optimize()` skips a spec constant (never folds it into a literal nor folds through it),
and the GLSL emitters (compute kernel AND stage/material) lower it to a module-scope `layout(constant_id = N) const t _specN =
default;` referenced by name. Non-spec targets (D3D12/CUDA, and the mesh/RT emitters) read the plain `Const` = its DEFAULT — the
honest portable realization (a baked default IS a variant pinned to that value), no undeclared-name risk.

`VulkanComputeContext::create_pipeline_from_spirv(..., specs)` (new overload; the old 3-arg delegates with empty specs) builds a
`VkSpecializationInfo` from the (constant_id, 4-byte value) pairs and sets `cpci.stage.pSpecializationInfo` — the driver folds the
values into the ISA at pipeline-creation. `[d12]` GREEN (Vulkan): ONE cooked bundle (756 B SPIR-V) built 4 pipelines — scale 2.0 /
3.5 / 0.25 + unbound default(1.0) — all GPU-correct, zero re-cook; re-cook is byte-identical (the value is not in the IR).

Complementary to D3 cook-time variants: bake the axes that change control flow, spec-constant the cheap numeric toggles.

## Gates

- **kir 52420/230** (unchanged — the additive `ckir.hpp` spec-const flag/builder/optimize-guard + GLSL emitter changes regress
  nothing, incl. the HLSL byte-exact tests).
- **Vulkan D-band + material/raster/neural 1072/1072** (`[d6]`…`[d12]`, `[cook]`, `[variant]`, `[ubergraph]`, `[material]`,
  `[raster]`, `[neural]`, `[d4]`, `[d5]`).
- **DX12 968/104** (the `ckir.hpp` ripple compiles + runs clean).
- **tidy clean** (LLVM 20.1.8) on every touched file — one catch fixed: a function-local `constexpr int kMaxSpec` is a
  `LocalConstant` → must be `lower_case` (`max_spec`); the file-scope `kMaxBindings` keeps its `k` prefix (GlobalConstant).

## Build-environment notes (cost real time this session)

- **Builds:** PowerShell tool `cmd.exe /c '"…vcvars64.bat" >nul 2>&1 && "…cmake.exe" --build …'` WORKS. In the **Bash** tool,
  `cmd /c` needs **`MSYS_NO_PATHCONV=1`** (MSYS rewrites `/c` → `C:\`, so `cmd` launches interactively and ignores the command).
  A PowerShell `Start-Job` does NOT survive across separate PowerShell-tool invocations (fresh session each call).
- Host i9-14900K: cap `-j` and don't run two heavy MSVC compiles at once (a lightweight test run alongside a capped build is fine).

**D1–D12 DONE ⇒ CKIR is a full shipping pipeline: IR-as-crdr → cook → variants → zero-compile load + pipeline cache → hot-reload
→ parallel cook + async warmup + spec constants.** Detour's remaining work is the OFF-* offline path tracer.
