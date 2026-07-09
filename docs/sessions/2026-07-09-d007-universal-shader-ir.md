# 2026-07-09 — D-007: CKIR becomes the universal shader IR (ADR-0101) + full vec/mat/quat corpus on Vulkan + DX12

> Detour doc: **`docs/detours/D-007-ckir-universal-shader-ir.md`**. North-star ADR:
> **`docs/decisions/0101-ir-is-source-of-truth-for-all-shaders.md`**. Corpus reference:
> **`docs/systems/shader-ir-corpus-and-stages.md`**. This log = the whole arc from the multi-kernel
> scheduler through the complete backend-agnostic shader-math corpus. All work uncommitted.

## The arc

1. **Multi-kernel scheduler (v17-e).** `KirBackendVulkan::run_graph` executes an arbitrary multi-op graph as a SEQUENCE
   of GPU kernels in ONE command buffer with on-GPU intermediate buffers (reachability → materialize graph-inputs +
   non-fusable ops + their operands → ascending-id topo order → per-node mini-graph via `KGraph::clone` reusing every
   emitter → one submit). Proven: `ScanSum(Mul(x,y))` bit-exact, Mul stays on-GPU.
2. **Full radix sort on GPU** — the scan-based stable split (16 passes) authored as ONE CKIR graph (~272 nodes → ~112
   kernels) runs through `run_graph`, bit-exact vs `std::sort`. Needed reduce/broadcast/iota/scatter scheduler branches +
   `emit_broadcast_glsl`/`emit_iota_glsl`; F32-exact-integer key trick keeps scan/reduce/scatter on the float emitters.
3. **ADR-0101 — the IR is the single source of truth for every shader.** Deep-look research at Slang / MaterialX /
   Unreal / Unity → the layered architecture (author → IR core+profiles → codegen → cook → runtime). GLSL/HLSL/etc. are
   OUTPUTS only. Registered + the corpus/stage reference doc written (mesh-first, geometry-legacy).
4. **Phase A — the full math/value corpus in the IR + CPU oracle:**
   - scalar intrinsics (transcendentals + smoothstep/radians/degrees/mod/fma/cbrt) + comparisons (gt/ge/ne) + bit ops
     (not/count/lsb/msb/extract/insert/reverse/reinterpret/ldexp) + modf.
   - vector VALUES: `KNode.comps` (1=scalar, 2/3/4=vecN, 9/16=matN; interleaved component storage); vec2/3/4 + `VecConcat`
     (→vec4) + arbitrary `Swizzle` (.yzx/.wzyx) + dot/cross/normalize/length/distance/reflect/refract/faceforward +
     any/all + splat + componentwise.
   - matrices: `MatFromCols` (mat3/mat4, GPU-constructible) + mat·vec/mat·mat/transpose/determinant/inverse/outerProduct.
   - interpolation + quaternions (quat=vec4): slerp/nlerp + quat mul/conj/rotate/axis-angle/to-mat3. All bit-exact
     (algebraic) / consistency-validated (quats: rotate≡mat·v, q·conj≡identity, slerp(q,q)≡q) on the CPU oracle.
5. **GPU vec/mat/quat EMITTERS — both backends.**
   - **Vulkan** (`emit_vec_glsl`): comps-aware vecN/matN temps, interleaved `in[gid*comps+k]` I/O, GLSL builtins +
     emitted quat/slerp helpers; `graph_uses_vec` routing reuses `dispatch_glsl` with numel*comps byte counts. The 4th
     KNode operand `d` (mat4-from-cols) threaded through all reachability + optimize hash/CSE (+ `comps` in the CSE key).
   - **DX12** (`emit_vec_hlsl`): mirror; HLSL builtins (rsqrt/frac/lerp), the ROW-major matrix convention
     (`transpose(floatNxN(cols))` construct + `mul(M,v)` + `M[row][col]` output extract) + emitted `crd_inv3`/`crd_outer`/
     `crd_qmat` helpers (no HLSL builtins). mat4-inverse deferred (huge cofactor formula).

## Results

- The ENTIRE vec/mat/quat/interp corpus (GLM-equivalent) now RUNS on **Vulkan AND DX12**, authored once from the IR —
  the frontier "author once → runs everywhere" bar, hit. Backend-agnostic like scalars/morton/radix.
- Suites green throughout, zero regression: **kir 126 · kir-vulkan 32979 · kir-dx12 30800.** Each op bit-exact
  (algebraic) or ULP-bounded (transcendental) vs the CPU oracle / a float reference. HLSL mat convention verified by the
  `M·M⁻¹·v ≈ v` GPU test (a row/column-major bug would break it).

## Scars / findings

- **mat construction via VecConcat breaks on GPU** — `mat3=concat(concat(c0,c1),c2)` makes a `comps=6` intermediate; no
  `vec6` type. Fix: the native `MatFromCols` op (`mat3(c0,c1,c2)` directly). vec4=concat(vec3,float) is fine.
