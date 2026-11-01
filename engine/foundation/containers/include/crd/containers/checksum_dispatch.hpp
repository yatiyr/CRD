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

// The families' known-answer self-checks (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which their resolution runs:
// nullptr when every routine of `table` reproduces its known answers, else the name of the first that did not.
[[nodiscard]] const char* crc_self_check(const CrcKernels& table) noexcept;
[[nodiscard]] const char* hash_self_check(const HashKernels& table) noexcept;

} // namespace crd::containers
