# Lesson 11 — The Shader-Stage Frontier: every stage, what it's for, and the cutting-edge we build on it

> A lecture, not a status report. The phase docs say *what* the D-007 detour ships; this says *what each shader stage
> actually is*, *where it's used*, *what frontier techniques ride on it*, *which backends light up when we're done*, and
> *how the node editor / shader tools fit*. Written so someone who has never touched a mesh shader or a ray-tracing SBT
> comes out able to reason about the whole GPU. Grounded in Cerid's own IR (`crd::kir`) — the 14 `KStage`s and their
> builtins in `engine/kir/include/crd/kir/ckir.hpp` — and the D-007 master table (`docs/detours/D-007-gpu-program-system.md`).

---

## 0. The one idea to hold onto

A GPU is not one pipeline. It is **three programmable pipelines** that share the same silicon (ALUs, caches, memory):

1. **The raster (graphics) pipeline** — turns triangles into pixels. Fixed-function rasterization sits in the middle;
   programmable stages bracket it.
2. **The ray-tracing pipeline** — shoots rays into an acceleration structure and runs shaders at hits/misses. No triangles-
   to-pixels; instead rays-to-radiance.
3. **Compute** — a bare grid of threads with no fixed-function anything. The universal substrate; everything else can be
   rebuilt on top of it.

CKIR (our IR) sits **above all three**. You author a graph of typed ops once; the emitters lower it to GLSL / HLSL / WGSL /
MSL / PTX / HIP, and each of those becomes SPIR-V / DXIL / etc. A *stage* (`KStage`) tells the backend *which pipeline slot*
this graph fills, so the same value-algebra (add, dot, sample, select, the coop-vector matmul…) is legal in a vertex shader,
a fragment shader, a ray hit shader, or a compute kernel — the stage only changes what the *inputs* and *outputs* are wired
to (a vertex attribute vs. a ray payload vs. a storage buffer). That is the whole trick: **one algebra, many stages, every
backend.**

The 14 stages Cerid declares from day one (`enum class KStage`, mirrored one-for-one by `crd::gpu::ShaderStage`):

```
Compute · Vertex · TessControl · TessEval · Geometry · Fragment · Task · Mesh
       · RayGen · Intersection · AnyHit · ClosestHit · Miss · Callable
```

Rule we hold to (B3-a): **declare the whole domain, refuse the unimplemented part loudly.** A backend that cannot yet emit
`Mesh` returns `nullptr` from `create_program` — it never silently pretends a mesh shader is a compute kernel.

---

## 1. The raster geometry front-end (the classic pipeline)

Data flows: **vertices → [tessellation] → [geometry] → rasterizer (fixed) → fragments → framebuffer.** The bracketed stages
are optional. This is the pipeline that has drawn every game since the 1990s; it is still how most pixels reach most screens.

### 1.1 Vertex shader (`KStage::Vertex`)

**What it is.** Runs once per input vertex. Its one required job is to write **clip-space position** (`gl_Position` /
`SV_Position`) — where this vertex lands in the 4D homogeneous cube the rasterizer clips against. It may also pass
*interpolants* (colour, UVs, normals, tangents) down to the fragment shader; the rasterizer interpolates them across the
triangle.

**Inputs** (Cerid builtins): `VertexIndex`, `InstanceIndex`. Vertex *attributes* arrive as `StageIn(type, location)` leaves.
**Output:** `KEntry::position` (+ interpolants via `KEntry::out[]`).

**Where it's used.** Everything rasterized. Static meshes, skinned characters (the vertex shader applies the bone-matrix
palette — set-3 in ADR-0102), instanced foliage (one draw, `InstanceIndex` picks the transform), GPU-driven particles,
full-screen passes (our B3-e triangle: three positions synthesized from `VertexIndex`, no vertex buffer at all).

**Frontier on this stage.** The modern vertex shader is *thin* — heavy geometry work has moved to compute + mesh shaders.
But it is where **skinning**, **morph targets**, and **procedural vertex animation** (wind, water, cloth-preview) live, and
where **vertex-pulling** (read vertices from a storage buffer by index instead of a fixed vertex layout — bindless geometry)
turns the input-assembler into just another buffer read.

