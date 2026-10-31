// The batched entries of the tensor kernel table (crd/hesap/tensor/tensor_kernels.hpp BatchedKernels), defined in
// tensor_batched_kernels.cpp and named by the table in tensor_kernels.cpp: declarations only, in the level's namespace.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{

template <typename T> struct BatchedKernel
{
    static void gemm_tiny(const BatchedGemm<T>& g, crd::u64 b0, crd::u64 b1) noexcept;
    static void cholesky(const BatchedFactor<T>& f, crd::u64 b0, crd::u64 b1) noexcept;
    static void lu(const BatchedFactor<T>& f, crd::u64 b0, crd::u64 b1) noexcept;
    static void svd(const BatchedSvd<T>& s, crd::u64 b0, crd::u64 b1) noexcept;
    static void cholesky_solve(const BatchedSolve<T>& s, crd::u64 b0, crd::u64 b1) noexcept;
    static void lu_solve(const BatchedSolve<T>& s, crd::u64 b0, crd::u64 b1) noexcept;
};

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
