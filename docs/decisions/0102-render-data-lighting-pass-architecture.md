# ADR-0102 — Render-data, lighting & pass architecture: how the shader IR feeds a frontier renderer

- **Status:** Accepted (2026-07-09) — user decision ("we need full awesome and gold standard graphics… how we are going to use global data like the camera, time… the gpu context might hold it and we must decide it now… uber shaders… look at our current shader and material system… WE NEED TO THINK ALL OF THEM AT ONCE AND MAKE A GOLD STANDARD ARCHITECTURE").
- **Phase:** 3.1.6 v17 detour **D-007** (universal shader IR) — this ADR decides the render-data/lighting/pass contract that **Phase B (material profile)** is built against.
- **Tags:** `renderer` `shader` `ir` `materials` `lighting` `deferred` `forward-plus` `frame-graph` `bindless` `architecture` `north-star`
- **Depends / amends:** ADR-0099 (crd-gpu-context = device only) · ADR-0100 (one GPU compute manager) · ADR-0101 (IR is the source of truth) · ADR-0048 (the material system: MaterialTemplate/Instance, cooking, variants). This ADR does **not** change those — it **connects** the CKIR shader IR to the existing renderer and decides the data model.

## Context — the question, and what already exists

Shaders take camera/skinning matrices, many lights (esp. Forward+), and feed each other across passes (shadow maps, CSM, G-buffer). We must decide **now**, holistically: where global data lives, deferred vs Forward+ (or both), multi-pass, uber-shaders — and how CKIR materials (ADR-0101) plug into all of it.

**Audit — the engine already has a mature, frontier-shaped renderer (do NOT rebuild it):**
- `crd-gpu-context` (ADR-0099): a live **device** — instance/device/queues/allocator, **no** frame/camera/light semantics. Compute + render are separate concerns *on top* of it.
- `crd-renderer`: a **frame graph** (`FrameGraph`/`PassBuilder`: transient resources, read/write declarations, auto barriers, execute callbacks) · `IRenderPath` with **Forward / Forward+ (clustered) / Deferred / Visibility-Buffer explicitly planned** · `PerFrameUbo` (view/proj/inv/camera/time — **set 0, binding 0**) · `PerDrawPush` (model matrix, push constant) · `MaterialDomain` (Surface/PostProcess/Compute/Decal/UI) · `PassType` (DepthPrepass/Shadow/Forward) · `RasterState`.
- `crd-renderer` material runtime (ADR-0048): `MaterialTemplate`/`MaterialInstance` — typed params cooked to a UBO, **per-pass** vert+frag pairs, PSO state, and **`ShaderOption` variants** (`variant_for_pass`) — i.e. **uber-shaders/permutations already exist**.
- `crd-shader` (`Effect`/`Module`): the shader payload with **reflection** (descriptor bindings, params, push constants, vertex attributes, spec constants). **The shaders themselves are hand-written GLSL → SPIR-V — exactly what CKIR replaces.**

So this is a **unification**, not a greenfield. CKIR becomes the shader **source**; everything else is reused.

## Deep look — frontier consensus (research 2026-07-09)