*Cerid example — B3-e's whole vertex shader, authored in the IR:*
```cpp
const int vid = g.builtin(KBuiltin::VertexIndex);           // int
const int x   = g.select(eq0, x0, g.select(eq1, x1, x2));   // pick one of 3 corners
const int pos = g.vec4(x, y, z, w);
ve.position = pos;                                          // → gl_Position / SV_Position
```
The emitters turn this into `gl_Position = ...` (GLSL) or a `VSOut{ float4 clip : SV_Position }` struct (HLSL) — you never
wrote either.

### 1.2 Tessellation — TessControl + TessEval (`KStage::TessControl`, `KStage::TessEval`)

**What it is.** Hardware tessellation subdivides a coarse *patch* (a triangle or quad of control points) into many small
triangles **on-chip**, so you ship little geometry over the bus but render fine geometry. Two programmable stages bracket a
fixed-function tessellator:

- **TessControl (TCS / "hull" in HLSL)** runs once per output control point *and* emits the **tessellation levels** — how
  finely to subdivide, usually chosen per-edge from screen-space size or curvature. Builtins: `InvocationId` (which control
  point), `PatchVertexCount`.
- The fixed **tessellator** then generates barycentric sample points.
- **TessEval (TES / "domain" in HLSL)** runs once per generated vertex, reads its barycentric `TessCoord`, and evaluates the
  final surface position — e.g. a Bézier/B-spline patch, or a flat triangle **displaced along its normal by a height map**.

**Where it's used.** Terrain with distance-adaptive detail, displacement mapping (bricks/rock that are *actually* bumpy, not
just normal-mapped), water surfaces (Gerstner waves evaluated in the domain shader), smooth character surfaces (PN-triangles,
Catmull-Clark approximations), and — critically — **any platform without mesh shaders** (mobile, WebGPU, older GPUs). This is
the *portable* displacement path.

**Frontier vs. legacy.** On the desktop frontier, mesh shaders + cluster acceleration structures largely *supersede*
tessellation (they cull and LoD at meshlet granularity, no patch-level bottleneck). But tessellation is not dead: it is the
only hardware displacement path that runs everywhere, and it is genuinely good at continuous LoD for terrain/water. Cerid
declares TCS/TES as real stages with real builtins (`TessCoord`, `PatchVertexCount`, `InvocationId`) — see the **gap note in
§10**: we are promoting this from "legacy-emulated" to a real emitter path so the displacement frontier is portable, not
Vulkan/DX12-only.

### 1.3 Geometry shader (`KStage::Geometry`)

**What it is.** Runs once per *primitive* (triangle/line/point) and can **emit a variable number of output primitives** —
amplify one triangle into several, or route a primitive to a specific framebuffer **`Layer`** (builtin) or viewport.

**Where it's used, honestly.** It *can* do point-sprite expansion, single-pass cubemap/shadow rendering (emit the same
triangle to 6 layers), silhouette extrusion for stencil shadows, hair-fin generation, wireframe/barycentric overlays.

