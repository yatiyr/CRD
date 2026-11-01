// The hesap-tensor kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the routines of
// crd/hesap/tensor/tensor_kernels.hpp.

#pragma once

#include <crd/hesap/tensor/tensor_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::hesap::tensor
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<TensorKernels>> tensor_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<TensorKernels>& tensor_kernels() noexcept;

// The family's known-answer self-check (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which its resolution runs:
// nullptr when every routine of `table` gives its known answers, else the failing routine's name.
[[nodiscard]] const char* tensor_self_check(const TensorKernels& table) noexcept;

} // namespace crd::hesap::tensor
