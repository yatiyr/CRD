// The elementwise, Tier D and einsum entries of the tensor kernel table (crd/hesap/tensor/tensor_kernels.hpp), defined
// in tensor_ew_kernels.cpp and named by the table in tensor_kernels.cpp: declarations only, in the level's namespace.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{

// TensorKernels::ew_binary_f32[Op] and ew_binary_f64[Op].
template <typename T, BinaryOp Op> struct EwBinaryKernel
{
    static void walk(const EwBinaryWalk<T>& walk) noexcept;
};

// The ReduceKernels of Op.
template <typename T, ReduceOp Op> struct TierDKernel
{
    static T block(const T* p, crd::u64 n, T init) noexcept;
    static void rows(T* out, const T* src, crd::u64 len, crd::u64 o0, crd::u64 o1) noexcept;
    static void columns(T* out, const T* src, crd::u64 rows, crd::u64 w, crd::u64 j0, crd::u64 j1) noexcept;
};

// TensorKernels::einsum_thin_* and einsum_smallm_*.
template <typename T> struct EinsumKernel
{
    static void thin(crd::u64 m, crd::u64 k, crd::u64 n, const T* a, const T* b, T* c) noexcept;
    static void smallm(crd::u64 m, crd::u64 k, crd::u64 n, const T* a, const T* b, T* c, bool a_trans) noexcept;
};

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
