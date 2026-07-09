# D-007 — CKIR becomes the universal GPU-program IR (the single source of truth for every shader + kernel)

**Status:** ACTIVE (opened 2026-07-09). **Phase A (CKIR core) COMPLETE.** Re-scoped 2026-07-09 (session close): D-007 =
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
complete for the math scope. **Type-layer gaps → B0** (not silent debt): float-only emitter `vtype`/`htype` (no `ivec`/
`uvec`/`bvec`, comparisons → float 0/1); no `mat2` (comps4=vec4); no struct/array aggregates.

## Fan-out — every backend gets the full corpus

| backend | lang | status |
|---|---|---|
| Vulkan | GLSL | ✅ full corpus (scalar + vec/mat/quat + control flow) |
| DX12 | HLSL | ✅ full corpus (mat4-inverse the one deferred cofactor helper) |
| WebGPU / Metal / CUDA | WGSL / MSL / PTX | ⬜ mirror the comps-aware emitter (compute-only for CUDA — no material stages) |

## Phase B — material / shading capability ⬜ NEXT (makes the IR able to express EVERY shader)

> **Frontier bar:** Slang + MaterialX + OpenPBR 1.1 + mesh-shaders + DXR/VK_KHR_ray_tracing. Mesh-first, geometry-legacy.
> Each slice: CPU-oracle + Vulkan + DX12 (WGSL/MSL follow), validated by RENDER parity vs a reference where it renders.
> Phase B delivers the CAPABILITY (the IR can express it); the exhaustive node library + the actual render pipelines grow
> in the rendering phase. Authoring via C++ builders (no node-editor UI in D-007).

| slice | one-line contract (frontier grounding) | gate |
|---|---|---|
| **B0** | **TYPE-SYSTEM completion** — `ivec`/`uvec`/`bvec` + `bool` results + `mat2`/non-square + fixed-size **arrays** + **structs** (`Light`, `Material`, interpolant/vertex structs). Type-aware emitters. | oracle + GPU bit-exact on int/bool-vec + a struct/array round-trip |
| **B1** | **Fragment-stage foundation** — DERIVATIVES `dFdx`/`dFdy`/`fwidth` (+coarse/fine) · interpolation qualifiers (flat/noperspective/centroid/sample) · `discard`/clip (alpha-test) · frag-depth · early-Z intent | quad-derivative + alpha-tested draw vs reference |
| **B2** | **Texture & sampler system** — separable texture+sampler IR types (combined-sampler lowering for GLSL) · full corpus: `sample`/`sampleLod`/`sampleBias`/`sampleGrad`/`sampleCmp`(shadow)/`texelFetch`/`gather`/`gatherCmp`/`queryLod`/`textureSize` · dims 1D/2D/3D/Cube/Array/MS · bindless seam | textured + shadow-compare sample vs reference |
| **B3** | **Stage & I/O model + resource binding** — vertex + fragment stages · entry points · I/O classification (attributes/interpolants/UBO/push-constants/storage/textures/samplers) · per-stage builtins · the **ADR-0102 frequency-set model** (0 frame / 1 pass / 2 material / 3 object) + bindless/`gl_DrawID` seam · compute-vs-material profile split | a VS+FS pair from the IR renders a lit triangle on Vulkan + DX12 |
| **B4** | **Mesh-shading pipeline** — TASK (amplification) + MESH stages · meshlet I/O (`SetMeshOutputs`, per-vertex+per-primitive, payload) · compute-culling → meshlet dispatch. Geometry/tessellation legacy-emulated or out | mesh+task path emits culled meshlets on Vulkan |
| **B5** | **PBR SURFACE output — lighting-agnostic (ADR-0102 D3)** — an **OpenPBR 1.1** / glTF surface-param struct (base metal/dielectric + coat + fuzz + thin-film emission + thin-walled; metallic-roughness; +clearcoat/sheen/transmission) · shading-model variants incl **NPR/STYLIZED (toon/cel, gooch, outline, hatching)** · domains (opaque/masked/translucent/additive) — material outputs surface, does NOT light | the surface-param struct drives a forward AND a G-buffer path; toon variant renders |
| **B6** | **Material node library (MaterialX-parity + NPR)** — SOURCE (noise perlin/simplex/**worley-cellular**/**fBm** · shapes · geometric position/normal/tangent/bitangent/UV · texture) · OPERATOR (math · logical · adjustment remap/contrast/HSV/gamma/curve · compositing over/in/out/mix/blend-modes · conditional · channel split/combine/swizzle · convolution blur/sharpen) · UV (panner/rotator/tiling/**triplanar**) · **NPR nodes** | each node bit-exact vs the MaterialX reference node |
| **B7** | **Material lowering + stage split** — graph → IR compiler: per-vertex/per-fragment CLASSIFICATION ("cheapest correct stage") · const-fold + DCE · graph→IR→backend round-trip bit-stable · variants seam (static switches → `ShaderOption`) | round-trip identity + 2-backend render parity + optimal stage-split |
| **B8** | **Renderer integration (ADR-0102) — the material RENDERS** — lower a CKIR material into the existing `MaterialTemplate`/`Effect`/cook path (params→set 2, textures, `variant_for_pass`) · the shared **lighting library** (GGX BRDF + clustered light loop + shadow/CSM sampling from set 1) inline (Forward+) or in a lighting pass (Deferred) · skinning (set-3 palette) · G-buffer (MRT) write · shadow/depth variant | a CKIR material renders a **lit, shadowed, skinned** object through the frame graph on Vulkan + DX12 (Forward+ AND a deferred G-buffer path) |
| **B9** | **Ray tracing (when needed)** — RT stages (raygen / closest-hit / any-hit / miss / intersection / callable) + acceleration-structure binding + ray payload/attributes + the **inline ray-query** path (`rayQuery`) · DXR + `VK_KHR_ray_tracing`; dispatch via ADR-0100 | an RT shadow / AO / reflection kernel from the IR runs on Vulkan (DX12 mirror) |

**Also expressible after Phase B (built in the rendering/physics phases, no new IR needed):** GPU **particles** (compute
sim + material render) · **post-FX** (fullscreen material passes) · **cloth / mesh deformation / crowds / ragdolls**
(compute profile — the physics phase) · volumetrics · decals.

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
5. (Optional / opportunistic) mat4-inverse on DX12 · WGSL/MSL/CUDA fan-out of the whole corpus.
6. Every touched module's suite green (kir / kir-vulkan / kir-dx12 + new material/cook modules); per-slice DoD.

**On exit:** the IR is the single source of truth for every shader + kernel, able to express ML/AI, FFT, simulations,
skinning, particles, lighting, PBR + stylized/NPR rendering, ray tracing, and effects — portably across all backends. **The
main roadmap resumes at Phase 3.1.6 v17 hesap-gpu compute**, then physics → rendering → UI → editor → node editor.
