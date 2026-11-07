// The replicated-motion kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the interpolation and extrapolation
// of crd/hesap/net/motion_kernels.hpp.

#pragma once

#include <crd/core/kernel_dispatch.hpp>
#include <crd/hesap/net/motion_kernels.hpp>

#include <span>

namespace crd::hesap::net
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<MotionKernels>> motion_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<MotionKernels>& motion_kernels() noexcept;

// The family's known-answer self-check (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which its resolution runs:
// nullptr when `table` gives its known answers, else the failing routine's name.
[[nodiscard]] const char* motion_self_check(const MotionKernels& table) noexcept;

} // namespace crd::hesap::net
