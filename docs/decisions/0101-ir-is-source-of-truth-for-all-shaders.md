# ADR-0101 — The IR is the single source of truth for every shader (compute AND material); backend languages are outputs only

- **Status:** Accepted (2026-07-09) — user decision ("we must ensure that our IR is the source of truth… materials and all shader stuff handled just like in any other frontier engine… IR must be very consistent, control flow to any other functions").
- **Phase:** 3.1.6 v17 (GPU compute / CKIR) — architecture slice **v17-i / v17-e**, extending CKIR from a compute kernel IR to the **universal shader IR**.
- **Tags:** `kir` `shader` `ir` `materials` `node-editor` `codegen` `crdr` `architecture` `north-star`
- **Depends / amends:** ADR-0098 (CKIR — the kernel compiler + IR) · ADR-0099 (crd-gpu-context) · ADR-0100 (one GPU compute manager). This ADR **generalizes CKIR** from "compute kernel IR" to "universal GPU-program IR (compute + material profiles)".

## Context — the inconsistency to fix

Shaders are authored/stored three inconsistent ways today:
1. **CKIR compute** (GEMM, morton, radix, the scheduler): IR-first — the graph is authored, GLSL/HLSL/… are *emitted*, compiled per backend. **Correct.**
2. **Geometry kernels** (`geometry-bvh-gpu/*.comp`): hand-written **GLSL**, cooked to `.comp.spv`, loaded by name. **Backend-locked.**
3. **Rendering** (`crd-shader` Effect/Module): hand-written **GLSL/HLSL** text → SPIR-V. **Backend-locked.**

