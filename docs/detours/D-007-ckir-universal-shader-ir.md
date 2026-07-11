# D-007 — CKIR becomes the universal GPU-program IR (the single source of truth for every shader + kernel)

**Status:** ACTIVE (opened 2026-07-09) — **PAUSED at B3 pending detour [D-008](D-008-gpu-context-convergence.md)**
(the gpu-context convergence, [ADR-0103](../decisions/0103-gpu-context-owns-every-gpu-program.md)). The raster emitters
must land behind `crd::gpu::IGpuContext::create_program(KGraph, KEntry)`, not `crd::shader::compile_glsl`.
**Phase A (CKIR core) COMPLETE.** **Phase B: slice B0 (type system) COMPLETE — B0-0…B0-4 ✅; B3-a ✅; B3-a′ ✅ (the
14-stage model + 32-builtin table + `entry_valid`; kir 400 asserts / 43 cases).
NEXT = D-008 C0/C1, then back here at B3-c.** B0 delivered `KType`, mat2 + non-square
matrices, bool/bvec/ivec/uvec, and struct/array aggregates; session `docs/sessions/2026-07-10-d007-b0-type-system.md`.
B0 still needs its multi-config DoD before it is *closed*.
Re-scoped 2026-07-09 (session close): D-007 =
the **IR substrate** — Phase A (core ✅) + **Phase B (material/shading capability)** + **Phase D (cook)**. The **front-ends
(node editor UI + text shader language = Phase C) are DEFERRED to the editor phase** (user decision: "we are not going to
touch front end of the node editor"). Authoring during D-007 + the rendering phase is via the **C++ builders** (which exist).
**On exit, the GPU/shader system can EXPRESS everything** below; the actual systems (render pipelines, physics compute,
particles, editor) are built ON this IR in their own phases.

**The mission for this GPU system (why the IR must be complete):** compute the best + most interesting things end to end —
**ML / AI · FFT · simulations · skinning · particles · light systems · incredible rendering that out-beauties Unreal ·
ray tracing when needed · beautiful + stylized effects (toon / NPR) · anything we can do with a GPU** — portably across ALL
backends, correct + performant + bit-exact. D-007 is the substrate that makes all of it expressible from one IR.

**North-star ADRs:** `0101-ir-is-source-of-truth-for-all-shaders.md` (the IR) + `0102-render-data-lighting-pass-architecture.md`
(how the IR feeds the renderer). Corpus: `docs/systems/shader-ir-corpus-and-stages.md`. Sessions: `docs/sessions/2026-07-09-d007-*.md`.
**Owner pipeline:** research → coder → tester → docs-keeper; per-slice DoD (CPU-oracle bit-exact + per-backend GPU
validation + zero regression; per-slice-check at close).

---

## The overarching roadmap (the sequence we are following — captured so nothing is lost)

1. **D-007 (NOW)** — universal shader/kernel IR: Phase A (core ✅) + Phase B (material/shading capability) + Phase D (cook).
   Front-ends deferred. Authored via C++ builders. → the IR can express everything.
2. **→ hesap (v17+)** — resume the numerical / GPU-compute stack (ML/AI/FFT/linear-algebra/solvers/etc.) on the IR.
3. **→ eylem / physics** — GPU-compute physics on CKIR: **cloth, mesh deformation, hugely-crowded simulations, ragdolls**,
   broadphase. (Crush PhysX/Jolt WITH determinism.)
4. **→ rendering** — build the render pipelines (Forward+ / Deferred / hybrid / ray tracing / particles / post-FX / NPR)
   on the Phase-B material profile + the ADR-0102 architecture.
5. **→ UI system** — built on top of the shader/material system.
6. **→ first editor.**
7. **→ node editor + text shader language (Phase C)** — built at editor time, on the C1 node-schema data model (locked now).

> This detour hardens the IR core that steps 2–7 all stand on. If a slice grows past its contract, promote it to its own
> phase — don't let it quietly take over the roadmap.

## Why

Shaders/kernels were authored/stored three inconsistent ways: CKIR compute (IR-first ✅), geometry kernels (hand-written
GLSL ✗), rendering Effects (hand-written GLSL/HLSL ✗). GLSL/HLSL are backend *languages*; SPIR-V/DXIL backend *bytecodes* —
authoring/storing as any of them is lock-in AND makes a node editor impossible. **ADR-0101: one backend-neutral IR is the
single source of truth; every backend language/bytecode is a derived output.** Prior art: Slang (language+IR→all backends),
MaterialX (material graph→codegen), Unreal/Unity (graph→HLSL+variants), OpenPBR (the surface model), mesh-shaders.

## Render-data & renderer integration — DECIDED (ADR-0102)

The engine ALREADY has a mature, frontier-shaped renderer (frame graph · `IRenderPath` Forward/Forward+/Deferred/VisBuffer
planned · `PerFrameUbo` camera/time · `MaterialTemplate`+`ShaderOption` variants · reflection · cooking). **CKIR does NOT
replace it — it replaces the hand-written GLSL as the shader SOURCE**; the material/cook/bind infra is reused. Contract
Phase B builds against (full detail in ADR-0102):

- **Globals (camera/time/constants) live in the renderer's per-frame set 0, NOT the GPU context** (upholds ADR-0099:
  gpu-context = DEVICE only; renderer owns frame data + lights + frame graph + render paths).
