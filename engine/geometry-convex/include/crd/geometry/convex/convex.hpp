#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — umbrella include for the convex-shape narrow-phase
// substrate (Phase 3.1.7 v2; ADR-0076 §1).
//
// v2a ships: ConvexShape concept + SupportPoint + four `support()` overloads
// (Sphere / OBB3 / Capsule3 / ConvexHullView), GJK distance + boolean overlap
// in shape A's local frame with the index-match termination contract.
//
// Later slices extend this surface:
//   v2c — EPA penetration depth + contact normal (`epa.hpp`)
//   v2d — SAT box-pair fast path (`sat.hpp`)
//   v2e — `ConvexHullView` ray / closest / contains via GJK
//   v2f — GJK-based convex shapecast (`gjk_cast.hpp`)
//   v2g — hill-climbing hull support overload (vertex-adjacency `ConvexHullView`)
//   v2h — Vec4f/Vec8f SIMD-batched hull support (`hull_support_simd.cpp`)
//   v2i — f64 instantiation + aerospace orbital corpus
//   v2j — Sutherland-Hodgman clipping + feature enumeration
// ---------------------------------------------------------------------------

#include <crd/geometry/convex/convex_hull_2d.hpp>
#include <crd/geometry/convex/epa.hpp>
#include <crd/geometry/convex/feature_clip.hpp>
#include <crd/geometry/convex/gjk.hpp>
#include <crd/geometry/convex/hull_queries.hpp>
#include <crd/geometry/convex/sat.hpp>
#include <crd/geometry/convex/shapecast.hpp>
#include <crd/geometry/convex/support.hpp>

// ---------------------------------------------------------------------------
// v2i — f64 instantiation pins (Phase 3.1.7 v2i; ADR-0076 §4 pin #14).
//
// The convex substrate has been templated on `MathScalar T` throughout v2a..h
// — these static_asserts pin that the f64 path stays first-class. They fire
// at compile time if any future change accidentally introduces an f32-only
// assumption that breaks the `ConvexShape<S, f64>` concept (e.g. embedding
// an f32 literal in a templated `support()` overload, or losing an f64
// specialisation of an epsilon constant).
//
// **The contract**: every shipped shape type satisfies `ConvexShape<S, T>`
// for `T ∈ {f32, f64}`. Aerospace consumers calling
// `gjk_distance<f64>(...)` against any of these shapes get full f64
// precision through the entire kernel — including support evaluation,
// sub-distance reduction, simplex bookkeeping, and EPA polytope expansion.
//
// f32 (1 m + 7 sig digits ≈ 0.0001 m absolute precision) loses sub-meter
// precision past ~10⁶ m (megameter scale). f64 (1 m + 15 sig digits ≈
// 1e-9 m absolute) holds to nanometer precision through ~10⁹ m (giga-
// meter / lunar-scale) and beyond. Pick `T` by the bounding magnitude of
// the inputs the kernel sees.
//
// SoA SIMD path (`support_simd_f32` / `Vec8f`) is f32-only by construction;
// the `if constexpr (T == f32)` guard in `support_with_hint` routes f64
// callers to the AoS linear-scan / hill-climb paths transparently. No
// runtime cost on the f64 code path for the SoA-absent dispatch.
// ---------------------------------------------------------------------------

namespace crd::geometry::convex
{
static_assert(ConvexShape<primitives::Sphere<crd::f32>, crd::f32>);
static_assert(ConvexShape<primitives::Sphere<crd::f64>, crd::f64>);
static_assert(ConvexShape<primitives::OBB3<crd::f32>, crd::f32>);
static_assert(ConvexShape<primitives::OBB3<crd::f64>, crd::f64>);
static_assert(ConvexShape<primitives::Capsule3<crd::f32>, crd::f32>);
static_assert(ConvexShape<primitives::Capsule3<crd::f64>, crd::f64>);
static_assert(ConvexShape<primitives::ConvexHullView<crd::f32>, crd::f32>);
static_assert(ConvexShape<primitives::ConvexHullView<crd::f64>, crd::f64>);
static_assert(ConvexShape<PointShape<crd::f32>, crd::f32>);
static_assert(ConvexShape<PointShape<crd::f64>, crd::f64>);
} // namespace crd::geometry::convex
