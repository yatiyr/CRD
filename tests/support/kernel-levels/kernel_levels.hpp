// The levels a kernel family's differential case runs (ADR-0134 §7, §8). crd_add_kernel_level_tests
// (cmake/CrdSimd.cmake) puts this header on every kernel suite's include path.
//
// The lane's own run executes every level the host does, each against portable's bits, in one process. Under an
// applied CRD_CPU_LEVEL (a <suite>-kernels-at-<level> arm, an emulated avx512 arm or a lane's -at-sse2 pass) a case
// runs portable and the selected level only: the arm exists for its own level, and the lane's run covers the others. A
// refused level runs every level the host executes, as the lane's run does. A feature variant also needs its features
// usable on the host; an arm's masked kernel_features would hide the variant its SDE CPU model was chosen for.

#pragma once

#include <crd/core/cpu_features.hpp>
#include <crd/math/kernel_dispatch.hpp>

namespace crd::test
{

[[nodiscard]] inline bool kernel_level_runs_here(crd::CpuLevel level, crd::CpuFeatureSet features = {}) noexcept
{
    const crd::CpuSelection& s = crd::cpu_selection();
    if (!s.features.usable.has_all(features))
    {
        return false;
    }
    if (s.override_status == crd::CpuOverride::Applied)
    {
        return level == crd::CpuLevel::Portable || level == s.selected;
    }
    return crd::cpu_level_includes(s.detected, level);
}

template <typename Table> [[nodiscard]] bool kernel_level_runs_here(const crd::math::KernelLevel<Table>& level) noexcept
{
    return kernel_level_runs_here(level.level, level.features);
}

} // namespace crd::test
