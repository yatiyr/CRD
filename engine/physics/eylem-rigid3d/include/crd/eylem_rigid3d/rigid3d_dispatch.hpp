// The eylem-rigid3d kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the fixed-substep body step
// (crd/eylem_rigid3d/rigid3d_kernels.hpp).

#pragma once

#include <crd/eylem_rigid3d/rigid3d_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::eylem_rigid3d
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<Rigid3dKernels>> rigid3d_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<Rigid3dKernels>& rigid3d_kernels() noexcept;

} // namespace crd::eylem_rigid3d
