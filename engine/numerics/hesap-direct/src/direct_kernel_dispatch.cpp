// The hesap-direct kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/hesap/direct/direct_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::direct
{
namespace
{

using crd::math::KernelLevel;

// CRD_KERNELS_DIRECT: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody =
    (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) | (1 << CRD_KERNEL_LEVEL_AVX2);
static_assert((CRD_KERNELS_DIRECT & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_DIRECT, PORTABLE), "the portable level is always built");

constexpr KernelLevel<DirectKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kDirectKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_DIRECT, SSE2)
    {crd::CpuLevel::Sse2, &kDirectKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_DIRECT, AVX2)
    {crd::CpuLevel::Avx2, &kDirectKernelsAvx2, {}},
#endif
};

} // namespace

std::span<const KernelLevel<DirectKernels>> direct_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<DirectKernels>& direct_kernels() noexcept
{
    static const KernelLevel<DirectKernels>& selected = crd::math::process_kernel_level<DirectKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::direct