**Why it's discouraged.** The geometry shader serializes primitives through a small on-chip buffer with unpredictable output
counts — it is a **notorious throughput bottleneck** on every modern GPU. The frontier answer to *every* geometry-shader use
case is **mesh shaders** (amplify in parallel, no serialization) or **multiview** (single-pass stereo/cubemap without a GS).
Cerid supports it for ports and correctness but never as the amplification path. (B3-a: "Geometry: supported for ports, never
the amplification path (mesh is).")

### 1.4 Fragment shader (`KStage::Fragment`, "pixel shader")

**What it is.** Runs once per *covered sample* (roughly, per pixel the triangle touches) and outputs colour(s) to the
framebuffer — and optionally depth. This is where **shading** happens: sample textures, evaluate the BRDF, run the light
loop, apply fog. Inputs are the *interpolated* vertex outputs plus builtins: `FragCoord` (screen xy + depth + 1/w),
`FrontFacing`, `SampleId`, `PointCoord`, `PrimitiveId`, `Layer`.

**Where it's used.** Literally all surface appearance in a rasterizer: PBR materials, decals, terrain blending, post-process
(a full-screen triangle + a fragment shader is the canonical post-effect), UI. In a **deferred** renderer the fragment
shader writes a G-buffer (albedo/normal/roughness/…) and a later full-screen pass lights it; in **Forward+** it runs the
clustered light loop inline.

**Frontier on this stage** — this is where a *lot* of the cutting edge lives, and it maps to specific Cerid slices:

- **Derivatives** `dFdx`/`dFdy`/`fwidth` (slice **B1**) — the fragment shader runs in 2×2 quads so neighbouring pixels can
  differentiate any value. This drives **mip selection**, analytic anti-aliasing of procedural patterns, and screen-space
  normal reconstruction.
- **`discard` / alpha-test / early-Z** (B1) — foliage cutouts, and the depth-test-before-shading optimization.
- **Variable Rate Shading (VRS)** (`VK_KHR_fragment_shading_rate`, slices **B1/B4**) — shade one result for a 2×2 or 4×4
  block where the eye won't notice (peripheral vision, motion-blurred regions, dense foliage). A large, cheap frame-time win;
  driven per-draw, per-primitive (from the mesh shader), or per-region via an image.
- **Fragment interlock / Rasterizer-Ordered Views (ROV)** (`VK_EXT_fragment_shader_interlock`, B1) — lets fragments that map
  to the same pixel run in **primitive order** with a critical section. This is how you do **order-independent transparency
  (OIT)**, single-pass **voxelization**, and programmable blending that fixed-function blend can't express.
- **Conservative rasterization** (B1) — rasterize a triangle if it touches a pixel *at all* (not just at the sample point);
  the basis of accurate voxelization and some GI/occlusion techniques.
- **Sampler feedback** (`SV_FeedbackTexture`, slice **B2**) — the GPU records *which mip/tile of a texture was actually
  sampled* this frame, so a **virtual-texturing / Nanite-material** system streams in only the texels the camera can see.

---

## 2. The modern geometry front-end: mesh shading

The classic `Vertex → … → Fragment` front-end has a structural problem: the input assembler and the vertex shader are a
**fixed, per-vertex funnel**. You cannot cull a cluster of triangles before shading its vertices; you cannot decide *at
runtime* how much geometry to emit without a geometry shader's serial bottleneck. **Mesh shading** replaces the whole front
half of the pipeline with two *compute-like* stages.

### 2.1 Task / Amplification shader (`KStage::Task`)

**What it is.** A compute-style workgroup that runs *before* geometry exists. It decides **how many mesh workgroups to
launch** (`SetMeshOutputs`-style dispatch) and passes them a **payload**. This is where **GPU culling** lives: frustum cull,
occlusion cull (against a Hi-Z pyramid), backface-cluster cull, LoD selection — all in parallel, before a single vertex is
transformed. Builtins: the workgroup family (`WorkgroupId`, `LocalInvocationId`, …).

### 2.2 Mesh shader (`KStage::Mesh`)

**What it is.** A compute-style workgroup that **outputs a small indexed triangle cluster ("meshlet")** directly — up to
~128 vertices / 256 primitives per workgroup — writing per-vertex *and* per-primitive attributes. No input assembler, no
vertex-buffer layout; the mesh shader *pulls* its data however it likes.

**Where it's used — this is the Nanite pipeline.** Virtualized/nanite-style geometry works like this:
1. Offline, chop meshes into **meshlets** and build a LoD DAG.
2. At runtime, a **task shader** picks the right LoD per cluster from screen error and culls invisible clusters.
3. **Mesh shaders** emit the surviving meshlets straight to the rasterizer.
4. **Per-primitive VRS** and per-primitive culling happen in the same shader.

This is how you render film-quality, billion-triangle scenes at real-time rates. Slice **B4** builds it: Task + Mesh stages,
meshlet I/O, compute-cull → meshlet dispatch, per-primitive VRS, and — the bridge to ray tracing — **emitting the same
cluster data that feeds a cluster acceleration structure** (RTX Mega Geometry, so the rasterizer and the ray tracer share one
geometry representation).

**Why it wins.** Everything the geometry and tessellation stages did serially, the mesh pipeline does in parallel, at
cluster granularity, with culling *before* transform. It is strictly the frontier front-end.

---

## 3. The ray-tracing pipeline

Rasterization answers "for this triangle, which pixels?" Ray tracing answers the *inverse and more general* question: "for
this ray, what does it hit, and what light comes back?" That inversion is what makes **accurate reflections, refraction, soft
shadows, ambient occlusion, and full global illumination / path tracing** possible without screen-space hacks.

The scene lives in an **acceleration structure (AS)** — a two-level BVH: **bottom-level (BLAS)** per mesh, **top-level
(TLAS)** of instances. You bind shaders through a **Shader Binding Table (SBT)** — an indexed table that says "for a hit on
*this* geometry, run *that* closest-hit shader." Six stages, all sharing the RT builtins (`LaunchId`, `LaunchSize`,
`WorldRayOrigin/Direction`, `ObjectRayOrigin/Direction`, `HitT`, `HitKind`, `InstanceCustomIndex`, `ObjectToWorld`, …):

