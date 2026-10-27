// The hesap-stats kernels of this archive and the level the process runs (SIMD-DISPATCH).

#include <crd/hesap/stats/stats_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::stats
{
namespace
{

using crd::math::KernelLevel;

// CRD_KERNELS_STATS: the levels crd_add_cpu_kernels built (../CMakeLists.txt). Each needs its entry below.
constexpr int kLevelsWithBody =
    (1 << CRD_KERNEL_LEVEL_PORTABLE) | (1 << CRD_KERNEL_LEVEL_SSE2) | (1 << CRD_KERNEL_LEVEL_AVX2);
static_assert((CRD_KERNELS_STATS & ~kLevelsWithBody) == 0, "a built level has no table entry here");
static_assert(CRD_KERNEL_BUILT(CRD_KERNELS_STATS, PORTABLE), "the portable level is always built");

constexpr KernelLevel<StatsKernels> kLevels[] = {
    {crd::CpuLevel::Portable, &kStatsKernelsPortable, {}},
#if CRD_KERNEL_BUILT(CRD_KERNELS_STATS, SSE2)
    {crd::CpuLevel::Sse2, &kStatsKernelsSse2, {}},
#endif
#if CRD_KERNEL_BUILT(CRD_KERNELS_STATS, AVX2)
    {crd::CpuLevel::Avx2, &kStatsKernelsAvx2, {}},
#endif
};

} // namespace

std::span<const KernelLevel<StatsKernels>> stats_kernel_levels() noexcept
{
    return kLevels;
}

const KernelLevel<StatsKernels>& stats_kernels() noexcept
{
    static const KernelLevel<StatsKernels>& selected = crd::math::process_kernel_level<StatsKernels>(kLevels);
    return selected;
}

} // namespace crd::hesap::stats
