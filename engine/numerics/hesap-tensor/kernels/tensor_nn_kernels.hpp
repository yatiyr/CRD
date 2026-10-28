// The f32 CNN entries of the tensor kernel table (crd/hesap/tensor/tensor_kernels.hpp NnPool2d, NnConv2d), defined in
// tensor_nn_kernels.cpp and named by the table in tensor_kernels.cpp: declarations only, in the level's namespace.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{

struct NnKernel
{
    static void pool2d_planes(const NnPool2d& c, crd::u32 lo, crd::u32 hi) noexcept;
    static void conv2d_images(const NnConv2d& c, crd::f32* scratch, crd::u32 lo, crd::u32 hi) noexcept;
};

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