- **RayGen (`KStage::RayGen`)** — the entry point, launched over a 2D grid like a compute shader. It builds primary rays
  (usually from the camera), calls `traceRay`, and writes the result (radiance) to an image. A path tracer's whole outer loop
  lives here.
- **Intersection (`KStage::Intersection`)** — custom hit test for **non-triangle primitives** (analytic spheres, curves,
  voxels, SDFs, splats). Triangles use the fixed-function intersector; you only write this for procedural geometry.
- **AnyHit (`KStage::AnyHit`)** — runs at *every* candidate hit along the ray, before the nearest is known. Used for
  **alpha-tested transparency** (foliage, chain-link, decals): test the texture's alpha and `ignoreIntersection` if it's a
  hole. Must be cheap and order-independent.
- **ClosestHit (`KStage::ClosestHit`)** — runs once, at the nearest hit. This is the ray-tracing analog of the fragment
  shader: fetch the surface, evaluate the BRDF, spawn shadow/GI rays, write the payload.
- **Miss (`KStage::Miss`)** — runs when the ray hits nothing: sample the sky/environment, return ambient.
- **Callable (`KStage::Callable`)** — a shader you invoke *from* another RT shader by index, like a virtual function. Used to
  factor a material/BSDF library so the hit shader stays small and the SBT stays flat.

**Inline ray queries (`rayQuery`)** are the other flavour: instead of the SBT machinery, *any* stage (compute, fragment, even
a mesh shader) can open a ray query, traverse the AS inline, and read the hit — perfect for a single shadow ray, an AO probe,
or a GI gather without a full RT pipeline.

**The frontier on ray tracing** (this is where 2024–2025 hardware moved, and where slices **B9** (shaders) + **C3** (device)
aim):

- **Shader Execution Reordering (SER)** — `VK_EXT_ray_tracing_invocation_reorder` / DXR 1.2. After a trace, rays that hit the
  same material are *reordered into coherent groups* before the hit shader runs, so divergent path tracing stops thrashing
  the SIMD lanes. **20–100% faster path tracing** for a few lines of shader change. This is the single biggest RT win of the
  generation.
- **Opacity Micromaps (OMM)** — `VK_EXT_opacity_micromap`. Bake a micro-grid of "opaque / transparent / unknown" onto a
  triangle so the hardware skips most alpha-test any-hit invocations. Foliage-heavy scenes get dramatically cheaper.
- **Cluster Acceleration Structures / RTX Mega Geometry** — `VK_NV_cluster_acceleration_structure`. Build the BVH over the
  *same meshlet clusters* the mesh shader rasterizes, with streaming LoD and dynamic tessellation/displacement, so you can
  **ray trace Nanite-scale geometry** (previously the BVH build/memory made that impossible). This is the mesh↔RT bridge.
- **Ray-traced curves & spheres (hair/fur)** — `VK_NV_ray_tracing_linear_swept_spheres` (LSS) and DXR curve primitives.
  Native hardware intersection of **linear-swept spheres and curves** so you can path-trace *hair, fur, and foliage* without
  exploding them into millions of triangles. This is the 2025 frontier for character rendering — **and it was missing from
  our plan; see §10.**
- **Motion blur AS** — time-parameterized acceleration structures for correct ray-traced motion blur.

Downstream techniques the RT pipeline unlocks (built in the rendering phase, *expressible* by the IR now): **ReSTIR**
(reservoir spatiotemporal importance resampling — real-time many-light path tracing), **path-traced GI / reference path
tracer**, **ray-traced reflections/shadows/AO** as drop-ins, and **DDGI / irradiance probes**.

---

## 4. Compute — the universal substrate

**What it is.** A grid of workgroups of threads, no fixed function anything. You get shared memory per workgroup, atomics,
barriers, and — the frontier part — **subgroup (wave) ops** and **cooperative matrix/vector**. Builtins: the whole workgroup
family (`GlobalInvocationId`, `LocalInvocationId`, `WorkgroupId`, `NumWorkgroups`, `LocalInvocationIndex`).