GLSL/HLSL are backend-specific *languages*; SPIR-V/DXIL are backend-specific *bytecodes*. Authoring or storing a shader as any of them is a lock-in — and it makes a node editor impossible (a node graph can't round-trip through hand-written GLSL). The fix: **one backend-neutral IR is the source of truth; every backend language/bytecode is a derived output.**

## Deep look — how frontier engines solve this

- **[Slang](https://shader-slang.org/)** (NVIDIA → Khronos): a shader **language** + compiler with a custom **IR** that targets SPIR-V, DXIL, DXBC, HLSL, GLSL, Metal, CUDA, C++, WebGPU. Modules compile offline to IR and link at runtime. **This is our exact vision, proven at frontier scale** — one source → all backends, for compute *and* graphics *and* CUDA. It is the north-star prior art.
- **[Unreal](https://dev.epicgames.com/community/learning/knowledge-base/0qGY/how-the-unreal-engine-translates-a-material-graph-to-hlsl)**: the Material graph is translated to HLSL (each node = a small HLSL snippet) then compiled per platform; the HLSL view is **read-only** (generated, never edited). Shader **permutations** are the notorious cost.
- **[Unity ShaderGraph](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.5/changelog/CHANGELOG.html)**: node graph + a **Master Stack** of output *Blocks* + *Targets* → HLSL → SRP; shader **variants/keywords** with a hard permutation limit; a **Custom Function** node is the inline-HLSL **escape hatch** — and it *disables the SRP batcher*, i.e. hand-written code pays a portability/perf tax.
- **[MaterialX](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/DeveloperGuide/ShaderGeneration.md)** (Academy Software Foundation): an open material **node-graph standard**; *ShaderGen* generates GLSL / OSL / MDL / MSL / Slang from nodegraphs; ships a **PBR surface node set** (energy-conserving BSDFs); **one generator per language, target-specialized by inheritance.**

**Lessons distilled:** (1) the **IR is the pivot** (Slang); (2) **graph → codegen, one snippet per node**, one generator per language (Unreal/Unity/MaterialX); (3) materials need a defined **surface/material model** + shading models + domains (MaterialX/Unreal), not free-form output; (4) **variants are a real cost** — minimize the axes, prefer spec-constants; (5) an **escape hatch** for hand-written backend code is necessary but must be explicitly **non-portable** (the "custom node breaks the batcher" tax).

## Decision

**CKIR — generalized to a universal GPU-program IR — is the single source of truth for every shader.** GLSL / HLSL / WGSL / MSL / CUDA / SPIR-V / DXIL / PTX / metallib are **outputs only**, produced by codegen; none is ever authored or stored as source. The system is five layers:

```
AUTHOR      node editor (visual, ↔ IR bidirectional) · our text shader language · C++ builders (interim)
              │  all produce ▼
IR (CKIR)   CORE: typed values (float/vecN/matN/int/bool/sampler/texture) + STRUCTURED CONTROL FLOW
            (if/else, for, while, break/continue) + full INTRINSIC library (math: exp/log/pow/sqrt/
            trig; common: smoothstep/clamp/mix/step/saturate/fract; geometric: dot/cross/normalize/
            reflect; derivative: dFdx/dFdy; texture: sample/sampleLod/textureGrad)
              ├ COMPUTE profile   — buffers, atomics, dispatch, workgroup shared  → IComputeContext
              └ MATERIAL profile  — surface outputs (baseColor/normal/roughness/metallic/emissive/
                opacity), varyings, samplers, derivatives; stages vertex/fragment; a PBR surface
                model + shading models + material domains (MaterialX-informed)      → RenderDevice
              │  codegen ▼ (one emitter per language, target-specialized; variant specialization)
OUTPUTS     GLSL→SPIR-V · HLSL→DXIL · WGSL · MSL→metallib · CUDA→PTX   (derived, never authored)
              │  cook / cache ▼
STORAGE     crdr: the IR GRAPH (source of truth, editable, backend-neutral) + cooked PER-BACKEND
            bytecode as 'SHDR' chunks (+ 'MATR' for material params). GLSL/source is NEVER stored.
              │  load ▼
RUNTIME     GpuContextManager → IComputeContext (compute) / RenderDevice (materials) → pipelines
```

**Core rules:**
1. **IR is authoritative.** Backend languages are emitted; backend bytecodes are cooked. Neither is a source or a stored authoring form.
2. **One core, two profiles.** Values + control flow + the intrinsic library are shared; the *compute* profile adds buffers/atomics/dispatch, the *material* profile adds the surface model + graphics intrinsics + the rasterizer stage I/O. (Per ADR-0099: separate concern, shared substrate.)
3. **Node graph ↔ IR is bidirectional** — a shader opens in the editor because the graph *is* the IR in visual form. The **text shader language** is a sibling front-end that parses to the same IR (Godot/Slang-style, backend-neutral). C++ builders are the interim front-end.
4. **Escape hatch = opaque import.** Hand-written backend code (legacy `.comp`, third-party HLSL) is supported as a single-backend, explicitly **non-portable** import — for interop, never as the authoring model.
5. **Variants are minimized.** Prefer spec-constants; a small explicit feature key drives the cook; no uncontrolled keyword explosion.

## The fix + migration

- **Compute shaders → CKIR.** This is *literally the rung work in progress*: bit ops → morton, atomics → radix, control flow (**rung 3**) → build_tree. Once CKIR expresses LBVH, the hand-written `geometry-bvh-gpu/*.comp` kernels are **retired** (authored in the IR, emitted to every backend). Rung 3 is the first concrete step of this ADR.
- **Rendering shaders → material profile.** Gated on building the material profile (surface model + graphics intrinsics + vertex/fragment I/O). Until then, rendering shaders remain hand-written GLSL/HLSL as a *transitional opaque import*, migrated once the profile exists.
- **Cooked storage → per-backend bytecode.** Retire `.comp.spv`-by-name; the cook step walks a graph and emits+compiles per backend into `crdr 'SHDR'` chunks; the editable IR graph is stored alongside.

## Phased plan

- **Phase A — harden the CKIR CORE** (serves *both* profiles): value types (vecN/matN beyond scalar/tensor), **structured control flow (= rung 3: if/for/while/break)**, the full intrinsic library. *Start here — rung 3 is step 1.*
- **Phase B — material/graphics profile:** stages + varyings, samplers/textures, derivatives, the PBR surface model + shading models + material domains.
- **Phase C — front-ends:** node editor (bidirectional IR↔graph) + our text shader language (parser → IR).
- **Phase D — storage/cook:** IR-graph-as-crdr + the cook step (emit+compile per backend → `SHDR`) + variant specialization.
- Compute (CKIR rungs → LBVH) proceeds in parallel — it *is* the same IR being hardened.

## Consequences

- **+** One source of truth; author once (node OR text) → every backend; no language/bytecode lock-in; node editor becomes possible (round-trips the IR); materials handled like a frontier engine (surface model + codegen + variants).
- **+** Compute and materials share the core (control flow, intrinsics, codegen, cook, storage) — the "separate concern, shared substrate" of ADR-0099/0100 realized at the shader layer.
- **−** Large, multi-phase build (Slang / MaterialX / UE material system are each large, long efforts). Scoped to our needs, but the material profile is a real mini-compiler + a PBR model.
- **−** Variant/permutation management is a permanent discipline (learn from Unreal/Unity: minimize axes, spec-constants).
- **−** An escape hatch stays necessary; it must remain explicitly non-portable so it never becomes the default.

**Prior art / sources:** [Slang](https://shader-slang.org/) · [Slang targets](http://shader-slang.org/slang/user-guide/targets) · [Unreal: material graph → HLSL](https://dev.epicgames.com/community/learning/knowledge-base/0qGY/how-the-unreal-engine-translates-a-material-graph-to-hlsl) · [MaterialX ShaderGen](https://github.com/AcademySoftwareFoundation/MaterialX/blob/main/documents/DeveloperGuide/ShaderGeneration.md) · [Unity ShaderGraph](https://docs.unity3d.com/Packages/com.unity.shadergraph@17.5/changelog/CHANGELOG.html).
