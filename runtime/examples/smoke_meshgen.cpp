#include <crd/meshgen/meshgen.hpp>

#include <crd/memory/allocator.hpp>
#include <crd/renderer/mesh_resource.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

static bool check_mesh(const crd::renderer::MeshResource& mesh, const char* name)
{
    if (mesh.primitives.size() != 1U)
    {
        std::fprintf(stderr, "FAIL [%s]: expected 1 primitive, got %zu\n", name, mesh.primitives.size());
        return false;
    }
    const auto& prim = mesh.primitives[0];
    if (prim.vertex_count == 0U)
    {
        std::fprintf(stderr, "FAIL [%s]: vertex_count == 0\n", name);
        return false;
    }
    if (prim.index_count == 0U)
    {
        std::fprintf(stderr, "FAIL [%s]: index_count == 0\n", name);
        return false;
    }
    if (mesh.vertices.size() != static_cast<std::size_t>(prim.vertex_count) * crd::renderer::kMeshVertexStride)
    {
        std::fprintf(stderr, "FAIL [%s]: vertex buffer size mismatch\n", name);
        return false;
    }
    if (mesh.indices.size() != static_cast<std::size_t>(prim.index_count) * 4U)
    {
        std::fprintf(stderr, "FAIL [%s]: index buffer size mismatch\n", name);
        return false;
    }
    // Check normals are unit-length
    const crd::u8* vdata = mesh.vertices.data();
    for (crd::u32 vi = 0; vi < prim.vertex_count; ++vi)
    {
        const float* nrm = reinterpret_cast<const float*>(vdata + vi * crd::renderer::kMeshVertexStride + 12U);
        const float len2 = nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2];
        if (std::abs(len2 - 1.0F) > 0.01F)
        {
            std::fprintf(stderr, "FAIL [%s]: vertex %u normal not unit length (len^2=%.4f)\n", name, vi, static_cast<double>(len2));
            return false;
        }
    }
    std::printf("OK   [%s]: %u verts, %u indices\n", name, prim.vertex_count, prim.index_count);
    return true;
}

int main()
{
    auto* a = crd::memory::default_allocator();
    bool ok = true;

    ok &= check_mesh(crd::meshgen::make_plane(a),         "plane");
    ok &= check_mesh(crd::meshgen::make_box(a),           "box");
    ok &= check_mesh(crd::meshgen::make_sphere(a),        "sphere");
    ok &= check_mesh(crd::meshgen::make_icosphere(a),     "icosphere");
    ok &= check_mesh(crd::meshgen::make_cylinder(a),      "cylinder");
    ok &= check_mesh(crd::meshgen::make_cone(a),          "cone");
    ok &= check_mesh(crd::meshgen::make_capsule(a),       "capsule");
    ok &= check_mesh(crd::meshgen::make_torus(a),         "torus");

    if (!ok)
    {
        std::fprintf(stderr, "smoke_meshgen FAILED\n");
        return EXIT_FAILURE;
    }
    std::printf("smoke_meshgen PASSED\n");
    return EXIT_SUCCESS;
}
