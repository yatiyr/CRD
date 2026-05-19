// Sandbox showcase automated coverage — Phase 3.1.7 v1-close debt payment.
//
// `render_geometry_showcase(state, buf, alloc, cache)` and
// `render_draw_showcase(state, buf)` are called for every mode + every
// per-mode parameter permutation that has a single dominant axis (selected
// primitive type, selected query type, selected tree kind, selected SDF
// kind). Each call must emit a non-zero line count into a TLSF-backed
// RenderBuffer — the v1j-b smoke only confirmed the Physics-default boot
// path didn't crash; this binary closes the zero-automation gap on the
// showcase render paths advisor flagged. Tests also pin the
// BvhViewerCache's fingerprint-based rebuild invariant.

#include "curves_showcase.hpp"
#include "geometry_showcase.hpp"

#include <crd/draw/render_buffer.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::sandbox::BvhViewerCache;
using crd::sandbox::GeometryShowcaseMode;
using crd::sandbox::GeometryShowcaseState;
using crd::sandbox::render_draw_showcase;
using crd::sandbox::render_geometry_showcase;
using crd::sandbox::SandboxScene;
using crd::sandbox::ShowcasePrimitive;
using crd::sandbox::ShowcaseQuery;
using crd::sandbox::ShowcaseSdfKind;
using crd::sandbox::ShowcaseTreeKind;
} // namespace

TEST_CASE("showcase: every primitive renders into a non-empty buffer", "[sandbox][showcase]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "showcase-test");
    BvhViewerCache cache(&alloc);
    crd::draw::RenderBuffer buf(&alloc);

    const ShowcasePrimitive prims[] = {
        ShowcasePrimitive::Sphere,   ShowcasePrimitive::Aabb,        ShowcasePrimitive::Obb,
        ShowcasePrimitive::Capsule,  ShowcasePrimitive::Cylinder,    ShowcasePrimitive::Plane,
        ShowcasePrimitive::Triangle, ShowcasePrimitive::Tetrahedron, ShowcasePrimitive::Frustum,
        ShowcasePrimitive::Ray,      ShowcasePrimitive::Segment,
    };
    for (const auto p : prims)
    {
        GeometryShowcaseState s;
        s.mode = GeometryShowcaseMode::PrimitiveViewer;
        s.primitive = p;
        buf.clear();
        render_geometry_showcase(s, buf, alloc, cache);
        INFO("primitive index=" << static_cast<int>(p));
        REQUIRE(buf.line_count() > 0U);
    }
}

TEST_CASE("showcase: every query mode renders into a non-empty buffer", "[sandbox][showcase]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "showcase-test");
    BvhViewerCache cache(&alloc);
    crd::draw::RenderBuffer buf(&alloc);

    const ShowcaseQuery queries[] = {ShowcaseQuery::Raycast, ShowcaseQuery::Overlap, ShowcaseQuery::ClosestPoint,
                                     ShowcaseQuery::SphereCast};
    for (const auto q : queries)
    {
        GeometryShowcaseState s;
        s.mode = GeometryShowcaseMode::QueryShowcase;
        s.qs_mode = q;
        buf.clear();
        render_geometry_showcase(s, buf, alloc, cache);
        INFO("query=" << static_cast<int>(q));
        // Backdrop AABBs alone are 8 prims × 12 edges = 96 lines minimum.
        REQUIRE(buf.line_count() >= 12U);
    }
}

TEST_CASE("showcase: every BVH tree kind renders + cache reuses across frames", "[sandbox][showcase]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 24, nullptr, "showcase-test"); // 16 MB
    BvhViewerCache cache(&alloc);
    crd::draw::RenderBuffer buf(&alloc);

    const ShowcaseTreeKind kinds[] = {ShowcaseTreeKind::Binary, ShowcaseTreeKind::Quad, ShowcaseTreeKind::Dynamic};
    for (const auto k : kinds)
    {
        GeometryShowcaseState s;
        s.mode = GeometryShowcaseMode::BvhViewer;
        s.bv_tree_kind = k;
        s.bv_n = 60;
        buf.clear();
        render_geometry_showcase(s, buf, alloc, cache);
        INFO("tree_kind=" << static_cast<int>(k));
        REQUIRE(buf.line_count() > 0U);

        // After first build, fingerprint is set. A second call with the
        // same state must reuse the cache (fingerprint unchanged); we can't
        // observe the rebuild count from the outside, but we can verify
        // that the second pass produces the same line count (deterministic
        // walk over the same tree).
        const crd::u64 fp_after_first = cache.fingerprint;
        REQUIRE(fp_after_first != 0U);
        const crd::usize first_count = buf.line_count();
        buf.clear();
        render_geometry_showcase(s, buf, alloc, cache);
        REQUIRE(cache.fingerprint == fp_after_first); // same params -> cache hit
        REQUIRE(buf.line_count() == first_count);

        // Mutating a slider must force a fingerprint change.
        s.bv_seed += 1;
        buf.clear();
        render_geometry_showcase(s, buf, alloc, cache);
        REQUIRE(cache.fingerprint != fp_after_first);
    }
}

