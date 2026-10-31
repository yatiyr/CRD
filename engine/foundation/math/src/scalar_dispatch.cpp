// The scalar transcendental tables of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/math/kernel_level.hpp>
#include <crd/math/scalar_dispatch.hpp>

namespace crd::math
{
namespace
{

// CRD_KERNELS_SCALAR: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_AVX2);
static_assert((CRD_KERNELS_SCALAR & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_SCALAR, PORTABLE), "the portable level is always built");

constexpr KernelLevel<ScalarMathKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kScalarMathKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_SCALAR, AVX2)
    {crd::CpuLevel::Avx2, &kScalarMathKernelsAvx2, {}},
#endif
};

} // namespace

std::span<const KernelLevel<ScalarMathKernels>> scalar_math_levels() noexcept
{
    return kLevels;
}

const KernelLevel<ScalarMathKernels>& scalar_math_level() noexcept
{
    static const KernelLevel<ScalarMathKernels>& selected = process_kernel_level<ScalarMathKernels>(kLevels);
    return selected;
}

const ScalarMathKernels& scalar_math() noexcept
{
    static const ScalarMathKernels& selected = *scalar_math_level().table;
    return selected;
}

} // namespace crd::math
