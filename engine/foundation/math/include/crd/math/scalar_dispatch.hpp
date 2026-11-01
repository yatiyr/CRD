// The scalar transcendental tables this archive built and the one the process runs (SIMD-DISPATCH, ADR-0134 §5): the
// list for the cross-level differential tests; general code reaches the routines through crd/math/scalar_kernels.hpp.

#pragma once

#include <crd/math/kernel_dispatch.hpp>
#include <crd/math/scalar_kernels.hpp>

#include <span>

namespace crd::math
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const KernelLevel<ScalarMathKernels>> scalar_math_levels() noexcept;
[[nodiscard]] const KernelLevel<ScalarMathKernels>& scalar_math_level() noexcept;

// The family's known-answer self-check (HW-RESILIENCE.3, crd/core/kernel_dispatch.hpp), which its resolution runs:
// nullptr when every routine of `table` returns its known bits, else the name of the first that did not.
[[nodiscard]] const char* scalar_math_self_check(const ScalarMathKernels& table) noexcept;

} // namespace crd::math
