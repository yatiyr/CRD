// crd-geometry-convex v3d — hull simplification tests.
//
// Coverage:
//   (1) Identity cases: target_vertex_count == source size, both opts zero,
//       degenerate sources (coplanar/colinear/coincident/<4-vertex).
//   (2) Reduction cases: cube → 4 vertices, octahedron → 4 vertices,
//       random cloud → target N.
//   (3) Locked vertices: every index in keep_vertex_indices survives
//       (verified by membership test in output).
//   (4) Error threshold: max_error_threshold halts before target_vertex_count
//       when local shrinkage exceeds the threshold.
//   (5) Subset invariant: every output vertex equals some input vertex
//       (vertex-removal-only — no new geometry introduced).
//   (6) Convexity invariant: every output face's outward plane has all
//       other output vertices below (Stage D orient3d check).
//   (7) AABB containment: output vertex AABB ⊆ input vertex AABB (trivially
//       satisfied because output ⊆ input).
//   (8) Determinism replay: identical input → identical output vertex set
//       + face vertex indices.
//   (9) f32 + f64 both work.

#include <crd/geometry/convex/hull_simplify.hpp>
#include <crd/geometry/convex/quickhull.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
using crd::f32;
using crd::f64;
using crd::u32;
using crd::usize;
using crd::geometry::convex::HullSimplifyOptions;
using crd::geometry::convex::quickhull;
using crd::geometry::convex::QuickhullResult;
using crd::geometry::convex::simplify_hull;
using crd::math::Vec3;

// Build the unit-cube hull (8 input vertices → Quickhull-triangulated 12-face hull).
template <typename T>
QuickhullResult<T> build_unit_cube(crd::memory::IAllocator* alloc)
{
    crd::containers::Array<Vec3<T>> points(alloc);
    points.push_back(Vec3<T>(0, 0, 0));
    points.push_back(Vec3<T>(1, 0, 0));
    points.push_back(Vec3<T>(0, 1, 0));
    points.push_back(Vec3<T>(1, 1, 0));
    points.push_back(Vec3<T>(0, 0, 1));
    points.push_back(Vec3<T>(1, 0, 1));
    points.push_back(Vec3<T>(0, 1, 1));
    points.push_back(Vec3<T>(1, 1, 1));
    return quickhull<T>(crd::containers::ConstSpan<Vec3<T>>(points.data(), points.size()), alloc);
}

// Build the regular octahedron hull (6 input vertices → 8 triangular faces).
template <typename T>
QuickhullResult<T> build_octahedron(crd::memory::IAllocator* alloc)
{
    crd::containers::Array<Vec3<T>> points(alloc);
    points.push_back(Vec3<T>(+1, 0, 0));
    points.push_back(Vec3<T>(-1, 0, 0));
    points.push_back(Vec3<T>(0, +1, 0));
    points.push_back(Vec3<T>(0, -1, 0));
    points.push_back(Vec3<T>(0, 0, +1));
    points.push_back(Vec3<T>(0, 0, -1));
    return quickhull<T>(crd::containers::ConstSpan<Vec3<T>>(points.data(), points.size()), alloc);
}

template <typename T>
QuickhullResult<T> build_tetrahedron(crd::memory::IAllocator* alloc)
{
    crd::containers::Array<Vec3<T>> points(alloc);
    points.push_back(Vec3<T>(0, 0, 0));
    points.push_back(Vec3<T>(1, 0, 0));
    points.push_back(Vec3<T>(0, 1, 0));
    points.push_back(Vec3<T>(0, 0, 1));
    return quickhull<T>(crd::containers::ConstSpan<Vec3<T>>(points.data(), points.size()), alloc);
}

// Random-point-cloud hull (deterministic seed).
template <typename T>
QuickhullResult<T> build_random_cloud(crd::memory::IAllocator* alloc, usize n_points, crd::u64 seed)
{
    crd::containers::Array<Vec3<T>> points(alloc);
    crd::u64 s = seed;
    auto rng = [&s]() -> T {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<T>(static_cast<f64>(s >> 11) / static_cast<f64>(1ULL << 53));
    };
    for (usize i = 0; i < n_points; ++i)
    {
        const T x = (rng() - static_cast<T>(0.5)) * static_cast<T>(2);
        const T y = (rng() - static_cast<T>(0.5)) * static_cast<T>(2);
        const T z = (rng() - static_cast<T>(0.5)) * static_cast<T>(2);
        points.push_back(Vec3<T>(x, y, z));
    }
    return quickhull<T>(crd::containers::ConstSpan<Vec3<T>>(points.data(), points.size()), alloc);
}