**Where it's used.** *Everything that isn't a triangle.* Cerid's entire hesap numerical stack targets it (GEMM at cuBLAS
parity, FFT, sparse solvers, ODEs). In rendering: light culling (clustered/tiled), Hi-Z build, GPU particle simulation, hair
simulation, bloom/blur/tonemap, SSAO/GTAO, screen-space GI, denoisers, skinning-to-buffer, and every step of GPU-driven
rendering (build draw lists, compact, sort). It is also the host for **inline ray queries** and the **build side of GPU-driven
rendering**.

**The frontier on compute** (slices **B11** shader-side, **C5** device-side):

- **Subgroup / wave / quad ops** (B11) — `subgroupAdd`/`broadcast`/`shuffle`/`ballot`/prefix-scan (GLSL), `Wave*` (HLSL),
  warp intrinsics (CUDA), SM 6.6 quad ops. Cross-lane communication *without* going through shared memory — the key to fast
  reductions, scans, and compaction. Cerid pins these to a **fixed reduction tree so results stay bit-deterministic** across
  backends (the mission's bit-exactness rule).
- **Cooperative matrix** — the tensor-core / WMMA path: a whole workgroup cooperates on a tile-matmul in one instruction.
  This is what put Cerid's GEMM at cuBLAS parity (v17-g, coopmat2). It is the compute-side tensor primitive.
- **Cooperative vectors** (slices **B10/C6**) — `VK_NV_cooperative_vector`, HLSL coop-vectors + SM 6.9 long vectors
  (`vector<T, 5..1024>`). Per-*invocation* small-matrix × vector + activation: i.e. **evaluate a small neural network inside a
  shader**. See §5.
- **Work graphs** (`VK_AMDX_shader_enqueue` / D3D12 Work Graphs SM 6.8, slices **B11/C5**) — the GPU **schedules its own
  work**: a node shader enqueues more nodes, with the driver load-balancing, no CPU round-trip. Called "the biggest addition
  since ray tracing." Plus **device-generated commands** (`VK_EXT_device_generated_commands` = D3D12 ExecuteIndirect on
  steroids): the GPU writes its own draw/dispatch stream.

---

## 5. Neural rendering — why cooperative vectors are the moat

This deserves its own section because it is the frontier *and* it is where Cerid has an advantage no shipping engine has.

**Cooperative vectors** let a shader evaluate a **small MLP (multi-layer perceptron)** per invocation — matrix-multiply-
accumulate + activation, on tensor hardware, inside a fragment/compute/hit shader. That unlocks:

- **Neural Texture Compression (NTC)** — store materials as a tiny neural field; decode texels on the fly at a fraction of
  the memory. (RTXNS / Intel demos.) Slice **B10** consumer.
- **Neural BRDF / neural materials** — replace an analytic BRDF or a huge measured-material table with a learned network that
  evaluates in the hit/fragment shader.
- **Neural radiance caching** — cache indirect lighting in a network the GPU trains *and* evaluates at runtime, so path
  tracing converges in far fewer samples.

**The moat.** CKIR *is* the autodiff graph. Cerid already shipped forward-mode (v15) and reverse-mode (v16) automatic
differentiation *in the IR* — the same graph that emits a shader can emit its **gradient**. So a neural material **and its
training gradient come from one IR**, differentiable by construction. No shipping engine does this: elsewhere the shader and
its derivative are two hand-written artifacts that drift. For us, "train a neural material" and "evaluate a neural material"
are the *same graph*, lowered twice. That is the single most distinctive thing this detour is building toward (slices
B10 + C6, resting on the v15/v16 autodiff already in the box).

---

## 6. The frontier techniques, mapped to stages (a cheat sheet)

