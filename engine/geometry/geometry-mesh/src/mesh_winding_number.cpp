// crd-geometry-mesh — generalised winding number impl (v4c).
//
// Direct O(N) summation of Van Oosterom-Strackee 1983 per-triangle solid
// angles, scaled by 1/(4π). See header for algorithm + ADR refs.

#include <crd/geometry/mesh/mesh_winding_number.hpp>

#include <crd/core/assert.hpp>
#include <crd/math/scalar.hpp>

#include <crd/math/cmath.hpp>

namespace crd::geometry::mesh
{

using crd::math::Vec3;

namespace
{

constexpr crd::f32 kInvFourPiF = 0.0795774715459476F; // 1 / (4π)

// Van Oosterom-Strackee 1983 — signed solid angle of triangle (v0, v1, v2)
// at point p. `a`, `b`, `c` are the offset vectors (vi - p) precomputed
// by the caller.
inline crd::f32 solid_angle(const Vec3<crd::f32>& a,
                             const Vec3<crd::f32>& b,
                             const Vec3<crd::f32>& c) noexcept
{
    // |a|, |b|, |c|
    const crd::f32 la = crd::math::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const crd::f32 lb = crd::math::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    const crd::f32 lc = crd::math::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);

    // Degenerate: p coincident with a vertex → 0 contribution from this
    // triangle (the surrounding triangles around the vertex provide the
    // local solid-angle accumulation).
    if (la == 0.0F || lb == 0.0F || lc == 0.0F)
    {
        return 0.0F;
    }

    // numerator = a · (b × c) — signed scalar triple product
    const crd::f32 cx = b.y * c.z - b.z * c.y;
    const crd::f32 cy = b.z * c.x - b.x * c.z;
    const crd::f32 cz = b.x * c.y - b.y * c.x;
    const crd::f32 num = a.x * cx + a.y * cy + a.z * cz;

    // denominator = |a||b||c| + (a·b)|c| + (b·c)|a| + (c·a)|b|
    const crd::f32 ab = a.x * b.x + a.y * b.y + a.z * b.z;
    const crd::f32 bc = b.x * c.x + b.y * c.y + b.z * c.z;
    const crd::f32 ca = c.x * a.x + c.y * a.y + c.z * a.z;
    const crd::f32 denom = la * lb * lc + ab * lc + bc * la + ca * lb;

    return 2.0F * crd::math::atan2(num, denom);
}

} // namespace

crd::f32 mesh_winding_number(const TriangleMeshViewf& view,
                              const Vec3<crd::f32>&     query) noexcept
{
    if (view.is_empty())
    {
        return 0.0F;
    }

    const auto vertices = view.vertices;
    const auto indices  = view.indices;
    const crd::u32 tri_count = view.triangle_count();

    crd::f32 w_sum = 0.0F;
    for (crd::u32 ti = 0U; ti < tri_count; ++ti)
    {
        const crd::u32 i0 = indices[ti * 3U + 0U];
        const crd::u32 i1 = indices[ti * 3U + 1U];
        const crd::u32 i2 = indices[ti * 3U + 2U];
        const Vec3<crd::f32> a = vertices[i0] - query;
        const Vec3<crd::f32> b = vertices[i1] - query;
        const Vec3<crd::f32> c = vertices[i2] - query;
        w_sum += solid_angle(a, b, c);
    }

    return w_sum * kInvFourPiF;
}

} // namespace crd::geometry::mesh
