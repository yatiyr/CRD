#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — 20 golden formula-IR manifests covering the
// iq SDF catalog. Phase 3.1.7 v9e-a (2026-05-18).
//
// Each `make_golden_*` function constructs a fully-valid IR for one SDF
// expression. Used by:
//   - test_formula_ir.cpp — `validate()` must return Ok for all 20.
//   - test_evaluator.cpp  — evaluator's output at sample points must match
//     a direct call to the equivalent `sd_*` / smin/op_* function (the
//     "the IR walk produces the same number as the C++ scalar reference"
//     contract that the v9e-b GLSL emission test will later extend to GPU
//     dispatch within 1 ULP).
//   - v9e-d cooker — these manifests are the seed library it emits to disk.
// ---------------------------------------------------------------------------

#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::shader_helpers::golden
{

// ---- 10 primitives -----------------------------------------------------
[[nodiscard]] FormulaIr make_sphere_unit(crd::memory::IAllocator* alloc) noexcept;        // 1
[[nodiscard]] FormulaIr make_box_unit(crd::memory::IAllocator* alloc) noexcept;            // 2
[[nodiscard]] FormulaIr make_round_box(crd::memory::IAllocator* alloc) noexcept;           // 3
[[nodiscard]] FormulaIr make_box_frame(crd::memory::IAllocator* alloc) noexcept;           // 4
[[nodiscard]] FormulaIr make_plane_y(crd::memory::IAllocator* alloc) noexcept;             // 5
[[nodiscard]] FormulaIr make_capsule(crd::memory::IAllocator* alloc) noexcept;             // 6
[[nodiscard]] FormulaIr make_cylinder(crd::memory::IAllocator* alloc) noexcept;            // 7
[[nodiscard]] FormulaIr make_cone(crd::memory::IAllocator* alloc) noexcept;                // 8
[[nodiscard]] FormulaIr make_torus(crd::memory::IAllocator* alloc) noexcept;               // 9
[[nodiscard]] FormulaIr make_triangle(crd::memory::IAllocator* alloc) noexcept;            // 10

// ---- 4 value-domain operators (binary smooth-min/max + 2 unary) --------
[[nodiscard]] FormulaIr make_smin_poly_union(crd::memory::IAllocator* alloc) noexcept;     // 11
[[nodiscard]] FormulaIr make_smin_cubic_union(crd::memory::IAllocator* alloc) noexcept;    // 12
[[nodiscard]] FormulaIr make_smin_exp_union(crd::memory::IAllocator* alloc) noexcept;      // 13
[[nodiscard]] FormulaIr make_smax_poly_intersect(crd::memory::IAllocator* alloc) noexcept; // 14
[[nodiscard]] FormulaIr make_op_round_sphere(crd::memory::IAllocator* alloc) noexcept;     // 15
[[nodiscard]] FormulaIr make_op_onion_sphere(crd::memory::IAllocator* alloc) noexcept;     // 16

// ---- 5 position-domain operators ---------------------------------------
[[nodiscard]] FormulaIr make_domain_repeat_sphere(crd::memory::IAllocator* alloc) noexcept;   // 17
[[nodiscard]] FormulaIr make_domain_mirror_sphere(crd::memory::IAllocator* alloc) noexcept;   // 18
[[nodiscard]] FormulaIr make_domain_elongate_sphere(crd::memory::IAllocator* alloc) noexcept; // 19
[[nodiscard]] FormulaIr make_domain_twist_cylinder(crd::memory::IAllocator* alloc) noexcept;  // 20
[[nodiscard]] FormulaIr make_domain_bend_capsule(crd::memory::IAllocator* alloc) noexcept;    // 21 (covers DomainBend too)

} // namespace crd::geometry::shader_helpers::golden
