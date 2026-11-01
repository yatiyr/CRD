// The hesap-autodiff kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the canned functions' forward
// gradient (crd/hesap/autodiff/autodiff_kernels.hpp).

#pragma once

#include <crd/hesap/autodiff/autodiff_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::hesap::autodiff
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<AutodiffKernels>> autodiff_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<AutodiffKernels>& autodiff_kernels() noexcept;

// The family's known-answer self-check (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which its resolution runs:
// nullptr when the canned gradients of `table` give their known answers, else
// "gradient_f64".
[[nodiscard]] const char* autodiff_self_check(const AutodiffKernels& table) noexcept;

} // namespace crd::hesap::autodiff