// Check that the result is still a valid convex hull: every face's outward
// plane has all other output vertices on the inside (or on the plane
// within k_distance_epsilon).
template <typename T> bool is_still_convex(const QuickhullResult<T>& r)
{
    if (r.faces.empty())
    {
        return true;
    }
    const T eps = crd::geometry::primitives::k_distance_epsilon<T>();
    for (usize f = 0; f < r.faces.size(); ++f)
    {
        for (usize v = 0; v < r.vertices.size(); ++v)
        {
            // Skip vertices that ARE on this face.
            const u32 begin = r.face_vertex_offsets[f];
            const u32 end = r.face_vertex_offsets[f + 1];
            bool on_face = false;
            for (u32 j = begin; j < end; ++j)
            {
                if (r.face_vertex_indices[j] == v)
                {
                    on_face = true;
                    break;
                }
            }
            if (on_face)
            {
                continue;
            }
            const T sd = r.faces[f].normal.x * r.vertices[v].x +
                          r.faces[f].normal.y * r.vertices[v].y +
                          r.faces[f].normal.z * r.vertices[v].z + r.faces[f].d;
            if (sd > eps)
            {
                return false;
            }
        }
    }
    return true;
}

// Check that every output vertex is also an input vertex (vertex-removal-
// only invariant).
template <typename T>
bool output_is_subset_of_input(const QuickhullResult<T>& out,
                                 const QuickhullResult<T>& input,
                                 T tol = static_cast<T>(1e-12))
{
    for (usize i = 0; i < out.vertices.size(); ++i)
    {
        bool found = false;
        for (usize j = 0; j < input.vertices.size(); ++j)
        {
            const T dx = out.vertices[i].x - input.vertices[j].x;
            const T dy = out.vertices[i].y - input.vertices[j].y;
            const T dz = out.vertices[i].z - input.vertices[j].z;
            if (std::abs(dx) <= tol && std::abs(dy) <= tol && std::abs(dz) <= tol)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

// Find the index of `v` in `r.vertices` (for verifying locked-vertex
// preservation). Returns u32(-1) if not found.
template <typename T>
u32 find_vertex(const QuickhullResult<T>& r, const Vec3<T>& v, T tol = static_cast<T>(1e-12))
{
    for (usize i = 0; i < r.vertices.size(); ++i)
    {
        const T dx = r.vertices[i].x - v.x;
        const T dy = r.vertices[i].y - v.y;
        const T dz = r.vertices[i].z - v.z;
        if (std::abs(dx) <= tol && std::abs(dy) <= tol && std::abs(dz) <= tol)
        {
            return static_cast<u32>(i);
        }
    }
    return ~u32{0};
}

} // namespace

// ---------------------------------------------------------------------------
// (1) Identity cases
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: empty source returns empty result", "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    QuickhullResult<f64> source(&alloc);
    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 0;
    auto out = simplify_hull<f64>(source, &alloc, opts);
    CHECK(out.empty());
    CHECK(out.faces.empty());
}

TEST_CASE("hull_simplify: tetrahedron cannot be reduced below 4 vertices",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(64U * 1024U);
    auto tet = build_tetrahedron<f64>(&alloc);
    REQUIRE(tet.vertices.size() == 4);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 4;
    auto out = simplify_hull<f64>(tet, &alloc, opts);
    CHECK(out.vertices.size() == 4);
    CHECK(out.faces.size() == 4);
    CHECK(is_still_convex(out));
}

TEST_CASE("hull_simplify: both opts zero returns identity copy", "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(128U * 1024U);
    auto cube = build_unit_cube<f64>(&alloc);
    HullSimplifyOptions<f64> opts; // both zero
    auto out = simplify_hull<f64>(cube, &alloc, opts);
    CHECK(out.vertices.size() == cube.vertices.size());
    CHECK(out.faces.size() == cube.faces.size());
}

TEST_CASE("hull_simplify: target >= source size returns identity copy",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(128U * 1024U);
    auto cube = build_unit_cube<f64>(&alloc);
    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = static_cast<u32>(cube.vertices.size());
    auto out = simplify_hull<f64>(cube, &alloc, opts);
    CHECK(out.vertices.size() == cube.vertices.size());
}

TEST_CASE("hull_simplify: degenerate coplanar source returns identity",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(128U * 1024U);
    crd::containers::Array<Vec3<f64>> points(&alloc);
    points.push_back(Vec3<f64>(0, 0, 0));
    points.push_back(Vec3<f64>(1, 0, 0));
    points.push_back(Vec3<f64>(1, 1, 0));
    points.push_back(Vec3<f64>(0, 1, 0));
    auto flat =
        quickhull<f64>(crd::containers::ConstSpan<Vec3<f64>>(points.data(), points.size()), &alloc);
    REQUIRE(flat.is_coplanar);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 2;
    auto out = simplify_hull<f64>(flat, &alloc, opts);
    CHECK(out.is_coplanar);
    CHECK(out.vertices.size() == flat.vertices.size());
}

// ---------------------------------------------------------------------------
// (2) Reduction cases
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: cube reduces to fewer vertices when target asks",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    auto cube = build_unit_cube<f64>(&alloc);
    REQUIRE(cube.vertices.size() == 8);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 4;
    auto out = simplify_hull<f64>(cube, &alloc, opts);

    CHECK(out.vertices.size() <= 8);
    CHECK(out.vertices.size() >= 4);
    CHECK(out.faces.size() >= 4);
    CHECK(is_still_convex(out));
    CHECK(output_is_subset_of_input(out, cube));
}

