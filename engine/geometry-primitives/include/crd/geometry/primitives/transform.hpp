#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives -- Transform-aware shape helpers. Phase 3.1.7 v11
// (2026-05-19).
//
// Free functions that take a 4x4 (or 3x3 for 2D) homogeneous transform matrix
// and a primitive shape and return the same primitive type transformed into
// the matrix's frame. Plus a `TransformedShape<Shape>` lightweight composition
// wrapper for consumers that want to carry "shape + transform" through scene
// graphs / ECS components.
//
// **Local-space-pure principle (D230, original §11.3 pin).** crd-geometry
// stays local-space. World-space dispatch (`world_raycast(scene, ray)` /
// `world_overlap(scene, box)`) is a CONSUMER concern -- `crd-scene-spatial`
// and `crd-eylem` already adapt. v11 ships shape transform helpers only.
//
// **D217 -- Module location.** Lives in `crd-geometry-primitives` (the
// shapes' home), NOT in a new `crd-geometry-runtime` module. The
// reserved-slot stays reserved per the 2026-05-14 scope decision: a
// world-space facade duplicating queries / shapes would create
// import-time consumer ambiguity for zero gain.
//
// **D218 -- `TransformedShape<Shape>` + trait-based scalar deduction.**
// Composition wrapper uses `shape_scalar<Shape>::type` per-shape
// specialisations so `TransformedShape<AABB3<f64>>` automatically carries
// a `Mat4d` to_world, with no precision-loss mismatch possible.
//
// **D219 -- `transform_aabb` 8-corner method.** Universal, conservative
// AABB-of-transformed-corners. The standard answer; no better
// axis-aligned bound exists for general affine transforms.
//
// **D220 -- `transform_obb` cascade.** Three-tier decision:
//   1. rigid-exact: rotation + translation only (orthonormal upper-3,
//      determinant = +-1). New OBB has rotated orientation, same
//      half_extents, translated center.
//   2. uniform-scale-exact: rigid + uniform scale s. Half_extents *= s.
//   3. general affine: conservative-via-axis-aligned-OBB. New OBB has
//      identity orientation, axis-aligned, sized to bound the affine-
//      transformed 8 corners. Returns OBB type for API uniformity even
//      though it's axis-aligned.
//
// **D221 -- `transform_sphere`.** Uniform-scale gives exact new sphere
// (radius * scale). Non-uniform gives a CONSERVATIVE bound: new radius
// = old radius * max axial scale. There is no exact bound for
// non-uniform scale of a sphere (it becomes an ellipsoid, which Cerid
// doesn't have a type for); loose bound is the standard answer.
//
// **D222 -- `transform_plane` via inverse-transpose 3x3.** A plane
// normal under linear transform M transforms by `inverse_transpose(M)`
// to preserve perpendicularity. Mathematically forced -- not optional.
//
// **D223 -- `transform_ray3` (forward) preserves direction magnitude.**
// Direction is NOT renormalized. This preserves the ray's t-parameter
// scaling so callers can compose ray transforms without losing the
// "hit at t=5 in world" semantics.
//
// **D224 -- `transform_ray3_to_local` inverse-direction pattern.**
// Takes `world_ray` + the consumer-provided `world_to_local` matrix
// (caller is responsible for computing the inverse). Returns a local-
// space ray suitable for a local-space raycast. **Precondition: the
// `world_to_local` matrix is rigid + uniform-scale** -- under
// non-uniform-scale or shear, `t_local` and `t_world` diverge by axis
// and a `LocalRayWithScale` return type would be needed. Asserts at
// entry on rigidity + uniform-scale check. The `LocalRayWithScale`
// variant is filed as `v11-ray-to-local-non-uniform` follow-on.
//
// **D225 -- Singular-matrix policy.** `transform_plane` requires the
// 3x3 inverse-transpose; a singular upper-3 (determinant ~= 0) means
// the world transform is degenerate. Assert at entry (`CRD_ASSERT` on
// `|det(M_3x3)| > epsilon`). No silent fallback -- per
// `feedback_quality_bar`, papering-over a degenerate input rules out
// the consumer's bug.
//
// **D226 -- Surface widened from phase-doc 7 entries to FULL primitive
// catalog.** User-mandated elite completeness: every concrete primitive
// type ships a transform helper. 14 3D entries (TransformedShape +
// AABB3 + OBB3 + Sphere + Capsule3 + Cylinder3 + Triangle3 + Tetrahedron
// + Plane + Ray3 + Segment3 + Line3 + Frustum + Ray3_to_local) + 7 2D
// peers (AABB2 + OBB2 + Circle + Capsule2 + Segment2 + Ray2 + Triangle2).
//
// **D227 -- 2D peers use Mat3<T>.** A 2D affine transform fits in a 3x3
// homogeneous matrix; column-major, translation in c2.
//
// **D228 -- MatrixAttributes cached once.** `compute_attributes(M)` runs
// at function entry, stores {is_identity, is_rigid, is_uniform_scale,
// uniform_scale_factor, max_axial_scale, determinant}. Reused across
// decisions. No re-computation per shape field.
//
// **D229 -- Identity-matrix bit-exact fast path.** `M == Mat4::identity()`
// short-circuits to bit-exact pass-through. Useful for batched static-
// geometry transforms where most matrices are identity.
//
// **D231 -- Negative-determinant (reflection) handled automatically.**
// 8-corner method works regardless of orientation. Radius bounds use
// `std::abs` on scale factors. Plane normal under reflection flips via
// inverse-transpose. OBB rigid-path preserves volume + flips
// orientation.
//
// **D232 -- `transform_frustum` batched inverse-transpose.** Computes
// `inverse_transpose(upper3(M))` ONCE and reuses across all 6 plane
// transforms internally. Saves 5 inverse-transpose computations per
// frustum vs. naively calling `transform_plane` six times.
//
// **D233 -- Typed boundary in `transform_typed.hpp`.** Quantity-aware
// `transform_*_typed` wrappers ship alongside (separate header).
// Strip-compute-retag pattern mirrors `queries_typed.hpp`.
// ---------------------------------------------------------------------------

