#pragma once

// The checksum kernel levels the process runs (SIMD-DISPATCH, ADR-0134 §5; crd/containers/checksum_kernels.hpp).

#include <crd/containers/checksum_kernels.hpp>
#include <crd/core/kernel_dispatch.hpp>

#include <span>

namespace crd::containers
{

// Every table this archive built, ascending (a variant after its level's place), and the one the process runs
// (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<CrcKernels>> crc_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<CrcKernels>& crc_kernels() noexcept;
[[nodiscard]] std::span<const crd::KernelLevel<HashKernels>> hash_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<HashKernels>& hash_kernels() noexcept;

} // namespace crd::containers