- **Frequency-based descriptor sets:** **0** frame (camera/time/env/IBL) · **1** pass/lighting (light buffer, cluster grid,
  shadow maps/CSM, G-buffer) · **2** material (params + textures) · **3** object (model, skinning palette, instance).
  Bindless / GPU-driven (draw-indirect, `gl_DrawID`-indexed) ready.
- **Material = SURFACE RESPONSE (OpenPBR params), lighting-agnostic; render path = LIGHTING technique** → one material works
  **Forward+ (default)** OR **Deferred** — HYBRID (deferred/vis-buffer opaque + forward transparent, the frontier norm) +
  ray-traced lighting when wanted. A shared CKIR lighting library (BRDF + clustered loop + shadows) is called inline
  (forward) or in the lighting pass (deferred).
- **Multi-pass (shadows / CSM / G-buffer / SSAO / RT) = the frame graph**; a material contributes per pass via
  `variant_for_pass`. **Skinning** = set-3 structured palette (VS-skin default). **Uber-shaders** = existing `ShaderOption`
  variants + Phase-D on-demand cook.

## Phase A — harden the CKIR core ✅ COMPLETE (CPU oracle + Vulkan + DX12)

| slice | one-line contract | gate / status |
|---|---|---|
| **A1** | algebraic + comparisons + bit ops (and/or/xor/not/shl/shr/count/lsb/msb/extract/insert/reverse) | ✅ oracle bit-exact + GPU |
| **A2** | transcendental + shader intrinsics (exp2/log2/rsqrt/tan/asin/acos/atan/atan2/sinh/cosh/cbrt/smoothstep/radians/degrees/mod/fma) + minor gaps (ldexp/floatBits↔int/modf) + translate/scale | ✅ oracle ULP + GPU |
| **A3** | VALUE TYPES — vec2/3/4 + swizzle + geometric + relational (any/all) + mat3/4 (mul/transpose/det/inverse/outer/from-cols) + interp (mix/step/smoothstep/slerp/nlerp) + quaternions (mul/conj/rotate/axis-angle/to-mat3) | ✅ Vulkan + DX12 (HLSL col-major convention) |
| **A4-t1** | FIXED-count loops (`unroll_for`) — compile-time unroll ⇒ pure dataflow | ✅ CPU + Vulkan + DX12, bit-exact |
| **A4-t2** | DYNAMIC control flow — `for_loop` (native per-thread GPU loop, divergent count, body-scoped), bounded `while_loop`, `switch_case`/`if`-branch | ✅ CPU oracle (max+mask ≡ GPU) + Vulkan + DX12, bit-exact |

**Suites:** kir 129 · kir-vulkan 32983 · kir-dx12 30804, zero regression. **Audit (2026-07-09):** op set matches this table;
complete for the math scope. **Type-layer gaps → B0 — ALL CLOSED:** ~~float-only emitter `vtype`/`htype` (no `ivec`/
`uvec`/`bvec`, comparisons → float 0/1)~~ **B0-3**; ~~no `mat2` (comps4=vec4)~~ **B0-1/B0-2**;
~~no struct/array aggregates~~ **B0-4**.

## Fan-out — every backend gets the full corpus

