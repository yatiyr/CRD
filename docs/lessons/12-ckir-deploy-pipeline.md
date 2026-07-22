# 12 — The CKIR deploy pipeline: from graph to GPU

> *How a shader authored as an IR graph becomes shippable, cached, deduplicated, zero-runtime-compile bytecode — and how one
> content hash ties the whole pipeline together. The concrete walkthrough behind ADR-0104 (D1–D5).*

Cerid does not ship GLSL, HLSL, or hand-written shader text. It ships a **graph** (the source of truth) and, cooked from it, a
bundle of per-backend bytecode. This lesson follows one kernel through all five deploy stages. Everything is real: the module is
`engine/shader-cook/`, the CLI is `shader_cook`, and the numbers below are from the actual gates.

---

## The two files

| file      | what it is | contents |
|-----------|------------|----------|
| **`.kgph`** | the IR at rest | FourCC `KGPH` + a `sizeof` layout guard + five POD pools (nodes · ext · struct-fields · struct-begins · stmts) + the entry. Versioned, little-endian, regenerable. |
| **`.crdr`** | the cooked bundle | the same chunked container format as textures/audio/meshes (ADR-0038). Chunks: the IR, its reflection, and one blob per backend. Content-addressed on disk. |

The `.kgph` is authoritative; the `.crdr` is disposable (you can always re-cook it from the IR).

---

## The five stages

### D1 — Author & save

You build a `KGraph` + `KEntry` in C++ (typed nodes: `buffer_decl`, `builtin`, `select`, `stmt_buffer_store`, …). No GLSL. Then
`serialize_graph(g, e)` writes the `.kgph`. Because the IR is the source of truth (ADR-0101), every backend and every reflection
regenerates from it, and a layout drift is *detected and cleanly rejected*, never mis-read.

Reflection is derived **straight from the IR** — `reflect(g, e)` walks the buffer/texture/sampler decls into a descriptor layout
and reads the workgroup size from the entry. No SPIRV-Cross, no parsing of compiled output.

### D2 — Cook

`cook_compute_shader(g, e, …)` emits the graph to all five backends and compiles the three with hosted toolchains to *real
bytecode*:

| backend | path | chunk | form | size (reverse kernel) |
|---------|------|-------|------|------|
| Vulkan  | GLSL → shaderc | `SPVC` | real bytecode | 956 B |
| D3D12   | HLSL → dxc     | `DXIC` | real bytecode | 3364 B |
| CUDA    | CUDA → NVRTC   | `PTX ` | real bytecode | 1008 B |
| Metal   | emit MSL       | `MSLC` | source | 388 B |
| WebGPU  | emit WGSL      | `WGSL` | source | 402 B |

Plus `KIR0` (the IR, 1524 B) and `REFL` (the reflection, 600 B) — eight chunks, one file. MSL/WGSL are cooked as *validated
source* because their final bytecode is produced by the target platform toolchain (Metal, naga), which aren't hosted on the dev
box. The cooked SPIR-V is **byte-identical** to what a runtime compile would produce.

### D3 — Permutate

An übershader with feature toggles → a variant per `key` bitmask. `cook_variant_matrix` cooks only the requested keys
(on-demand) and content-hashes each specialized graph so two keys that fold to the same kernel share one bundle. In the gate,
**4 requested keys collapse to 2 unique bundles** (a 50% cut), reported as telemetry. Full mechanics in
[lesson 14](14-variants-permutation-and-specialization.md).

### D4 — Load & cache

At runtime you read the `.crdr`, pick the chunk for the running backend, and hand the *precompiled bytecode* to the driver:
`create_pipeline_from_spirv` (Vulkan) / `create_pipeline_from_dxil` (D3D12). The binding count comes from the `REFL` chunk. **No
shaderc, no dxc on the hot path.**

The driver's *own* SPIR-V→ISA compile is cached across runs with a persistent pipeline cache: a **VkPipelineCache** (~111 KB) and
an **ID3D12PipelineLibrary** (~1.8 KB) serialize to disk and **warm-start** a fresh context. (D3D12 gotcha: the warm blob must be
*owned* — `CreatePipelineLibrary` does not copy it, it references it for the library's lifetime.)

### D5 — Hot-reload

`ReloadableCompute::reload(graph)` recooks and compares the content hash. Unchanged → a cheap no-op. Changed → build the new
pipeline from the freshly cooked bytecode and **atomically swap** it into the live slot, retiring the previous one for a
generation so in-flight GPU work never dangles. Edit a kernel ×1 → ×2 and the running pipeline changes under you, same context,
no restart.

---

## The elegance: one primitive, three jobs

The whole pipeline leans on a single value:

```cpp
content_hash = ResourceId::from_content(serialize_graph(g, e));   // 128-bit
```

It is the **cook cache key** (D2), the **variant dedup key** (D3), and the **hot-reload change detector** (D5). "Have we cooked
this?", "is this variant a duplicate?", and "did the shader change?" are the *same* comparison. Three more choices reinforce the
coherence:

- **The IR is the truth** → every cooked artifact is disposable and reproducible.
- **One container for everything** → shaders ride the same `.crdr` chunk format as textures/audio/meshes; nothing is
  special-cased.
- **Reflection without a reflector** → layouts read straight off the IR.
- **The runtime links no compiler** → shaderc/dxc/NVRTC live only in the offline cook.

---

## Verified on real hardware

- cooked SPIR-V runs **32/32** on Vulkan; cooked DXIL runs **32/32** on D3D12 (both zero-runtime-compile).
- cooked SPIR-V is byte-identical to a runtime compile; real PTX carries `.version`/`.target`.
- variants **4 → 2** deduped; hot-reload swap ×1 → ×2 in the same context.
- no regression: D3D12 full suite **968/104**.

The standalone CLI drives it from the shell: `shader_cook cook in.kgph -o out.crdr` and `shader_cook info out.crdr`.

---

*Companion: [13 — shaders, pipelines, materials & lighting](13-shaders-pipelines-materials-lighting.md) (the concepts),
[14 — variants & specialization](14-variants-permutation-and-specialization.md) (the deep dive). Reference: ADR-0104.*
