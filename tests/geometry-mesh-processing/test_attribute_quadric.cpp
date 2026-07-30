// test_attribute_quadric.cpp — REN-40-C1: the ATTRIBUTE quadric (Hoppe 1999).
//
// ⛔ WHAT THESE GATES ARE FOR. An LOD chain built by a position-only decimator
// keeps the silhouette and drags the TEXTURE across the surface — UV swimming as
// the level changes, which is worse than a coarser silhouette and is exactly the
// artefact an LOD chain must not have. These gates assert the attribute is
// actually IN the error being minimised, not fixed up afterwards.

#include <crd/geometry/mesh_processing/attribute_quadric.hpp>
#include <crd/geometry/mesh_processing/quadric.hpp>
#include <crd/math/vec.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace mp = crd::geometry::mesh_processing;
using V3     = crd::math::Vec3<crd::f32>;

namespace
{
// the plane through p1 with unit normal from the triangle winding, as
// `Quadric::from_plane` takes it
struct Plane
{
    crd::f32 a, b, c, d;
};

[[nodiscard]] Plane tri_plane(const V3& p1, const V3& p2, const V3& p3)
{
    const V3       u{p2.x - p1.x, p2.y - p1.y, p2.z - p1.z};
    const V3       w{p3.x - p1.x, p3.y - p1.y, p3.z - p1.z};
    V3             n{(u.y * w.z) - (u.z * w.y), (u.z * w.x) - (u.x * w.z), (u.x * w.y) - (u.y * w.x)};
    const crd::f32 len = crd::math::length(n);
    n                  = V3{n.x / len, n.y / len, n.z / len};
    return Plane{n.x, n.y, n.z, -((n.x * p1.x) + (n.y * p1.y) + (n.z * p1.z))};
}
} // namespace

// ⛔⛔ THE REDUCTION GATE. `M = 0` must be `Quadric<T>` BIT FOR BIT, not merely
// "the same idea" — otherwise the attribute path and the historical path can
// diverge silently and every existing decimation result is quietly at risk.
TEST_CASE("REN-40-C1 GATE: an attribute quadric with zero channels IS the plain quadric, bit for bit",
          "[geometry][mesh-processing][quadric][ren40][lod]")
{
    const V3    p1{0.0F, 0.0F, 0.0F};
    const V3    p2{1.0F, 0.0F, 0.0F};
    const V3    p3{0.0F, 1.0F, 0.0F};
    const Plane pl = tri_plane(p1, p2, p3);

    mp::Quadric<crd::f32> plain{};
    plain += mp::Quadric<crd::f32>::from_plane(pl.a, pl.b, pl.c, pl.d) * 0.5F;

    mp::AttributeQuadric<crd::f32, 0U>  aq{};
    mp::AttributeGradient<crd::f32>     none[1]{};
    mp::accumulate_face<crd::f32, 0U>(aq, pl.a, pl.b, pl.c, pl.d, none, 0.5F);
    const mp::Quadric<crd::f32> folded = mp::fold(aq);

    CHECK(std::memcmp(static_cast<const void*>(plain.data), static_cast<const void*>(folded.data),
                      sizeof(plain.data))
          == 0);
}

// The linear model must REPRODUCE the corner values exactly (it is the unique
// affine function through them) and its gradient must lie IN the plane — the
// component along the normal is undetermined by the three corners, and any
// non-zero value there predicts a different attribute for points off the
// surface, which is exactly where a merged vertex sits.
TEST_CASE("REN-40-C1 GATE: the per-face attribute model interpolates the corners and stays in-plane",
          "[geometry][mesh-processing][quadric][ren40][lod]")
{
    const V3 p1{0.0F, 0.0F, 0.0F};
    const V3 p2{2.0F, 0.0F, 0.0F};
    const V3 p3{0.0F, 3.0F, 1.0F}; // deliberately NOT axis-aligned
    const crd::f32 s1 = -0.25F;
    const crd::f32 s2 = 0.75F;
    const crd::f32 s3 = 2.5F;

    const auto grad = mp::plane_gradient(p1, p2, p3, s1, s2, s3);
    const auto at   = [&](const V3& v) { return (grad.g.x * v.x) + (grad.g.y * v.y) + (grad.g.z * v.z) + grad.d; };

    CHECK(at(p1) == Catch::Approx(s1).margin(1e-5));
    CHECK(at(p2) == Catch::Approx(s2).margin(1e-5));
    CHECK(at(p3) == Catch::Approx(s3).margin(1e-5));

    const Plane    pl    = tri_plane(p1, p2, p3);
    const crd::f32 g_dot_n = (grad.g.x * pl.a) + (grad.g.y * pl.b) + (grad.g.z * pl.c);
    CHECK(g_dot_n == Catch::Approx(0.0F).margin(1e-5));

    // a degenerate (collinear) triangle must yield a CONSTANT model, never a NaN
    const auto deg = mp::plane_gradient(p1, p2, V3{4.0F, 0.0F, 0.0F}, s1, s2, s3);
    CHECK(deg.g.x == 0.0F);
    CHECK(deg.g.y == 0.0F);
    CHECK(deg.g.z == 0.0F);
    CHECK(deg.d == s1);
}

