// The geometry-primitives kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5): the batch intersection
// kernels and the hull support scan (crd/geometry/primitives/primitive_kernels.hpp).

#pragma once

#include <crd/geometry/primitives/primitive_kernels.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/primitives/robust_ray_aabb.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>

namespace crd::geometry::primitives
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::math::KernelLevel<PrimitiveKernels>> primitive_kernel_levels() noexcept;
[[nodiscard]] const crd::math::KernelLevel<PrimitiveKernels>& primitive_kernels() noexcept;

// The kernels' plain form of a ray and its slab precompute.
[[nodiscard]] RaySlab ray_slab(const Ray3<crd::f32>& ray, const RayAABBPrecompute<crd::f32>& pre) noexcept;

} // namespace crd::geometry::primitives
