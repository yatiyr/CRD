# Shader IR — the intrinsic corpus + the stage model (the build reference for ADR-0101)

Living reference for the CKIR universal shader IR (ADR-0101). Every intrinsic + stage we intend to support, grounded in
the shader-language standards (GLSL/HLSL/MSL/SPIR-V), **CUDA** (compute), and **Unreal Engine** (materials). Tagged by
**profile** — `[C]` core (compute + material), `[K]` compute-only, `[M]` material/graphics-only — and by **status**
(✅ done · A2/A3/A4 = Phase-A step · B = Phase B material profile).

## 1. Intrinsic corpus

### Math — algebraic / common `[C]` (mostly bit-exact)
`abs`✅ `sign`✅ `floor`✅ `ceil`✅ `round`(roundEven)✅ `trunc`✅ `fract`✅ `min`✅ `max`✅ `clamp`✅ `mix`/lerp✅ `step`✅
`smoothstep`A2 `saturate`(=clamp 0..1)A2 `mod`/fmod A2 `fma`A2 `frexp`/`ldexp`/`modf`(multi-out)A3.

### Math — transcendental `[C]` (ULP on GPU until crd::math ports to emitters)
`exp`✅ `log`✅ `sqrt`✅ `pow`✅ `sin`✅ `cos`✅ `tanh`✅ · `exp2`A2 `log2`A2 `rsqrt`/inversesqrt A2 `tan`A2 `asin`A2 `acos`A2
`atan`A2 `atan2`A2 `sinh`A2 `cosh`A2 `cbrt`A2 `radians`A2 `degrees`A2. (all backed by `crd::math::*` — deterministic CPU ref.)

### Geometric `[C]` — needs vecN (A3)
`length` `distance` `dot` `cross` `normalize` `reflect` `refract` `faceforward`. → **A3**

### Vector / matrix `[C]` — needs vecN/matN (A3)
swizzle · componentwise · `transpose` `determinant` `inverse` `outerProduct` `matrixCompMult` · vec relational
(`lessThan`/`equal`/`any`/`all`/`not`). → **A3**

### Bit manipulation `[C]`
`Shl`✅ `Shr`✅ `BitAnd`✅ `BitOr`✅ `BitXor`✅ · `bitfieldExtract`/`Insert`/`Reverse` · `bitCount` `findLSB`/`findMSB` ·
`floatBitsToInt`/`Uint`/`intBitsToFloat` (asint/asuint/asfloat) · `packHalf2x16`/`unpack…`/`packUnorm…`. → later (A3/B)

### Derivatives `[M]` — fragment-primary (SM6.6 also compute/mesh, quad-based)
`dFdx`/`ddx` `dFdy`/`ddy` `fwidth` + coarse/fine (`ddx_coarse`/`ddx_fine`). → **Phase B** (material profile; a genuine
graphics-stage intrinsic — screen-space, not expressible in a plain compute kernel). [[project_central_shader_ir_and_node_editor]]

### Texture / sampling `[M]`
`sample`/texture `sampleLevel`/textureLod `sampleGrad`/textureGrad `sampleBias` `load`/texelFetch `gather`/textureGather
`sampleCmp` `getDimensions`/textureSize `textureQueryLod` · interpolation `interpolateAtCentroid`/`Sample`/`Offset`. → **B**

### Atomics `[K]` (also fragment w/ ext)
`atomicAdd`/`Min`/`Max`/`And`/`Or`/`Xor`/`Exchange`/`CompSwap`. Graph-level `ScatterAdd`✅ (histogram); in-kernel atomics
on shared/storage are the primitive form → as CKIR compute grows.

### Synchronization + subgroup/wave `[K]`
`barrier` `memoryBarrier` `groupMemoryBarrier` · subgroup/wave (GLSL `subgroup*` · HLSL `Wave*` · CUDA warp):
`subgroupAdd`/`WaveActiveSum`/`cg::reduce` · `subgroupBroadcast` · `subgroupShuffle`/`__shfl_sync` ·
`subgroupBallot`/`WaveActiveBallot`/`__ballot_sync` · `subgroupInclusive/ExclusiveAdd`/`WavePrefixSum` ·
`subgroupAll`/`Any` · CUDA `__syncthreads`/`__syncwarp`. → compute-perf work (later; the scheduler already chains kernels).

## 2. The stage model — mesh-first, geometry-as-legacy

Stages the profiles expose (each = a codegen entry + runtime pipeline):

- **Compute** `[K]` — the CKIR compute profile (✅).
- **Raster, MODERN (primary):** **Task/Amplification → Mesh → Fragment.** Compute programming model, high occupancy,
  built-in amplification + culling. DX12 (2019) · Vulkan `VK_EXT_mesh_shader` (2022). Mesh subsumes vertex+geometry(+much
  of tessellation).
- **Raster, CLASSIC (universal fallback):** **Vertex → Fragment** (always supported) + optional **Tessellation**
  (hull/control + domain/eval).
- **Geometry shaders — LEGACY / discouraged.** Industry consensus (NVIDIA/AMD/Khronos): "horrible programming model, low
  HW occupancy, limited topologies" — a single-thread amplification bottleneck. **Support the stage if a port needs it,
  but never build the pipeline around it; mesh shaders are the amplification path.**
- **Ray tracing:** raygen · intersection · any-hit · closest-hit · miss · callable (`TraceRay`/`ReportHit`).

Per-stage builtins (inputs/outputs) are part of each stage's profile: compute (`GlobalInvocationID`/`DispatchThreadID`,
shared mem), vertex (`VertexID`/`InstanceID`, `Position` out), fragment (`FragCoord`/`SV_Position`, `FrontFacing`,
`discard`), mesh (`SetMeshOutputCounts`, per-vertex/per-primitive out), RT (`WorldRayOrigin`, `HitT`, …).

## 3. How the corpus maps to the ADR-0101 phases

- **A2** (in progress) — finish the scalar Core intrinsics: transcendentals (exp2/log2/rsqrt/tan/asin/acos/atan/atan2/…),
  smoothstep, radians/degrees, mod, fma. Pure-additive, the proven unary/binary/ternary pattern.
- **A3** — value types (vecN/matN) → unlocks geometric (dot/cross/normalize/reflect) + vec/mat ops + vec relational + the
  remaining bit/pack ops.
- **A4** — structured control flow (if/for/while) — the one IR-structural change.
- **Phase B** — the material profile: derivatives, texture/sampling, interpolation, the stage model (vertex/fragment/mesh
  first; geometry legacy), the PBR surface model + shading models + material domains (MaterialX-informed).

**Sources:** [HLSL SM6.6 derivatives](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Derivatives.html) ·
[CUDA warp primitives](https://developer.nvidia.com/blog/using-cuda-warp-level-primitives/) ·
[Unreal material expressions](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-expressions-reference) ·
[Mesh shading (Khronos)](https://www.khronos.org/blog/mesh-shading-for-vulkan) ·
[Mesh vs geometry (DirectX)](https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html).
