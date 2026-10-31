// The hesap-dense kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>

namespace crd::hesap::dense
{
namespace
{

using crd::KernelLevel;

// CRD_KERNELS_DENSE: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_DENSE & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_DENSE, PORTABLE), "the portable level is always built");

constexpr KernelLevel<DenseKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kDenseKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_DENSE, SSE2)
    {crd::CpuLevel::Sse2, &kDenseKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_DENSE, AVX2)
    {crd::CpuLevel::Avx2, &kDenseKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_DENSE, AVX512)
    {crd::CpuLevel::Avx512, &kDenseKernelsAvx512, {}},
#endif
};

} // namespace

std::span<const KernelLevel<DenseKernels>> dense_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<DenseKernels>& dense_kernels() noexcept
{
    static const KernelLevel<DenseKernels>& selected = crd::process_kernel_level<DenseKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::dense
