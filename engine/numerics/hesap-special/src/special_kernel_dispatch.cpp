// The hesap-special kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/special/special_dispatch.hpp>

namespace crd::hesap::special
{
namespace
{

using crd::KernelLevel;

// CRD_KERNELS_SPECIAL: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_SPECIAL & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, PORTABLE), "the portable level is always built");

constexpr KernelLevel<SpecialKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kSpecialKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, SSE2)
    {crd::CpuLevel::Sse2, &kSpecialKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, AVX2)
    {crd::CpuLevel::Avx2, &kSpecialKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPECIAL, AVX512)
    {crd::CpuLevel::Avx512, &kSpecialKernelsAvx512, {}},
#endif
};

} // namespace

std::span<const KernelLevel<SpecialKernels>> special_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<SpecialKernels>& special_kernels() noexcept
{
    static const KernelLevel<SpecialKernels>& selected = crd::process_kernel_level<SpecialKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::special
