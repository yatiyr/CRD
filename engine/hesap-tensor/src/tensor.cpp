#include <crd/hesap/tensor/tensor.hpp>

namespace crd::hesap::tensor
{

const char* to_string(TensorStatus status) noexcept
{
    switch (status)
    {
        case TensorStatus::Ok:
            return "Ok";
        case TensorStatus::BadInput:
            return "BadInput";
        case TensorStatus::RankOverflow:
            return "RankOverflow";
        case TensorStatus::ShapeMismatch:
            return "ShapeMismatch";
        case TensorStatus::NotContiguous:
            return "NotContiguous";
        case TensorStatus::AllocFailed:
            return "AllocFailed";
        case TensorStatus::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

// Anchor explicit instantiations for the compute dtypes (compile-coverage for
// every member on the exact set the module ships; consumers still instantiate
// implicitly as usual).
template class TensorView<crd::f32>;
template class TensorView<crd::f64>;
template class Tensor<crd::f32>;
template class Tensor<crd::f64>;

} // namespace crd::hesap::tensor
