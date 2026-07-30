#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — ATTRIBUTE QUADRICS
//   (Hoppe 1999, "New Quadric Metric for Simplifying Meshes with Appearance
//    Attributes"; the same idea as Garland-Heckbert 1998 §5, in the
//    decomposed form that keeps the solve 3x3 instead of (3+m)x(3+m).)
//
// **WHY THIS EXISTS — the failure it prevents.** `Quadric<T>` measures only
// squared distance to the incident face planes. A decimator driven by it will
// happily collapse an edge that leaves the SURFACE where it was and drags the
// TEXTURE across it, because texture coordinates are not in the metric at all.
// The visible result is UV swimming: the mesh silhouette is right and the
// texture slides as the LOD changes — far worse than a slightly coarser
// silhouette, and exactly the artefact an LOD chain must not have.
//
// Transferring attributes AFTER a position-only collapse does not fix this: by
// then the collapse has already been CHOSEN without regard to the distortion it
// causes. The attributes have to be in the error being minimised.
//
// **THE FORM.** For a face with corners (p_i, s_i), i = 1..3, and m attribute
// channels, every channel j is an exactly-linear function over the triangle:
//
//     s_j(v) = g_j · v + d_j          with  g_j ⟂ n   (g_j lies in the plane)
//
// determined by the three corner values. The per-face error is then
//
//     E(v, s) = (n·v + d_n)²  +  Σ_j (s_j − g_j·v − d_j)²
//               \___________/     \_________________________/
//                 geometric              per-channel
//
// Summing over the faces around a vertex (area-weighted) and MINIMISING OVER s
// — which is what the merged vertex is free to choose — the s terms collapse
// analytically. With
//
//     N  = Σ_f w_f          G_j = Σ_f w_f g_jf          D_j = Σ_f w_f d_jf
//
// the stationary attribute is  s_j(v) = (G_j·v + D_j) / N,  and substituting it
// back leaves a quadric in v ALONE:
//
//     Q(v) = Q_geom+attr(v)  −  Σ_j (G_j·v + D_j)² / N
//
// So the whole thing folds back into an ordinary 4x4 `Quadric<T>` — which means
// `optimal_position` and the determinism contract carry over UNCHANGED. That is
// the entire reason for this decomposition: no (3+m)x(3+m) solve, no new linear
// algebra, no new failure modes in the inverse.
//
// **M = 0 REDUCES EXACTLY TO `Quadric<T>`** — same bits, not merely the same
// idea — so the attribute path and the historical path cannot silently diverge.
// The gate asserts it.
//
// **Determinism contract:** accumulation order is the caller's face order and
// every operation is deterministic FP per crd-math (no transcendentals, no
// std::sort on FP), so the folded quadric is byte-identical across compilers
// given byte-identical input — the same contract `qem_decimate` already holds.
//
// **Two-layer typing:** raw `<MathScalar T>` only; typed consumers ride
// wrappers at the API boundary, as `quadric.hpp` does.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/quadric.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::mesh_processing
{

// The per-face linear model of one attribute channel over a triangle:
// `s(v) = g·v + d`, valid on the triangle's plane. `plane_gradient` builds it.
template <crd::math::MathScalar T>
struct AttributeGradient
{
    crd::math::Vec3<T> g{T{0}, T{0}, T{0}};
    T                  d = T{0};
};

// Build the linear model of a scalar attribute over the triangle (p1,p2,p3)
// carrying values (s1,s2,s3).
//
// ⛔ THE GRADIENT MUST LIE IN THE TRIANGLE PLANE. Solving the 3-corner system
// alone leaves the component along the normal UNDETERMINED, and any non-zero
// value there makes the model predict a different attribute for points off the
// surface — which is precisely where the merged vertex will sit. Building it in
// an in-plane orthonormal frame is what pins that component to zero.
//
// Degenerate triangle (zero area) ⇒ a zero gradient and `d = s1`: the channel
// then contributes nothing but a constant, which is the correct limit and never
// a NaN.
template <crd::math::MathScalar T>
[[nodiscard]] AttributeGradient<T> plane_gradient(const crd::math::Vec3<T>& p1, const crd::math::Vec3<T>& p2,
                                                  const crd::math::Vec3<T>& p3, T s1, T s2, T s3) noexcept
{
    using V = crd::math::Vec3<T>;
    const V u{p2.x - p1.x, p2.y - p1.y, p2.z - p1.z};
    const V w{p3.x - p1.x, p3.y - p1.y, p3.z - p1.z};

    const T u_len2 = (u.x * u.x) + (u.y * u.y) + (u.z * u.z);
    if (!(u_len2 > T{0})) { return AttributeGradient<T>{V{T{0}, T{0}, T{0}}, s1}; }
    const T inv_u_len = T{1} / crd::math::length(u);
    const V e1{u.x * inv_u_len, u.y * inv_u_len, u.z * inv_u_len};

    // Gram-Schmidt: the second frame axis, in-plane and orthogonal to e1.
    const T w_dot_e1 = (w.x * e1.x) + (w.y * e1.y) + (w.z * e1.z);
    const V w_perp{w.x - (w_dot_e1 * e1.x), w.y - (w_dot_e1 * e1.y), w.z - (w_dot_e1 * e1.z)};
    const T wp_len2 = (w_perp.x * w_perp.x) + (w_perp.y * w_perp.y) + (w_perp.z * w_perp.z);
    if (!(wp_len2 > T{0})) { return AttributeGradient<T>{V{T{0}, T{0}, T{0}}, s1}; } // collinear ⇒ degenerate
    const T inv_wp_len = T{1} / crd::math::length(w_perp);
    const V e2{w_perp.x * inv_wp_len, w_perp.y * inv_wp_len, w_perp.z * inv_wp_len};

    // In (e1,e2) coordinates with p1 at the origin: p2 = (u2, 0), p3 = (w1, w2).
    const T u2 = crd::math::length(u);
    const T w1 = w_dot_e1;
    const T w2 = crd::math::length(w_perp);

    // s = s1 + alpha*x + beta*y  must hold at p2 and p3.
    const T alpha = (s2 - s1) / u2;
    const T beta  = ((s3 - s1) - (alpha * w1)) / w2;

    const V g{(alpha * e1.x) + (beta * e2.x), (alpha * e1.y) + (beta * e2.y), (alpha * e1.z) + (beta * e2.z)};
    const T d = s1 - ((g.x * p1.x) + (g.y * p1.y) + (g.z * p1.z));
    return AttributeGradient<T>{g, d};
}

// A quadric over position AND `M` attribute channels. `M = 0` is exactly
// `Quadric<T>` (the gate asserts bit-equality).
template <crd::math::MathScalar T, crd::u32 M>
struct AttributeQuadric
{
    Quadric<T>         geom{};                 // the plane quadric PLUS the per-channel v-only terms
    crd::math::Vec3<T> g[M > 0U ? M : 1U]{};   // Σ w_f · g_jf
    T                  d[M > 0U ? M : 1U]{};   // Σ w_f · d_jf
    T                  weight = T{0};          // Σ w_f  — the N that normalises the fold

    [[nodiscard]] static constexpr AttributeQuadric zero() noexcept { return AttributeQuadric{}; }
};

template <crd::math::MathScalar T, crd::u32 M>
constexpr AttributeQuadric<T, M>& operator+=(AttributeQuadric<T, M>& a, const AttributeQuadric<T, M>& b) noexcept
{
    a.geom += b.geom;
    for (crd::u32 j = 0; j < M; ++j)
    {
        a.g[j].x += b.g[j].x;
        a.g[j].y += b.g[j].y;
        a.g[j].z += b.g[j].z;
        a.d[j] += b.d[j];
    }
    a.weight += b.weight;
    return a;
}

template <crd::math::MathScalar T, crd::u32 M>
[[nodiscard]] constexpr AttributeQuadric<T, M> operator+(const AttributeQuadric<T, M>& a,
                                                         const AttributeQuadric<T, M>& b) noexcept
{
    AttributeQuadric<T, M> r = a;
    r += b;
    return r;
}

// Accumulate ONE face's contribution: the geometric plane quadric (already unit
// normal + offset, exactly as `Quadric::from_plane` takes it) and the `M`
// attribute gradients over that face, all scaled by `w` (the caller's area or
// unit weight — whichever it uses for the geometric term, so the two halves
// stay commensurable).
template <crd::math::MathScalar T, crd::u32 M>
void accumulate_face(AttributeQuadric<T, M>& q, T na, T nb, T nc, T nd, const AttributeGradient<T> (&grad)[M > 0U ? M : 1U],
                     T w) noexcept
{
    q.geom += Quadric<T>::from_plane(na, nb, nc, nd) * w;
    for (crd::u32 j = 0; j < M; ++j)
    {
        const auto& gj = grad[j];
        // the v-only half of (s_j − g_j·v − d_j)² once s_j is eliminated:
        //   v^T (g g^T) v + 2 (d g)·v + d²      — an ordinary plane quadric in (g, d)
        q.geom += Quadric<T>::from_plane(gj.g.x, gj.g.y, gj.g.z, gj.d) * w;
        q.g[j].x += w * gj.g.x;
        q.g[j].y += w * gj.g.y;
        q.g[j].z += w * gj.g.z;
        q.d[j] += w * gj.d;
    }
    q.weight += w;
}

// Fold to an ordinary 4x4 quadric in `v` alone by substituting the stationary
// attribute values. THIS is what makes the rest of the decimator — the 3x3
// solve, the cost evaluation, the determinism contract — apply unchanged.
//
// ⛔ `weight <= 0` (an isolated vertex, or M = 0) folds to `geom` untouched.
// Dividing by it would be a NaN dressed as a cost, and a NaN cost sorts
// arbitrarily in the heap — a nondeterministic decimation, which is the one
// thing this module promises never to produce.
template <crd::math::MathScalar T, crd::u32 M>
[[nodiscard]] Quadric<T> fold(const AttributeQuadric<T, M>& q) noexcept
{
    Quadric<T> r = q.geom;
    if constexpr (M > 0U)
    {
        if (!(q.weight > T{0})) { return r; }
        const T inv_n = T{1} / q.weight;
        for (crd::u32 j = 0; j < M; ++j)
        {
            // subtract (G_j·v + D_j)² / N — a plane quadric in (G_j, D_j), scaled
            r += Quadric<T>::from_plane(q.g[j].x, q.g[j].y, q.g[j].z, q.d[j]) * (-inv_n);
        }
    }
    return r;
}

// The attribute values the merged vertex should carry at `v` — the stationary
// point of the same quadric, so the position and its attributes come from ONE
// minimisation rather than a solve followed by a guess.
template <crd::math::MathScalar T, crd::u32 M>
void attributes_at(const AttributeQuadric<T, M>& q, const crd::math::Vec3<T>& v, T (&out)[M > 0U ? M : 1U]) noexcept
{
    if constexpr (M > 0U)
    {
        const T inv_n = (q.weight > T{0}) ? (T{1} / q.weight) : T{0};
        for (crd::u32 j = 0; j < M; ++j)
        {
            out[j] = ((q.g[j].x * v.x) + (q.g[j].y * v.y) + (q.g[j].z * v.z) + q.d[j]) * inv_n;
        }
    }
}

// The cost of placing the merged vertex at `v`, attributes included.
template <crd::math::MathScalar T, crd::u32 M>
[[nodiscard]] T evaluate_attr(const AttributeQuadric<T, M>& q, const crd::math::Vec3<T>& v) noexcept
{
    return evaluate(fold(q), v);
}

} // namespace crd::geometry::mesh_processing
