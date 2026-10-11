#pragma once

// Host memory pressure: the operating system's view of memory, as budget readings (DIAG.10c).
//
// A device loss, a failed allocation or a stall on a machine that is out of memory is not an engine defect until a
// reproduction says so. Before anything is attributed, the evidence has to exist: how full physical memory and the
// commit charge were, and how much this process held, at the moment. `read_host_memory` asks the OS and reports each
// as a `BudgetReading` (crd/core/budget_pressure.hpp), so the same probe judges it as every other bounded resource.
//
// - physical: the bound is the usable physical memory, in use is that minus what the OS reports available
//   (Windows: GlobalMemoryStatusEx; Linux: /proc/meminfo MemTotal and MemAvailable). No high water is kept.
// - commit: the commit charge against the commit limit. Windows enforces the limit (a commit beyond it fails) and
//   keeps the system's peak (GetPerformanceInfo CommitPeak). Linux enforces CommitLimit only in strict overcommit
//   mode (vm.overcommit_memory = 2); otherwise the reading is unbounded and its use is reported, never judged.
// - resident: this process's resident set (Windows: the working set; Linux: VmRSS), with its peak (Windows:
//   PeakWorkingSetSize; Linux: VmHWM). It has no bound.
//
// A reading the OS could not give is `read == false` and `reason` says why; it is never reported as zero use. No
// heap memory is allocated (the Linux files are read into a stack buffer), so it may be called on a failure path.
// One call judges one moment; system-wide numbers move with every other process, only the resident set is this
// process's own.

#include <crd/core/budget_pressure.hpp>
#include <crd/core/types.hpp>

namespace crd
{

struct HostMemoryReading
{
    BudgetReading physical;
    BudgetReading commit;
    BudgetReading resident;
    bool          commit_enforced = false; // the OS refuses a commit beyond the limit
    const char*   reason          = "";    // a static string, set when any reading could not be taken
};

// The host's memory now.
[[nodiscard]] HostMemoryReading read_host_memory() noexcept;

// The usable physical memory through a second OS interface (Windows: GetPerformanceInfo; Linux: sysinfo), sharing no
// code with `read_host_memory`: the independent check of `physical.capacity`. 0 when it could not be asked.
[[nodiscard]] crd::u64 host_physical_total_cross_check() noexcept;

} // namespace crd
