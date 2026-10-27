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

// The working set a sparse product counts as cache resident: the smallest L2 of the process's CPUs
// (crd/core/cpu_features.hpp), 2 MiB where no CPUID leaf describes the caches. It picks the SELL sweep and the
// operators' serial-or-parallel threshold.
[[nodiscard]] crd::usize sparse_resident_bytes() noexcept;

} // namespace crd::hesap::sparse
