// The hesap-sparse kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the SELL-C-σ spmv of every
// value type (crd/hesap/sparse/sparse_kernels.hpp).

#pragma once

#include <crd/hesap/sparse/sparse_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::hesap::sparse
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::math::KernelLevel<SparseKernels>> sparse_kernel_levels() noexcept;
[[nodiscard]] const crd::math::KernelLevel<SparseKernels>& sparse_kernels() noexcept;

} // namespace crd::hesap::sparse
