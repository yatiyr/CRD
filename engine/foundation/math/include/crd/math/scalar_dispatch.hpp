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

} // namespace crd::math
