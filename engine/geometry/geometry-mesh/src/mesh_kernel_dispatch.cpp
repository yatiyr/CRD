// The geometry-mesh kernels of this archive, their known-answer self-check (HW-RESILIENCE.3) and the level the process
// runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/geometry/mesh/mesh_dispatch.hpp>

#include <bit>
#include <cstddef>
#include <limits>

namespace crd::geometry::mesh
{
namespace
{

using crd::f32;
using crd::KernelLevel;
using crd::u16;
using crd::u32;
using crd::usize;

// CRD_KERNELS_MESH: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_MESH & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_MESH, PORTABLE), "the portable level is always built");

constexpr KernelLevel<MeshKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kMeshKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_MESH, SSE2)
    {crd::CpuLevel::Sse2, &kMeshKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_MESH, AVX2)
    {crd::CpuLevel::Avx2, &kMeshKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_MESH, AVX512)
    {crd::CpuLevel::Avx512, &kMeshKernelsAvx512, {}},
#endif
};

// ---- Known-answer self-check (HW-RESILIENCE.3) ---------------------------------------------------------------------

// A two-leaf BVH over 29 triangles, each the right triangle (0, 0, z), (2, 0, z), (0, 2, z) at z = 40 - i, but
// triangles 1 and 3 at z = 3 and triangle 28 at z = -20. The leaves hold slots 0-8 and 9-28 of the reversed primitive
// order, so at avx512 the first leaf is one sixteen-lane step with nine valid and the second a sixteen-lane step and
// an eight-lane tail with four valid (on Vec8f: eight, eight and four), and the tail holds triangles 3 and 1. The ray
// (1/4, 1/4, -10) + t (0, 0, 1) passes triangle 28 behind its origin, crosses the first leaf's nearest triangle (27,
// t = 23) and enters the second leaf at t = 13, where triangles 3 and 1 tie: the lower index, 1, at t = 13 with
// barycentric weights (3/4, 1/8, 1/8). Every value is exact. Culling back faces, the same ray hits nothing (each
// triangle faces away from it).
constexpr u32 kCheckTriangles = 29;

struct CheckNode
{
    f32 lo[3];
    f32 hi[3];
    u32 left_first;
    u16 prim_count;
    u16 pad;
};
static_assert(sizeof(CheckNode) == kBvhNodeBytes && offsetof(CheckNode, left_first) == kBvhNodeLeftFirst &&
              offsetof(CheckNode, prim_count) == kBvhNodePrimCount);

struct CheckMesh
{
    f32 vertices[kCheckTriangles * 3U][3];
    u32 indices[kCheckTriangles * 3U];
    u32 prim_indices[kCheckTriangles];
    CheckNode nodes[3];
};

constexpr CheckMesh make_check_mesh() noexcept
{
    CheckMesh m{};
    for (u32 i = 0; i < kCheckTriangles; ++i)
    {
        f32 z = 40.0F - static_cast<f32>(i);
        if (i == 1U || i == 3U)
        {
            z = 3.0F;
        }
        else if (i == 28U)
        {
            z = -20.0F;
        }
        const f32 corners[3][2] = {{0.0F, 0.0F}, {2.0F, 0.0F}, {0.0F, 2.0F}};
        for (u32 c = 0; c < 3U; ++c)
        {
            m.vertices[i * 3U + c][0] = corners[c][0];
            m.vertices[i * 3U + c][1] = corners[c][1];
            m.vertices[i * 3U + c][2] = z;
            m.indices[i * 3U + c] = i * 3U + c;
        }
        m.prim_indices[i] = kCheckTriangles - 1U - i;
    }
    m.nodes[0] = CheckNode{{0.0F, 0.0F, -20.0F}, {2.0F, 2.0F, 40.0F}, 1U, 0U, 0U};
    m.nodes[1] = CheckNode{{0.0F, 0.0F, -20.0F}, {2.0F, 2.0F, 20.0F}, 0U, 9U, 0U};
    m.nodes[2] = CheckNode{{0.0F, 0.0F, 3.0F}, {2.0F, 2.0F, 40.0F}, 9U, 20U, 0U};
    return m;
}

constexpr CheckMesh kCheckMesh = make_check_mesh();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr crd::geometry::primitives::RaySlab kCheckRay{
    {0.25F, 0.25F, -10.0F}, {0.0F, 0.0F, 1.0F}, {kInf, kInf, 1.0F}, {0U, 0U, 0U}, 1.0F};

void resolve_mesh() noexcept
{
    (void)mesh_kernels();
}

constinit crd::KernelFamily g_family{"geometry.mesh", &resolve_mesh, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

const char* mesh_self_check(const MeshKernels& table) noexcept
{
    const MeshBvhView view{reinterpret_cast<const unsigned char*>(kCheckMesh.vertices), kCheckMesh.indices,
                           reinterpret_cast<const unsigned char*>(kCheckMesh.nodes), kCheckMesh.prim_indices, 0U};
    MeshRayHitData hit{};
    const bool found = table.raycast_mt8(view, kCheckRay, 100.0F, false, hit);
    const bool exact = std::bit_cast<u32>(hit.t) == std::bit_cast<u32>(13.0F) && hit.triangle == 1U &&
                       std::bit_cast<u32>(hit.bary[0]) == std::bit_cast<u32>(0.75F) &&
                       std::bit_cast<u32>(hit.bary[1]) == std::bit_cast<u32>(0.125F) &&
                       std::bit_cast<u32>(hit.bary[2]) == std::bit_cast<u32>(0.125F);
    MeshRayHitData culled{};
    if (!found || !exact || table.raycast_mt8(view, kCheckRay, 100.0F, true, culled))
    {
        return "raycast_mt8";
    }
    return nullptr;
}

std::span<const KernelLevel<MeshKernels>> mesh_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<MeshKernels>& mesh_kernels() noexcept
{
    static const KernelLevel<MeshKernels>& selected =
        crd::process_checked_kernel_level<MeshKernels>(kLevels, &mesh_self_check, g_family);
    return selected;
}

} // namespace crd::geometry::mesh