| backend | lang | status |
|---|---|---|
| Vulkan | GLSL | ✅ full corpus (scalar + vec/mat/quat + control flow) + B0 type layer (`vec/ivec/uvec/bvec`, `matCxR`, `lessThan()` family) |
| DX12 | HLSL | ✅ full corpus + B0 type layer (`floatN/intN/uintN/boolN`, `floatRxC`, `crd_inv2`) — mat4-inverse the one deferred cofactor helper |
| WebGPU | WGSL | ✅ **type layer FANNED OUT** (2026-07-10): `vec/matCxR<f32>` · `vec3<bool>` · bool comparisons · struct SROA · mat2/non-square. Emitted `crd_inv2`/`crd_inv3` (WGSL has no `inverse()`), column-built outer product (no `outerProduct()`), `select()` (no `?:`), `faceForward`. **Runs on real hardware here**, gated vs the CPU oracle. ⚠ ULP-tolerant, never bit-exact — WGSL has no `precise`. Remaining: dynamic control flow (`For`) refuses loudly |
| CUDA | PTX | ✅ **type layer FANNED OUT by SCALARIZATION** (2026-07-10). CUDA has **no native vector arithmetic** (`float3` carries no operators) and no matrix type, so a value of `comps` components becomes `comps` scalar temps and every op is emitted componentwise. Aggregates come FREE: a component index resolves back to its producing scalar at emit time (so CUDA even handles a `Select` of structs, which the GLSL/HLSL SROA path must refuse). `--fmad=false --prec-div=true --prec-sqrt=true` ⇒ componentwise ops are **bit-exact** vs the oracle. **Runs on real hardware here.** Compute-only — no material stages. Remaining: `For`, mat4 det/inverse refuse loudly |
| Metal | MSL | ✅ **type layer FANNED OUT** (2026-07-10): native `float3`/`float3x3`/`bool3`, real `?:`, column-first `floatCxR` (as GLSL). Emitted `crd_inv2`/`crd_inv3` + column-built outer (MSL has neither `inverse()` nor `outerProduct()`). **Not buildable on this host** ⇒ gated STRUCTURALLY, incl. a `temps_well_formed` check (every referenced `tN` declared; no `t-1`) that is itself proven to bite. Real compile + bit-exact run = ADR-0098 §3 **v17-n** (GH-Actions Apple silicon) |
| ROCm | HIP | ⬜ not built on this host ⇒ ADR-0098 §3 **v17-o** (RunPod AMD burst) |

**Fan-out policy (user direction 2026-07-10):** the mission (`feedback_mission_portable_gpu_compute_all_backends`) is ALL
backends, and it overrides this doc's earlier "(Optional / opportunistic)" wording for the fan-out. Mirror the type layer
to every backend **we can validate on this machine** (WGSL + CUDA on hardware, MSL emit-only), rather than letting the
2-backend debt widen with every Phase-B slice.

> **⚠ Cross-backend rule learned in B0-3: GLSL is the TYPE-STRICT backend.** `float + bool` compiles on DX12 (HLSL
> implicitly promotes) and is a hard compile error on Vulkan; GLSL also rejects `precise bvec3` and has no `<` on
> vectors. An IR-level type error therefore appears as a **Vulkan-only `run() == false`**, and a DX12-green result proves
> nothing about IR type correctness. Always run the Vulkan suite; fix the IR, never loosen the GLSL emitter.

## Phase B — material / shading capability ⬜ NEXT (makes the IR able to express EVERY shader)

> **Frontier bar:** Slang + MaterialX + OpenPBR 1.1 + mesh-shaders + DXR/VK_KHR_ray_tracing. Mesh-first, geometry-legacy.
> Each slice: CPU-oracle + Vulkan + DX12 (WGSL/MSL follow), validated by RENDER parity vs a reference where it renders.
> Phase B delivers the CAPABILITY (the IR can express it); the exhaustive node library + the actual render pipelines grow
> in the rendering phase. Authoring via C++ builders (no node-editor UI in D-007).

| slice | one-line contract (frontier grounding) | gate |
|---|---|---|
| **B0** | **TYPE-SYSTEM completion** — `ivec`/`uvec`/`bvec` + `bool` results + `mat2`/non-square + fixed-size **arrays** + **structs** (`Light`, `Material`, interpolant/vertex structs). Type-aware emitters. | oracle + GPU bit-exact on int/bool-vec + a struct/array round-trip |

### B0 sub-slices (session 2026-07-10 — `docs/sessions/2026-07-10-d007-b0-type-system.md`)