#include <crd/containers/static_array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>

namespace crd::geometry::primitives
{

// ---------------------------------------------------------------------------
// detail_transform -- internal helpers.
// ---------------------------------------------------------------------------

namespace detail_transform
{

// Homogeneous point/dir multiplies. The extra Vec4 round-trip compiles down
// to the same machine code as a hand-rolled 4x3 multiply on optimised builds;
// the explicit homogeneous coordinates make the intent obvious to the reader.

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> mul_point(const crd::math::Mat4<T>& m,
                                                      const crd::math::Vec3<T>& p) noexcept
{
    const crd::math::Vec4<T> h(p.x, p.y, p.z, static_cast<T>(1));
    const crd::math::Vec4<T> r = m * h;
    return crd::math::Vec3<T>(r.x, r.y, r.z);
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec3<T> mul_dir(const crd::math::Mat4<T>& m,
                                                    const crd::math::Vec3<T>& d) noexcept
{
    const crd::math::Vec4<T> h(d.x, d.y, d.z, static_cast<T>(0));
    const crd::math::Vec4<T> r = m * h;
    return crd::math::Vec3<T>(r.x, r.y, r.z);
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Mat3<T> upper3(const crd::math::Mat4<T>& m) noexcept
{
    return crd::math::Mat3<T>(crd::math::Vec3<T>(m.c0.x, m.c0.y, m.c0.z),
                              crd::math::Vec3<T>(m.c1.x, m.c1.y, m.c1.z),
                              crd::math::Vec3<T>(m.c2.x, m.c2.y, m.c2.z));
}

// 2D helpers via Mat3 (homogeneous 3x3).

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec2<T> mul_point2(const crd::math::Mat3<T>& m,
                                                       const crd::math::Vec2<T>& p) noexcept
{
    const crd::math::Vec3<T> h(p.x, p.y, static_cast<T>(1));
    const crd::math::Vec3<T> r = m * h;
    return crd::math::Vec2<T>(r.x, r.y);
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Vec2<T> mul_dir2(const crd::math::Mat3<T>& m,
                                                     const crd::math::Vec2<T>& d) noexcept
{
    const crd::math::Vec3<T> h(d.x, d.y, static_cast<T>(0));
    const crd::math::Vec3<T> r = m * h;
    return crd::math::Vec2<T>(r.x, r.y);
}

template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Mat2<T> upper2(const crd::math::Mat3<T>& m) noexcept
{
    return crd::math::Mat2<T>(crd::math::Vec2<T>(m.c0.x, m.c0.y),
                              crd::math::Vec2<T>(m.c1.x, m.c1.y));
}

// 3x3 determinant (cofactor expansion along the first column).
template <crd::math::MathScalar T>
[[nodiscard]] constexpr T det3(const crd::math::Mat3<T>& m) noexcept
{
    return m.c0.x * (m.c1.y * m.c2.z - m.c2.y * m.c1.z)
         - m.c0.y * (m.c1.x * m.c2.z - m.c2.x * m.c1.z)
         + m.c0.z * (m.c1.x * m.c2.y - m.c2.x * m.c1.y);
}

// 3x3 inverse-transpose. Computes `(inverse(M))^T = adjugate / det`.
// Used by `transform_plane` (per D222) -- preserves perpendicularity under
// general affine.
template <crd::math::MathScalar T>
[[nodiscard]] constexpr crd::math::Mat3<T> inverse_transpose_3(const crd::math::Mat3<T>& m) noexcept
{
    const T cof00 =  (m.c1.y * m.c2.z - m.c2.y * m.c1.z);
    const T cof01 = -(m.c1.x * m.c2.z - m.c2.x * m.c1.z);
    const T cof02 =  (m.c1.x * m.c2.y - m.c2.x * m.c1.y);
    const T cof10 = -(m.c0.y * m.c2.z - m.c2.y * m.c0.z);
    const T cof11 =  (m.c0.x * m.c2.z - m.c2.x * m.c0.z);
    const T cof12 = -(m.c0.x * m.c2.y - m.c2.x * m.c0.y);
    const T cof20 =  (m.c0.y * m.c1.z - m.c1.y * m.c0.z);
    const T cof21 = -(m.c0.x * m.c1.z - m.c1.x * m.c0.z);
    const T cof22 =  (m.c0.x * m.c1.y - m.c1.x * m.c0.y);

    const T det      = m.c0.x * cof00 + m.c0.y * cof01 + m.c0.z * cof02;
    const T inv_det  = static_cast<T>(1) / det;
    // Note: adjugate is transpose of cofactor matrix; inverse = adjugate / det.
    // We want INVERSE-TRANSPOSE = (cofactor / det)^TT = cofactor / det.
    return crd::math::Mat3<T>(crd::math::Vec3<T>(cof00 * inv_det, cof01 * inv_det, cof02 * inv_det),
                              crd::math::Vec3<T>(cof10 * inv_det, cof11 * inv_det, cof12 * inv_det),
                              crd::math::Vec3<T>(cof20 * inv_det, cof21 * inv_det, cof22 * inv_det));
}

// Cached attributes of a Mat4 transform (D228).
template <crd::math::MathScalar T>
struct MatrixAttributes3
{
    bool is_identity      = false;
    bool is_rigid         = false;       // pure rotation + translation (no scale, no shear).
    bool is_uniform_scale = false;       // rotation + uniform scale + translation.
    T    uniform_scale_factor = static_cast<T>(1);
    T    max_axial_scale  = static_cast<T>(1);
    T    determinant      = static_cast<T>(1);
};

template <crd::math::MathScalar T>
[[nodiscard]] inline bool is_identity_mat4(const crd::math::Mat4<T>& m) noexcept
{
    const auto id = crd::math::Mat4<T>::identity();
    return m.c0.x == id.c0.x && m.c0.y == id.c0.y && m.c0.z == id.c0.z && m.c0.w == id.c0.w
        && m.c1.x == id.c1.x && m.c1.y == id.c1.y && m.c1.z == id.c1.z && m.c1.w == id.c1.w
        && m.c2.x == id.c2.x && m.c2.y == id.c2.y && m.c2.z == id.c2.z && m.c2.w == id.c2.w
        && m.c3.x == id.c3.x && m.c3.y == id.c3.y && m.c3.z == id.c3.z && m.c3.w == id.c3.w;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline bool is_identity_mat3(const crd::math::Mat3<T>& m) noexcept
{
    const auto id = crd::math::Mat3<T>::identity();
    return m.c0.x == id.c0.x && m.c0.y == id.c0.y && m.c0.z == id.c0.z
        && m.c1.x == id.c1.x && m.c1.y == id.c1.y && m.c1.z == id.c1.z
        && m.c2.x == id.c2.x && m.c2.y == id.c2.y && m.c2.z == id.c2.z;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline MatrixAttributes3<T> compute_attributes(const crd::math::Mat4<T>& m) noexcept
{
    MatrixAttributes3<T> a;
    a.is_identity = is_identity_mat4(m);

    const auto upper       = upper3(m);
    const T    col0_len_sq = crd::math::length_squared(upper.c0);
    const T    col1_len_sq = crd::math::length_squared(upper.c1);
    const T    col2_len_sq = crd::math::length_squared(upper.c2);
    const T    col0_len    = static_cast<T>(std::sqrt(static_cast<double>(col0_len_sq)));
    const T    col1_len    = static_cast<T>(std::sqrt(static_cast<double>(col1_len_sq)));
    const T    col2_len    = static_cast<T>(std::sqrt(static_cast<double>(col2_len_sq)));

    a.max_axial_scale = col0_len;
    if (col1_len > a.max_axial_scale) { a.max_axial_scale = col1_len; }
    if (col2_len > a.max_axial_scale) { a.max_axial_scale = col2_len; }

    a.determinant = det3(upper);

    // Uniform scale: all three column lengths agree within a relative epsilon.
    const T eps          = crd::math::default_epsilon<T>() * static_cast<T>(10);
    const T avg_col_len  = (col0_len + col1_len + col2_len) / static_cast<T>(3);
    const T scale_spread = std::max({std::abs(col0_len - col1_len), std::abs(col1_len - col2_len),
                                      std::abs(col0_len - col2_len)});
    a.is_uniform_scale = (avg_col_len > eps) && (scale_spread <= eps * avg_col_len);
    a.uniform_scale_factor = avg_col_len;

    // Rigid: uniform scale = 1 + orthogonal columns. Check pairwise dots.
    if (a.is_uniform_scale && std::abs(a.uniform_scale_factor - static_cast<T>(1)) <= eps)
    {
        const T d01 = crd::math::dot(upper.c0, upper.c1);
        const T d12 = crd::math::dot(upper.c1, upper.c2);
        const T d02 = crd::math::dot(upper.c0, upper.c2);
        a.is_rigid  = std::abs(d01) <= eps && std::abs(d12) <= eps && std::abs(d02) <= eps;
    }
    return a;
}

// 2D matrix attributes (Mat3 homogeneous).
template <crd::math::MathScalar T>
struct MatrixAttributes2
{
    bool is_identity         = false;
    bool is_uniform_scale    = false;
    T    uniform_scale_factor = static_cast<T>(1);
    T    max_axial_scale     = static_cast<T>(1);
    T    determinant         = static_cast<T>(1);
};

template <crd::math::MathScalar T>
[[nodiscard]] inline MatrixAttributes2<T> compute_attributes_2d(const crd::math::Mat3<T>& m) noexcept
{
    MatrixAttributes2<T> a;
    a.is_identity = is_identity_mat3(m);

    const auto upper       = upper2(m);
    const T    col0_len_sq = crd::math::length_squared(upper.c0);
    const T    col1_len_sq = crd::math::length_squared(upper.c1);
    const T    col0_len    = static_cast<T>(std::sqrt(static_cast<double>(col0_len_sq)));
    const T    col1_len    = static_cast<T>(std::sqrt(static_cast<double>(col1_len_sq)));

    a.max_axial_scale      = (col0_len > col1_len) ? col0_len : col1_len;
    a.determinant          = upper.c0.x * upper.c1.y - upper.c0.y * upper.c1.x;

    const T eps          = crd::math::default_epsilon<T>() * static_cast<T>(10);
    const T avg_col_len  = (col0_len + col1_len) / static_cast<T>(2);
    a.is_uniform_scale   = (avg_col_len > eps) && (std::abs(col0_len - col1_len) <= eps * avg_col_len);
    a.uniform_scale_factor = avg_col_len;
    return a;
}

} // namespace detail_transform

// ---------------------------------------------------------------------------
// shape_scalar trait + TransformedShape composition wrapper (D218).
// ---------------------------------------------------------------------------

template <typename Shape> struct shape_scalar; // undefined: triggers SFINAE / readable error.

template <typename T> struct shape_scalar<AABB3<T>>       { using type = T; };
template <typename T> struct shape_scalar<OBB3<T>>        { using type = T; };
template <typename T> struct shape_scalar<Sphere<T>>      { using type = T; };
template <typename T> struct shape_scalar<Capsule3<T>>    { using type = T; };
template <typename T> struct shape_scalar<Cylinder3<T>>   { using type = T; };
template <typename T> struct shape_scalar<Triangle3<T>>   { using type = T; };
template <typename T> struct shape_scalar<Tetrahedron<T>> { using type = T; };
template <typename T> struct shape_scalar<Plane<T>>       { using type = T; };
template <typename T> struct shape_scalar<Ray3<T>>        { using type = T; };
template <typename T> struct shape_scalar<Segment3<T>>    { using type = T; };
template <typename T> struct shape_scalar<Line3<T>>       { using type = T; };
template <typename T> struct shape_scalar<Frustum<T>>     { using type = T; };
template <typename T> struct shape_scalar<AABB2<T>>       { using type = T; };
template <typename T> struct shape_scalar<OBB2<T>>        { using type = T; };
template <typename T> struct shape_scalar<Circle<T>>      { using type = T; };
template <typename T> struct shape_scalar<Capsule2<T>>    { using type = T; };
template <typename T> struct shape_scalar<Cylinder2<T>>   { using type = T; };
template <typename T> struct shape_scalar<Triangle2<T>>   { using type = T; };
template <typename T> struct shape_scalar<Segment2<T>>    { using type = T; };
template <typename T> struct shape_scalar<Ray2<T>>        { using type = T; };
template <typename T> struct shape_scalar<Line2<T>>       { using type = T; };

template <typename Shape>
using shape_scalar_t = typename shape_scalar<Shape>::type;

// Lightweight composition wrapper. Value type; cheaply copyable. The
// transform matrix is dimensionless (M is rotation+scale+translation in
// world-space metric); the wrapped shape carries its own scalar type.
template <typename Shape>
struct TransformedShape
{
    Shape                                      shape{};
    crd::math::Mat4<shape_scalar_t<Shape>>     to_world =
        crd::math::Mat4<shape_scalar_t<Shape>>::identity();
};

// ---------------------------------------------------------------------------
// 3D transforms.
// ---------------------------------------------------------------------------

// transform_aabb -- 8-corner method (D219). Universal, conservative.
template <crd::math::MathScalar T>
[[nodiscard]] inline AABB3<T> transform_aabb(const crd::math::Mat4<T>& m,
                                              const AABB3<T>&           box) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return box; } // D229 fast path

    using crd::math::Vec3;
    const Vec3<T> corners[8] = {
        {box.min.x, box.min.y, box.min.z}, {box.max.x, box.min.y, box.min.z},
        {box.min.x, box.max.y, box.min.z}, {box.max.x, box.max.y, box.min.z},
        {box.min.x, box.min.y, box.max.z}, {box.max.x, box.min.y, box.max.z},
        {box.min.x, box.max.y, box.max.z}, {box.max.x, box.max.y, box.max.z},
    };
    Vec3<T> tmin = detail_transform::mul_point(m, corners[0]);
    Vec3<T> tmax = tmin;
    for (int i = 1; i < 8; ++i)
    {
        const Vec3<T> p = detail_transform::mul_point(m, corners[i]);
        tmin.x = (p.x < tmin.x) ? p.x : tmin.x;
        tmin.y = (p.y < tmin.y) ? p.y : tmin.y;
        tmin.z = (p.z < tmin.z) ? p.z : tmin.z;
        tmax.x = (p.x > tmax.x) ? p.x : tmax.x;
        tmax.y = (p.y > tmax.y) ? p.y : tmax.y;
        tmax.z = (p.z > tmax.z) ? p.z : tmax.z;
    }
    return AABB3<T>{tmin, tmax};
}

// transform_obb -- D220 three-tier cascade.
template <crd::math::MathScalar T>
[[nodiscard]] inline OBB3<T> transform_obb(const crd::math::Mat4<T>& m,
                                            const OBB3<T>&            obb) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return obb; } // D229

    const auto attrs = detail_transform::compute_attributes(m);
    const auto upper = detail_transform::upper3(m);

    if (attrs.is_rigid)
    {
        // Tier 1: pure rotation + translation. Orientation rotates, half_extents
        // unchanged, center translates.
        OBB3<T> out;
        out.center       = detail_transform::mul_point(m, obb.center);
        out.half_extents = obb.half_extents;
        out.orientation  = upper * obb.orientation;
        return out;
    }

    if (attrs.is_uniform_scale)
    {
        // Tier 2: rigid + uniform scale s. Scale-out from rotation matrix to
        // keep orientation orthonormal, then scale half_extents by s.
        OBB3<T> out;
        out.center             = detail_transform::mul_point(m, obb.center);
        out.half_extents.x     = obb.half_extents.x * attrs.uniform_scale_factor;
        out.half_extents.y     = obb.half_extents.y * attrs.uniform_scale_factor;
        out.half_extents.z     = obb.half_extents.z * attrs.uniform_scale_factor;
        const T inv_s          = static_cast<T>(1) / attrs.uniform_scale_factor;
        crd::math::Mat3<T> rot = upper;
        rot.c0                 = rot.c0 * inv_s;
        rot.c1                 = rot.c1 * inv_s;
        rot.c2                 = rot.c2 * inv_s;
        out.orientation        = rot * obb.orientation;
        return out;
    }

    // Tier 3: general affine. Compute the OBB's 8 world-space corners and
    // build an axis-aligned-in-world OBB bounding them. Returns OBB type for
    // API uniformity (identity orientation, half_extents from AABB).
    using crd::math::Vec3;
    const Vec3<T> axes[3] = {
        obb.orientation.c0 * obb.half_extents.x,
        obb.orientation.c1 * obb.half_extents.y,
        obb.orientation.c2 * obb.half_extents.z,
    };
    Vec3<T> tmin{}, tmax{};
    bool    first = true;
    for (int sx = -1; sx <= 1; sx += 2)
    {
        for (int sy = -1; sy <= 1; sy += 2)
        {
            for (int sz = -1; sz <= 1; sz += 2)
            {
                const Vec3<T> corner_local =
                    obb.center + axes[0] * static_cast<T>(sx) + axes[1] * static_cast<T>(sy)
                    + axes[2] * static_cast<T>(sz);
                const Vec3<T> p = detail_transform::mul_point(m, corner_local);
                if (first)
                {
                    tmin = p;
                    tmax = p;
                    first = false;
                }
                else
                {
                    tmin.x = (p.x < tmin.x) ? p.x : tmin.x;
                    tmin.y = (p.y < tmin.y) ? p.y : tmin.y;
                    tmin.z = (p.z < tmin.z) ? p.z : tmin.z;
                    tmax.x = (p.x > tmax.x) ? p.x : tmax.x;
                    tmax.y = (p.y > tmax.y) ? p.y : tmax.y;
                    tmax.z = (p.z > tmax.z) ? p.z : tmax.z;
                }
            }
        }
    }
    OBB3<T> out;
    out.center       = (tmin + tmax) * static_cast<T>(0.5);
    out.half_extents = (tmax - tmin) * static_cast<T>(0.5);
    out.orientation  = crd::math::Mat3<T>::identity();
    return out;
}

// transform_sphere -- D221: uniform-scale exact, non-uniform max-axial loose.
template <crd::math::MathScalar T>
[[nodiscard]] inline Sphere<T> transform_sphere(const crd::math::Mat4<T>& m,
                                                 const Sphere<T>&          sphere) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return sphere; } // D229
    const auto attrs = detail_transform::compute_attributes(m);
    Sphere<T> out;
    out.center = detail_transform::mul_point(m, sphere.center);
    out.radius = sphere.radius
               * (attrs.is_uniform_scale ? attrs.uniform_scale_factor : attrs.max_axial_scale);
    return out;
}

