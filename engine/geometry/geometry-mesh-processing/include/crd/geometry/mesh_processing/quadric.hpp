#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7b Quadric<T> (Garland-Heckbert 1997).
//
// A `Quadric<T>` is a symmetric 4x4 matrix `K` carried per vertex of a mesh
// (and per edge during decimation). Its meaning:
//
//   For a plane `p = (a, b, c, d)` with `a² + b² + c² = 1` and `d = -n·v0`
//   (i.e., the plane through `v0` with unit normal `n`), the fundamental
//   error quadric for that plane is
//
//       K_p = p * p^T            (4x4 outer product)
//
//   And the squared distance from a point `v` (treated as `v_hom = (v, 1)`)
//   to the plane is exactly
//
//       D²(v) = v_hom^T K_p v_hom        (Garland-Heckbert 1997 eq. 3)
//
//   Summing K over all faces incident to a vertex gives a *per-vertex*
//   quadric whose evaluation at any point measures the total squared
//   distance to all those original planes. After an edge collapse, the
//   merged vertex's quadric is the sum of the two endpoints' quadrics, and
//   the optimal merged position is the minimiser of that quadric.
//
// **Storage layout (LOCKED — referenced by hash-stable serialisation):**
// 10 unique floats for the symmetric 4x4
//
//     row 0:  data[0]  data[1]  data[2]  data[3]
//     row 1:  data[1]  data[4]  data[5]  data[6]
//     row 2:  data[2]  data[5]  data[7]  data[8]
//     row 3:  data[3]  data[6]  data[8]  data[9]
//
// **Determinism contract:** all operations use deterministic FP per
// `crd-math` (no transcendentals; no std::sort on FP). The 3x3 inverse in
// `optimal_position` is crd-math's cofactor inverse (`try_inverse`, no
// pivoting), so the same input yields the same output across compilers.
//
// **Two-layer typing:** raw `<MathScalar T>` template only. Typed
// (`Quantity`-wrapped) consumers ride wrappers in `quadric_typed.hpp` at
// the API boundary (added at slice close if a typed-surface consumer pulls).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <optional>

namespace crd::geometry::mesh_processing
{

template <crd::math::MathScalar T>
struct Quadric
{
    // 10 unique entries of the symmetric 4x4 (see header comment for layout).
    T data[10] = {T{0}, T{0}, T{0}, T{0}, T{0}, T{0}, T{0}, T{0}, T{0}, T{0}};

    // Outer-product constructor: `K = (a, b, c, d) * (a, b, c, d)^T`.
    [[nodiscard]] static constexpr Quadric from_plane(T a, T b, T c, T d) noexcept
    {
        Quadric q{};
        q.data[0] = a * a;
        q.data[1] = a * b;
        q.data[2] = a * c;
        q.data[3] = a * d;
        q.data[4] = b * b;
        q.data[5] = b * c;
        q.data[6] = b * d;
        q.data[7] = c * c;
        q.data[8] = c * d;
        q.data[9] = d * d;
        return q;
    }

    [[nodiscard]] static constexpr Quadric zero() noexcept { return Quadric{}; }
};

template <crd::math::MathScalar T>
[[nodiscard]] constexpr Quadric<T> operator+(const Quadric<T>& a, const Quadric<T>& b) noexcept
{
    Quadric<T> r{};
    for (int i = 0; i < 10; ++i)
    {
        r.data[i] = a.data[i] + b.data[i];
    }
    return r;
}

template <crd::math::MathScalar T>
constexpr Quadric<T>& operator+=(Quadric<T>& a, const Quadric<T>& b) noexcept
{
    for (int i = 0; i < 10; ++i)
    {
        a.data[i] += b.data[i];
    }
    return a;
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr Quadric<T> operator*(const Quadric<T>& q, T s) noexcept
{
    Quadric<T> r{};
    for (int i = 0; i < 10; ++i)
    {
        r.data[i] = q.data[i] * s;
    }
    return r;
}

// Evaluate `v_hom^T K v_hom` where `v_hom = (v.x, v.y, v.z, 1)`. Returns the
// quadric cost (= squared distance summed over the planes whose K's are in q
// — for a single plane K this is exactly the squared point-plane distance).
template <crd::math::MathScalar T>
[[nodiscard]] constexpr T evaluate(const Quadric<T>& q, const crd::math::Vec3<T>& v) noexcept
{
    // Expanded form, exploiting symmetry: each off-diagonal contributes
    // twice (once above and once below diagonal).
    //   = m00·x² + m11·y² + m22·z² + m33
    //   + 2(m01·xy + m02·xz + m03·x + m12·yz + m13·y + m23·z)
    const T x = v.x;
    const T y = v.y;
    const T z = v.z;
    return q.data[0] * x * x + q.data[4] * y * y + q.data[7] * z * z + q.data[9]
         + T{2} * (q.data[1] * x * y + q.data[2] * x * z + q.data[3] * x
                 + q.data[5] * y * z + q.data[6] * y
                 + q.data[8] * z);
}

// Find the position `v` that minimises `v_hom^T K v_hom`. Setting the
// gradient to zero gives the 3x3 system
//
//     [ m00 m01 m02 ] [ vx ]     [ m03 ]
//     [ m01 m11 m12 ] [ vy ]  =  -[ m13 ]
//     [ m02 m12 m22 ] [ vz ]     [ m23 ]
//
// Returns `nullopt` if the 3x3 is singular (det < `det_epsilon`); caller
// then falls back to a midpoint or endpoint candidate (Garland-Heckbert
// 1997 §4.3). The default epsilon is tuned for `f32`; for `f64` a smaller
// value (~1e-20) is appropriate but the default is safe (overrejects, never
// underrejects).
template <crd::math::MathScalar T>
[[nodiscard]] std::optional<crd::math::Vec3<T>> optimal_position(
    const Quadric<T>& q,
    T                 det_epsilon = static_cast<T>(1e-10)) noexcept
{
    // The symmetric upper-left 3x3 block, by columns.
    const crd::math::Mat3<T> m{crd::math::Vec3<T>{q.data[0], q.data[1], q.data[2]},
                               crd::math::Vec3<T>{q.data[1], q.data[4], q.data[5]},
                               crd::math::Vec3<T>{q.data[2], q.data[5], q.data[7]}};
    crd::math::Mat3<T> inv{};
    if (!crd::math::try_inverse(m, inv, det_epsilon))
    {
        return std::nullopt;
    }
    return inv * crd::math::Vec3<T>{-q.data[3], -q.data[6], -q.data[8]};
}

} // namespace crd::geometry::mesh_processing