| Technique | Stages it rides on | Cerid slice(s) |
|---|---|---|
| Nanite-style virtualized geometry | Task + Mesh (+ cluster AS) | B4, C3 |
| Path tracing / reference GI | RayGen + ClosestHit + Miss + AnyHit | B9, C3 |
| Faster path tracing (coherence) | SER on the RT pipeline | B9, C3 |
| Real-time many-light (ReSTIR) | RayGen + Compute (reservoirs) | B9 + rendering phase |
| Ray-traced hair / fur | Intersection / LSS + ClosestHit | **B9, C3 (added — §10)** |
| Order-independent transparency | Fragment + ROV/interlock | B1 |
| Virtual texturing / streaming | Fragment + sampler feedback | B2 |
| Variable rate shading | Fragment / Mesh (per-primitive) | B1, B4 |
| Displacement / terrain LoD | TessControl + TessEval (or Mesh) | **B4 + tess (§10)** |
| Neural materials / NTC / radiance cache | Compute / Fragment / ClosestHit + coop-vectors | B10, C6 |
| GPU-driven rendering | Compute + work graphs + DGC | B11, C5 |
| Skinning / morph / procedural vertex | Vertex (or Compute-to-buffer) | B8, rendering phase |
| Stylized / NPR (toon, gooch, hatching, outline) | Fragment (+ material graph) | B5, B6 |
| Single-pass stereo (VR) | multiview (Vertex/Mesh + `Layer`/ViewIndex) | **candidate — §10** |

---

## 7. Why one IR — the payoff of the whole detour

Every engine that supports N backends usually writes its shaders N times (or once in HLSL and cross-compiles, losing
control). Cerid inverts it: **you author the IR once**, and:

- **Portability by construction** — GLSL/HLSL/WGSL/MSL/PTX/HIP are *derived*. A new backend is a new emitter, not a shader
  rewrite. (I1/I2 invariants: no shading language or bytecode ever leaves a backend; consumers hold an opaque `IGpuProgram`.)
- **Bit-exactness** — the IR pins rounding and reduction order, so the same kernel gives the same bits on Vulkan, DX12, and
  CUDA. That is the mission ("ALL backends perfect + bit-exact").
- **One optimizer** — CSE / const-fold / DCE / stage-split run on the IR, once, benefiting every backend.
- **Differentiability** — §5. The gradient is another lowering of the same graph.
- **Tooling leverage** — reflection, cooking, hot-reload, and the future node editor all operate on *one* well-typed
  representation, not on six languages' worth of text.

---

## 8. Which backends are truly enabled when the detour ends

Two different questions: (a) can we **emit** a stage's code for a backend, and (b) can we **execute** it through a device
context. Emission is universal wherever the stage exists; execution follows each API's real frontier surface.

| Backend | Language | Compute | Raster (VS/FS/mesh) | Ray tracing | Neural (coop-vec) | Work graphs | After the detour |
|---|---|---|---|---|---|---|---|
| **Vulkan** | GLSL→SPIR-V | ✅ | ✅ (shader objects, dynamic rendering, mesh) | ✅ (KHR RT + NV cluster/LSS/SER/OMM) | ✅ (`VK_NV_cooperative_vector`) | ✅ (`VK_AMDX_shader_enqueue`) | **the reference — the entire frontier runs here** |
| **DX12** | HLSL→DXIL | ✅ | ✅ (mesh shaders) | ✅ (DXR 1.2) | ✅ (SM 6.9 coop-vectors) | ✅ (Work Graphs SM 6.8) | **compute + raster shipped (C1/C4); HLSL emits every stage, so RT/mesh/neural device contexts are a short mirror** |
| **WebGPU** | WGSL | ✅ | ✅ (VS/FS) | ⏳ future WebGPU extension | ⏳ | ✗ | **browser reach: compute + raster now; RT/mesh await WebGPU's own frontier** |
| **CUDA** | PTX | ✅ | ✗ (no graphics pipeline) | via OptiX (not our path) | ✅ (tensor cores) | ✗ | **compute + neural/tensor; no raster/RT-graphics (API has none)** |
| **HIP** | (GCN/RDNA) | ✅ (v17-o) | ✗ | ✗ | ~ | ✗ | **compute (AMD), later host** |
| **Metal** | MSL | ✅ | ✅ (Metal mesh) | ✅ (Metal RT) | ~ (simdgroup-matrix) | ✗ | **emit-complete now; device validation at Part C** |

The honest one-liner: **Vulkan is the full-frontier reference; DX12 is at parity for compute + raster with HLSL ready for the
rest; WebGPU/CUDA/HIP/Metal light up exactly the subset their APIs actually expose.** The IR emits for all of them — the gate
is each platform's own capabilities, not our source.

---

## 9. Node editors and shader-authoring tools — the plan

