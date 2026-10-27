// The batch small-float conversion kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/math/float_convert.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::math
{
namespace
{

// CRD_KERNELS_FLOAT_CONVERT: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_SSE42) | (1 << CRD_KERNEL_LEVEL_AVX2);
static_assert((CRD_KERNELS_FLOAT_CONVERT & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, PORTABLE), "the portable level is always built");

constexpr KernelLevel<FloatConvertKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kFloatConvertKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, SSE2)
    {crd::CpuLevel::Sse2, &kFloatConvertKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, SSE42)
    {crd::CpuLevel::Sse42, &kFloatConvertKernelsSse42, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_FLOAT_CONVERT, AVX2)
    {crd::CpuLevel::Avx2, &kFloatConvertKernelsAvx2, {}},
#endif
};

} // namespace

std::span<const KernelLevel<FloatConvertKernels>> float_convert_levels() noexcept
{
    return kLevels;
}

const KernelLevel<FloatConvertKernels>& float_convert_kernels() noexcept
{
    static const KernelLevel<FloatConvertKernels>& selected = process_kernel_level<FloatConvertKernels>(kLevels);
    return selected;
}

} // namespace crd::math
