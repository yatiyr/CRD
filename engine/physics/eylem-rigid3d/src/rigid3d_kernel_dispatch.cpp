// The eylem-rigid3d kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/eylem_rigid3d/rigid3d_dispatch.hpp>

namespace crd::eylem_rigid3d
{
namespace
{

using crd::KernelLevel;

// CRD_KERNELS_RIGID3D: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_RIGID3D & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, PORTABLE), "the portable level is always built");

constexpr KernelLevel<Rigid3dKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kRigid3dKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, SSE2)
    {crd::CpuLevel::Sse2, &kRigid3dKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, AVX2)
    {crd::CpuLevel::Avx2, &kRigid3dKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_RIGID3D, AVX512)
    {crd::CpuLevel::Avx512, &kRigid3dKernelsAvx512, {}},
#endif
};

} // namespace

std::span<const KernelLevel<Rigid3dKernels>> rigid3d_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<Rigid3dKernels>& rigid3d_kernels() noexcept
{
    static const KernelLevel<Rigid3dKernels>& selected = crd::process_kernel_level<Rigid3dKernels>(kLevels);
    return selected;
}

} // namespace crd::eylem_rigid3d