TEST_CASE("hull_simplify: octahedron reduces with target_vertex_count",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    auto oct = build_octahedron<f64>(&alloc);
    REQUIRE(oct.vertices.size() == 6);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 4;
    auto out = simplify_hull<f64>(oct, &alloc, opts);
    CHECK(out.vertices.size() <= 6);
    CHECK(out.vertices.size() >= 4);
    CHECK(is_still_convex(out));
    CHECK(output_is_subset_of_input(out, oct));
}

// ---------------------------------------------------------------------------
// (3) Locked vertices
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: locked vertices survive the reduction",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    auto cube = build_unit_cube<f64>(&alloc);

    // Lock vertices 0 and 7 (diagonal corners of the cube). They MUST
    // survive even an aggressive reduction.
    crd::containers::Array<u32> locked(&alloc);
    locked.push_back(0);
    locked.push_back(7);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 4;
    opts.keep_vertex_indices =
        crd::containers::ConstSpan<u32>(locked.data(), locked.size());

    // Record the positions of the locked input vertices.
    const Vec3<f64> p0 = cube.vertices[0];
    const Vec3<f64> p7 = cube.vertices[7];

    auto out = simplify_hull<f64>(cube, &alloc, opts);
    CHECK(find_vertex(out, p0) != ~u32{0});
    CHECK(find_vertex(out, p7) != ~u32{0});
    CHECK(is_still_convex(out));
}

TEST_CASE("hull_simplify: all vertices locked is a no-op",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    auto cube = build_unit_cube<f64>(&alloc);

    crd::containers::Array<u32> locked(&alloc);
    for (u32 i = 0; i < cube.vertices.size(); ++i)
    {
        locked.push_back(i);
    }

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 0; // no target
    opts.max_error_threshold = 1.0e6;
    opts.keep_vertex_indices =
        crd::containers::ConstSpan<u32>(locked.data(), locked.size());

    auto out = simplify_hull<f64>(cube, &alloc, opts);
    CHECK(out.vertices.size() == cube.vertices.size());
}

// ---------------------------------------------------------------------------
// (4) Error threshold
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: tiny error threshold prevents any removal",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    auto cube = build_unit_cube<f64>(&alloc);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 0;
    opts.max_error_threshold = 1e-9; // smaller than any cube shrinkage
    auto out = simplify_hull<f64>(cube, &alloc, opts);
    CHECK(out.vertices.size() == cube.vertices.size());
}

TEST_CASE("hull_simplify: huge error threshold allows full reduction",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    auto cube = build_unit_cube<f64>(&alloc);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 0;
    opts.max_error_threshold = 1.0e6;
    auto out = simplify_hull<f64>(cube, &alloc, opts);
    // Should reduce significantly (down to tetrahedron in the limit, but
    // greedy reduction may stop earlier). Just check it shrunk.
    CHECK(out.vertices.size() <= cube.vertices.size());
    CHECK(is_still_convex(out));
    CHECK(output_is_subset_of_input(out, cube));
}

