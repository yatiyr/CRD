#pragma once

// crd-memory -- what an allocator's statistics cover (DIAG.11b).
//
// A build with CRD_DEBUG counts every allocator's operations from construction. A profiling build without CRD_DEBUG
// (win-shipping-profile) counts an allocator only once a reader requests it, so the runtime-disabled mode pays one
// relaxed load per operation instead of five atomic read-modify-writes (ADR-0133 ID-5). A build without either compiles
// the statistics out. This header holds only the answer a reader gets, so the profiler's public headers can carry it
// without the counters themselves.

#include <crd/core/types.hpp>

namespace crd::memory
{
enum class StatisticsCoverage : u8
{
    // Every allocation and free since the allocator was constructed: a CRD_DEBUG build, or a request made before the
    // allocator's first allocation.
    Complete = 0,
    // Counted only on request, and nothing has requested it (the runtime-disabled mode of a profiling build).
    NotRequested = 1,
    // Requested after the allocator's first allocation. A free cannot tell a block allocated before the request from
    // one allocated after it, so nothing is counted rather than a balance that drifts.
    TooLate = 2,
    // The allocator keeps no counts of its own: a decorator, a composite or a test double. The allocators it forwards
    // to, or is built from, keep theirs.
    Elsewhere = 3,
    // This build compiles the statistics out (neither CRD_DEBUG nor CRD_ENABLE_PROFILING).
    CompiledOut = 4,
    // Only a reader of recorded statistics sees this: the record carries no coverage (a capture written before the
    // profiler stamped it, 2026-10-19) or one this build does not know. A live allocator never reports it.
    Unrecorded = 5,
};

// Stable lower-case name, used by the diagnostic commands and the profiler panel.
[[nodiscard]] constexpr const char* statistics_coverage_name(StatisticsCoverage c) noexcept
{
    switch (c)
    {
        case StatisticsCoverage::Complete:
            return "complete";
        case StatisticsCoverage::NotRequested:
            return "not_requested";
        case StatisticsCoverage::TooLate:
            return "too_late";
        case StatisticsCoverage::Elsewhere:
            return "elsewhere";
        case StatisticsCoverage::CompiledOut:
            return "compiled_out";
        case StatisticsCoverage::Unrecorded:
            return "unrecorded";
    }
    return "unrecorded";
}
} // namespace crd::memory
