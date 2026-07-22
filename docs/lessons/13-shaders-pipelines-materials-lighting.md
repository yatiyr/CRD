# 13 — Shaders, pipelines, materials & lighting, the Cerid way

> *A concepts primer. What each of these words means plainly, and how CKIR realizes it. Read this before the deploy-pipeline
> walkthrough ([12](12-ckir-deploy-pipeline.md)) and the variant deep-dive ([14](14-variants-permutation-and-specialization.md)).
> Complements [11 — the shader-stage frontier](11-the-shader-stage-frontier.md), which covers the 14 GPU stages in detail.*

Everything that runs on the GPU in Cerid is authored as one thing: a **typed graph** in a single IR (CKIR). This lesson defines
the vocabulary, then walks each system.

---

## The vocabulary, in one place

| term | plainly | Cerid realization |
|------|---------|-------------------|
| **Shader** | a program that runs on the GPU | a `KGraph` + `KEntry` (the CKIR IR); the shader text is *emitted* from it |
| **Compute shader / kernel** | a program run by a grid of threads over buffers, no fixed graphics role | a `KEntry` with an imperative statement body |
| **Pipeline** | the GPU object binding compiled bytecode + fixed state so work can be dispatched | `create_pipeline_from_spirv` / `_from_dxil` |
| **Material** | what a *surface* is made of — albedo, normal, roughness, metalness, emission. **Not** how it's lit | a surface graph (MaterialX-style nodes) |
| **Lighting** | how a lit surface looks — the transport that consumes material outputs and produces shaded pixels | a separate renderer pass (+ GI) |
| **Variant** | one configuration of a shader that has optional features | a `key` bitmask over feature toggles |
| **Übershader** | one shader covering *all* feature combinations, unused parts switched off per variant | a graph with `ShaderOption` toggles |
| **Übergraph** | the graph form of an übershader, specialized per variant | pin options + `specialize()` → DCE |
| **Permutation** | the full set of variants; the matrix of every toggle combination | `cook_variant_matrix`, content-hash deduped |

---

## Compute shaders & kernels

A compute shader is the simplest thing to grasp because it has no graphics baggage: it's a function run by many threads at once,
reading and writing buffers. Cerid calls one a **kernel**. Threads are grouped into **workgroups**; threads in a workgroup share
fast **shared memory** and can synchronize at a **barrier**. You launch a grid of workgroups with a **dispatch**.

In CKIR a kernel is a `KEntry` whose body is an ordered list of *statements* — stores, loads, barriers, loops. It reads like
code, but it's data (a graph), so it can be serialized, hashed, and re-emitted to any backend. The same kernel graph emits to
GLSL, HLSL, CUDA, MSL and WGSL, and is checked bit-for-bit against a CPU oracle (`eval_cpu_kernel`) before ever touching a GPU.

Compute is also where Cerid runs its heavy numerical work — FFTs, sorts, scans, GEMM, ray tracing, and the neural nets below.

> **Compute and rendering are separate concerns.** Compute is general GPU work over buffers; rendering adds fixed-function
> stages and framebuffers. Both draw from the same CKIR IR and the same device context.

---

## Pipelines — how shaders actually run

A compiled shader can't run alone. The driver needs a **pipeline**: the bytecode plus the fixed state it runs under.

- **Compute pipeline** — one compute shader + its resource layout. Bind buffers, dispatch a grid. Bytecode: SPIR-V / DXIL / PTX;
  layout from the IR reflection.
- **Raster pipeline** — a chain of stages (vertex → mesh/tessellation → fragment) plus blend, depth, cull state and render
  targets.

Cerid builds a pipeline directly from **precompiled bytecode** — no compiler at runtime — and caches the driver's shader→ISA
compile across runs with a persistent pipeline cache. See [lesson 12](12-ckir-deploy-pipeline.md) for the cache mechanics.

---

## Materials — what a surface is made of

A material answers one question: *at this point on the surface, what are the shading inputs?* It outputs a small set of **surface
properties** — base color, normal, roughness, metalness, emission, opacity — and, by deliberate design in Cerid, it does **not**
do any lighting.

```
Material out  →  base color · normal · roughness · metalness · emission · opacity   (the BRDF inputs at this pixel)
Material graph→  texture fetches · UV math · noise · blends · masks  (~90 MaterialX-style nodes)
Not here      →  lights · shadows · GI · tone-mapping   (those belong to the lighting pass, downstream)
```

