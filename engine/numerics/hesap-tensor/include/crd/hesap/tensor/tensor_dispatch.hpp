// The hesap-tensor kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the f32 permute regions and the
// Tier R binned sum (crd/hesap/tensor/tensor_kernels.hpp).

#pragma once

#include <crd/hesap/tensor/tensor_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::hesap::tensor
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::math::KernelLevel<TensorKernels>> tensor_kernel_levels() noexcept;
[[nodiscard]] const crd::math::KernelLevel<TensorKernels>& tensor_kernels() noexcept;

} // namespace crd::hesap::tensor
