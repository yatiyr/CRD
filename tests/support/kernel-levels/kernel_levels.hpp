// The levels a kernel family's differential case runs (ADR-0134 §7, §8). crd_add_kernel_level_tests
// (cmake/CrdSimd.cmake) puts this header on every kernel suite's include path.
//
// The lane's own run executes every level the host does, each against portable's bits, in one process. Under an
// applied CRD_CPU_LEVEL (a <suite>-kernels-at-<level> arm, an emulated avx512 arm or a lane's -at-sse2 pass) a case
// runs portable and the selected level only: the arm exists for its own level, and the lane's run covers the others. A
// refused level runs every level the host executes, as the lane's run does. A feature variant also needs its features
// usable on the host; an arm's masked kernel_features would hide the variant its SDE CPU model was chosen for.
//
// Each family's known-answer self-check (HW-RESILIENCE.3) runs on the same tables: self_check_levels() checks every
// table the host runs, and kernel_family() finds a family's resolution record.

#pragma once

#include <crd/core/cpu_features.hpp>
#include <crd/core/kernel_dispatch.hpp>

#include <span>
#include <string_view>

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

template <typename Table> [[nodiscard]] bool kernel_level_runs_here(const crd::KernelLevel<Table>& level) noexcept
{
    return kernel_level_runs_here(level.level, level.features);
}

// What a family's self-check found on the tables the host runs: how many ran and failed, and the first failure's table
// and routine.
struct SelfCheckRun
{
    crd::usize ran = 0U;
    crd::usize failed = 0U;
    crd::CpuLevel level = crd::CpuLevel::Portable;
    crd::CpuFeatureSet features;
    const char* routine = nullptr;
};

template <typename Table>
[[nodiscard]] SelfCheckRun self_check_levels(std::span<const crd::KernelLevel<Table>> levels,
                                             crd::KernelSelfCheck<Table> check) noexcept
{
    SelfCheckRun run;
    for (const crd::KernelLevel<Table>& level : levels)
    {
        if (!kernel_level_runs_here(level))
        {
            continue;
        }
        ++run.ran;
        const char* failed = check(*level.table);
        if (failed != nullptr && run.failed++ == 0U)
        {
            run.level = level.level;
            run.features = level.features;
            run.routine = failed;
        }
    }
    return run;
}

// The listed family `name` after every family has resolved, or nullptr when the image does not link it.
[[nodiscard]] inline const crd::KernelFamily* kernel_family(std::string_view name) noexcept
{
    (void)crd::run_kernel_self_checks();
    for (const crd::KernelFamily* f = crd::kernel_families(); f != nullptr; f = f->next)
    {
        if (name == f->name)
        {
            return f;
        }
    }
    return nullptr;
}

} // namespace crd::test
