// The hesap-sparse kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the SELL-C-σ spmv of every
// value type and the real SDDMM rows (crd/hesap/sparse/sparse_kernels.hpp).

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
// (crd/core/cpu_features.hpp: CPUID's, else the OS's), 2 MiB where neither describes one. It picks the SELL sweep and
// the operators' serial-or-parallel threshold.
[[nodiscard]] crd::usize sparse_resident_bytes() noexcept;

// What one job of a parallel sparse product counts as cache resident while every logical CPU runs one: its share of
// the smallest L2 (CpuSelection::l2_share_bytes), sparse_resident_bytes() where the OS describes no L2.
[[nodiscard]] crd::usize sparse_job_resident_bytes() noexcept;

} // namespace crd::hesap::sparse
