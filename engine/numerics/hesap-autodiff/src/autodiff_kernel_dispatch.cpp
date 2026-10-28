// The hesap-autodiff kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/autodiff/autodiff_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::autodiff
{
namespace
{

using crd::math::KernelLevel;

// CRD_KERNELS_AUTODIFF: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_AUTODIFF & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, PORTABLE), "the portable level is always built");

constexpr KernelLevel<AutodiffKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kAutodiffKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, SSE2)
    {crd::CpuLevel::Sse2, &kAutodiffKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, AVX2)
    {crd::CpuLevel::Avx2, &kAutodiffKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_AUTODIFF, AVX512)
    {crd::CpuLevel::Avx512, &kAutodiffKernelsAvx512, {}},
#endif
};

} // namespace

std::span<const KernelLevel<AutodiffKernels>> autodiff_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<AutodiffKernels>& autodiff_kernels() noexcept
{
    static const KernelLevel<AutodiffKernels>& selected = crd::math::process_kernel_level<AutodiffKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::autodiff