// ⭐ The point of the whole exercise, stated as a dichotomy:
//   - attribute varying LINEARLY over a flat patch  ⇒ the model is EXACT
//     ⇒ zero attribute error anywhere on the plane (only geometry costs).
//   - two faces DISAGREEING about the gradient      ⇒ a strictly positive floor
//     no placement can remove — the metric now sees the distortion a
//     position-only quadric is blind to.
TEST_CASE("REN-40-C1 GATE: the attribute term is zero for a consistent field and POSITIVE for a conflicting one",
          "[geometry][mesh-processing][quadric][ren40][lod]")
{
    // ── a flat patch in z = 0, attribute s = x (perfectly linear) ──
    const V3 a{0.0F, 0.0F, 0.0F};
    const V3 b{1.0F, 0.0F, 0.0F};
    const V3 c{1.0F, 1.0F, 0.0F};
    const V3 d{0.0F, 1.0F, 0.0F};

    const auto add_face = [](mp::AttributeQuadric<crd::f32, 1U>& q, const V3& q1, const V3& q2, const V3& q3,
                             crd::f32 v1, crd::f32 v2, crd::f32 v3) {
        const Plane                     pl = tri_plane(q1, q2, q3);
        mp::AttributeGradient<crd::f32> gr[1]{mp::plane_gradient(q1, q2, q3, v1, v2, v3)};
        mp::accumulate_face<crd::f32, 1U>(q, pl.a, pl.b, pl.c, pl.d, gr, 1.0F);
    };

    mp::AttributeQuadric<crd::f32, 1U> consistent{};
    add_face(consistent, a, b, c, a.x, b.x, c.x);
    add_face(consistent, a, c, d, a.x, c.x, d.x);

    // anywhere ON the plane the total error must be ~0: geometry is exact there
    // and the linear model reproduces s = x exactly.
    const V3 probe{0.37F, 0.62F, 0.0F};
    CHECK(mp::evaluate_attr(consistent, probe) == Catch::Approx(0.0F).margin(1e-4));

    crd::f32 got[1]{};
    mp::attributes_at(consistent, probe, got);
    CHECK(got[0] == Catch::Approx(probe.x).margin(1e-4)); // the field it should carry

    // ── the same geometry, but the two faces disagree about the attribute ──
    // (this is a UV seam / a texture that jumps across the diagonal)
    mp::AttributeQuadric<crd::f32, 1U> conflicting{};
    add_face(conflicting, a, b, c, 0.0F, 0.0F, 0.0F); // face 1 says s ≡ 0
    add_face(conflicting, a, c, d, 1.0F, 1.0F, 1.0F); // face 2 says s ≡ 1

    const crd::f32 floor_cost = mp::evaluate_attr(conflicting, probe);
    CHECK(floor_cost > 0.1F); // strictly positive: no placement reconciles them

    // and the CONSISTENT case must be cheaper than the conflicting one — the
    // ordering is what steers the decimator away from UV-destroying collapses
    CHECK(mp::evaluate_attr(consistent, probe) < floor_cost);

    // the stationary attribute for the conflicting patch is the weighted mean
    mp::attributes_at(conflicting, probe, got);
    CHECK(got[0] == Catch::Approx(0.5F).margin(1e-4));
}

// The module's standing promise. Two identical accumulations must produce
// byte-identical quadrics — a cost that varies by a bit sorts differently in the
// decimator's heap, and a differently-ordered heap is a different mesh.
TEST_CASE("REN-40-C1 GATE: attribute quadric accumulation is byte-identical across repeats",
          "[geometry][mesh-processing][quadric][ren40][lod]")
{
    const auto build = [] {
        mp::AttributeQuadric<crd::f32, 2U> q{};
        for (crd::u32 i = 0; i < 8U; ++i)
        {
            const auto  f  = static_cast<crd::f32>(i);
            const V3    p1{f * 0.13F, 0.0F, 0.0F};
            const V3    p2{1.0F + (f * 0.07F), 0.29F, f * 0.11F};
            const V3    p3{0.0F, 1.0F + (f * 0.03F), 0.5F};
            const Plane pl = tri_plane(p1, p2, p3);
            mp::AttributeGradient<crd::f32> gr[2]{
                mp::plane_gradient(p1, p2, p3, f, f + 1.0F, f * 2.0F),
                mp::plane_gradient(p1, p2, p3, -f, 0.5F, f * 0.25F),
            };
            mp::accumulate_face<crd::f32, 2U>(q, pl.a, pl.b, pl.c, pl.d, gr, 0.5F + (f * 0.01F));
        }
        return mp::fold(q);
    };
    const mp::Quadric<crd::f32> q1 = build();
    const mp::Quadric<crd::f32> q2 = build();
    CHECK(std::memcmp(static_cast<const void*>(q1.data), static_cast<const void*>(q2.data), sizeof(q1.data)) == 0);
}
