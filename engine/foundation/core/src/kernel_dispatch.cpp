// The build's kernel level cap (SIMD-DISPATCH, ADR-0134 §4): CRD_KERNEL_LEVEL_CAP comes from CrdSimd.cmake, the value
// crd-simd-flags carries. The listing of the self-checked kernel families (HW-RESILIENCE.3).

#include <crd/core/kernel_dispatch.hpp>
#include <crd/core/kernel_level.hpp>

#include <atomic>

namespace crd
{

static_assert(static_cast<int>(crd::CpuLevel::Portable) == CRD_KERNEL_LEVEL_PORTABLE);
static_assert(static_cast<int>(crd::CpuLevel::Sse2) == CRD_KERNEL_LEVEL_SSE2);
static_assert(static_cast<int>(crd::CpuLevel::Sse42) == CRD_KERNEL_LEVEL_SSE42);
static_assert(static_cast<int>(crd::CpuLevel::Avx2) == CRD_KERNEL_LEVEL_AVX2);
static_assert(static_cast<int>(crd::CpuLevel::Avx512) == CRD_KERNEL_LEVEL_AVX512);
static_assert(static_cast<int>(crd::CpuLevel::Neon) == CRD_KERNEL_LEVEL_NEON);

crd::CpuLevel kernel_level_cap() noexcept
{
    return static_cast<crd::CpuLevel>(CRD_KERNEL_LEVEL_CAP);
}

namespace
{
// Constant-initialised, so a listing at any point of the image's start finds it ready.
constinit std::atomic<const KernelFamily*> g_families{nullptr};
} // namespace

KernelFamilyListing::KernelFamilyListing(KernelFamily& family) noexcept
{
    const KernelFamily* head = g_families.load(std::memory_order_relaxed);
    do
    {
        family.next = head;
    } while (!g_families.compare_exchange_weak(head, &family, std::memory_order_release, std::memory_order_relaxed));
}

const KernelFamily* kernel_families() noexcept
{
    return g_families.load(std::memory_order_acquire);
}

crd::u32 run_kernel_self_checks() noexcept
{
    crd::u32 demoted = 0U;
    for (const KernelFamily* f = kernel_families(); f != nullptr; f = f->next)
    {
        f->resolve();
        demoted += f->result.status == KernelCheckStatus::Passed ? 0U : 1U;
    }
    return demoted;
}

const char* kernel_check_status_name(KernelCheckStatus status) noexcept
{
    switch (status)
    {
        case KernelCheckStatus::NotRun:
            return "not-run";
        case KernelCheckStatus::Passed:
            return "passed";
        case KernelCheckStatus::Demoted:
            return "demoted";
        case KernelCheckStatus::FloorFailed:
            return "floor-failed";
    }
    return "unknown";
}

} // namespace crd
