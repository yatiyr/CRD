#pragma once

// Umbrella header for the first shipped slice of crd-math.
//
// v1a ships:
//   - scalar helpers (`pi`, radians/degrees, approx compare)
//   - `Vec2<T>`, `Vec3<T>`, `Vec4<T>`
//   - `f32` / `f64` aliases (`Vec3f`, `Vec3d`, ...)
//
// Later slices add matrices, quaternions, transforms, and primitive geometry.

#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::math
{
int force_link_math() noexcept;
} // namespace crd::math