- **Lighting paths are converging on a HYBRID.** Deferred/clustered gives 2–8× with many lights but stores 20+ B/pixel, has no native transparency, and fights MSAA; Forward+ (clustered forward) uses cheap registers, handles transparency + MSAA. The frontier norm (Unity HDRP, Eidos "Deferred+", Angelo's Hybrid engine) is **deferred/visibility-buffer for opaque + forward for transparent**. A **visibility buffer** (store triangle-ID only, resolve material later) is the GPU-driven end-state.
- **The material must be lighting-agnostic** to work in any path — it outputs **surface parameters**, the path decides lighting. (Unreal/Frostbite model.)
- **Bindless + frequency-based descriptor sets + GPU-driven** is the direction: a global descriptor table bound once and *indexed*; per-frame set bound once; per-draw data indexed via push-constant / `gl_DrawID`; the scene in big buffers for indirect draws.

## Decision

**D1 — Layering (confirm ADR-0099): globals do NOT live in the GPU context.** `crd-gpu-context` = device. `crd-renderer` owns **all frame data** (the `PerFrameUbo`: camera/view/proj/time/global constants), the light buffers, the frame graph, and the render paths. CKIR + the material profile own the **shader source** (replacing hand-written GLSL), lowered into the stage modules the existing cook/reflection/binding infra already consumes. **Camera, time, and global constants live in the renderer's per-frame descriptor set — bound once per frame — never in the GPU context.**

**D2 — Frequency-based descriptor-set binding model (the contract every CKIR shader + the renderer agree on):**

| set | frequency | contents | owner |
|---|---|---|---|
| **0** | per-FRAME/view | camera (view/proj/inv/…), time, viewport, global constants, environment (IBL cubemaps, sky) | renderer `FrameContext` (= `PerFrameUbo` today) |
| **1** | per-PASS/lighting | light list (structured buffer), clustered/tiled light grid (Forward+), shadow maps + CSM cascade matrices (texture array), G-buffer inputs (deferred lighting pass), SSAO/probes | the active `IRenderPath` |
| **2** | per-MATERIAL | material param UBO + material textures/samplers | `MaterialTemplate` (ADR-0048) |
| **3** | per-OBJECT/draw | model matrix (push constant), skinning-palette offset + skinning matrix buffer, instance/transform data | per-draw; bindless/`gl_DrawID`-indexed for GPU-driven |

Bindless-ready: textures/buffers may resolve through a global bindless table indexed from set 3 / push constants (the GPU-driven end-state). The model matches the existing set 0 + push constant and is the AAA standard.

**D3 — Material = SURFACE RESPONSE (lighting-agnostic); render path = LIGHTING technique. (The pivotal decision.)** A CKIR material graph outputs **OpenPBR/glTF surface parameters** (base color, normal, metallic, roughness, AO, emissive, opacity, + clearcoat/sheen/transmission). It does **not** compute lighting. The render path applies it:
- **Forward+ (clustered forward, DEFAULT):** after the material yields surface params, the fragment shader loops the set-1 clustered lights + samples shadows → shades via the shared BRDF. One geometry pass; handles transparency + MSAA.
- **Deferred (opaque, for heavy-light scenes):** the material writes surface params to the **G-buffer** (MRT); a full-screen lighting pass reads G-buffer + lights + shadows → shades.
- **We support BOTH as a HYBRID** (deferred/vis-buffer opaque + forward transparent). Visibility-buffer is the future GPU-driven path. **The same material works in any path** because it only emits surface params. A shared CKIR **lighting library** (BRDF + light loop + shadow sampling) is called inline (forward) or in the lighting pass (deferred).

**D4 — Multi-pass = the FRAME GRAPH (exists).** Shadow maps, CSM, G-buffer, SSAO, depth-prepass are frame-graph passes (transient resources + barriers + ordering). A material contributes per pass via `variant_for_pass`: the **Shadow/DepthPrepass** pass uses the material's **vertex** graph (+ alpha-test fragment for masked) → depth only; the **Forward/GBuffer** pass uses the full material. **CSM** = N cascade passes → a shadow-map array + cascade-select matrices in set 1.

**D5 — Skinning + per-object data.** Skinning palette = a structured buffer (set 3) indexed by bone indices + weights (vertex attributes); **vertex-stage skinning** is the default, with an optional **compute pre-skinning** pass (frame graph) for heavy cases. Large scenes use structured buffers + bindless/instance-index + **draw-indirect** (GPU-driven).

**D6 — Uber-shaders / variants = the existing `ShaderOption` + variant system + Phase-D cook.** One CKIR graph + static switches → `ShaderOption` (OPTS chunk) → specialized variants (`variant_for_pass`), cooked **on-demand + dedup'd** (UE5-style, ADR-0101 Phase D3). CKIR static branches lower to specialization.

**D7 — D-007 scope boundary (what we design now vs build later).** D-007 **defines and validates the seam**: the binding model (D2), the surface-parameter output contract + lighting-agnostic materials (D3), the resource types (structured buffers, textures/samplers, MRT), and the material profile that lowers a CKIR graph into the existing cook/bind infra — proven by a material that **renders** through the frame graph on Vulkan + DX12. It does **not** build the full Forward+/Deferred pipelines or the light-culling compute — **those remain the post-hesap RENDERING phase**, now buildable on CKIR materials because the contract is fixed here.

## Consequences

- **One material, any path** — the surface/lighting split makes materials portable across Forward+, Deferred, and a future visibility buffer; supports the hybrid opaque-deferred + transparent-forward norm.
- **The existing renderer/material/cook stack is reused** — CKIR slots in as the shader frontend; `MaterialTemplate`/variants/frame-graph/reflection/cooking all stay. Low-risk, high-leverage.
- **Globals are unambiguous** — camera/time/constants are renderer frame data (set 0), never the GPU context (ADR-0099 upheld).
- **GPU-driven + bindless are open**, not blocked — the frequency model + draw-indirect + bindless table are the growth path.
- **Phase B now has a concrete data contract** (the sets, the surface-param struct, the resource types) to build against, and the planned `IRenderPath`s (Forward/Forward+/Deferred/VisBuffer) have a material contract to consume.
