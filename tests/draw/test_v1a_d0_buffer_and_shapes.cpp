// Phase 3.1 v1a-draw d0 — RenderBuffer + shape-generator tests.
//
// Covers:
//   - Color packing (RGBA8 layout)
//   - PrimFlags packing/unpacking (depth + category + width unit + picking)
//   - DebugPoint / DebugLine / DebugTriangle layout pins (ADR-0066 §5.1)
//   - RenderBuffer add / clear / append / shift / capacity hints
//   - box_wire_to: 12 lines, correct corner positions
//   - aabb_wire_to: midpoint + half-extent decomposition matches box_wire_to

#include <catch2/catch_test_macros.hpp>

#include <crd/draw/draw.hpp>

#include <type_traits>

using namespace crd::draw;

// ===========================================================================
// API freeze pins (compile-time)
// ===========================================================================

TEST_CASE("v1a-draw d0 Color packs to 4 bytes RGBA layout", "[draw][v1a-draw][freeze]")
{
    STATIC_REQUIRE(sizeof(Color) == 4);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Color>);

    constexpr Color kCol{0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE(kCol.r == 0xDE);
    REQUIRE(kCol.g == 0xAD);
    REQUIRE(kCol.b == 0xBE);
    REQUIRE(kCol.a == 0xEF);

    // Packed layout: alpha in high byte, then b, g, r (matches LittleEndian
    // RGBA8 GPU upload semantics).
    REQUIRE(kCol.packed_rgba() == 0xEFBEADDEU);

    // Named constants are sane.
    REQUIRE(kRed.packed_rgba()   == 0xFF0000FFU);  // a=ff, b=00, g=00, r=ff
    REQUIRE(kGreen.packed_rgba() == 0xFF00FF00U);
    REQUIRE(kBlue.packed_rgba()  == 0xFFFF0000U);
}

TEST_CASE("v1a-draw d0 PrimFlags pack/unpack round-trips", "[draw][v1a-draw][freeze]")
{
    STATIC_REQUIRE(sizeof(PrimFlags) == 4);

    const auto flags = PrimFlags::make(DepthMode::XRay, Category::Physics,
                                       /*width_in_world_units=*/true, /*picking=*/0xBEEF);
    REQUIRE(flags.depth()                == DepthMode::XRay);
    REQUIRE(flags.category()             == Category::Physics);
    REQUIRE(flags.width_in_world_units() == true);
    REQUIRE(flags.picking_id()           == 0xBEEF);

    // Default flags = always-visible, debug category, pixel widths, no picking.
    // d0d-fix flipped the default depth mode from Test to Always so debug
    // primitives are visible by default; consumers opt INTO depth-test or XRay.
    REQUIRE(kDefaultFlags.depth()                == DepthMode::Always);
    REQUIRE(kDefaultFlags.category()             == Category::Debug);
    REQUIRE(kDefaultFlags.width_in_world_units() == false);
    REQUIRE(kDefaultFlags.picking_id()           == 0);
}

TEST_CASE("v1a-draw d0 DepthMode/Category enum values are pinned", "[draw][v1a-draw][freeze]")
{
    STATIC_REQUIRE(static_cast<int>(DepthMode::Test)   == 0);
    STATIC_REQUIRE(static_cast<int>(DepthMode::Always) == 1);
    STATIC_REQUIRE(static_cast<int>(DepthMode::XRay)   == 2);

    STATIC_REQUIRE(static_cast<int>(Category::Physics)  == 0);
    STATIC_REQUIRE(static_cast<int>(Category::Audio)    == 1);
    STATIC_REQUIRE(static_cast<int>(Category::Sdf)      == 2);
    STATIC_REQUIRE(static_cast<int>(Category::Nav)      == 3);
    STATIC_REQUIRE(static_cast<int>(Category::Scene)    == 4);
    STATIC_REQUIRE(static_cast<int>(Category::Renderer) == 5);
    STATIC_REQUIRE(static_cast<int>(Category::Debug)    == 9);
}

TEST_CASE("v1a-draw d0 primitive record layouts are pinned", "[draw][v1a-draw][freeze]")
{
    STATIC_REQUIRE(sizeof(DebugPoint)    == 28);
    STATIC_REQUIRE(sizeof(DebugLine)     == 40);
    STATIC_REQUIRE(sizeof(DebugTriangle) == 48);
    STATIC_REQUIRE(std::is_trivially_copyable_v<DebugPoint>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<DebugLine>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<DebugTriangle>);
}

