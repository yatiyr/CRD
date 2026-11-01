// Runtime CPU dispatch of kernel families (SIMD-DISPATCH, ADR-0134 §5).
//
// A kernel family is a table of function pointers, one per routine (a whole conversion or panel loop, never a
// microkernel). Its kernel units define one table per CPU level they are built for and one per feature variant of a
// level (crd_add_cpu_kernels VARIANTS, cmake/CrdSimd.cmake: the level's unit built again with one more feature, such
// as AVX-VNNI on avx2), and the family's dispatcher lists the built tables in ascending level order, portable first,
// each variant after its level's plain table. The process resolves the table once from cpu_selection(); every built
// table stays reachable for the cross-level differential tests. The mechanism sits beside the detection in crd-core so
// a family of any module, the foundation's own included, dispatches the same way.
//
// Known-answer self-checks (HW-RESILIENCE.3): a family's resolution runs its self-check on the table the CPU selects
// before the process uses it. A table whose routines do not reproduce their known answers bit for bit (a CPU that
// misexecutes that level's instructions) is passed over for the next eligible table below it, a failing feature
// variant for its level's plain table, down to the portable one; the process keeps running on the table that passed,
// and the family's KernelCheckResult says which table failed, in which routine. Every family is listed when the image
// starts (KernelFamilyListing), so run_kernel_self_checks() reaches each one the image links and the doctor reports
// them all.

#pragma once

#include <crd/core/cpu_features.hpp>
#include <crd/core/fault_seam.hpp>
#include <crd/core/types.hpp>

#include <span>

