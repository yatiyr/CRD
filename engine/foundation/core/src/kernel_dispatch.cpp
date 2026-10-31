// The build's kernel level cap (SIMD-DISPATCH, ADR-0134 §4): CRD_KERNEL_LEVEL_CAP comes from CrdSimd.cmake, the value
// crd-simd-flags carries.

#include <crd/core/kernel_dispatch.hpp>
#include <crd/core/kernel_level.hpp>

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

} // namespace crd