// ===========================================================================
// RenderBuffer behaviour
// ===========================================================================

TEST_CASE("v1a-draw d0 RenderBuffer empty/clear basics", "[draw][v1a-draw][buffer]")
{
    RenderBuffer buf;
    REQUIRE(buf.empty());
    REQUIRE(buf.point_count()    == 0);
    REQUIRE(buf.line_count()     == 0);
    REQUIRE(buf.triangle_count() == 0);

    add_line_to(buf, {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, kRed);
    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.line_count() == 1);

    buf.clear();
    REQUIRE(buf.empty());
    REQUIRE(buf.line_count() == 0);
}

TEST_CASE("v1a-draw d0 RenderBuffer append merges in order", "[draw][v1a-draw][buffer]")
{
    RenderBuffer a;
    RenderBuffer b;

    add_line_to(a, {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, kRed);
    add_line_to(b, {2.0F, 0.0F, 0.0F}, {3.0F, 0.0F, 0.0F}, kGreen);
    add_line_to(b, {4.0F, 0.0F, 0.0F}, {5.0F, 0.0F, 0.0F}, kBlue);

    a.append(b);
    REQUIRE(a.line_count() == 3);

    // Order is deterministic: a's existing lines first, then b's lines in
    // their order. Required for replay reproducibility.
    REQUIRE(a.lines()[0].a.x == 0.0F);
    REQUIRE(a.lines()[1].a.x == 2.0F);
    REQUIRE(a.lines()[2].a.x == 4.0F);
}

TEST_CASE("v1a-draw d0 RenderBuffer shift translates every primitive", "[draw][v1a-draw][buffer]")
{
    RenderBuffer buf;
    add_line_to(buf, {1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}, kRed);

    buf.shift({100.0F, 0.0F, 0.0F});
    REQUIRE(buf.lines()[0].a.x == 101.0F);
    REQUIRE(buf.lines()[0].b.x == 104.0F);
    REQUIRE(buf.lines()[0].a.y == 2.0F);   // y, z unchanged
    REQUIRE(buf.lines()[0].b.z == 6.0F);
}

// ===========================================================================
// Shape generators
// ===========================================================================

TEST_CASE("v1a-draw d0 box_wire_to emits 12 edges", "[draw][v1a-draw][shapes]")
{
    RenderBuffer buf;

    // Identity transform, half_extents = (1,1,1) -> unit cube [-1, +1]^3.
    box_wire_to(buf, crd::math::Mat4f::identity(), {1.0F, 1.0F, 1.0F}, kCyan);

    REQUIRE(buf.line_count() == 12);

    // Every line must be unit-length OR sqrt(0) (no degenerate edges).
    // The cube has 12 unit-length edges; the symmetric layout means we
    // should see 4 along each axis after axis-aligned grouping.
    crd::usize axis_x_edges = 0;
    crd::usize axis_y_edges = 0;
    crd::usize axis_z_edges = 0;
    for (const auto& l : buf.lines())
    {
        const crd::math::Vec3f d{l.b.x - l.a.x, l.b.y - l.a.y, l.b.z - l.a.z};
        if      (std::abs(d.x) > 0.5F && std::abs(d.y) < 1e-5F && std::abs(d.z) < 1e-5F) ++axis_x_edges;
        else if (std::abs(d.y) > 0.5F && std::abs(d.x) < 1e-5F && std::abs(d.z) < 1e-5F) ++axis_y_edges;
        else if (std::abs(d.z) > 0.5F && std::abs(d.x) < 1e-5F && std::abs(d.y) < 1e-5F) ++axis_z_edges;
    }
    REQUIRE(axis_x_edges == 4);
    REQUIRE(axis_y_edges == 4);
    REQUIRE(axis_z_edges == 4);

    // Color packing propagates through.
    REQUIRE(buf.lines()[0].color == kCyan.packed_rgba());
}

TEST_CASE("v1a-draw d0 box_wire_to scales by half_extents", "[draw][v1a-draw][shapes]")
{
    RenderBuffer buf;
    box_wire_to(buf, crd::math::Mat4f::identity(), {2.0F, 3.0F, 5.0F}, kWhite);

    // Find the longest edge along each axis. With half_extents = (2, 3, 5),
    // edge lengths should be 4, 6, 10 along x, y, z respectively.
    crd::f32 max_x = 0.0F;
    crd::f32 max_y = 0.0F;
    crd::f32 max_z = 0.0F;
    for (const auto& l : buf.lines())
    {
        max_x = std::max(max_x, std::abs(l.b.x - l.a.x));
        max_y = std::max(max_y, std::abs(l.b.y - l.a.y));
        max_z = std::max(max_z, std::abs(l.b.z - l.a.z));
    }
    REQUIRE(max_x == 4.0F);
    REQUIRE(max_y == 6.0F);
    REQUIRE(max_z == 10.0F);
}

TEST_CASE("v1a-draw d0 aabb_wire_to is equivalent to box_wire_to with mid+half_extents",
          "[draw][v1a-draw][shapes]")
{
    RenderBuffer aabb_buf;
    RenderBuffer box_buf;

    // AABB from (1, 2, 3) to (5, 8, 13). Mid = (3, 5, 8); half = (2, 3, 5).
    aabb_wire_to(aabb_buf, {1.0F, 2.0F, 3.0F}, {5.0F, 8.0F, 13.0F}, kAabb);

    crd::math::Mat4f world = crd::math::Mat4f::identity();
    world.c3.x = 3.0F;
    world.c3.y = 5.0F;
    world.c3.z = 8.0F;
    box_wire_to(box_buf, world, {2.0F, 3.0F, 5.0F}, kAabb);

    REQUIRE(aabb_buf.line_count() == 12);
    REQUIRE(box_buf.line_count()  == 12);

    // Bit-exact line equivalence (packed colors + endpoints).
    for (crd::usize i = 0; i < 12; ++i)
    {
        REQUIRE(aabb_buf.lines()[i].a.x  == box_buf.lines()[i].a.x);
        REQUIRE(aabb_buf.lines()[i].a.y  == box_buf.lines()[i].a.y);
        REQUIRE(aabb_buf.lines()[i].a.z  == box_buf.lines()[i].a.z);
        REQUIRE(aabb_buf.lines()[i].b.x  == box_buf.lines()[i].b.x);
        REQUIRE(aabb_buf.lines()[i].b.y  == box_buf.lines()[i].b.y);
        REQUIRE(aabb_buf.lines()[i].b.z  == box_buf.lines()[i].b.z);
        REQUIRE(aabb_buf.lines()[i].color == box_buf.lines()[i].color);
    }
}

TEST_CASE("v1a-draw d0 add_line_to + add_point_to round-trip flags + lifetime",
          "[draw][v1a-draw][shapes]")
{
    RenderBuffer buf;

    const auto flags = PrimFlags::make(DepthMode::Always, Category::Physics,
                                       /*width_in_world_units=*/true, /*picking=*/42);
    add_line_to(buf, {0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, kRed,
                /*width_px=*/2.5F, flags, /*lifetime_s=*/0.75F);
    add_point_to(buf, {2.0F, 0.0F, 0.0F}, kGreen, /*size_px=*/8.0F, flags, /*lifetime_s=*/1.5F);

    REQUIRE(buf.lines()[0].width                  == 2.5F);
    REQUIRE(buf.lines()[0].lifetime_s             == 0.75F);
    REQUIRE(buf.lines()[0].flags.depth()          == DepthMode::Always);
    REQUIRE(buf.lines()[0].flags.category()       == Category::Physics);
    REQUIRE(buf.lines()[0].flags.picking_id()     == 42);

    REQUIRE(buf.points()[0].size_px               == 8.0F);
    REQUIRE(buf.points()[0].lifetime_s            == 1.5F);
    REQUIRE(buf.points()[0].flags.picking_id()    == 42);
}

TEST_CASE("v1a-draw d0 RenderBuffer reserve hints don't break empty()", "[draw][v1a-draw][buffer]")
{
    RenderBuffer buf;
    buf.reserve_lines(1024);
    buf.reserve_triangles(256);
    buf.reserve_points(512);
    REQUIRE(buf.empty()); // reserves capacity, not size
    REQUIRE(buf.line_count() == 0);
}
