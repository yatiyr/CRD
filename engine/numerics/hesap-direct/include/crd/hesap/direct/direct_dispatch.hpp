// The hesap-direct kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5) and the f64 and f32 kernels of a table
// (crd/hesap/direct/direct_kernels.hpp). The element types are dense::kHasDenseKernels'.

#pragma once

#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/direct/direct_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>
#include <type_traits>

namespace crd::hesap::direct
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<DirectKernels>> direct_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<DirectKernels>& direct_kernels() noexcept;

// The kernels of element type T in a table, and in the one the process runs.
template <typename T> [[nodiscard]] constexpr const DirectTypeKernels<T>& kernels_of(const DirectKernels& k) noexcept
{
    static_assert(dense::kHasDenseKernels<T>, "the direct kernels are f64 and f32");
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return k.f64_kernels;
    }
    else
    {
        return k.f32_kernels;
    }
}

template <typename T> [[nodiscard]] const DirectTypeKernels<T>& direct_kernels_of() noexcept
{
    return kernels_of<T>(*direct_kernels().table);
}

} // namespace crd::hesap::direct
