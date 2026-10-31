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

} // namespace crd::hesap::autodiff
