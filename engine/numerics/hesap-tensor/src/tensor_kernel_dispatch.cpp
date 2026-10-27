// The hesap-tensor kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::tensor
{
namespace
{

using crd::math::KernelLevel;

// CRD_KERNELS_TENSOR: the levels crd_add_cpu_kernels built (../CMakeLists.txt), and CRD_KERNELS_TENSOR_AVX2_AVXVNNI
// its variant. Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_AVX2);
static_assert((CRD_KERNELS_TENSOR & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_TENSOR, PORTABLE), "the portable level is always built");

constexpr KernelLevel<TensorKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kTensorKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_TENSOR, AVX2)
    {crd::CpuLevel::Avx2, &kTensorKernelsAvx2, {}},
#endif
#if CRD_KERNELS_TENSOR_AVX2_AVXVNNI
    {crd::CpuLevel::Avx2, &kTensorKernelsAvx2AvxVnni, crd::CpuFeatureSet{}.with(crd::CpuFeature::AvxVnni)},
#endif
};

} // namespace

std::span<const KernelLevel<TensorKernels>> tensor_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<TensorKernels>& tensor_kernels() noexcept
{
    static const KernelLevel<TensorKernels>& selected = crd::math::process_kernel_level<TensorKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::tensor