// transform_capsule3 / transform_cylinder3 -- a + b transformed; radius
// bounded by max-axial-scale (non-uniform) or scale factor (uniform).
template <crd::math::MathScalar T>
[[nodiscard]] inline Capsule3<T> transform_capsule3(const crd::math::Mat4<T>& m,
                                                     const Capsule3<T>&        cap) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return cap; } // D229
    const auto attrs = detail_transform::compute_attributes(m);
    Capsule3<T> out;
    out.a      = detail_transform::mul_point(m, cap.a);
    out.b      = detail_transform::mul_point(m, cap.b);
    out.radius = cap.radius
               * (attrs.is_uniform_scale ? attrs.uniform_scale_factor : attrs.max_axial_scale);
    return out;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Cylinder3<T> transform_cylinder3(const crd::math::Mat4<T>& m,
                                                       const Cylinder3<T>&       cyl) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return cyl; } // D229
    const auto attrs = detail_transform::compute_attributes(m);
    Cylinder3<T> out;
    out.a      = detail_transform::mul_point(m, cyl.a);
    out.b      = detail_transform::mul_point(m, cyl.b);
    out.radius = cyl.radius
               * (attrs.is_uniform_scale ? attrs.uniform_scale_factor : attrs.max_axial_scale);
    return out;
}

