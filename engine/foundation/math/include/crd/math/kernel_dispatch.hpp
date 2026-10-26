// Runtime CPU dispatch of kernel families (SIMD-DISPATCH, ADR-0134 §5).
//
// A kernel family is a table of function pointers, one per routine (a whole conversion or panel loop, never a
// microkernel). Its kernel units define one table per CPU level they are built for (crd_add_cpu_kernels,
// cmake/CrdSimd.cmake), and the family's dispatcher lists the built tables in ascending level order, portable first.
// The process resolves the table once from cpu_selection().selected; every built level stays reachable for the
// cross-level differential tests.

#pragma once

#include <crd/core/cpu_features.hpp>
#include <crd/core/types.hpp>

#include <span>

namespace crd::math
{

template <typename Table> struct KernelLevel
{
    crd::CpuLevel level = crd::CpuLevel::Portable;
    const Table* table = nullptr;
};

// The highest level this build compiles kernels for: CRD_SIMD_LEVEL scalar gives portable, sse2 gives sse2, avx2
// gives avx2, auto gives the architecture's top level (per configuration under the native Visual Studio profiles).
[[nodiscard]] crd::CpuLevel kernel_level_cap() noexcept;

// The level a process at `selected` runs: the last of `levels` (ascending, portable first) that both `selected` and
// `cap` include.
template <typename Table>
[[nodiscard]] const KernelLevel<Table>& select_kernel_level(std::span<const KernelLevel<Table>> levels,
                                                            crd::CpuLevel selected, crd::CpuLevel cap) noexcept
{
    const KernelLevel<Table>* chosen = levels.data();
    for (const KernelLevel<Table>& entry : levels)
    {
        if (crd::cpu_level_includes(selected, entry.level) && crd::cpu_level_includes(cap, entry.level))
        {
            chosen = &entry;
        }
    }
    return *chosen;
}

} // namespace crd::math
