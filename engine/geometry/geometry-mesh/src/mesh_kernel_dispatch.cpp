// The geometry-mesh kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/geometry/mesh/mesh_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::geometry::mesh
{
namespace
{

using crd::math::KernelLevel;

// CRD_KERNELS_MESH: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody =
    (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) | (1 << CRD_KERNEL_LEVEL_AVX2);
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
};

} // namespace

std::span<const KernelLevel<MeshKernels>> mesh_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<MeshKernels>& mesh_kernels() noexcept
{
    static const KernelLevel<MeshKernels>& selected = crd::math::process_kernel_level<MeshKernels>(kLevels);
    return selected;
}

} // namespace crd::geometry::mesh
