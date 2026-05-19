# Session 2026-05-18 — geometry-v9e-b GLSL emitter + ULP-conformance GPU dispatch

> **Retroactive log written 2026-05-19** — slice shipped 2026-05-18 but
> its session log wasn't authored at the time; this fills the gap.

## Summary

Shipped Phase 3.1.7 v9e-b: GLSL emitter that walks a `FormulaIr` (from
v9e-a) and emits a complete GLSL compute shader. Verified end-to-end on
GPU via `compile_glsl` (shaderc) → SPIR-V → Vulkan dispatch against the
C++ ground-truth evaluator for all 21 golden manifests within mixed
ULP+absolute tolerance.

## What landed

### Public API (`engine/geometry-shader-helpers/include/crd/geometry/shader_helpers/glsl_emitter.hpp`)

```cpp
[[nodiscard]] crd::containers::StringView glsl_helpers_prelude() noexcept;

[[nodiscard]] crd::containers::String emit_glsl_sdf_function(
    const FormulaIr& ir,
    crd::containers::StringView function_name,
    crd::memory::IAllocator* alloc) noexcept;

[[nodiscard]] crd::containers::String emit_glsl_conformance_shader(
    const FormulaIr& ir,
    crd::memory::IAllocator* alloc) noexcept;

struct alignas(16) GlslConformancePushConstants {
    float    grid_origin[3];
    float    pad0;
    float    grid_step[3];
    crd::u32 grid_resolution;
};
```

### Emission style: SSA, not nested-expression

Each IR node becomes a `float n_<i>` local. Position-domain operators
(`domain_repeat` / `_mirror` / `_elongate` / `_twist` / `_bend`)
introduce a `vec3 p_<i>` warp local for their child.

**Why SSA**: position-domain operators warp the query point before
evaluating their child. A nested-expression form like
`sd_sphere(domain_twist(p, 2.0), 0.1)` collapses when warps compose:
chains of inlined warps become unreadable, and impossible at depth.
SSA emission with one `p_<i>` per warp node is uniform regardless of
depth.

### Fixed-text helpers prelude

`glsl_helpers_prelude()` returns a constant string containing every
`sd_*` primitive + every smin/smax/op/domain operator from
`signed_distance.hpp` + `formulary.hpp`. Mirrors Inigo Quilez's
GLSL helpers verbatim modulo the determinism fixes below.

### Deterministic Cephes-poly port

The 3 transcendental-using operators (`smin_exp`, `domain_twist`,
`domain_bend`) initially used GPU-native `exp` / `sin` / `cos`. The
corresponding C++ code uses `crd::math::deterministic::{exp2, sin, cos}`
(Cephes polynomial ports, ~3 ULP across compilers). GPU-vs-CPU
divergence on these ops exceeded 1 ULP.

**Fix**: ported the Cephes polynomials into the GLSL prelude as
`crd_det_sin / crd_det_cos / crd_det_exp / crd_det_exp2 / crd_det_log /
crd_det_log2`. The transcendental-using operators now use these — same
polynomial approximations as the CPU side, so the GPU output matches
the C++ ground truth bit-for-bit on the polynomial range and within
1 ULP near boundaries.

### Critical fixes (catalogued because they ate hours)

- **`crd_sign_bit` must return mask (0 or 0x80000000)**, not 0/1. The
  wrong version put `domain_bend` at ~4.6 M ULP error; correct version
  is 14 ULP. Bug surfaced because GLSL `sign()` returns -1/0/+1 and
  the C++ code uses bit-mask semantics.
- **`smin_poly` uses `kk = k * 4.0` internally**, matching C++.
  Initially I used `k` directly; gave 747 448 ULP error → 2 ULP after
  the fix.
- **`smin_cubic` uses `kk = k * 6.0`**, same fix class.
- **GLSL `max3` collides with extension builtin** — renamed to
  `crd_max3`.
- **GLSL `flat` is reserved** as a variable name — renamed to
  `flat_idx`.

### Tests (`tests/geometry-shader-helpers/test_glsl_emit.cpp`)

`emit_glsl_conformance_shader` produces a complete compute shader for
each of the 21 golden manifests. Per manifest:

1. Compile via `crd::shader::compile_glsl` (shaderc dynamic-load) →
   SPIR-V.
2. Bind on Vulkan via `crd-rhi-compute` (Phase 3.1.7.6 substrate).
3. Dispatch over a 32³ sample grid bound origin `(-1.5, -1.5, -1.5)`
   step `(0.09375, 0.09375, 0.09375)`.
4. Read back the storage buffer → C++.
5. For every sample, evaluate `evaluate<float>(ir, p)` and
   `ulp_compare` against the GPU result with mixed ULP+abs tolerance
   (1 ULP OR 1e-6 absolute — the abs fallback covers
   catastrophic-cancellation cases near zero crossings).

Total: 21 manifests × 32³ samples = 688 128 per-pixel comparisons,
all passing.

## Files added

- `engine/geometry-shader-helpers/include/crd/geometry/shader_helpers/glsl_emitter.hpp` (~83 LOC)
- `engine/geometry-shader-helpers/src/glsl_emitter.cpp` (~667 LOC, incl. the helpers prelude string)
- `tests/geometry-shader-helpers/test_glsl_emit.cpp` (~424 LOC)