// transform_triangle3 / transform_tetrahedron -- each vertex transformed.
template <crd::math::MathScalar T>
[[nodiscard]] inline Triangle3<T> transform_triangle3(const crd::math::Mat4<T>& m,
                                                       const Triangle3<T>&       tri) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return tri; } // D229
    return Triangle3<T>{detail_transform::mul_point(m, tri.a), detail_transform::mul_point(m, tri.b),
                         detail_transform::mul_point(m, tri.c)};
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Tetrahedron<T> transform_tetrahedron(const crd::math::Mat4<T>& m,
                                                           const Tetrahedron<T>&     tet) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return tet; } // D229
    return Tetrahedron<T>{detail_transform::mul_point(m, tet.a), detail_transform::mul_point(m, tet.b),
                           detail_transform::mul_point(m, tet.c), detail_transform::mul_point(m, tet.d)};
}

// transform_plane -- D222 via inverse-transpose 3x3. D225 asserts on singular.
template <crd::math::MathScalar T>
[[nodiscard]] inline Plane<T> transform_plane(const crd::math::Mat4<T>& m,
                                               const Plane<T>&           plane) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return plane; } // D229

    const auto                  upper = detail_transform::upper3(m);
    [[maybe_unused]] const T    det   = detail_transform::det3(upper);
    CRD_ASSERT(std::abs(det) > crd::math::default_epsilon<T>()); // D225

    // Pick any point on the original plane: p = -d * n. Transform it.
    const auto    n_old      = plane.normal;
    const T       d_old      = plane.d;
    const auto    p_on_plane = n_old * (-d_old);
    const auto    p_new      = detail_transform::mul_point(m, p_on_plane);
    const auto    inv_t      = detail_transform::inverse_transpose_3(upper);
    auto          n_new      = inv_t * n_old;
    // Renormalize the new normal (inverse-transpose can change its magnitude).
    (void) crd::math::try_normalize(n_new);
    const T       d_new      = -crd::math::dot(n_new, p_new);
    return Plane<T>{n_new, d_new};
}