- **HLSL is row-major, GLSL/our-storage column-major** — the settled convention (transpose-construct + mul + element-
  extract) is the only safe way; verified by test, never by eyeballing.
- **Inner-loop index vars shadow the outer node-loop `i`** (C4456 warning-as-error) twice — rename to `s`/`ri`/`cj`.
- **CSE must key on `comps` (and `d`)** — else two nodes differing only in vector width would merge (latent bug, fixed).

## A4 structured control flow — COMPLETE (same session)

- **Tier 1 — fixed-count loops (`unroll_for`):** compile-time unroll ⇒ pure dataflow ⇒ all backends for free, bit-exact.
- **Tier 2 — DYNAMIC control flow (gold standard, all backends):** `for_loop(count, init, body_fn)` builds body-scoped
  `LoopIndex`/`LoopAcc` leaves + a `For(count,init,body)` node. **Body-scoping** — loop-varying nodes (LoopIndex/LoopAcc +
  consumers; For is a barrier) are evaluated/emitted PER-ITERATION, not top-level:
  - **Oracle:** refactored the eval's per-node if-else into a reusable `eval_node` lambda (validated by the whole suite
    staying green — the safe way to touch the oracle) + `eval_for_loop` that simulates GPU divergence via max-iteration +
    per-element **masked** update (`it < count[e]`) ⇒ bit-exact vs the native GPU loop.
  - **Emitters:** `emit_vec_glsl`/`emit_vec_hlsl` refactored into an `emit_expr` lambda + a For block emitting a **native
    per-thread `for(int li=0; li<int(count); li++){ …body… }`** with the body cone inline (LoopIndex→`float(li)`,
    LoopAcc→the acc temp); `graph_uses_vec` routes For graphs to the vec emitter.
  - `while_loop` = For + per-step `Select` (bounded / GPU-safe: freeze-on-condition). `switch_case` / `if`-branch =
    `Select` multiplex (branchless — the shader-correct form).
  - PROVEN: divergent per-element index-using `for` on CPU oracle + Vulkan + DX12, bit-exact (`[controlflow]`).
- **Suites:** kir 129 · kir-vulkan 32983 · kir-dx12 30804, zero regression. **Phase A of D-007 is COMPLETE.**

## Architecture decisions (same session, deep-researched + locked)

- **B/C/D refined to frontier grade** (research: Slang · MaterialX · OpenPBR 1.1 · mesh/task shaders · DXR/`VK_KHR_ray_tracing`
  · SPIRV-Cross/naga/Tint · Unreal permutations). Phase A audited (op set matches; type-layer gaps → B0).
- **ADR-0102 — render-data / lighting / pass architecture (NEW, Accepted).** Audit: the engine ALREADY has a mature
  frontier renderer (frame graph · `IRenderPath` Forward/Forward+/Deferred/VisBuffer planned · `PerFrameUbo` · `Material
  Template`+variants · cooking, ADR-0048) — CKIR replaces the hand-written GLSL SOURCE only (unification, not greenfield).
  Decisions: globals live in the renderer's **per-frame set 0, NOT the GPU context** (upholds ADR-0099); **frequency-based
  descriptor sets** (0 frame / 1 pass-lighting / 2 material / 3 object; bindless/GPU-driven ready); **material = surface
  response (OpenPBR params), lighting-agnostic; render path = lighting technique** → one material works Forward+ OR Deferred
  (hybrid: deferred opaque + forward transparent); multi-pass (shadows/CSM/G-buffer) = the frame graph + `variant_for_pass`;
  skinning = set-3 palette; uber-shaders = existing `ShaderOption` variants.

## D-007 RE-SCOPED (session close 2026-07-09) + the roadmap

D-007 = the **IR substrate** = Phase A (✅) + **Phase B** (material/shading capability, B0..B9 incl **ray tracing** + **NPR/
toon**) + **Phase D** (cook). The **front-ends (node editor UI + text DSL = Phase C) are DEFERRED to the editor phase**
(user: "we are not going to touch front end of the node editor"); authored via the C++ builders. Only the **C1 node-schema /
IR↔graph round-trip design invariant** is locked now (cheap insurance the IR is provably node-editable later). On exit the IR
can EXPRESS everything: ML/AI · FFT · simulations · skinning · particles · lighting · PBR + stylized/NPR rendering beyond
Unreal · ray tracing · effects — portably across all backends.

**The roadmap (captured so nothing is lost):** D-007 → **hesap** (resume numerical/GPU-compute) → **eylem/physics** (crush
PhysX/Jolt + **GPU-compute cloth / mesh deformation / crowds / ragdolls** on CKIR) → **rendering** (Forward+/Deferred/RT/
particles/post-FX/NPR pipelines on the Phase-B material profile) → **UI system** on the shader system → **first editor** →
**node editor + text shader language**. **NEXT concrete step: Phase B, entry B0 (type-system completion).**
