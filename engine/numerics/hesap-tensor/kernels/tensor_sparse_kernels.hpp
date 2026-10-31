// The sparse tensor entries of the tensor kernel table (crd/hesap/tensor/tensor_kernels.hpp SparseTensorKernels),
// defined in tensor_sparse_kernels.cpp and named by the table in tensor_kernels.cpp: declarations only, in the level's
// namespace.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{

template <typename T> struct SparseKernel
{
    static void ttm_scatter(const SparseTtmWalk<T>& w, crd::u64 eb, crd::u64 ee, T* dst) noexcept;
    static void ttm_staged(const SparseTtmWalk<T>& w, crd::u64 eb, crd::u64 ee, T* tile, T* out) noexcept;
    static void fold(T* out, const T* parts, crd::u64 osize, crd::u32 nparts) noexcept;
    static void mttkrp_csf(const MttkrpCsfWalk<T>& w, crd::u64 rb, crd::u64 re, T* tmp) noexcept;
    static void mttkrp_coo(const MttkrpCooWalk<T>& w, T* tmp) noexcept;
};

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