TEST_CASE("showcase: every SDF kind emits cross samples within the band", "[sandbox][showcase]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "showcase-test");
    BvhViewerCache cache(&alloc);
    crd::draw::RenderBuffer buf(&alloc);

    const ShowcaseSdfKind kinds[] = {
        ShowcaseSdfKind::Sphere, ShowcaseSdfKind::Box,        ShowcaseSdfKind::RoundBox,
        ShowcaseSdfKind::Torus,  ShowcaseSdfKind::Octahedron, ShowcaseSdfKind::Capsule,
        ShowcaseSdfKind::Cone,   ShowcaseSdfKind::BoxFrame,   ShowcaseSdfKind::Cylinder,
    };
    for (const auto k : kinds)
    {
        GeometryShowcaseState s;
        s.mode = GeometryShowcaseMode::SdfHeatmap;
        s.sdf_kind = k;
        s.sdf_grid_res = 8; // 8^3 = 512 samples, fast
        buf.clear();
        render_geometry_showcase(s, buf, alloc, cache);
        INFO("sdf_kind=" << static_cast<int>(k));
        // Each kept sample emits 3 lines (cross_3d). With max_distance=1.5
        // band-pass we expect at least a few hundred samples around the
        // surface; pin a generous floor.
        REQUIRE(buf.line_count() >= 30U);
    }
}

TEST_CASE("showcase: render_draw_showcase emits the historic d0d demo primitives", "[sandbox][showcase][draw-api]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "showcase-test");
    crd::draw::RenderBuffer buf(&alloc);
    GeometryShowcaseState s;
    render_draw_showcase(s, buf);
    // Axis triad (3) + box wire (12) + sphere wire (varies) + capsule wire
    // (varies) + arrow (>=3) + cross (3) + arc (segments=32) = well into the
    // 50+ line range. Plus 3 solid wire/solid triangle batches.
    REQUIRE(buf.line_count() > 30U);
    REQUIRE(buf.triangle_count() > 0U); // the *_solid variants emit triangles
}

TEST_CASE("showcase: line_width is threaded into emissions", "[sandbox][showcase]")
{
    // Smoke-only: switching line_width doesn't crash and doesn't change
    // line_count (width is a per-emission attribute, not a primitive count).
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 18, nullptr, "showcase-test");
    BvhViewerCache cache(&alloc);
    crd::draw::RenderBuffer buf(&alloc);
    GeometryShowcaseState s;
    s.mode = GeometryShowcaseMode::PrimitiveViewer;
    s.primitive = ShowcasePrimitive::Aabb;
    s.line_width = 1.0F;
    buf.clear();
    render_geometry_showcase(s, buf, alloc, cache);
    const crd::usize n_at_1 = buf.line_count();

    s.line_width = 6.0F;
    buf.clear();
    render_geometry_showcase(s, buf, alloc, cache);
    const crd::usize n_at_6 = buf.line_count();

    REQUIRE(n_at_1 == n_at_6);
    REQUIRE(n_at_1 > 0U);
    // Spot-check the width landed on at least one line in the buffer.
    REQUIRE(buf.line_count() > 0U);
    const auto lines = buf.lines();
    REQUIRE(lines[0].width == 6.0F);
}

TEST_CASE("showcase: SandboxScene enum values are stable", "[sandbox][showcase]")
{
    // ABI pin -- the dropdown indices in the sandbox UI rely on these.
    static_assert(static_cast<int>(SandboxScene::Physics) == 0);
    static_assert(static_cast<int>(SandboxScene::GeometryViz) == 1);
    static_assert(static_cast<int>(SandboxScene::DrawShowcase) == 2);
    static_assert(static_cast<int>(SandboxScene::CurvesShowcase) == 3);
    REQUIRE(true);
}

// v10e curves showcase coverage.
TEST_CASE("showcase: every curve kind renders into a non-empty buffer", "[sandbox][showcase][curves]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "showcase-test");
    crd::draw::RenderBuffer    buf(&alloc);
    using crd::sandbox::CurvesShowcaseState;
    using crd::sandbox::render_curves_showcase;
    using crd::sandbox::ShowcaseCurveKind;
    const ShowcaseCurveKind kinds[] = {
        ShowcaseCurveKind::Polyline,    ShowcaseCurveKind::QuadBezier, ShowcaseCurveKind::CubicBezier,
        ShowcaseCurveKind::CubicHermite, ShowcaseCurveKind::CatmullRom, ShowcaseCurveKind::BSpline,
        ShowcaseCurveKind::CircularArc, ShowcaseCurveKind::EllipseArc, ShowcaseCurveKind::Helix,
    };
    for (const auto k : kinds)
    {
        CurvesShowcaseState s;
        s.kind = k;
        buf.clear();
        render_curves_showcase(s, buf, alloc);
        INFO("curve kind=" << static_cast<int>(k));
        REQUIRE(buf.line_count() > 0U);
    }
}

TEST_CASE("showcase: frame mode toggle adds 3*N lines worth of hairs",
          "[sandbox][showcase][curves]")
{
    crd::memory::TlsfAllocator alloc(crd::usize{1} << 22, nullptr, "showcase-test");
    crd::draw::RenderBuffer    buf(&alloc);
    using crd::sandbox::CurvesShowcaseState;
    using crd::sandbox::render_curves_showcase;
    using crd::sandbox::ShowcaseFrameMode;

    CurvesShowcaseState s;
    s.frame_mode = ShowcaseFrameMode::Off;
    s.show_control_points = false;
    s.n_samples           = 16U;
    buf.clear();
    render_curves_showcase(s, buf, alloc);
    const crd::usize n_off = buf.line_count();

    s.frame_mode = ShowcaseFrameMode::Frenet;
    buf.clear();
    render_curves_showcase(s, buf, alloc);
    const crd::usize n_frenet = buf.line_count();

    s.frame_mode = ShowcaseFrameMode::Rmf;
    buf.clear();
    render_curves_showcase(s, buf, alloc);
    const crd::usize n_rmf = buf.line_count();

    // Open CubicBezier with n_samples=16 -> 17 frames -> 51 hair lines.
    REQUIRE(n_frenet >= n_off + 3U * 17U);
    REQUIRE(n_rmf    >= n_off + 3U * 17U);
}
