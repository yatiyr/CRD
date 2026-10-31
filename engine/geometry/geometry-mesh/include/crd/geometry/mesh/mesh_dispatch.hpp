// The geometry-mesh kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the BVH raycast with Moller-Trumbore
// on eight leaf triangles a step (crd/geometry/mesh/mesh_kernels.hpp).

#pragma once

#include <crd/geometry/mesh/mesh_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::geometry::mesh
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<MeshKernels>> mesh_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<MeshKernels>& mesh_kernels() noexcept;

} // namespace crd::geometry::mesh