// transform_ray3 -- forward direction transform; magnitude preserved (D223).
template <crd::math::MathScalar T>
[[nodiscard]] inline Ray3<T> transform_ray3(const crd::math::Mat4<T>& m,
                                             const Ray3<T>&            ray) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return ray; } // D229
    return Ray3<T>{detail_transform::mul_point(m, ray.origin),
                    detail_transform::mul_dir(m, ray.direction)};
}

// transform_ray3_to_local -- D224 inverse-direction pattern. Precondition:
// world_to_local is rigid + uniform-scale; asserts at entry.
template <crd::math::MathScalar T>
[[nodiscard]] inline Ray3<T> transform_ray3_to_local(const Ray3<T>&            world_ray,
                                                      const crd::math::Mat4<T>& world_to_local) noexcept
{
    [[maybe_unused]] const auto attrs = detail_transform::compute_attributes(world_to_local);
    CRD_ASSERT(attrs.is_uniform_scale); // D224 precondition: rigid + uniform-scale

    return Ray3<T>{detail_transform::mul_point(world_to_local, world_ray.origin),
                    detail_transform::mul_dir(world_to_local, world_ray.direction)};
}

// transform_segment3 / transform_line3.
template <crd::math::MathScalar T>
[[nodiscard]] inline Segment3<T> transform_segment3(const crd::math::Mat4<T>& m,
                                                     const Segment3<T>&        seg) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return seg; } // D229
    return Segment3<T>{detail_transform::mul_point(m, seg.a), detail_transform::mul_point(m, seg.b)};
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Line3<T> transform_line3(const crd::math::Mat4<T>& m,
                                               const Line3<T>&           line) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return line; } // D229
    return Line3<T>{detail_transform::mul_point(m, line.point),
                     detail_transform::mul_dir(m, line.direction)};
}