| slice | contract | status |
|---|---|---|
| **B0-0** | `KGraph::optimize()` never remapped the 4th operand `d` (mat4 column) → stale/OOB index after DCE+CSE renumber. Root fix + `operands_valid()` structural invariant asserted in the pass. | ✅ regression test proven to fail on the unfixed code first |
| **B0-1** | **`KType`** — one composed type `{scalar, kind, rows, cols}` replaces the `(dtype, comps)` pair (SPIR-V / Slang model). A bare component count cannot tell `vec4` from `mat2` and carries no scalar type. Full type in the CSE key. | ✅ zero-regression: all suites landed on their exact prior counts |
| **B0-2** | **`mat2` + non-square R×C** — `MatFromCols`/`MatVecMul`/`MatMatMul`/`MatTranspose` generalized; emitters keyed on the TYPE, never on comps; `input_mat`; HLSL `crd_inv2` + generic R×C outer product. | ✅ oracle + Vulkan + DX12 |
| **B0-3** | **bool-typed comparisons + `bvec`/`ivec`/`uvec`** — `bool`/`bvecN` results (GLSL/HLSL/SPIR-V semantics); `DType::U32` appended; GLSL `lessThan()` family + `vec/ivec/uvec/bvec`, HLSL `floatN/intN/uintN/boolN`; `any`/`all` take a bvec; bool stores as float 0/1 (std430 has no `bool`). | ✅ oracle + Vulkan + DX12 |
| **B0-4** | **fixed-size ARRAYS + STRUCTS** — struct registry on `KGraph` (CSR field table), **variadic operands** (four slots can't hold N fields; the ext pool is walked by DCE/CSE/renumber/emitters), `StructMake`/`ArrayMake`/`FieldGet`/`ArrayGet`, **SROA lowering** (no GPU struct decl, no std430). | ✅ oracle + Vulkan + DX12 |

**Suites after B0** (win-debug): kir **200** · kir-vulkan **33010** · kir-dx12 **30821** · kir-webgpu **30791** ·
kir-cuda **79944**. Every pre-B0 assertion still passes. All 27 `crd-kir` files tidy-clean.
**Not yet run:** the multi-config DoD (`per-slice-check.ps1 -IncludeRelease` + clang-cl + gcc) — the B0-CLOSE activity.

**B0-4 scope boundaries (stated, not silently omitted):** `array_get` takes a CONSTANT index — a dynamic index needs a
real array in memory, i.e. a **buffer-backed** access (**B3**), not a value-layer one. The GPU emitters require an
aggregate to come from a `Make` node (a `Select` of structs would need a real GLSL/HLSL struct type; they return a loud
`run()==false` rather than guess — the CPU oracle is fully general). **No std430 struct layout** appears, because SROA'd
value aggregates never reach a buffer; **buffer-backed structs are B3**, where ADR-0102's frequency-set model puts them.

**Findings that changed the substrate (detail in the session log):**
- **The tidy gate reported `clean` for files it never PARSED** (missing `-I` ⇒ TU never parsed ⇒ 0 diagnostics, and
  `"file not found"` was filtered out). `backend_vulkan.cpp` passed the DoD gate un-analysed; **178 real violations**
  across `crd-kir` were invisible. Gate repaired (unresolved include ⇒ `UNGATED`; missing path ⇒ `MISSING`; both hard
  failures; `-I` globbed from `engine/*/include`; `.cpp` driven from a PCH-stripped compile DB) and all 178 cleared.
- **GLSL is the TYPE-STRICT backend.** `float + bool` compiles on DX12 (HLSL promotes) and fails on Vulkan. An IR type
  error surfaces as a Vulkan-only `run() == false`; a DX12-green result proves nothing. Fix the IR, never the emitter.
- **`KOp::Cast` was unimplemented in the WGSL/MSL/CUDA emitters** (silent `default: return false`). Added to all three.
| **B1** | **Fragment-stage foundation** — DERIVATIVES `dFdx`/`dFdy`/`fwidth` (+coarse/fine) · interpolation qualifiers (flat/noperspective/centroid/sample) · `discard`/clip (alpha-test) · frag-depth · early-Z intent · **per-fragment/per-draw VRS output** (`SV_ShadingRate` / `gl_ShadingRateEXT`, `VK_KHR_fragment_shading_rate`) · **fragment-shader interlock / ROV** (`VK_EXT_fragment_shader_interlock` — ordered UAV access for OIT / voxelization) · **conservative-raster coverage input** (`gl_FragFullyCoveredNV` / `SV_InnerCoverage`) | quad-derivative + alpha-tested draw vs reference |

> **⚠ ORDERING CORRECTED (2026-07-10, user direction): B3 runs BEFORE B1 and B2.** B1's gate is an *alpha-tested draw*,
> and `dFdx`/`dFdy`/`fwidth`/`discard`/frag-depth are fragment-stage constructs — `docs/systems/shader-ir-corpus-and-stages.md`
> tags derivatives `[M]`: *"a genuine graphics-stage intrinsic — screen-space, not expressible in a plain compute kernel."*
> CKIR today is **compute-only**: no stage concept exists, every emitter hardcodes `layout(local_size_x = 256)` /
> `numthreads(256,1,1)`, and both GPU backends create compute pipelines. **B1 and B2 are therefore unbuildable until the
> stage & I/O model (B3) exists.** New order: **B3-core → B1 → B2 → B4…**
>
> **⚠⚠ B3-b/c/d as first written were WRONG and are withdrawn (2026-07-10, user direction).** They gated on
> `crd::shader::compile_glsl(Stage::Vertex)` — making **CKIR depend on the module that owns GLSL**, the exact inversion
> ADR-0101 exists to delete. The plan was faithfully following **ADR-0099 §6**, an Accepted decision that contradicted
> ADR-0101 and has now been **superseded by [ADR-0103](../decisions/0103-gpu-context-owns-every-gpu-program.md)**.
>
> The raster emitters now land behind the **`crd-gpu-context` program seam** (`create_program(KGraph, KEntry)`), built in
> detour **[D-008](D-008-gpu-context-convergence.md)**. `crd-shader` will not know GLSL or HLSL; each backend owns its
> language and its compiler privately. D-008 C4 also gives DX12 a raster path, so B3's original
> "renders a lit triangle on Vulkan + DX12" gate becomes reachable rather than needing to be watered down.
| **B2** | **Texture & sampler system** — separable texture+sampler IR types (combined-sampler lowering for GLSL) · full corpus: `sample`/`sampleLod`/`sampleBias`/`sampleGrad`/`sampleCmp`(shadow)/`texelFetch`/`gather`/`gatherCmp`/`queryLod`/`textureSize` · dims 1D/2D/3D/Cube/Array/MS · bindless seam · **sampler feedback** (D3D12 `SV_FeedbackTexture*` / streaming residency — virtual-texture / Nanite-class LoD) | textured + shadow-compare sample vs reference |
| **B3** | **Stage & I/O model + resource binding** — vertex + fragment stages · entry points · I/O classification (attributes/interpolants/UBO/push-constants/storage/textures/samplers) · per-stage builtins · the **ADR-0102 frequency-set model** (0 frame / 1 pass / 2 material / 3 object) + bindless/`gl_DrawID` seam · compute-vs-material profile split | 🔨 **IN PROGRESS.** See B3 sub-slices below |

### B3 sub-slices (started 2026-07-10)

| slice | contract | status |
|---|---|---|
| **B3-a** | **IR stage model** — `KStage {Compute,Vertex,Fragment}` · `KBuiltin` · new leaves `StageIn` (location-indexed; ONE op serves a vertex attribute and a fragment interpolant, disambiguated by the entry's stage, as SPIR-V models it) · `Builtin` (type fixed by the builtin) · **`UniformBlock` = a STRUCT-typed leaf at (set, binding)** whose members are read with the existing `field_get` — reusing the B0-4 registry instead of a second aggregate, and **`set` IS ADR-0102's frequency slot** · `KEntry` (position / frag-depth / location-indexed outputs) · `dset` in the CSE key. | ✅ green (kir 254) |
| **B3-a′** | **COMPLETE the stage model** — `KStage` grows from `{Compute,Vertex,Fragment}` to the **14 SPIR-V execution models** (+ `TessControl` `TessEval` `Geometry` `Task` `Mesh` `RayGen` `Intersection` `AnyHit` `ClosestHit` `Miss` `Callable`); `stage_mask::*` sets (`kWorkgroup` = compute\|task\|mesh · `kRayIncoming` · `kRayHit`); **32 builtins** in ONE `builtin_info` table carrying type *and* legal-stage mask together, so the two can never drift; `entry_valid(g, entry, &why)`. A 3-value enum bakes a 3-stage assumption into every emitter `switch`. | ✅ **kir 400 assertions / 43 cases**; gate bites — `FragCoord` in a vertex entry is REJECTED, and the same graph is valid as a fragment entry |
| **B3-b** | ⛔ **WITHDRAWN** — gated on `crd::shader::compile_glsl`. Superseded by ADR-0103 / **D-008 C0** (the `crd-gpu-context` program seam). | ⛔ |
| **B3-c** | **GLSL VS+FS emitters** — hoist the value-expression switch out of `emit_vec_glsl` into a shared statement emitter (compute + raster must not duplicate ~60 cases), then stage prologue/epilogue: `layout(location)` in/out, `layout(set,binding,std140) uniform`, `gl_Position` / colour attachments, `gl_VertexIndex`/`gl_FragCoord`/`gl_FrontFacing`. **Gate: a real compiler, reached through `crd::gpu::IGpuContext::create_program(graph, entry)`** — never `crd-shader`. | ⬜ blocked on D-008 C0 |
| **B3-d** | **HLSL VS+FS mirror** — `SV_Position` / `SV_VertexID` / `SV_IsFrontFace`, `cbuffer` at `register(bN, spaceS)`. Gate: the same seam, DX12 backend → DXIL. | ⬜ blocked on D-008 C0 |
| **B3-e** | **The actual draw** — offscreen triangle + pixel readback through `IRasterContext` (D-008 C1), Vulkan; DX12 mirror once D-008 C4 lands. | ⬜ blocked on D-008 C1 |

**Const-fold scar caught in B3-a:** a stage leaf has NO operands, so `optimize()`'s const-fold treats all of its absent
operands as constant and folds it into a compile-time value. A vec/struct leaf is saved by the `comps() != 1` guard, so
the case that actually bites is a **scalar** one — a folded `gl_VertexIndex` means *every vertex is vertex 0*. Guarded,
and the guard is **proven to bite** (test fails when it is removed).
| **B4** | **Mesh-shading pipeline** — TASK (amplification) + MESH stages · meshlet I/O (`SetMeshOutputs`, per-vertex+per-primitive, payload) · compute-culling → meshlet dispatch · **per-primitive VRS output** (`gl_PrimitiveShadingRateEXT`/`SV_ShadingRate`) · **cluster/meshlet emission that feeds CLUSTER acceleration structures** (RTX Mega Geometry — the mesh↔RT bridge; the AS build itself is D-008 C3). Geometry/tessellation legacy-emulated or out | mesh+task path emits culled meshlets on Vulkan |
| **B5** | **PBR SURFACE output — lighting-agnostic (ADR-0102 D3)** — an **OpenPBR 1.1** / glTF surface-param struct (base metal/dielectric + coat + fuzz + thin-film emission + thin-walled; metallic-roughness; +clearcoat/sheen/transmission) · shading-model variants incl **NPR/STYLIZED (toon/cel, gooch, outline, hatching)** · domains (opaque/masked/translucent/additive) — material outputs surface, does NOT light | the surface-param struct drives a forward AND a G-buffer path; toon variant renders |
| **B6** | **Material node library (MaterialX-parity + NPR)** — SOURCE (noise perlin/simplex/**worley-cellular**/**fBm** · shapes · geometric position/normal/tangent/bitangent/UV · texture) · OPERATOR (math · logical · adjustment remap/contrast/HSV/gamma/curve · compositing over/in/out/mix/blend-modes · conditional · channel split/combine/swizzle · convolution blur/sharpen) · UV (panner/rotator/tiling/**triplanar**) · **NPR nodes** | each node bit-exact vs the MaterialX reference node |
| **B7** | **Material lowering + stage split** — graph → IR compiler: per-vertex/per-fragment CLASSIFICATION ("cheapest correct stage") · const-fold + DCE · graph→IR→backend round-trip bit-stable · variants seam (static switches → `ShaderOption`) | round-trip identity + 2-backend render parity + optimal stage-split |
| **B8** | **Renderer integration (ADR-0102) — the material RENDERS** — lower a CKIR material into the existing `MaterialTemplate`/`Effect`/cook path (params→set 2, textures, `variant_for_pass`) · the shared **lighting library** (GGX BRDF + clustered light loop + shadow/CSM sampling from set 1) inline (Forward+) or in a lighting pass (Deferred) · skinning (set-3 palette) · G-buffer (MRT) write · shadow/depth variant | a CKIR material renders a **lit, shadowed, skinned** object through the frame graph on Vulkan + DX12 (Forward+ AND a deferred G-buffer path) |
| **B9** | **Ray tracing — including the DXR-1.2 / SM-6.9 frontier** — RT stages (raygen / closest-hit / any-hit / miss / intersection / callable) + AS binding + ray payload/attributes + the **inline ray-query** path (`rayQuery`) · **SHADER EXECUTION REORDERING** (`HitObject` + `ReorderThread` / `hitObjectNV`; `VK_EXT_ray_tracing_invocation_reorder` + DXR 1.2 SER — the 20–100% path-tracing divergence win, a few IR lines) · **opacity-micromap-aware any-hit** (`VK_EXT_opacity_micromap` + `RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS`) · **ray position fetch** + ray flags · **cluster-AS geometry** (RTX Mega Geometry) · DXR + `VK_KHR_ray_tracing`; dispatch via ADR-0100 | an RT shadow/AO/reflection kernel from the IR runs on Vulkan (DX12 mirror); SER measured ≥1 (reorder is a no-op-correct hint) |
| **B10** | **NEURAL SHADING — cooperative vectors + long vectors (the 2025 frontier, and OUR autodiff moat)** — a **cooperative-vector op class** in the IR: per-invocation small-MLP eval (matmul-accumulate + activation) emitting `GL_NV_cooperative_vector` / `SPV_NV_cooperative_vector` (Vulkan) · HLSL **cooperative vectors** + SM-6.9 **long vectors** `vector<T,5..1024>` (DX12, cross-vendor AMD/Intel/NVIDIA/Qualcomm) · CUDA/Metal tensor paths. **Differentiable by construction** — CKIR *is* the autodiff graph (v15/v16), so a neural material/BRDF and its gradient come from ONE IR; **no shipping engine does this.** Consumers: neural texture compression (NTC) decode · neural BRDF / neural radiance-cache eval. | bit-exact vs a CPU MLP reference + **gradient check**, `{1..16}` deterministic (fixed-tree accumulation); cross-backend conformance |
| **B11** | **Wave/subgroup + quad ops + GPU-DRIVEN node shaders** — subgroup/wave intrinsics (`subgroupAdd`/`WaveActiveSum` · broadcast · shuffle/`__shfl` · ballot/vote · inclusive/exclusive prefix), **fixed-tree so reductions stay deterministic** (the corpus §1 tags these compute-perf) · quad ops (derivatives in compute, SM 6.6) · **work-graph NODE shaders** (node attributes → `SPV_AMDX_shader_enqueue`, the shader half of D-008 C5's GPU self-scheduling) · device-generated-command shaders | subgroup reduction bit-exact vs the fixed-tree CPU-ref; a work-graph node emits + runs on the device path |

**Also expressible after Phase B (built in the rendering/physics phases, no new IR needed):** GPU **particles** (compute
sim + material render) · **post-FX** (fullscreen material passes) · **cloth / mesh deformation / crowds / ragdolls**
(compute profile — the physics phase) · volumetrics · decals.

### Frontier shader-language coverage (2025–26 research, 2026-07-10) — the IR/emitter side

Every capability below is now assigned to a slice above; the device/dispatch half lives in **[D-008](D-008-gpu-context-convergence.md)**.

| capability | real extension(s) / status | our slice |
|---|---|---|
| Cooperative vectors (neural inference in shaders) | `VK_NV_cooperative_vector` · HLSL cooperative vectors, SM 6.9 **retail** (Agility 1.619 / DXC 1.9.2602.16); cross-vendor in progress | **B10** |
| HLSL long vectors `vector<T,5..1024>` | SM 6.9 retail — ML as vector/matrix ops in-shader | **B10** |
| Neural texture compression (NTC) | RTXNS / Intel coop-vector demo — decode via cooperative vectors | **B10** (consumer) |
| Shader Execution Reordering (SER) | `VK_EXT_ray_tracing_invocation_reorder` (was `VK_NV_…`) · DXR 1.2 SER, SM 6.9; 20–100% path-tracing | **B9** |
| Opacity micromaps (OMM) | `VK_EXT_opacity_micromap` · DXR 1.2, HW on RTX 40/50 | **B9** (shader) + D-008 C3 (build) |
| Cluster acceleration structures (RTX Mega Geometry) | `VK_NV_cluster_acceleration_structure` (driver ≥ 572.16, 2025); supersedes displaced micro-mesh | **B4** (emit) + D-008 C3 (build) |
| Variable rate shading (VRS) | `VK_KHR_fragment_shading_rate` — per-draw/primitive/attachment | **B1** / **B4** |
| Fragment-shader interlock / ROV | `VK_EXT_fragment_shader_interlock` — OIT, voxelization | **B1** |
| Conservative rasterization coverage | `VK_EXT_conservative_rasterization` + inner-coverage input | **B1** (shader) + D-008 C1 (state) |
| Sampler feedback | D3D12 `SV_FeedbackTexture*` — streaming / virtual texturing | **B2** |
| Subgroup / wave / quad ops | GLSL `subgroup*` · HLSL `Wave*` · CUDA warp · SM 6.6 quad | **B11** |
| Work-graph node shaders | `SPV_AMDX_shader_enqueue` (`VK_AMDX_shader_enqueue` experimental + mesh nodes) · D3D12 Work Graphs (SM 6.8 retail) | **B11** (shader) + D-008 C5 (dispatch) |

**Sources:** [VK_NV_cooperative_vector](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_NV_cooperative_vector.html) · [D3D12 Cooperative Vector](https://devblogs.microsoft.com/directx/cooperative-vector/) · [HLSL long vectors](https://microsoft.github.io/hlsl-specs/proposals/0026-hlsl-long-vector-type/) · [SM 6.9 retail](https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/) · [VK_EXT_ray_tracing_invocation_reorder (SER)](https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder) · [D3D12 SER](https://devblogs.microsoft.com/directx/shader-execution-reordering/) · [VK_EXT_opacity_micromap](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_opacity_micromap.html) · [RTX Mega Geometry / cluster AS](https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples/) · [Work Graphs mesh nodes (Vulkan)](https://gpuopen.com/learn/gpu-workgraphs-mesh-nodes-vulkan/).

## Phase D — storage / cook ⬜ (make authored shaders shippable + zero-runtime-compile)

| slice | one-line contract (frontier grounding) | gate |
|---|---|---|
| **D1** | **IR-as-crdr + reflection** — serialize/deserialize the KGraph + material-graph + stage/pipeline metadata as a versioned crdr resource (replaces stored GLSL/HLSL) · REFLECTION straight from the IR: bindings, descriptor-set layouts, vertex-attribute layout, push-constants, workgroup size, entry points, spec-constants | on-disk round-trip + reflection feeds a real pipeline create |
| **D2** | **Cook — IR → per-backend bytecode** — offline: IR → GLSL→SPIR-V (glslang) / HLSL→DXIL (DXC) / MSL / WGSL / PTX at build time · SPIRV-Cross/naga-class validation · content-hash cook cache · parallel cook | cooked bytecode runs on every backend, matches the runtime-emitted path |
| **D3** | **Variant / permutation system** — feature-toggle → static SPECIALIZATION matrix · variant key + DEDUP · ON-DEMAND compilation (only what the scene needs, UE5-style) · permutation-reduction controls · variant telemetry | a material with N static switches cooks only reachable variants; dedup measured |
| **D4** | **Runtime load + pipeline cache** — load cooked bytecode · `VkPipelineCache`/D3D12 PSO-cache (partial via ComputeDevice) · spec-const binding · ASYNC warmup · zero runtime-compile in ship | ship build does zero shader compile at runtime |
| **D5** | **Hot-reload** — IR/graph/text edit → recook affected variants → ATOMIC pipeline swap (extends Phase 2.3 v1) | live edit → recook → swap, no glitch |

## Phase C — front-ends ⬜ DEFERRED to the editor phase (step 7 of the roadmap)

Per the decided sequence, the AUTHORING front-ends are built when the editor exists — NOT in D-007. Captured here so the
IR is designed for them; **only the C1 design invariant is locked now** (cheap insurance that the IR is provably editable).

| slice | one-line contract | when |
|---|---|---|
| **C1** | **Node schema + IR↔graph round-trip (design invariant — LOCK NOW)** — every KOp/builder/stage-construct ⇒ a nodedef (typed ports/defaults/categories); graph→IR + IR→graph is loss-less; subgraph encapsulation | validate the round-trip design during D-007 (a decompile test); full editor use = editor phase |
| **C2** | **Node editor UI** (crd-ui/imgui) — canvas, typed ports/wires, palette/search, groups/subgraphs, inspector, undo/redo, live preview | editor phase |
| **C3** | **Text shader language (Slang-inspired DSL)** — lexer/parser/typecheck → IR; functions/structs/arrays/generics/stage-attributes/modules | editor phase (C++ builders suffice until then) |
| **C4** | **Shared semantic layer + tooling** — typecheck/diagnostics/stdlib/LSP for BOTH graph + text; the **text ≡ graph ≡ IR** invariant | editor phase |
| **C5** | **Live preview + hot-reload + variant authoring** in the editor | editor phase |

## Frontier grounding (what this plan is measured against)

- **Slang** (Khronos) — HLSL-superset + generics/interfaces/modules/**autodiff**/multi-target IR. North star for C + the IR-to-many-targets model.
- **MaterialX** (ASWF) — node interchange + ShaderGen (GLSL/OSL/MDL/MSL/Slang) + node taxonomy incl **NPR**. Grounds B6 + C1.
- **OpenPBR 1.1** (ASWF, Aug 2025) — the **slab layered surface** model (base/coat/fuzz/thin-film/thin-walled). Grounds B5.
- **Mesh/Task shaders** (DX12 2019 / Vulkan `VK_EXT_mesh_shader` 2022) — meshlets replace IA/VS/GS/tess. Grounds B4.
- **DXR / `VK_KHR_ray_tracing`** — RT pipeline + inline ray-query. Grounds B9.
- **SPIRV-Cross / naga / Tint** — SPIR-V↔GLSL/MSL/HLSL/WGSL + reflection. Grounds D1/D2.
- **Unreal/Unity permutations** — static switches → variants + on-demand compile (~60% cut). Grounds D3.

## Exit criteria (D-007 = A + B + D; front-ends deferred)

1. **Phase A (CKIR core): DONE** — full corpus + control flow, bit-exact on CPU oracle + Vulkan + DX12.
2. **Phase B (material/shading capability)** — B0..B9: type completion + fragment foundation + textures + stages + mesh +
   lighting-agnostic OpenPBR **+ NPR/toon** surface + MaterialX-parity nodes + stage-split + **renderer integration**
   (a lit/shadowed/skinned material renders through the frame graph, Forward+ AND deferred) + **ray-tracing** stages.
3. **Phase D (cook)** — D1 (IR-as-crdr + reflection) + D2 (cook) minimum to ship; D3 (variants) + D4 (cache) + D5 (hot-reload)
   as the rendering path demands them.
4. **Phase C (front-ends): DEFERRED** — only the C1 round-trip design invariant is validated in D-007; the node editor +
   text DSL are built at editor time (roadmap step 7).
5. **Backend fan-out is NOT optional** (superseded 2026-07-10 by the standing mission — all backends). Mirror the type
   layer + corpus to every backend validatable on the dev host (WGSL + CUDA on hardware; MSL emit-only). Metal/HIP real
   runs are ADR-0098 §3 v17-n / v17-o. (Still opportunistic: mat4-inverse on DX12.)
6. Every touched module's suite green (kir / kir-vulkan / kir-dx12 + new material/cook modules); per-slice DoD.

**On exit:** the IR is the single source of truth for every shader + kernel, able to express ML/AI, FFT, simulations,
skinning, particles, lighting, PBR + stylized/NPR rendering, ray tracing, and effects — portably across all backends. **The
main roadmap resumes at Phase 3.1.6 v17 hesap-gpu compute**, then physics → rendering → UI → editor → node editor.
