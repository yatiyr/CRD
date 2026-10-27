// The hesap-special kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the Lanczos lgamma and tgamma batch
// loops (crd/hesap/special/special_kernels.hpp).

#pragma once

#include <crd/hesap/special/special_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::hesap::special
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::math::KernelLevel<SpecialKernels>> special_kernel_levels() noexcept;
[[nodiscard]] const crd::math::KernelLevel<SpecialKernels>& special_kernels() noexcept;

} // namespace crd::hesap::special
