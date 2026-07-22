# Session — 2026-07-22 · D2 the offline shader cook (CKIR graph → `.crdr` bundle)

**Ask:** "let's go with D2 … full gold standard system." D2 (ADR-0104) = the OFFLINE cook: turn a CKIR shader graph into a
self-contained `.crdr` bundle carrying the IR + reflection + per-backend bytecode, so the runtime ships with ZERO shader
compilation on the hot path.

## What landed

**`crd-shader-cook` module** (`engine/shader-cook/`, mirrors `asset_cooker`'s lib+CLI shape) — a BUILD-TIME cook that links both
GPU backends' compilers. `cook_compute_shader(KGraph, KEntry, name, opts) → CookResult`:
- serializes the IR (D1 `serialize_graph`) → the bundle's source of truth + the **content-hash key** (`ResourceId::from_content`);
- derives the reflection (D1 `reflect`) → a POD blob;
- emits + compiles each target: **SPIR-V** (GLSL→shaderc) and **DXIL** (HLSL→dxc) as REAL bytecode; **CUDA / MSL / WGSL** as
  emitted source (their final bytecode is produced by the target platform's toolchain — Metal/naga aren't hosted here);
- packs everything as a standard **CRDR container** (ADR-0038): chunks `KIR0` (IR) · `REFL` · `SPVC` (compute SPIR-V) · `DXIC`
  (DXIL) · `CUDA` · `MSLC` · `WGSL`. Optional zstd per-blob (`add_chunk_compressed`).
- **content-hash cache**: `<cache_dir>/<id>_<backends><z>.crdr` — an unchanged graph + backend set re-uses the cooked bytes verbatim.

**Read path** (the seam D4 builds on): `read_shader_bundle` + `ShaderBundle::bytecode(backend)` / `ir()` / `reflection()` — just
`crdr_read` + pick-your-backend-chunk. No compilers at runtime.

**`create_pipeline_from_dxil`** on the DX12 compute context — the missing zero-runtime-compile load primitive (mirrors Vulkan's
`create_pipeline_from_spirv`). Refactored the HLSL path's root-sig+PSO tail into a shared `build_dxil_pipeline(device, code, len,
…)` used by both `create_pipeline_from_hlsl` (compile-then-build) and `create_pipeline_from_dxil` (load precompiled).

**`shader_cook` CLI** (`tools/shader-cook/`): `cook <in.kgph> -o <out.crdr> [--backends …] [--cache <dir>] [--compress]` +
`info <bundle.crdr>` (chunk dump).

## Verification (both production backends RUN their cooked bytecode)

`[cook]` gate, reverse kernel (2 F32 buffers, ls=32), all 5 backends cooked (spirv=956 dxil=3364 cuda=217 msl=388 wgsl=402 B):
1. **Byte-identical** — the cooked `SPVC` blob == a fresh runtime GLSL→SPIR-V of the same graph, byte for byte.
2. **Round-trip** — `read_shader_bundle` finds every chunk (IR + reflection + all 5 backends).
3. **Runs (Vulkan)** — `create_pipeline_from_spirv(cooked SPIR-V)` → dispatch → **32/32 reversed** (the cooked bytecode, not a
   fresh compile).
4. **Runs (DX12)** — `create_pipeline_from_dxil(cooked DXIL)` → dispatch → **32/32 reversed** — zero runtime dxc.
5. **Cache** — second cook returns `from_cache` with byte-identical bytes.
6. **CLI** — `.kgph` → `.crdr` (7 chunks, 7720 B); `info` dumps `KIR0/CUDA/DXIC/MSLC/SPVC/REFL/WGSL`.

No regression: **DX12 full suite 957/103** (the `build_dxil_pipeline` refactor is on every DX12 kernel's path); Vulkan `[cook]`
green. clang-tidy (LLVM-20.1.8, warnings-as-errors) clean on `cook.cpp`, `cook.hpp`, `main.cpp`, `dx12_compute_context.{hpp,cpp}`.

### Scar fixed in passing — latent kir -Wswitch gap

Cooking pulls the CUDA/MSL/WGSL compute emitters together for the first time in a non-test TU, which surfaced a latent
`clang-diagnostic-switch`: the RT/RT-pipeline `KStmtKind`s (`TraceRayClosest/Hit/Curves/Pipeline`, `PayloadStore`,
`ReorderThread`, `IgnoreHitIf`) were added for GLSL/HLSL inline rayQuery but never handled in the three non-RT emitters' body
switches. Fixed with an **explicit grouped no-op case** (not a catch-all `default:` — so a future NON-RT `KStmtKind` still trips
`-Wswitch` and gets wired to every backend). These backends have no inline-RT path, so such a kernel is never routed to them.

### Consistency — `crd::math` over `std` in touched test code

The cook engine (`cook.cpp`) carries **zero** `std` math (the Math Mandate target). A tone-quantiser lambda in a touched *test*
had picked up `std::lround` (following an earlier session's god-ray precedent + a tidy `bugprone-incorrect-roundings` hint) —
switched to `crd::math::round` (which exists: `round`/`lround`/`nearbyint`). The mandate is an ENGINE rule (deterministic/portable
shipped math); host-side test scaffolding has latitude, but our own math is the right default even there.

## Honest scope

- **Real bytecode:** SPIR-V (shaderc) + DXIL (dxc) — the two shipping backends, both verified to **load+run** from the bundle —
  plus **real PTX** (CUDA→NVRTC, guarded on CUDAToolkit; `--fmad=false`/`--prec-div=true`/`--prec-sqrt=true` @ virtual
  `compute_75` so the PTX is portable + matches the kir-cuda runtime's determinism flags). `ptx=1008 B`, carries `.version`/`.target`
  — the CUDA target's shipping bytecode, cooked alongside the CUDA source (source kept as the fallback for other-arch recompile).
- **Emitted source:** MSL / WGSL — the bundle carries validated source; the final AIR/WGSL-bytecode is produced by the target
  platform toolchain (Metal, naga/tint), which aren't hosted on this Windows box.
- **Next:** D3 (variants/permutations — cook the (graph × define-set) matrix into one bundle) → D4 (runtime load + pipeline-cache
  from the bundle) → D5 (hot-reload).

## Proposed commit (user commits — no AI co-author trailer)

```
feat(d2): offline shader cook — CKIR graph -> .crdr bundle (IR + reflection + per-backend bytecode)

Add crd-shader-cook: cook_compute_shader serializes the IR (D1), derives
reflection, emits+compiles SPIR-V (shaderc) and DXIL (dxc) to real bytecode,
emits CUDA/MSL/WGSL source, and packs a CRDR bundle (ADR-0038) with a
content-hash cache. Add DX12 create_pipeline_from_dxil (the zero-runtime-compile
load primitive; shared build_dxil_pipeline with the HLSL path) and a shader_cook
CLI (cook/info). Fix a latent -Wswitch gap in the CUDA/MSL/WGSL emitters
(RT KStmtKinds handled with an explicit grouped no-op).

[cook]: reverse kernel cooked to all 5 backends; cooked SPIR-V is byte-identical
to the runtime compile and runs 32/32 on Vulkan; cooked DXIL runs 32/32 on DX12;
cache re-uses byte-for-byte. DX12 full suite 957/103, no regression.
```
