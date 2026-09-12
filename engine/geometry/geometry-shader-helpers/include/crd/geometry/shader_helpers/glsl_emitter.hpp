#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — GLSL backend. Phase 3.1.7 v9e-b (2026-05-18).
//
// Walks a `FormulaIr` and emits a GLSL source string containing:
//   1. The `glsl_helpers_prelude()` — fixed GLSL versions of every `sd_*`
//      primitive + every smin/smax/op/domain operator from
//      `signed_distance.hpp` / `formulary.hpp`.
//   2. A user-named `float <fn>(vec3 p)` SDF function emitted by walking the
//      IR top-down in SSA style (each IR node becomes a `float n_<i>` local
//      + position-domain ops introduce a `vec3 p_<i>` local for their child).
//
// **Why SSA-style (not nested-expression)**: position-domain operators
// (`domain_repeat` / `_mirror` / `_elongate` / `_twist` / `_bend`) warp the
// query point before evaluating their child. A nested expression form would
// have to inline the warp into every reference — `sd_sphere(domain_twist(p,
// 2.0), 0.1)` — which gets unreadable when warps compose and impossible when
// a primitive is the child of a chain of warps. SSA emission with one
// `p_<i>` per warp node is uniform regardless of depth.
//
// **ULP-conformance contract**: `evaluate<float>(ir, p)` (C++) must agree
// with `<fn>(vec3(p))` (compiled GLSL, dispatched on GPU) within 1 ULP
// per query point. Verified by the conformance test on all 21 golden
// manifests against a 64³ sample grid.
//
// **Determinism caveat**: the 3 transcendental-using operators (`smin_exp`,
// `domain_twist`, `domain_bend`) currently emit GPU-native `exp` / `sin` /
// `cos` calls. The corresponding C++ functions use `crd::math::deterministic::
// {exp2, sin, cos}` (Cephes polynomial ports, ~3 ULP across compilers).
// GPU-vs-CPU divergence on these ops can exceed 1 ULP — the conformance
// test uses a per-manifest tolerance schema. Porting the deterministic
// polynomials to GLSL (v9e-b-stage-2) closes the gap.
// ---------------------------------------------------------------------------

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::shader_helpers
{

// Fixed-text GLSL prelude with every `sd_*` + operator helper. Stable across
// calls; cooker (v9e-d) writes this verbatim to the runtime shader library.
[[nodiscard]] crd::containers::StringView glsl_helpers_prelude() noexcept;

// Emit `float <function_name>(vec3 p) { ... }` for the given IR. Caller is
// responsible for prepending the prelude (or use `emit_glsl_conformance_shader`
// which builds a complete compute shader).
//
// Pre-condition: `validate(ir).status == Ok`. Asserts on invalid IR.
[[nodiscard]] crd::containers::String emit_glsl_sdf_function(
    const FormulaIr&            ir,
    crd::containers::StringView function_name,
    crd::memory::IAllocator*    alloc) noexcept;

// Emit a complete compute shader: prelude + sdf() + a `main()` that samples
// a 3D grid and writes the SDF values to a storage buffer. Used by the
// ULP-conformance test (and any consumer who wants to evaluate a generated
// SDF on the GPU directly without authoring their own wrapper).
//
// The compute kernel reads the grid origin / step / resolution from push
// constants (see `GlslConformancePushConstants` below for the C++-side
// layout). Output buffer is bound at `set=0, binding=0` as a flat
// `float[]` indexed as `(z * res + y) * res + x`.
[[nodiscard]] crd::containers::String emit_glsl_conformance_shader(
    const FormulaIr&            ir,
    crd::memory::IAllocator*    alloc) noexcept;

// Push-constant struct laid out to match the conformance shader's
// `layout(push_constant) uniform Push { ... }` block.
struct alignas(16) GlslConformancePushConstants
{
    float    grid_origin[3];
    float    pad0;
    float    grid_step[3];
    crd::u32 grid_resolution;
};
static_assert(sizeof(GlslConformancePushConstants) == 32U,
              "GlslConformancePushConstants must match the GLSL std140 push-constant layout");

} // namespace crd::geometry::shader_helpers
