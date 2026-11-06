// The erasure-code kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the GF(2^8) dot product of
// crd/hesap/net/erasure_kernels.hpp.

#pragma once

#include <crd/core/kernel_dispatch.hpp>
#include <crd/hesap/net/erasure_kernels.hpp>

#include <span>

namespace crd::hesap::net
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<ErasureKernels>> erasure_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<ErasureKernels>& erasure_kernels() noexcept;

// The family's known-answer self-check (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which its resolution runs:
// nullptr when `table`'s dot product gives its known answers, else the failing routine's name.
[[nodiscard]] const char* erasure_self_check(const ErasureKernels& table) noexcept;

} // namespace crd::hesap::net
