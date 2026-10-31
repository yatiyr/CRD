// The hesap-tensor kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>

namespace crd::hesap::tensor
{
namespace
{

using crd::KernelLevel;

// CRD_KERNELS_TENSOR: the levels crd_add_cpu_kernels built (../CMakeLists.txt), and CRD_KERNELS_TENSOR_AVX2_AVXVNNI
// and CRD_KERNELS_TENSOR_AVX512_AVX512VNNI its variants. Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_TENSOR & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_TENSOR, PORTABLE), "the portable level is always built");

constexpr KernelLevel<TensorKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kTensorKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_TENSOR, SSE2)
    {crd::CpuLevel::Sse2, &kTensorKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_TENSOR, AVX2)
    {crd::CpuLevel::Avx2, &kTensorKernelsAvx2, {}},
#endif
#if CRD_KERNELS_TENSOR_AVX2_AVXVNNI
    {crd::CpuLevel::Avx2, &kTensorKernelsAvx2AvxVnni, crd::CpuFeatureSet{}.with(crd::CpuFeature::AvxVnni)},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_TENSOR, AVX512)
    {crd::CpuLevel::Avx512, &kTensorKernelsAvx512, {}},
#endif
#if CRD_KERNELS_TENSOR_AVX512_AVX512VNNI
    {crd::CpuLevel::Avx512, &kTensorKernelsAvx512Avx512Vnni, crd::CpuFeatureSet{}.with(crd::CpuFeature::Avx512Vnni)},
#endif
};

} // namespace

std::span<const KernelLevel<TensorKernels>> tensor_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<TensorKernels>& tensor_kernels() noexcept
{
    static const KernelLevel<TensorKernels>& selected = crd::process_kernel_level<TensorKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::tensor
