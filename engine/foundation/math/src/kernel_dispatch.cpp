// The build's kernel level cap (SIMD-DISPATCH, ADR-0134 §4): CRD_KERNEL_LEVEL_CAP comes from crd-simd-flags. The
// process's kernel determinism mode (§7).

#include <crd/math/kernel_dispatch.hpp>
#include <crd/math/kernel_level.hpp>

#include <atomic>

namespace crd::math
{

static_assert(static_cast<int>(crd::CpuLevel::Portable) == CRD_KERNEL_LEVEL_PORTABLE);
static_assert(static_cast<int>(crd::CpuLevel::Sse2) == CRD_KERNEL_LEVEL_SSE2);
static_assert(static_cast<int>(crd::CpuLevel::Sse42) == CRD_KERNEL_LEVEL_SSE42);
static_assert(static_cast<int>(crd::CpuLevel::Avx2) == CRD_KERNEL_LEVEL_AVX2);
static_assert(static_cast<int>(crd::CpuLevel::Avx512) == CRD_KERNEL_LEVEL_AVX512);
static_assert(static_cast<int>(crd::CpuLevel::Neon) == CRD_KERNEL_LEVEL_NEON);

namespace
{
// Relaxed: the store happens before every plan build the setting thread orders after it (its own, or a job it then
// submits); a build racing the store may read either mode.
std::atomic<DeterminismMode> g_kernel_determinism{DeterminismMode::Default};
} // namespace

DeterminismMode kernel_determinism() noexcept
{
    return g_kernel_determinism.load(std::memory_order_relaxed);
}

void set_kernel_determinism(DeterminismMode mode) noexcept
{
    g_kernel_determinism.store(mode, std::memory_order_relaxed);
}

crd::CpuLevel kernel_level_cap() noexcept
{
    return static_cast<crd::CpuLevel>(CRD_KERNEL_LEVEL_CAP);
}

} // namespace crd::math
