#include <crd/geometry/viz/primitives.hpp>

#include <crd/math/vec.hpp>

#include <cmath>

namespace crd::geometry::viz
{
namespace
{
using crd::f32;
using crd::math::Vec3f;

// Pick an `up` direction not collinear with `n` for a tangent-frame basis.
[[nodiscard]] Vec3f pick_up(const Vec3f& n) noexcept
{
    if (std::abs(n.y) < 0.9F)
    {
        return Vec3f(0.0F, 1.0F, 0.0F);
    }
    return Vec3f(1.0F, 0.0F, 0.0F);
}

[[nodiscard]] f32 vec_length(const Vec3f& v) noexcept
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

[[nodiscard]] Vec3f normalize_or_zero(const Vec3f& v) noexcept
{
    const f32 l = vec_length(v);
    if (l <= 0.0F)
    {
        return Vec3f(0.0F, 0.0F, 0.0F);
    }
    return Vec3f(v.x / l, v.y / l, v.z / l);
}

[[nodiscard]] Vec3f cross_v(const Vec3f& a, const Vec3f& b) noexcept
{
    return Vec3f(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

[[nodiscard]] f32 dot_v(const Vec3f& a, const Vec3f& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

void draw(crd::draw::RenderBuffer& buf, const primitives::Plane<crd::f32>& plane, Vec3f anchor, crd::f32 size_world,
          crd::u32 grid_divisions, crd::draw::Color color, crd::f32 width_px, crd::draw::PrimFlags flags,
          crd::f32 lifetime_s)
{
    // Build a tangent frame in the plane: `u` and `v` perpendicular to the
    // plane normal, and emit a `size_world`-sized square centred at `anchor`
    // projected onto the plane (so the patch lies *on* the plane).
    const Vec3f n = normalize_or_zero(plane.normal);
    if (n.x == 0.0F && n.y == 0.0F && n.z == 0.0F)
    {
        return; // degenerate plane — nothing meaningful to draw
    }
    const Vec3f up = pick_up(n);
    const Vec3f u = normalize_or_zero(cross_v(n, up));
    const Vec3f v = cross_v(n, u); // already unit since n + u are
    // Project `anchor` onto the plane: anchor' = anchor - (n·anchor + d)·n.
    const crd::f32 signed_dist = dot_v(n, anchor) + plane.d;
    const Vec3f a_proj(anchor.x - n.x * signed_dist, anchor.y - n.y * signed_dist, anchor.z - n.z * signed_dist);

    // Grid lines in (u, v) plane.
    const crd::f32 half = size_world * 0.5F;
    const crd::u32 divs = grid_divisions == 0U ? 1U : grid_divisions;
    const crd::f32 step = size_world / static_cast<crd::f32>(divs);
    for (crd::u32 i = 0U; i <= divs; ++i)
    {
        const crd::f32 t = -half + static_cast<crd::f32>(i) * step;
        // U-direction line at v=t.
        const Vec3f a1(a_proj.x + -half * u.x + t * v.x, a_proj.y + -half * u.y + t * v.y,
                       a_proj.z + -half * u.z + t * v.z);
        const Vec3f b1(a_proj.x + half * u.x + t * v.x, a_proj.y + half * u.y + t * v.y,
                       a_proj.z + half * u.z + t * v.z);
        crd::draw::add_line_to(buf, a1, b1, color, width_px, flags, lifetime_s);
        // V-direction line at u=t.
        const Vec3f a2(a_proj.x + t * u.x + -half * v.x, a_proj.y + t * u.y + -half * v.y,
                       a_proj.z + t * u.z + -half * v.z);
        const Vec3f b2(a_proj.x + t * u.x + half * v.x, a_proj.y + t * u.y + half * v.y,
                       a_proj.z + t * u.z + half * v.z);
        crd::draw::add_line_to(buf, a2, b2, color, width_px, flags, lifetime_s);
    }
}

namespace
{
// Solve 3 plane equations (n_i · x + d_i = 0, i = 1..3) for the intersection
// point. Returns `false` if the planes are not in general position.
[[nodiscard]] bool intersect_three_planes(const primitives::Plane<f32>& p1, const primitives::Plane<f32>& p2,
                                          const primitives::Plane<f32>& p3, Vec3f& out) noexcept
{
    const Vec3f n1 = p1.normal;
    const Vec3f n2 = p2.normal;
    const Vec3f n3 = p3.normal;
    const Vec3f n23 = cross_v(n2, n3);
    const f32 det = dot_v(n1, n23);
    if (det == 0.0F)
    {
        return false;
    }
    const Vec3f n31 = cross_v(n3, n1);
    const Vec3f n12 = cross_v(n1, n2);
    // Solution: x = (-d1·(n2×n3) - d2·(n3×n1) - d3·(n1×n2)) / det.
    const Vec3f num(-p1.d * n23.x - p2.d * n31.x - p3.d * n12.x, -p1.d * n23.y - p2.d * n31.y - p3.d * n12.y,
                    -p1.d * n23.z - p2.d * n31.z - p3.d * n12.z);
    out = Vec3f(num.x / det, num.y / det, num.z / det);
    return true;
}
} // namespace

void draw(crd::draw::RenderBuffer& buf, const primitives::Frustum<crd::f32>& frustum, crd::draw::Color color,
          crd::f32 width_px, crd::draw::PrimFlags flags, crd::f32 lifetime_s)
{
    // Canonical plane ordering: { Left, Right, Bottom, Top, Near, Far }.
    // 8 corners = intersection of (L|R) ∩ (B|T) ∩ (N|F).
    constexpr int kL = 0;
    constexpr int kR = 1;
    constexpr int kB = 2;
    constexpr int kT = 3;
    constexpr int kN = 4;
    constexpr int kF = 5;
    Vec3f corners[8];
    bool ok[8];
    const auto& p = frustum.planes;
    ok[0] = intersect_three_planes(p[kL], p[kB], p[kN], corners[0]); // near-bottom-left
    ok[1] = intersect_three_planes(p[kR], p[kB], p[kN], corners[1]); // near-bottom-right
    ok[2] = intersect_three_planes(p[kR], p[kT], p[kN], corners[2]); // near-top-right
    ok[3] = intersect_three_planes(p[kL], p[kT], p[kN], corners[3]); // near-top-left
    ok[4] = intersect_three_planes(p[kL], p[kB], p[kF], corners[4]); // far-bottom-left
    ok[5] = intersect_three_planes(p[kR], p[kB], p[kF], corners[5]); // far-bottom-right
    ok[6] = intersect_three_planes(p[kR], p[kT], p[kF], corners[6]); // far-top-right
    ok[7] = intersect_three_planes(p[kL], p[kT], p[kF], corners[7]); // far-top-left

    auto edge = [&](int i, int j) noexcept {
        if (ok[i] && ok[j])
        {
            crd::draw::add_line_to(buf, corners[i], corners[j], color, width_px, flags, lifetime_s);
        }
    };
    // Near quad.
    edge(0, 1);
    edge(1, 2);
    edge(2, 3);
    edge(3, 0);
    // Far quad.
    edge(4, 5);
    edge(5, 6);
    edge(6, 7);
    edge(7, 4);
    // Connectors.
    edge(0, 4);
    edge(1, 5);
    edge(2, 6);
    edge(3, 7);
}

} // namespace crd::geometry::viz