// transform_frustum -- D232: compute inverse-transpose ONCE, reuse across 6
// plane transforms.
template <crd::math::MathScalar T>
[[nodiscard]] inline Frustum<T> transform_frustum(const crd::math::Mat4<T>& m,
                                                   const Frustum<T>&         frustum) noexcept
{
    if (detail_transform::is_identity_mat4(m)) { return frustum; } // D229

    const auto                  upper = detail_transform::upper3(m);
    [[maybe_unused]] const T    det   = detail_transform::det3(upper);
    CRD_ASSERT(std::abs(det) > crd::math::default_epsilon<T>()); // D225

    const auto inv_t = detail_transform::inverse_transpose_3(upper);

    Frustum<T> out;
    for (crd::usize i = 0U; i < 6U; ++i)
    {
        const auto& plane     = frustum.planes[i];
        const auto  n_old     = plane.normal;
        const T     d_old     = plane.d;
        const auto  p_old     = n_old * (-d_old);
        const auto  p_new     = detail_transform::mul_point(m, p_old);
        auto        n_new     = inv_t * n_old;
        (void) crd::math::try_normalize(n_new);
        const T     d_new     = -crd::math::dot(n_new, p_new);
        out.planes[i]         = Plane<T>{n_new, d_new};
    }
    return out;
}

