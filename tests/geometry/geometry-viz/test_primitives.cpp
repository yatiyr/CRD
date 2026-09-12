// crd-geometry-viz v1j-a — primitive adapter tests.
//
// Each overload of `draw(RenderBuffer&, Shape, ...)` must emit *some* lines
// for a well-formed input (zero is a regression — the shape didn't render).
// For shapes with a known fixed line count (AABB = 12 edges, Triangle = 3
// edges, Tetrahedron = 6 edges), pin the exact count. For shapes with
// segment/ring counts dependent on the (lat, lon, segments) parameters
// (Sphere, Capsule), assert a non-zero line count and roughly the expected
// order of magnitude.

#include <crd/draw/render_buffer.hpp>
#include <crd/geometry/viz/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::f32;
using crd::usize;
using crd::draw::RenderBuffer;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::Cylinder3;
using crd::geometry::primitives::Frustum;
using crd::geometry::primitives::Line3;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::Segment3;
using crd::geometry::primitives::Sphere;
using crd::geometry::primitives::Tetrahedron;
using crd::geometry::primitives::Triangle3;
using crd::math::Mat3f;
using crd::math::Vec3f;
namespace viz = crd::geometry::viz;
} // namespace

TEST_CASE("viz::draw(AABB3) emits exactly 12 edges", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, AABB3<f32>(Vec3f(0, 0, 0), Vec3f(1, 1, 1)));
    REQUIRE(buf.line_count() == 12U);
}

TEST_CASE("viz::draw(OBB3) emits exactly 12 edges with rotation", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    OBB3<f32> obb(Vec3f(5, 0, 0), Vec3f(1, 1, 1), Mat3f::identity());
    viz::draw(buf, obb);
    REQUIRE(buf.line_count() == 12U);
}

TEST_CASE("viz::draw(Sphere) emits a non-trivial wireframe", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Sphere<f32>(Vec3f(0, 0, 0), 1.0F));
    REQUIRE(buf.line_count() > 0U);
}

TEST_CASE("viz::draw(Capsule3) emits a non-trivial wireframe", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Capsule3<f32>(Vec3f(0, 0, 0), Vec3f(0, 2, 0), 0.5F));
    REQUIRE(buf.line_count() > 0U);
}

TEST_CASE("viz::draw(Cylinder3) emits a non-trivial wireframe", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Cylinder3<f32>(Vec3f(0, 0, 0), Vec3f(0, 2, 0), 0.5F));
    REQUIRE(buf.line_count() > 0U);
}

TEST_CASE("viz::draw(Triangle3) emits exactly 3 edges", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Triangle3<f32>(Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0)));
    REQUIRE(buf.line_count() == 3U);
}

TEST_CASE("viz::draw(Tetrahedron) emits exactly 6 edges", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Tetrahedron<f32>(Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1)));
    REQUIRE(buf.line_count() == 6U);
}

TEST_CASE("viz::draw(Plane patch) emits grid_divisions*2 + 2 lines", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Plane<f32>(Vec3f(0, 1, 0), 0.0F), Vec3f(0, 0, 0), 4.0F, 4U);
    // 4 grid divisions -> 5 lines per direction × 2 directions = 10 lines.
    REQUIRE(buf.line_count() == 10U);
}

TEST_CASE("viz::draw(Plane) degenerate-normal yields no output", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Plane<f32>(Vec3f(0, 0, 0), 0.0F));
    REQUIRE(buf.line_count() == 0U);
}

TEST_CASE("viz::draw(Ray3) emits one segment", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Ray3<f32>(Vec3f(0, 0, 0), Vec3f(1, 0, 0)), 5.0F);
    REQUIRE(buf.line_count() == 1U);
}

TEST_CASE("viz::draw(Segment3) emits one segment", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Segment3<f32>(Vec3f(0, 0, 0), Vec3f(1, 0, 0)));
    REQUIRE(buf.line_count() == 1U);
}

TEST_CASE("viz::draw(Line3) emits one centred segment", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    viz::draw(buf, Line3<f32>(Vec3f(0, 0, 0), Vec3f(1, 0, 0)), 10.0F);
    REQUIRE(buf.line_count() == 1U);
}

TEST_CASE("viz::draw(Frustum) emits 12 edges from 6 planes", "[geometry][viz][primitives]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);
    // Hand-build a unit-cube frustum: 6 axis-aligned planes facing inward.
    Frustum<f32> f;
    f.planes[0] = Plane<f32>(Vec3f(1, 0, 0), 1.0F);   // left:   x = -1, normal +x, d = +1
    f.planes[1] = Plane<f32>(Vec3f(-1, 0, 0), 1.0F);  // right:  x = +1, normal -x, d = +1
    f.planes[2] = Plane<f32>(Vec3f(0, 1, 0), 1.0F);   // bottom: y = -1
    f.planes[3] = Plane<f32>(Vec3f(0, -1, 0), 1.0F);  // top:    y = +1
    f.planes[4] = Plane<f32>(Vec3f(0, 0, 1), 1.0F);   // near:   z = -1
    f.planes[5] = Plane<f32>(Vec3f(0, 0, -1), 1.0F);  // far:    z = +1
    viz::draw(buf, f);
    REQUIRE(buf.line_count() == 12U);
}
