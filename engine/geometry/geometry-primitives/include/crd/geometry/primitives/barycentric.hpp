#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — barycentric & tetrahedron utilities (Phase 3.1.7
// v0d). Ericson, "Real-Time Collision Detection" §3.4.
//
//   * `barycentric(Tetrahedron, p)` → the 4 weights (signed-volume ratios) and
//     `contains(Tetrahedron, p)` (all weights ≥ −ε — works for either tetra
//     orientation since the ratios are sign-stable).
//   * `from_barycentric` — the inverse of `barycentric` for `Triangle3` /
//     `Triangle2` / `Tetrahedron` (reconstruct the point from its weights).
//   * `decompose_prism_to_tets(bottom, top)` — the canonical 3-tetrahedron
//     split of a triangular prism / linear wedge (the form used by Marching
//     Tetrahedra and FEM hex-to-tet meshing; a fixed diagonal convention so
//     the split is consistent across a shared quad face).
//
// `barycentric(Triangle3,·)` / `contains(Triangle3,·)` / the 2D triangle forms
// already live in `primitives.hpp` (migrated/added in v0a–v0b); this header only
// adds the tetrahedron forms + `from_barycentric` + the prism decomposition.
// No transcendental libm calls; no allocation; constexpr where the algorithm allows.
// ---------------------------------------------------------------------------

#include <crd/containers/static_array.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::MathValue;
using crd::math::Vec2;
using crd::math::Vec3;
using crd::math::Vec4;

// ---- Tetrahedron barycentric / containment --------------------------------

// Barycentric weights (u_a, u_b, u_c, u_d) of `p` w.r.t. `tet` — each is the
// signed-volume ratio vol(...,p,...)/vol(a,b,c,d) with `p` substituted for the
// corresponding vertex; they sum to 1. A degenerate (flat) tetrahedron asserts.
template <MathScalar T>
[[nodiscard]] constexpr Vec4<T> barycentric(const Tetrahedron<T>& tet, const Vec3<T>& p) noexcept
{
    // 6·vol with `x` as the first vertex: dot(b−x, cross(c−x, d−x)).
    const auto vol6 = [](const Vec3<T>& w, const Vec3<T>& x, const Vec3<T>& y, const Vec3<T>& z)
    {
        return crd::math::dot(x - w, crd::math::cross(y - w, z - w));
    };

    const T denom = vol6(tet.a, tet.b, tet.c, tet.d);
    CRD_ASSERT(!crd::math::approx_zero(denom));
    const T inv = static_cast<T>(1) / denom;
    const T ua = vol6(p, tet.b, tet.c, tet.d) * inv; // p replaces a → opposite face bcd
    const T ub = vol6(tet.a, p, tet.c, tet.d) * inv; // p replaces b
    const T uc = vol6(tet.a, tet.b, p, tet.d) * inv; // p replaces c
    const T ud = vol6(tet.a, tet.b, tet.c, p) * inv; // p replaces d
    return Vec4<T>(ua, ub, uc, ud);
}

template <MathScalar T>
[[nodiscard]] constexpr bool contains(const Tetrahedron<T>& tet, const Vec3<T>& p,
                                      T epsilon = crd::math::default_epsilon<T>()) noexcept
{
    const Vec4<T> bc = barycentric(tet, p);
    return bc.x >= -epsilon && bc.y >= -epsilon && bc.z >= -epsilon && bc.w >= -epsilon;
}

// ---- Reconstruction (inverse of `barycentric`) ----------------------------

template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> from_barycentric(const Triangle3<T>& tri, const Vec3<T>& weights) noexcept
{
    return tri.a * weights.x + tri.b * weights.y + tri.c * weights.z;
}
template <MathScalar T>
[[nodiscard]] constexpr Vec2<T> from_barycentric(const Triangle2<T>& tri, const Vec3<T>& weights) noexcept
{
    return tri.a * weights.x + tri.b * weights.y + tri.c * weights.z;
}
template <MathScalar T>
[[nodiscard]] constexpr Vec3<T> from_barycentric(const Tetrahedron<T>& tet, const Vec4<T>& weights) noexcept
{
    return tet.a * weights.x + tet.b * weights.y + tet.c * weights.z + tet.d * weights.w;
}

// ---- 3-tetrahedron decomposition of a triangular prism / linear wedge ------
//
// `bottom` = (a₀,b₀,c₀), `top` = (a₁,b₁,c₁) with vertex i of `top` "above"
// vertex i of `bottom`. Splits the wedge into 3 tetrahedra through vertex a₀,
// with the diagonal on each quad face chosen consistently (the "lowest local
// index wins" rule, so two wedges sharing a quad face produce matching tets):
//   { a₀, b₀, c₀, c₁ },  { a₀, b₀, c₁, b₁ },  { a₀, b₁, c₁, a₁ }.
// The three tets partition the wedge; for a *right* prism their volumes sum to
// base-area · height.
template <MathScalar T>
[[nodiscard]] constexpr crd::containers::StaticArray<Tetrahedron<T>, 3>
decompose_prism_to_tets(const Triangle3<T>& bottom, const Triangle3<T>& top) noexcept
{
    return crd::containers::StaticArray<Tetrahedron<T>, 3>{
        Tetrahedron<T>(bottom.a, bottom.b, bottom.c, top.c),
        Tetrahedron<T>(bottom.a, bottom.b, top.c, top.b),
        Tetrahedron<T>(bottom.a, top.b, top.c, top.a),
    };
}

} // namespace crd::geometry::primitives