**Yes — a full visual node editor and a text shader DSL are planned.** They are deliberately **not** part of this detour;
they land in the **editor phase** (roadmap step 7: hesap → physics → rendering → UI → editor → node editor). Building the UI
now would be building on sand — the IR has to be complete and stable first.

What the detour *does* lock now is the **round-trip design invariant** (cheap insurance that the IR is provably editable):
every `KOp`, every builder, and every stage construct maps to a **typed node definition**, and `graph → IR → graph` is
**loss-less** — validated by a decompile test. So the IR is being built *node-editable by construction*: when the editor
phase arrives, the node editor is a *view* over the existing graph, not a reimplementation.

At editor time we build (all resting on this one IR):
- **The visual node editor** — drag nodes (the SOURCE/OPERATOR/UV/NPR library of slice B6, plus every stage construct),
  wire typed ports, live-preview the compiled result. This is the Unreal-material-editor / MaterialX-graph experience, but
  over *our* IR, so a graph you build visually is the same object the cooker ships and the autodiff differentiates.
- **A Slang-inspired text DSL** — for people who'd rather type; same semantic layer, same IR out.
- **A shared semantic / LSP layer** — hover types, autocomplete, go-to-definition, inline errors — because the IR is
  fully typed (B0's `KType`), the language server is reflection over the graph.
- **Live-preview authoring + hot-reload** — edit graph/text → recook affected variants → atomic pipeline swap (slice D5).

So: the *tools* come later, but everything this detour ships is a deliberate down-payment on making those tools trivial.

---

## 10. Gaps we found in the frontier — and added to the slices

Reviewing the 14 stages and the capability tables against the 2025 hardware frontier surfaced real omissions. Per the
"no compromises / full frontier by construction" mandate, these were **added to the D-007 master table** (2026-07-11):

1. **Ray-traced curves & spheres (hair / fur / foliage)** — `VK_NV_ray_tracing_linear_swept_spheres` (LSS) + DXR curve
   primitives. The RT rows had cluster AS, OMM, SER, and motion AS but **no native curve/sphere primitive** — so path-traced
   hair would have meant exploding strands into millions of triangles. This is the 2025 character-rendering frontier. Added
   to **B9** (intersection/hit reads the swept-sphere/curve hit) + **C3** (the AS build for LSS/curve geometry).

2. **Hardware tessellation as a *real* path, not "legacy-emulated"** — the stage model already declares `TessControl` /
   `TessEval` with correct builtins (`TessCoord`, `PatchVertexCount`, `InvocationId`), but the plan lumped them into B4 as
   emulated. Tessellation is the **portable displacement path** (mobile / WebGPU / older GPUs have no mesh shaders), so it
   earns a real emitter. Promoted to an explicit slice alongside B4.

3. **Cross-vendor cluster geometry** — the cluster-AS row named only NVIDIA's `VK_NV_cluster_acceleration_structure`; noted
   AMD's **DGF (Dense Geometry Format)** as the cross-vendor counterpart so the "Mega Geometry" capability isn't single-
   vendor. Folded into C3's contract.

4. **Single-pass stereo / multiview (VR/XR)** — flagged as a *candidate* (not yet scheduled): `VK_KHR_multiview` + a
   `ViewIndex` builtin, the frontier replacement for geometry-shader-based stereo. Left as a candidate because VR is not yet
   a committed target; recorded so it isn't forgotten if it becomes one.

The master table and its capability→slice tables now carry (1)–(3); (4) is parked as a documented candidate.

---

## 11. The one-paragraph takeaway

Three pipelines (raster, ray tracing, compute), fourteen programmable stages, one typed IR sitting above all of them.
The classic raster front-end (vertex → tessellation → geometry → fragment) still draws most pixels and owns displacement,
VRS, OIT, and virtual texturing; the modern front-end (task → mesh) owns Nanite-scale virtualized geometry; the ray-tracing
pipeline (raygen → intersection → any-hit → closest-hit → miss → callable) owns accurate light transport, now made practical
by SER, cluster AS, and — newly on our plan — swept-sphere hair; and compute is the universal substrate that also hosts the
neural frontier (cooperative vectors) where Cerid's differentiable IR is a genuine, singular advantage. Author once in CKIR,
run everywhere it can run, differentiate for free, and — at the editor phase — wire it all together in a visual node graph.