namespace crd
{

template <typename Table> struct KernelLevel
{
    crd::CpuLevel level = crd::CpuLevel::Portable;
    const Table* table = nullptr;
    crd::CpuFeatureSet features; // a variant's features beyond its level; empty for a level's plain table
};

// The highest level this build compiles kernels for: CRD_SIMD_LEVEL scalar gives portable, sse2 gives sse2, avx2
// gives avx2, auto gives the architecture's top level (per configuration under the native Visual Studio profiles).
[[nodiscard]] crd::CpuLevel kernel_level_cap() noexcept;

// Whether a process at `selected` with `features` under `cap` may run `entry`.
template <typename Table>
[[nodiscard]] bool kernel_level_eligible(const KernelLevel<Table>& entry, crd::CpuLevel selected,
                                         crd::CpuFeatureSet features, crd::CpuLevel cap) noexcept
{
    return crd::cpu_level_includes(selected, entry.level) && crd::cpu_level_includes(cap, entry.level) &&
           features.has_all(entry.features);
}

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
        if (kernel_level_eligible(entry, selected, features, cap))
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

// ---- Known-answer self-checks (HW-RESILIENCE.3) ---------------------------------------------------------------------

// A family's self-check: runs every routine of `table` through its function pointers on fixed inputs and compares each
// result with its known answer bit for bit. nullptr when all match, else the static name of the first routine that
// did not. It allocates nothing and finishes in microseconds.
template <typename Table> using KernelSelfCheck = const char* (*)(const Table& table) noexcept;

enum class KernelCheckStatus : crd::u8
{
    NotRun,      // not resolved
    Passed,      // the selected table passed and runs
    Demoted,     // the selected table failed; a lower one passed and runs
    FloorFailed, // every eligible table failed, the portable one too: the portable table runs (the CPU misexecutes
                 // plain scalar code, a likely hardware fault)
};

// What a family's resolution found.
struct KernelCheckResult
{
    KernelCheckStatus status = KernelCheckStatus::NotRun;
    crd::CpuLevel selected_level = crd::CpuLevel::Portable; // the table select_kernel_level names
    crd::CpuFeatureSet selected_features;
    crd::CpuLevel level = crd::CpuLevel::Portable; // the table that runs
    crd::CpuFeatureSet features;
    const char* failed_routine = nullptr; // the selected table's first failing routine (Demoted, FloorFailed)
    bool injected = false;                // that failure was the kernel-check fault seam's, not the check's
    crd::u32 tables_failed = 0U;          // eligible tables that failed, the selected one included
};

// The checked table a process at `selected` with `features` under `cap` runs: the eligible tables from
// select_kernel_level's choice down, each checked (or failed by the kernel-check fault seam, consulted with `family`)
// until one passes; the portable table when none does. `result` says what happened.
template <typename Table>
[[nodiscard]] const KernelLevel<Table>&
select_checked_kernel_level(std::span<const KernelLevel<Table>> levels, crd::CpuLevel selected,
                            crd::CpuFeatureSet features, crd::CpuLevel cap, const char* family,
                            KernelSelfCheck<Table> check, KernelCheckResult& result) noexcept
{
    const KernelLevel<Table>& top = select_kernel_level(levels, selected, features, cap);
    result = KernelCheckResult{};
    result.selected_level = top.level;
    result.selected_features = top.features;
    const auto top_index = static_cast<crd::usize>(&top - levels.data());
    for (crd::usize step = 0U; step <= top_index; ++step)
    {
        const crd::usize i = top_index - step;
        const KernelLevel<Table>& entry = levels[i];
        if (i != 0U && !kernel_level_eligible(entry, selected, features, cap))
        {
            continue;
        }
        const bool injected = crd::consult_kernel_check_fault(family, entry.level, entry.features);
        const char* failed = injected ? "injected" : check(*entry.table);
        if (failed == nullptr)
        {
            result.status = result.tables_failed == 0U ? KernelCheckStatus::Passed : KernelCheckStatus::Demoted;
            result.level = entry.level;
            result.features = entry.features;
            return entry;
        }
        if (result.tables_failed == 0U)
        {
            result.failed_routine = failed;
            result.injected = injected;
        }
        ++result.tables_failed;
    }
    result.status = KernelCheckStatus::FloorFailed;
    result.level = levels.front().level;
    result.features = levels.front().features;
    return levels.front();
}

// One dispatched family: a constant-initialised object of its dispatch unit (constinit), with a KernelFamilyListing
// beside it. `resolve` calls the family's accessor, whose first call runs process_checked_kernel_level; `result` is
// written by that call and read after calling `resolve` (the accessor's static guard orders the two).
struct KernelFamily
{
    const char* name = "";                // "<module>.<family>", for example "containers.crc"
    void (*resolve)() noexcept = nullptr; // resolves the family's table, once per process
    KernelCheckResult result;
    const KernelFamily* next = nullptr; // the listing's link
};

// Lists `family` for run_kernel_self_checks() and kernel_families(): a namespace-scope object beside it, so the
// family is listed when the image starts.
class KernelFamilyListing
{
public:
    explicit KernelFamilyListing(KernelFamily& family) noexcept;
};

// The listed families, the last listed first.
[[nodiscard]] const KernelFamily* kernel_families() noexcept;

// Resolves every listed family, each running its self-check at most once per process, and returns how many run a
// table other than their selected one (Demoted or FloorFailed).
crd::u32 run_kernel_self_checks() noexcept;

// The checked table this process runs: select_checked_kernel_level at cpu_selection()'s level and kernel features and
// the build cap, `family.result` written. Call it once per family, from the function-local static of its accessor.
template <typename Table>
[[nodiscard]] const KernelLevel<Table>& process_checked_kernel_level(std::span<const KernelLevel<Table>> levels,
                                                                     KernelSelfCheck<Table> check,
                                                                     KernelFamily& family) noexcept
{
    const crd::CpuSelection& s = crd::cpu_selection();
    return select_checked_kernel_level(levels, s.selected, s.kernel_features, kernel_level_cap(), family.name, check,
                                       family.result);
}

[[nodiscard]] const char* kernel_check_status_name(KernelCheckStatus status) noexcept;

} // namespace crd
