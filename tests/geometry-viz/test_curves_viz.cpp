// crd-geometry-viz v10e -- curve adapter tests.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/geometry/curves/curves.hpp>
#include <crd/geometry/viz/curves.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::f32;
using crd::usize;
using crd::draw::RenderBuffer;
using crd::math::Vec3f;
namespace viz    = crd::geometry::viz;
namespace curves = crd::geometry::curves;
} // namespace

TEST_CASE("viz::draw_polyline open: emits n-1 lines", "[geometry][viz][curves]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);

    Vec3f pts[] = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(2, 0, 0), Vec3f(3, 0, 0)};
    curves::Polyline3View<f32> poly{crd::containers::ConstSpan<Vec3f>(pts, 4), false};
    viz::draw_polyline(buf, poly);
    REQUIRE(buf.line_count() == 3U);
}

TEST_CASE("viz::draw_polyline closed: emits n lines", "[geometry][viz][curves]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);

    Vec3f pts[] = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0)};
    curves::Polyline3View<f32> poly{crd::containers::ConstSpan<Vec3f>(pts, 4), true};
    viz::draw_polyline(buf, poly);
    REQUIRE(buf.line_count() == 4U);
}

TEST_CASE("viz::draw_curve samples curve into n_segments lines", "[geometry][viz][curves]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);

    curves::CubicBezier3<f32> b{Vec3f(0, 0, 0), Vec3f(1, 1, 0), Vec3f(2, -1, 0), Vec3f(3, 0, 0)};
    viz::draw_curve(buf, b, 16U, &alloc);
    // Open Bezier => 16 segments == 17 sampled points => 16 line emissions.
    REQUIRE(buf.line_count() == 16U);
}

TEST_CASE("viz::draw_tangent_frame emits 3 lines per sample", "[geometry][viz][curves]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);

    curves::CircularArc3<f32> arc{};
    arc.center        = Vec3f(0, 0, 0);
    arc.axis_u        = Vec3f(1, 0, 0);
    arc.axis_v        = Vec3f(0, 0, 1);
    arc.radius        = 1.0F;
    arc.sweep_radians = 3.14159265F;
    arc.closed        = false;

    viz::draw_tangent_frame(buf, arc, 8U, 0.2F);
    // Open: n_samples + 1 = 9 frames, 3 lines each = 27.
    REQUIRE(buf.line_count() == 27U);
}

TEST_CASE("viz::draw_rmf emits 3 lines per frame", "[geometry][viz][curves]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "viz-test");
    RenderBuffer buf(&alloc);

    curves::CurveFrame<f32> frames[] = {
        {Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1)},
        {Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1)},
        {Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1)},
    };
    Vec3f points[] = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(2, 0, 0)};
    viz::draw_rmf(buf, crd::containers::ConstSpan<curves::CurveFrame<f32>>(frames, 3),
                  crd::containers::ConstSpan<Vec3f>(points, 3), 0.5F);
    REQUIRE(buf.line_count() == 9U);
}