// ---------------------------------------------------------------------------
// 2D peers (D226/D227).
// ---------------------------------------------------------------------------

template <crd::math::MathScalar T>
[[nodiscard]] inline AABB2<T> transform_aabb2(const crd::math::Mat3<T>& m,
                                               const AABB2<T>&           box) noexcept
{
    if (detail_transform::is_identity_mat3(m)) { return box; }

    using crd::math::Vec2;
    const Vec2<T> corners[4] = {
        {box.min.x, box.min.y}, {box.max.x, box.min.y},
        {box.min.x, box.max.y}, {box.max.x, box.max.y},
    };
    Vec2<T> tmin = detail_transform::mul_point2(m, corners[0]);
    Vec2<T> tmax = tmin;
    for (int i = 1; i < 4; ++i)
    {
        const Vec2<T> p = detail_transform::mul_point2(m, corners[i]);
        tmin.x = (p.x < tmin.x) ? p.x : tmin.x;
        tmin.y = (p.y < tmin.y) ? p.y : tmin.y;
        tmax.x = (p.x > tmax.x) ? p.x : tmax.x;
        tmax.y = (p.y > tmax.y) ? p.y : tmax.y;
    }
    return AABB2<T>{tmin, tmax};
}

template <crd::math::MathScalar T>
[[nodiscard]] inline OBB2<T> transform_obb2(const crd::math::Mat3<T>& m,
                                             const OBB2<T>&            obb) noexcept
{
    if (detail_transform::is_identity_mat3(m)) { return obb; }

    const auto attrs = detail_transform::compute_attributes_2d(m);
    const auto upper = detail_transform::upper2(m);

    if (attrs.is_uniform_scale)
    {
        // Rigid+uniform tier: orientation rotates (possibly with scale removed),
        // center translates, half_extents *= scale.
        const T  inv_s = static_cast<T>(1) / attrs.uniform_scale_factor;
        crd::math::Mat2<T> rot = upper;
        rot.c0 = rot.c0 * inv_s;
        rot.c1 = rot.c1 * inv_s;
        OBB2<T> out;
        out.center       = detail_transform::mul_point2(m, obb.center);
        out.half_extents = obb.half_extents * attrs.uniform_scale_factor;
        out.orientation  = rot * obb.orientation;
        return out;
    }

    // General-affine tier: bound via axis-aligned 4 corners.
    using crd::math::Vec2;
    const Vec2<T> axes[2] = {obb.orientation.c0 * obb.half_extents.x,
                              obb.orientation.c1 * obb.half_extents.y};
    Vec2<T> tmin{}, tmax{};
    bool first = true;
    for (int sx = -1; sx <= 1; sx += 2)
    {
        for (int sy = -1; sy <= 1; sy += 2)
        {
            const Vec2<T> corner_local =
                obb.center + axes[0] * static_cast<T>(sx) + axes[1] * static_cast<T>(sy);
            const Vec2<T> p = detail_transform::mul_point2(m, corner_local);
            if (first)
            {
                tmin = p;
                tmax = p;
                first = false;
            }
            else
            {
                tmin.x = (p.x < tmin.x) ? p.x : tmin.x;
                tmin.y = (p.y < tmin.y) ? p.y : tmin.y;
                tmax.x = (p.x > tmax.x) ? p.x : tmax.x;
                tmax.y = (p.y > tmax.y) ? p.y : tmax.y;
            }
        }
    }
    OBB2<T> out;
    out.center       = (tmin + tmax) * static_cast<T>(0.5);
    out.half_extents = (tmax - tmin) * static_cast<T>(0.5);
    out.orientation  = crd::math::Mat2<T>::identity();
    return out;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Circle<T> transform_circle(const crd::math::Mat3<T>& m,
                                                 const Circle<T>&          c) noexcept
{
    if (detail_transform::is_identity_mat3(m)) { return c; }
    const auto attrs = detail_transform::compute_attributes_2d(m);
    Circle<T> out;
    out.center = detail_transform::mul_point2(m, c.center);
    out.radius = c.radius
               * (attrs.is_uniform_scale ? attrs.uniform_scale_factor : attrs.max_axial_scale);
    return out;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Capsule2<T> transform_capsule2(const crd::math::Mat3<T>& m,
                                                     const Capsule2<T>&        cap) noexcept
{
    if (detail_transform::is_identity_mat3(m)) { return cap; }
    const auto attrs = detail_transform::compute_attributes_2d(m);
    Capsule2<T> out;
    out.a      = detail_transform::mul_point2(m, cap.a);
    out.b      = detail_transform::mul_point2(m, cap.b);
    out.radius = cap.radius
               * (attrs.is_uniform_scale ? attrs.uniform_scale_factor : attrs.max_axial_scale);
    return out;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Segment2<T> transform_segment2(const crd::math::Mat3<T>& m,
                                                     const Segment2<T>&        seg) noexcept
{
    if (detail_transform::is_identity_mat3(m)) { return seg; }
    return Segment2<T>{detail_transform::mul_point2(m, seg.a), detail_transform::mul_point2(m, seg.b)};
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Ray2<T> transform_ray2(const crd::math::Mat3<T>& m,
                                             const Ray2<T>&            ray) noexcept
{
    if (detail_transform::is_identity_mat3(m)) { return ray; }
    return Ray2<T>{detail_transform::mul_point2(m, ray.origin),
                    detail_transform::mul_dir2(m, ray.direction)};
}

template <crd::math::MathScalar T>
[[nodiscard]] inline Triangle2<T> transform_triangle2(const crd::math::Mat3<T>& m,
                                                       const Triangle2<T>&       tri) noexcept
{
    if (detail_transform::is_identity_mat3(m)) { return tri; }
    return Triangle2<T>{detail_transform::mul_point2(m, tri.a), detail_transform::mul_point2(m, tri.b),
                         detail_transform::mul_point2(m, tri.c)};
}

} // namespace crd::geometry::primitives
