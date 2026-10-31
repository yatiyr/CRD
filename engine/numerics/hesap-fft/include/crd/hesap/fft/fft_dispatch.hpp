// The hesap-fft kernel level the process runs (SIMD-DISPATCH, ADR-0134 §5) and the f64 and f32 kernels of a table
// (crd/hesap/fft/fft_kernels.hpp).

#pragma once

#include <crd/hesap/fft/fft_kernels.hpp>
#include <crd/math/kernel_dispatch.hpp>

#include <span>
#include <type_traits>

namespace crd::hesap::fft
{

// Every level this archive built, ascending, and the one the process runs (resolved once from cpu_selection()).
[[nodiscard]] std::span<const crd::KernelLevel<FftKernels>> fft_kernel_levels() noexcept;
[[nodiscard]] const crd::KernelLevel<FftKernels>& fft_kernels() noexcept;

// The kernels of element type T (f64 or f32, the types FftPlan is built for) in a table, and in the one the process
// runs.
template <typename T> [[nodiscard]] constexpr const FftTypeKernels<T>& kernels_of(const FftKernels& k) noexcept
{
    static_assert(std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>, "the fft kernels are f64 and f32");
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return k.f64_kernels;
    }
    else
    {
        return k.f32_kernels;
    }
}

template <typename T> [[nodiscard]] const FftTypeKernels<T>& fft_kernels_of() noexcept
{
    return kernels_of<T>(*fft_kernels().table);
}

} // namespace crd::hesap::fft
