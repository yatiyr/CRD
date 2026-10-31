// The f32 CNN, layernorm and activation entries of the tensor kernel table (crd/hesap/tensor/tensor_kernels.hpp
// NnPool2d, NnConv2d, NnLayerNormF32, NnActF32), defined in tensor_nn_kernels.cpp and named by the table in
// tensor_kernels.cpp: declarations only, in the level's namespace.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/tensor/tensor_kernels.hpp>

namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
{

struct NnKernel
{
    static void pool2d_planes(const NnPool2d& c, crd::u32 lo, crd::u32 hi) noexcept;
    static void conv2d_images(const NnConv2d& c, crd::f32* scratch, crd::u32 lo, crd::u32 hi) noexcept;
    static void layernorm_rows(const NnLayerNormF32& c, crd::u32 lo, crd::u32 hi) noexcept;
    static void act_rows(const NnActF32& c, crd::u32 lo, crd::u32 hi) noexcept;
};

} // namespace crd::hesap::tensor::inline CRD_MATH_LEVEL_NS
