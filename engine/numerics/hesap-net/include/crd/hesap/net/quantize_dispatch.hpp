// The quantisation kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the bounded floats and smallest-three
// quaternions of crd/hesap/net/quantize_kernels.hpp.

#pragma once

#include <crd/core/kernel_dispatch.hpp>
#include <crd/hesap/net/quantize_kernels.hpp>

#include <span>

namespace crd::hesap::net
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<QuantizeKernels>> quantize_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<QuantizeKernels>& quantize_kernels() noexcept;

// The family's known-answer self-check (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which its resolution runs:
// nullptr when `table` gives its known answers, else the failing routine's name.
[[nodiscard]] const char* quantize_self_check(const QuantizeKernels& table) noexcept;

} // namespace crd::hesap::net
