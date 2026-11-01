// The hesap-stats kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): Philox and Threefry bulk fills and the
// Student-t score reduction (crd/hesap/stats/stats_kernels.hpp).

#pragma once

#include <crd/hesap/stats/stats_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::hesap::stats
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<StatsKernels>> stats_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<StatsKernels>& stats_kernels() noexcept;

// The family's known-answer self-check (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which its resolution runs:
// nullptr when the generators and the Student-t score sum of `table` give their known
// answers, else the failing routine's name.
[[nodiscard]] const char* stats_self_check(const StatsKernels& table) noexcept;

} // namespace crd::hesap::stats
