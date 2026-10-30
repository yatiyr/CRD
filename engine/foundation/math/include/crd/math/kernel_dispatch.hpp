// Runtime CPU dispatch of kernel families (SIMD-DISPATCH, ADR-0134 §5).
//
// A kernel family is a table of function pointers, one per routine (a whole conversion or panel loop, never a
// microkernel). Its kernel units define one table per CPU level they are built for and one per feature variant of a
// level (crd_add_cpu_kernels VARIANTS, cmake/CrdSimd.cmake: the level's unit built again with one more feature, such
// as AVX-VNNI on avx2), and the family's dispatcher lists the built tables in ascending level order, portable first,
// each variant after its level's plain table. The process resolves the table once from cpu_selection(); every built
// table stays reachable for the cross-level differential tests.

#pragma once

#include <crd/core/cpu_features.hpp>
#include <crd/core/types.hpp>
#include <crd/math/determinism_mode.hpp>

#include <span>

namespace crd::math
{

template <typename Table> struct KernelLevel
{
    crd::CpuLevel level = crd::CpuLevel::Portable;
    const Table* table = nullptr;
    crd::CpuFeatureSet features; // a variant's features beyond its level; empty for a level's plain table
};

// The determinism mode the process's kernel families plan under (ADR-0134 §7), Default until set. Under Default each
// CPU level runs the plan its family measured fastest there, within the family's declared tolerance across levels;
// under CrossPlatform and BackwardCompat every level runs the family's one canonical plan, so a bit-exact family gives
// the same bits on every level (replays, lockstep, golden tests). A plan reads the mode when it is built; code sets it
// (no environment variable), before it builds the plans whose bits must match; a scene's eylem configuration names its
// own mode and does not set this one.
[[nodiscard]] DeterminismMode kernel_determinism() noexcept;
void set_kernel_determinism(DeterminismMode mode) noexcept;

// The highest level this build compiles kernels for: CRD_SIMD_LEVEL scalar gives portable, sse2 gives sse2, avx2
// gives avx2, auto gives the architecture's top level (per configuration under the native Visual Studio profiles).
[[nodiscard]] crd::CpuLevel kernel_level_cap() noexcept;

// The table a process at `selected` with `features` (CpuSelection::kernel_features) runs: the last of `levels`
// (ascending, portable first) whose level both `selected` and `cap` include and whose features `features` holds.
template <typename Table>
[[nodiscard]] const KernelLevel<Table>& select_kernel_level(std::span<const KernelLevel<Table>> levels,
                                                            crd::CpuLevel selected, crd::CpuFeatureSet features,
                                                            crd::CpuLevel cap) noexcept
{
    const KernelLevel<Table>* chosen = levels.data();
    for (const KernelLevel<Table>& entry : levels)
    {
        if (crd::cpu_level_includes(selected, entry.level) && crd::cpu_level_includes(cap, entry.level) &&
            features.has_all(entry.features))
        {
            chosen = &entry;
        }
    }
    return *chosen;
}

// The table this process runs: select_kernel_level at cpu_selection()'s level and kernel features and the build cap.
template <typename Table>
[[nodiscard]] const KernelLevel<Table>& process_kernel_level(std::span<const KernelLevel<Table>> levels) noexcept
{
    const crd::CpuSelection& s = crd::cpu_selection();
    return select_kernel_level(levels, s.selected, s.kernel_features, kernel_level_cap());
}

} // namespace crd::math
