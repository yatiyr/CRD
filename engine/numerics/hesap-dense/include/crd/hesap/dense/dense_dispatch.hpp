// The hesap-dense kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5) and the f64 and f32 kernels of a table
// (crd/hesap/dense/dense_kernels.hpp).

#pragma once

#include <crd/hesap/dense/dense_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>
#include <type_traits>

namespace crd::hesap::dense
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::math::KernelLevel<DenseKernels>> dense_kernel_levels() noexcept;
[[nodiscard]] const crd::math::KernelLevel<DenseKernels>& dense_kernels() noexcept;

// The element types with dense kernels; the others run their scalar loops.
template <typename T>
inline constexpr bool kHasDenseKernels = std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>;

// The kernels of element type T in a table, and in the one the process runs.
template <typename T> [[nodiscard]] constexpr const DenseTypeKernels<T>& kernels_of(const DenseKernels& k) noexcept
{
    static_assert(kHasDenseKernels<T>, "the dense kernels are f64 and f32");
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return k.f64_kernels;
    }
    else
    {
        return k.f32_kernels;
    }
}

template <typename T> [[nodiscard]] const DenseTypeKernels<T>& dense_kernels_of() noexcept
{
    return kernels_of<T>(*dense_kernels().table);
}

} // namespace crd::hesap::dense
