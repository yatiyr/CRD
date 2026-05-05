#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <crd/meshgen/meshgen.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderer/mesh_resource.hpp>

#include <cmath>

namespace
{

crd::memory::IAllocator* alloc()
{
    return crd::memory::default_allocator();
}

// Verify basic invariants that all generated meshes must satisfy.
void check_mesh_invariants(const crd::renderer::MeshResource& mesh)
{
    REQUIRE(mesh.primitives.size() == 1U);
    const auto& prim = mesh.primitives[0];
    REQUIRE(prim.vertex_count > 0U);
    REQUIRE(prim.index_count > 0U);
    REQUIRE(prim.index_count % 3U == 0U); // must be triangles
    REQUIRE(mesh.vertices.size() == static_cast<std::size_t>(prim.vertex_count) * crd::renderer::kMeshVertexStride);
    REQUIRE(mesh.indices.size()  == static_cast<std::size_t>(prim.index_count)  * 4U);

    // Normals must be unit length
    const crd::u8* vdata = mesh.vertices.data();
    for (crd::u32 vi = 0; vi < prim.vertex_count; ++vi)
    {
        const float* nrm = reinterpret_cast<const float*>(vdata + vi * crd::renderer::kMeshVertexStride + 12U);
        const float len2 = nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2];
        CHECK(len2 == Catch::Approx(1.0F).epsilon(0.01F));
    }

    // Tangent w must be +1 or -1
    for (crd::u32 vi = 0; vi < prim.vertex_count; ++vi)
    {
        const float* tan = reinterpret_cast<const float*>(vdata + vi * crd::renderer::kMeshVertexStride + 32U);
        const float w = tan[3];
        CHECK((w == Catch::Approx(1.0F).epsilon(0.01F) || w == Catch::Approx(-1.0F).epsilon(0.01F)));
    }

    // All indices must be in range
    const crd::u32* idata = reinterpret_cast<const crd::u32*>(mesh.indices.data());
    for (crd::u32 ii = 0; ii < prim.index_count; ++ii)
    {
        REQUIRE(idata[ii] < prim.vertex_count);
    }
}

} // namespace

TEST_CASE("make_plane: default params", "[meshgen][plane]")
{
    auto m = crd::meshgen::make_plane(alloc());
    check_mesh_invariants(m);
    // 1x1 quad = 4 verts, 6 indices
    CHECK(m.primitives[0].vertex_count == 4U);
    CHECK(m.primitives[0].index_count  == 6U);
}

TEST_CASE("make_plane: 2x3 subdivisions", "[meshgen][plane]")
{
    auto m = crd::meshgen::make_plane(alloc(), 2.0F, 3.0F, 2U, 3U);
    check_mesh_invariants(m);
    CHECK(m.primitives[0].vertex_count == 3U * 4U);   // (2+1)*(3+1)=12
    CHECK(m.primitives[0].index_count  == 2U * 3U * 6U); // 2*3 quads * 6 indices each = 36
}

TEST_CASE("make_box: default params", "[meshgen][box]")
{
    auto m = crd::meshgen::make_box(alloc());
    check_mesh_invariants(m);
    // 6 faces * 4 verts = 24 verts, 6 faces * 6 indices = 36
    CHECK(m.primitives[0].vertex_count == 24U);
    CHECK(m.primitives[0].index_count  == 36U);
}

TEST_CASE("make_sphere: default params", "[meshgen][sphere]")
{
    auto m = crd::meshgen::make_sphere(alloc());
    check_mesh_invariants(m);
    // (lat_bands+1)*(lon_bands+1) = 17*33 = 561 verts
    CHECK(m.primitives[0].vertex_count == 17U * 33U);
}

TEST_CASE("make_sphere: normal equals position normalized", "[meshgen][sphere]")
{
    auto m = crd::meshgen::make_sphere(alloc(), 2.0F, 8U, 16U);
    check_mesh_invariants(m);
    const crd::u8* vdata = m.vertices.data();
    for (crd::u32 vi = 0; vi < m.primitives[0].vertex_count; ++vi)
    {
        const float* pos = reinterpret_cast<const float*>(vdata + vi * crd::renderer::kMeshVertexStride);
        const float* nrm = reinterpret_cast<const float*>(vdata + vi * crd::renderer::kMeshVertexStride + 12U);
        const float len = std::sqrt(pos[0]*pos[0] + pos[1]*pos[1] + pos[2]*pos[2]);
        CHECK(len == Catch::Approx(2.0F).epsilon(0.01F));
        CHECK(nrm[0] == Catch::Approx(pos[0] / len).epsilon(0.01F));
        CHECK(nrm[1] == Catch::Approx(pos[1] / len).epsilon(0.01F));
        CHECK(nrm[2] == Catch::Approx(pos[2] / len).epsilon(0.01F));
    }
}

TEST_CASE("make_icosphere: default params", "[meshgen][icosphere]")
{
    auto m = crd::meshgen::make_icosphere(alloc());
    check_mesh_invariants(m);
    // Verify vertices are on unit sphere (radius 1)
    const crd::u8* vdata = m.vertices.data();
    for (crd::u32 vi = 0; vi < m.primitives[0].vertex_count; ++vi)
    {
        const float* pos = reinterpret_cast<const float*>(vdata + vi * crd::renderer::kMeshVertexStride);
        const float len = std::sqrt(pos[0]*pos[0] + pos[1]*pos[1] + pos[2]*pos[2]);
        CHECK(len == Catch::Approx(1.0F).epsilon(0.01F));
    }
}

TEST_CASE("make_cylinder: default params", "[meshgen][cylinder]")
{
    auto m = crd::meshgen::make_cylinder(alloc());
    check_mesh_invariants(m);
}

TEST_CASE("make_cone: default params", "[meshgen][cone]")
{
    auto m = crd::meshgen::make_cone(alloc());
    check_mesh_invariants(m);
}

TEST_CASE("make_capsule: default params", "[meshgen][capsule]")
{
    auto m = crd::meshgen::make_capsule(alloc());
    check_mesh_invariants(m);
}

TEST_CASE("make_torus: default params", "[meshgen][torus]")
{
    auto m = crd::meshgen::make_torus(alloc());
    check_mesh_invariants(m);
}

TEST_CASE("all shapes: vertex/index buffers non-empty", "[meshgen][smoke]")
{
    auto* a = alloc();
    const auto shapes = {
        crd::meshgen::make_plane(a),
        crd::meshgen::make_box(a),
        crd::meshgen::make_sphere(a),
        crd::meshgen::make_icosphere(a),
        crd::meshgen::make_cylinder(a),
        crd::meshgen::make_cone(a),
        crd::meshgen::make_capsule(a),
        crd::meshgen::make_torus(a),
    };
    for (const auto& mesh : shapes)
    {
        CHECK(!mesh.vertices.empty());
        CHECK(!mesh.indices.empty());
        CHECK(mesh.primitives.size() == 1U);
    }
}
