// The hesap-dense kernels of this archive and the level the process runs (SIMD-DISPATCH), each table checked against
// known answers before its first use (HW-RESILIENCE.3, dense_self_check.cpp).

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

void resolve_dense() noexcept
{
    (void)dense_kernels();
}

constinit crd::KernelFamily g_family{"hesap.dense", &resolve_dense, {}, nullptr};
const crd::KernelFamilyListing kListing{g_family};

} // namespace

std::span<const KernelLevel<DenseKernels>> dense_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<DenseKernels>& dense_kernels() noexcept
{
    static const KernelLevel<DenseKernels>& selected =
        crd::process_checked_kernel_level<DenseKernels>(kLevels, &dense_self_check, g_family);
    return selected;
}

} // namespace crd::hesap::dense
