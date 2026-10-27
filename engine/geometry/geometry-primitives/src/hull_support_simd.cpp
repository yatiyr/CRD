// crd-geometry-primitives — the batched hull support (v2h): one call of the level's scan
// (crd/geometry/primitives/primitive_kernels.hpp) over the hull's padded coordinate columns, eight vertices a step.
// The scan keeps the strictly greater projection, so an exact tie goes to the lower index, as the linear scan
// `support(hull, dir)` decides; the padded lanes repeat vertex 0 and lose to it.

#include <crd/core/assert.hpp>
#include <crd/geometry/primitives/primitive_dispatch.hpp>
#include <crd/geometry/primitives/primitives.hpp>

namespace crd::geometry::primitives
{

SupportPoint<crd::f32> support_simd_f32(const ConvexHullView<crd::f32>& hull, const Vec3<crd::f32>& dir) noexcept
{
    CRD_ASSERT(!hull.vertices.empty());
    CRD_ASSERT(!hull.vx_soa.empty());
    CRD_ASSERT(hull.vx_soa.size() == hull.vy_soa.size());
    CRD_ASSERT(hull.vx_soa.size() == hull.vz_soa.size());
    const crd::usize padded = hull.vx_soa.size();
    CRD_ASSERT(padded >= hull.vertices.size());
    CRD_ASSERT((padded % kBatchLanes) == 0U); // the padding contract
    const crd::u32 best = primitive_kernels().table->hull_support(hull.vx_soa.data(), hull.vy_soa.data(),
                                                                  hull.vz_soa.data(), padded, dir.x, dir.y, dir.z);
    CRD_ASSERT(best < hull.vertices.size());
    return SupportPoint<crd::f32>{hull.vertices[best], best};
}

} // namespace crd::geometry::primitives
