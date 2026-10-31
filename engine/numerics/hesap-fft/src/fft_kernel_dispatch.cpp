// The hesap-fft kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/fft/fft_dispatch.hpp>

namespace crd::hesap::fft
{
namespace
{

using crd::KernelLevel;

// CRD_KERNELS_FFT: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_FFT & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_FFT, PORTABLE), "the portable level is always built");

constexpr KernelLevel<FftKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kFftKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_FFT, SSE2)
    {crd::CpuLevel::Sse2, &kFftKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_FFT, AVX2)
    {crd::CpuLevel::Avx2, &kFftKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_FFT, AVX512)
    {crd::CpuLevel::Avx512, &kFftKernelsAvx512, {}},
#endif
};

} // namespace

std::span<const KernelLevel<FftKernels>> fft_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<FftKernels>& fft_kernels() noexcept
{
    static const KernelLevel<FftKernels>& selected = crd::process_kernel_level<FftKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::fft
