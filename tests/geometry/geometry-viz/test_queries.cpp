// crd-geometry-viz v1j-a — query-result visualisation tests.

#include <crd/containers/array.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/geometry/viz/queries.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::f32;
using crd::usize;
using crd::draw::RenderBuffer;
using crd::geometry::primitives::Ray3;
using crd::math::Vec3f;
namespace viz = crd::geometry::viz;
} // namespace

TEST_CASE("viz::draw_ray_hit without normal: ray segment + small cross", "[geometry][viz][queries]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw_ray_hit(buf, Ray3<f32>(Vec3f(0, 0, 0), Vec3f(1, 0, 0)), 5.0F);
    // 1 ray segment + 3 cross axes (cross_3d_to emits one line per axis).
    REQUIRE(buf.line_count() == 4U);
}

TEST_CASE("viz::draw_ray_hit with normal: ray + cross + arrow", "[geometry][viz][queries]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw_ray_hit(buf, Ray3<f32>(Vec3f(0, 0, 0), Vec3f(1, 0, 0)), 5.0F, Vec3f(0, 1, 0), 0.5F);
    // Ray (1) + cross (3) + arrow (arrow_to emits >= 1 line for the shaft + heads).
    REQUIRE(buf.line_count() > 4U);
}

TEST_CASE("viz::draw_closest_point: segment + endpoints", "[geometry][viz][queries]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw_closest_point(buf, Vec3f(0, 0, 0), Vec3f(3, 4, 0));
    // 1 query→closest segment + 3 cross axes at `query`.
    REQUIRE(buf.line_count() == 4U);
    // 1 point at `closest`.
    REQUIRE(buf.point_count() == 1U);
}

TEST_CASE("viz::draw_normals: one arrow per point", "[geometry][viz][queries]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    const Vec3f pts[] = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(2, 0, 0)};
    const Vec3f nrm[] = {Vec3f(0, 1, 0), Vec3f(0, 1, 0), Vec3f(0, 1, 0)};
    viz::draw_normals(buf, crd::containers::ConstSpan<Vec3f>(pts, 3), crd::containers::ConstSpan<Vec3f>(nrm, 3));
    // Each `arrow_to` adds ≥ 1 line; with 3 normals we expect at least 3.
    REQUIRE(buf.line_count() >= 3U);
}

TEST_CASE("viz::draw_normals: empty input produces no output", "[geometry][viz][queries]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw_normals(buf, crd::containers::ConstSpan<Vec3f>(), crd::containers::ConstSpan<Vec3f>());
    REQUIRE(buf.line_count() == 0U);
    REQUIRE(buf.point_count() == 0U);
}
