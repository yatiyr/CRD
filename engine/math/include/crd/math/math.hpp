#pragma once

// Umbrella header for the currently shipped slices of crd-math.
//
// Shipped today:
//   - scalar helpers (`pi`, radians/degrees, approx compare, lerp/saturate/
//     smoothstep/damp interpolation primitives)
//   - `Vec2<T>`, `Vec3<T>`, `Vec4<T>` (with lerp / damp componentwise)
//   - `Mat2/3/4<T>`
//   - `Quat<T>` and rigid `Transform<T>`
//   - Penner easing curves (`ease_in_cubic`, `ease_out_bounce`, …)
//   - `f32` / `f64` aliases (`Vec3f`, `Quatd`, ...)
//   - `std::format` support for the core math types
//
// NOT here (Phase 3.1.7 v0a, ADR-0076 §13): primitive geometry — `Ray3` /
// `Plane` / `AABB3` / `Sphere` / `Triangle3` / `Frustum` + their closest-point /
// intersection helpers live in `crd-geometry-primitives`
// (`<crd/geometry/primitives/primitives.hpp>`). crd-math is the lean leaf
// numerics substrate; computational geometry is `crd-geometry`'s tier.

#include <crd/math/easing.hpp>
#include <crd/math/format.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>

namespace crd::math
{
int force_link_math() noexcept;
} // namespace crd::math
