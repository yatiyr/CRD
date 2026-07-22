# Session — 2026-07-22 · C6 cooperative-vector device enable (the B10 neural-shading device half)

**Ask:** proceed in order to C6 — enable `VK_NV_cooperative_vector`, leave no gaps, and be sure the path is real. C6 is the
DEVICE half of the B10 neural-shading moat: per-invocation matrix×vector (each pixel/thread runs a small MLP inline).

## What C6 is (vs coopmat, already shipped)

- **Cooperative *matrix*** (shipped): workgroup-cooperative M×N×K GEMM (NRC fused MLP + GEMM tier), emitted as whole-kernel
  templates because it doesn't fit CKIR's per-invocation statement tier.
- **Cooperative *vector*** (C6/B10): each **invocation** multiplies an in-memory weight matrix by its **own** activation vector —
  the inference primitive for neural shading. Because it's per-invocation, it **maps onto CKIR's statement tier**, so B10 can be a
  real CKIR op class, not just a template.

## Delivered (GPU-verified on the RTX 4070 Ti SUPER)

**Device enable** (`vulkan_context.cpp`, mirroring the coopmat2 enable, fully gated):
- detect `VK_NV_cooperative_vector` (rev 4 present) → chain `VkPhysicalDeviceCooperativeVectorFeaturesNV` into device creation,
  **feature-queried first** (`vkGetPhysicalDeviceFeatures2`) so we never request an unsupported bit; `cooperativeVectorTraining`
  enabled only when the device reports it.
- `vkGetPhysicalDeviceProperties2`(coopvec props) → supported stages + max component dim.
- interface accessors (`vulkan_context.hpp`): `cooperative_vector()`, `cooperative_vector_training()`, `coopvec_max_components()`,
  `coopvec_supported_stages()`.
- **Device reports:** coopvec=**YES**, training=**YES**, max_components=**1024**, stages=**0x3fff** (all 14, fragment included →
  per-pixel neural shading), **16** matmul type-combinations.

**The program path** (`[coopvec]` gates in `test_vulkan_context.cpp`):
- C6-a: the device comes up with the feature legally enabled; the type-combination property query returns 16 combos.
- C6-b: a real per-invocation `coopVecMatMulNV` compute kernel (`GL_NV_cooperative_vector` → `OpCooperativeVectorMatrixMulNV`,
  shaderc-compiled, RowMajor fp16 weights) DISPATCHES and matches a CPU fp16 oracle to **worst |device−oracle| = 0.00391**
  (matched fp16 accuracy — not bit-exact; tensor-core accumulation reorders the sum, the FP32-precise CKIR tier owns bit-exactness).

## Scars

- **fp16 input matmul supports ONLY {input f16, matrix f16, result f16}.** My first kernel used an fp32 result (the natural
  "fp16 weights, fp32 accumulate" mental model) → garbage (worst 6551). Enumerating `vkGetPhysicalDeviceCooperativeVectorPropertiesNV`
  showed the *only* fp16-input combo has `resultType = FLOAT16`; the rest are int8/fp8 quantized. Switching the result to fp16 fixed
  it instantly. **Always enumerate the supported combos before picking a type config.**
- **Offsets are BYTES, not elements** (the initial suspicion was wrong): load/store offset a multiple of 16, `matrixStride` a
  multiple of 16B, `matrixOffset` 64B-aligned. RowMajor tightly-packed weights multiply correctly with byte offsets — **no
  optimal-layout conversion needed for correctness** (the inferencing-optimal layout is a later perf lever, not a requirement).
- The toolchain (shaderc/glslang in VulkanSDK 1.4.341.1) compiles `GL_NV_cooperative_vector` cleanly — no offline-compile needed.

## Cross-vendor status (honest)

Vulkan is the primary + only coopvec HW here. DX12 "Cooperative Vectors" is SM6.9 / Agility-SDK preview — deferred to B10's DX12
pass (verify the installed DXC then). CUDA maps to tensor-core inference (the NRC moat already runs a fused MLP on CUDA via wmma).
Metal has no coopvec (simdgroup ops) — n/a.

## Verification

`[coopvec]` 11/2 GREEN (device enable + matvec==oracle). No device regression (`[program]` 31/5, coopmat2 path unaffected — the
enable is gated). clang-tidy (LLVM-20.1.8) on `vulkan_context.cpp`/`.hpp` + the test — clean.

## Next: B10

The CKIR coopvec op class — per-invocation MLP eval (matmul-accumulate + activation) as a config-keyed emitter → the first
neural-shading consumer (a per-pixel neural material/texture) + the performance proof (coopvec tensor-core inference vs a scalar
FMA MLP baseline — the "blazing fast" mandate).

## Proposed commit (user commits — no AI co-author trailer)

```
feat(c6): enable VK_NV_cooperative_vector — the B10 neural-shading device half

Device enable (gated, mirroring the coopmat2 path): detect the extension, chain
VkPhysicalDeviceCooperativeVectorFeaturesNV into device creation (feature-queried
first), query the coopvec properties, and expose cooperative_vector()/_training()/
coopvec_max_components()/coopvec_supported_stages() on IVulkanGpuContext.

Program path: a per-invocation coopVecMatMulNV compute kernel (GL_NV_cooperative_
vector) dispatches and matches a CPU fp16 oracle (worst 0.00391). RTX 4070 Ti SUPER
reports coopvec+training, max 1024 components, all stages, 16 matmul combos.

Scar: fp16 input matmul only supports an fp16 RESULT combo (fp32 result is garbage);
RowMajor byte-offset weights multiply correctly with no optimal-layout conversion.

[coopvec] 11/2, [program] 31/5 (no regression), tidy-clean.
```
