#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — C++ ground-truth evaluator for formula-IRs.
// Phase 3.1.7 v9e-a (2026-05-18).
//
// Walks an IR top-down, recursively, calling the matching `sd_*` /
// smin_*/op_*/domain_* function from `signed_distance.hpp` + `formulary.hpp`.
// This is the byte-exact reference the v9e-b GLSL emission + v9e-c HLSL
// emission must match to ULP per the ULP-conformance contract.
//
// **Why this also lives in the engine module (not just tests)**: production
// consumers (renderer DFAO, editor preview before the cooker has cooked) will
// CALL this evaluator directly on CPU to author/preview SDFs against the
// same math the GPU shader runs.
//
// Determinism: same `crd::math::deterministic` discipline as `formulary.hpp`
// — no libm transcendental from `<cmath>` for the warps. The primitive `sd_*`
// functions use `std::sqrt` / `std::abs` (IEEE-exact) and the deterministic
// math primitives only.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::shader_helpers
{

// Evaluates the IR at query point `p` (shape-LOCAL coordinates, just like
// the underlying `sd_*` functions). Returns the signed distance. Caller
// must ensure `validate(ir).status == Ok` before calling — invalid IRs
// trigger CRD_ASSERT.
//
// Templated for f32/f64 instantiation; the C++ reference is f32 by default
// (matching the v9e-b GLSL emission target). f64 is available for high-
// precision CPU preview / regression tests.
template <typename T>
[[nodiscard]] T evaluate(const FormulaIr& ir, const crd::math::Vec3<T>& p) noexcept;

extern template float  evaluate<float>(const FormulaIr&, const crd::math::Vec3<float>&)  noexcept;
extern template double evaluate<double>(const FormulaIr&, const crd::math::Vec3<double>&) noexcept;

} // namespace crd::geometry::shader_helpers
