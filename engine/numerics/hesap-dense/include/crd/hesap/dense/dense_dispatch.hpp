// The hesap-dense kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the f64 and f32 GEMM blocks
// (crd/hesap/dense/dense_kernels.hpp).

#pragma once

#include <crd/hesap/dense/dense_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::hesap::dense
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::math::KernelLevel<DenseKernels>> dense_kernel_levels() noexcept;
[[nodiscard]] const crd::math::KernelLevel<DenseKernels>& dense_kernels() noexcept;

} // namespace crd::hesap::dense