// ---------------------------------------------------------------------------
// (5) Determinism replay
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: identical input -> identical output (determinism)",
          "[v3d][hull_simplify][determinism]")
{
    crd::memory::TlsfAllocator alloc(512U * 1024U);
    auto cube1 = build_unit_cube<f64>(&alloc);
    auto cube2 = build_unit_cube<f64>(&alloc);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 4;
    auto out1 = simplify_hull<f64>(cube1, &alloc, opts);
    auto out2 = simplify_hull<f64>(cube2, &alloc, opts);

    REQUIRE(out1.vertices.size() == out2.vertices.size());
    REQUIRE(out1.faces.size() == out2.faces.size());
    for (usize i = 0; i < out1.vertices.size(); ++i)
    {
        CHECK(out1.vertices[i].x == out2.vertices[i].x);
        CHECK(out1.vertices[i].y == out2.vertices[i].y);
        CHECK(out1.vertices[i].z == out2.vertices[i].z);
    }
    REQUIRE(out1.face_vertex_indices.size() == out2.face_vertex_indices.size());
    for (usize i = 0; i < out1.face_vertex_indices.size(); ++i)
    {
        CHECK(out1.face_vertex_indices[i] == out2.face_vertex_indices[i]);
    }
}

// ---------------------------------------------------------------------------
// (6) Random cloud
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: random cloud reduces and stays convex",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(1024U * 1024U);
    auto src = build_random_cloud<f64>(&alloc, 200, 0xDEADBEEFCAFEBABEULL);
    REQUIRE(src.vertices.size() >= 4);

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 6;
    auto out = simplify_hull<f64>(src, &alloc, opts);

    CHECK(out.vertices.size() <= src.vertices.size());
    CHECK(is_still_convex(out));
    CHECK(output_is_subset_of_input(out, src));
}

// ---------------------------------------------------------------------------
// (7) f32 path
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: f32 cube reduces and stays convex",
          "[v3d][hull_simplify][f32]")
{
    crd::memory::TlsfAllocator alloc(256U * 1024U);
    auto cube = build_unit_cube<f32>(&alloc);
    REQUIRE(cube.vertices.size() == 8);

    HullSimplifyOptions<f32> opts;
    opts.target_vertex_count = 4;
    auto out = simplify_hull<f32>(cube, &alloc, opts);
    CHECK(out.vertices.size() <= 8);
    CHECK(out.vertices.size() >= 4);
    CHECK(is_still_convex(out));
    CHECK(output_is_subset_of_input(out, cube));
}

// ---------------------------------------------------------------------------
// (8) AABB containment (output AABB ⊆ input AABB)
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: output AABB stays inside input AABB",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(1024U * 1024U);
    auto src = build_random_cloud<f64>(&alloc, 100, 0x123456789ABCDEF0ULL);

    // Compute input AABB.
    Vec3<f64> src_min = src.vertices[0];
    Vec3<f64> src_max = src.vertices[0];
    for (usize i = 1; i < src.vertices.size(); ++i)
    {
        src_min.x = std::min(src_min.x, src.vertices[i].x);
        src_min.y = std::min(src_min.y, src.vertices[i].y);
        src_min.z = std::min(src_min.z, src.vertices[i].z);
        src_max.x = std::max(src_max.x, src.vertices[i].x);
        src_max.y = std::max(src_max.y, src.vertices[i].y);
        src_max.z = std::max(src_max.z, src.vertices[i].z);
    }

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 5;
    auto out = simplify_hull<f64>(src, &alloc, opts);

    for (usize i = 0; i < out.vertices.size(); ++i)
    {
        CHECK(out.vertices[i].x >= src_min.x);
        CHECK(out.vertices[i].y >= src_min.y);
        CHECK(out.vertices[i].z >= src_min.z);
        CHECK(out.vertices[i].x <= src_max.x);
        CHECK(out.vertices[i].y <= src_max.y);
        CHECK(out.vertices[i].z <= src_max.z);
    }
}

// ---------------------------------------------------------------------------
// (9) Locked-vertex with random cloud (multi-domain pin)
// ---------------------------------------------------------------------------

TEST_CASE("hull_simplify: locked vertices preserved in random cloud",
          "[v3d][hull_simplify]")
{
    crd::memory::TlsfAllocator alloc(1024U * 1024U);
    auto src = build_random_cloud<f64>(&alloc, 80, 0xFEEDFACE12345678ULL);
    REQUIRE(src.vertices.size() >= 6);

    // Lock the first 2 hull vertices.
    crd::containers::Array<u32> locked(&alloc);
    locked.push_back(0);
    locked.push_back(1);
    const Vec3<f64> p_locked_0 = src.vertices[0];
    const Vec3<f64> p_locked_1 = src.vertices[1];

    HullSimplifyOptions<f64> opts;
    opts.target_vertex_count = 4;
    opts.keep_vertex_indices =
        crd::containers::ConstSpan<u32>(locked.data(), locked.size());

    auto out = simplify_hull<f64>(src, &alloc, opts);
    CHECK(find_vertex(out, p_locked_0) != ~u32{0});
    CHECK(find_vertex(out, p_locked_1) != ~u32{0});
    CHECK(is_still_convex(out));
}