Keeping materials **surface-only** is what lets the same material work under any lighting model — forward, deferred,
path-traced — without rewriting it. The material graph is *lowered* before it ships: every node is classified by how often it
changes — **constant** (fold at cook time), **uniform** (per-draw), **per-vertex**, or **per-fragment** — and pushed to the
cheapest stage that's still correct. Constant sub-expressions fold away; per-vertex work leaves the fragment shader. This is the
same const-fold + DCE that powers variant specialization.

A material is just another CKIR graph. It cooks, caches, permutes and hot-reloads exactly like a compute kernel — the pipeline
doesn't special-case "materials."

### Neural materials — the same surface, learned

A **neural material** answers the exact same question — surface properties at a pixel — but instead of fetching textures and
blending them, it **evaluates a tiny neural network** trained to reproduce the material. The inputs (UV, view, a few parameters)
are frequency-encoded and fed through a small MLP; the outputs are the same BRDF inputs a conventional material produces.

| | conventional (textures) | neural (learned MLP) |
|-|--------------------------|----------------------|
| surface = | texture samples + node math | a per-pixel MLP eval |
| bound by | memory bandwidth + VRAM | compute (small matvecs) |
| authored by | artists / MaterialX | trained to match a reference (Adam) |
| trade-off | exact, heavy on storage | compact, tiny weights |

In Cerid the MLP runs through the **cooperative-vector** path — a hardware matmul primitive that made the fused inner loop
**12.5× faster** than a scalar MLP, reconstructing the material at ~**38.9 dB**, with training that runs **on the GPU itself**.
Same CKIR graph, same cook/cache/permute — the "material" just happens to be a network. The related **neural radiance cache**
uses the same trick for lighting, beating a vendor GEMM library 2.37×.

---

## Lighting — turning surfaces into pixels

Lighting is the pass that takes the material's surface outputs, adds the scene's lights and global illumination, and produces the
final shaded color. Because materials are surface-only, the *same* material feeds any of these:

- **Direct light** — evaluate the BRDF against each light; shadows via ray queries or shadow maps.
- **Global illumination** — **DDGI** irradiance probes · **ReSTIR** importance-resampled many-light sampling · a **neural
  radiance cache** for indirect.
- **Denoise** — **SVGF** edge-aware spatiotemporal filtering to clean the sampled signal.
- **Compose** — tone-map, expose, resolve to the framebuffer.

Every one of those stages is itself a CKIR graph (mostly compute kernels). So "lighting" is not a monolith: it's a set of kernels
and passes the renderer schedules, all authored, cooked and cached through the one pipeline in [lesson 12](12-ckir-deploy-pipeline.md).

> **The clean split — material = surface, lighting = transport — is the single most important architectural line.** It's why an
> artist's material survives a switch from forward rendering to a path tracer untouched.

---

## Variants & permutation (in one paragraph)

Real shaders have options (normal map on/off, N lights, skinned/static). Rather than write a separate shader per combination, you
write one **übershader** and produce a **variant** per combination by switching unused branches off. You can author a variant two
ways — an imperative per-key builder, or one **übergraph** with `ShaderOption` toggles that `specialize()` folds per key — and
both flow through the same `cook_variant_matrix`, deduplicated by content hash (in the gate, 4 keys → 2 unique bundles). The full
mechanics are [lesson 14](14-variants-permutation-and-specialization.md).

---

## Authoring — by hand today, node graphs tomorrow

Right now you author everything **programmatically in C++**: graph builders (`buffer_decl`, `select`, `stmt_buffer_store`, the
~90 MaterialX nodes) construct a KGraph. That's precise and scriptable, and it's how every kernel, material and lighting pass in
Cerid is built today.

The important part is that both authoring routes target the **same IR**. A future **node editor** is just a second front-end:
dragging nodes emits the exact same KGraph the C++ builders do. Nothing downstream — cook, variants, cache, load, hot-reload —
needs to know which front-end produced the graph.

> **The whole design in one sentence:** one IR is the meeting point — every way of authoring flows into it, every way of shipping
> flows out of it. Materials, lighting, compute, conventional or neural are all just graphs in that one IR, cooked and managed
> the same way.
