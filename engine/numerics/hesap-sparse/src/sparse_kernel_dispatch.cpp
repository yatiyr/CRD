// The hesap-sparse kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/core/cpu_features.hpp>
#include <crd/hesap/sparse/sparse_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::sparse
{
namespace
{

using crd::math::KernelLevel;

// CRD_KERNELS_SPARSE: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody = (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) |
                                (1 << CRD_KERNEL_LEVEL_AVX2) | (1 << CRD_KERNEL_LEVEL_AVX512);
static_assert((CRD_KERNELS_SPARSE & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, PORTABLE), "the portable level is always built");

constexpr KernelLevel<SparseKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kSparseKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, SSE2)
    {crd::CpuLevel::Sse2, &kSparseKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, AVX2)
    {crd::CpuLevel::Avx2, &kSparseKernelsAvx2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_SPARSE, AVX512)
    {crd::CpuLevel::Avx512, &kSparseKernelsAvx512, {}},
#endif
};

} // namespace

std::span<const KernelLevel<SparseKernels>> sparse_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<SparseKernels>& sparse_kernels() noexcept
{
    static const KernelLevel<SparseKernels>& selected = crd::math::process_kernel_level<SparseKernels>(kLevels);
    return selected;
}

crd::usize sparse_resident_bytes() noexcept
{
    const crd::u32 l2 = crd::cpu_selection().features.caches.l2_bytes;
    return l2 != 0U ? crd::usize{l2} : (crd::usize{1} << 21);
}

} // namespace crd::hesap::sparse
