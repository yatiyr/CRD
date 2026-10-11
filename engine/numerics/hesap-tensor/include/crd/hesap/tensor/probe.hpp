#pragma once
// ---------------------------------------------------------------------------
// crd-hesap-tensor — DIAG.10c: the shape/stride and value-domain probes at
// the tensor boundary.
//
// Thin wrappers over crd/core/layout_probe.hpp, which owns the checks so any
// other module's views inherit them. probe_tensor_layout checks a view's
// claim (rank, element count, offsets, the storage it says it lives in, and
// whether a written view addresses one element twice); a const view is a
// read, so a proven overlap (a broadcast) is reported but is no violation.
// probe_tensor_values walks a float or complex view and judges every value
// against a FloatDomain from its bits, naming the first offender by its
// coordinates (and lane 1 for a complex value's imaginary part). Integer and
// mask tensors are not float domains and have no value walk here.
// Neither probe changes a value.
// ---------------------------------------------------------------------------

#include "tensor.hpp"

#include <crd/core/layout_probe.hpp>
#include <crd/core/value_domain.hpp>

#include <type_traits>

namespace crd::hesap::tensor
{

// The layout claim of `view`, without a storage range (reported as unchecked).
template <typename T> [[nodiscard]] crd::LayoutReport probe_tensor_layout(const TensorView<T>& view) noexcept
{
    crd::LayoutExpectation expect{};
    expect.max_rank     = kMaxRank;
    expect.element_size = sizeof(T);
    expect.writable     = !std::is_const_v<T>;
    return crd::probe_layout(view.shape().data(), view.strides().data(), view.rank(), expect);
}

// The layout claim of `view` inside storage [storage_first, storage_end), in elements relative to view.data().
template <typename T>
[[nodiscard]] crd::LayoutReport probe_tensor_layout(const TensorView<T>& view, crd::i64 storage_first,
                                                    crd::i64 storage_end) noexcept
{
    crd::LayoutExpectation expect{};
    expect.max_rank      = kMaxRank;
    expect.element_size  = sizeof(T);
    expect.writable      = !std::is_const_v<T>;
    expect.has_storage   = true;
    expect.storage_first = storage_first;
    expect.storage_end   = storage_end;
    return crd::probe_layout(view.shape().data(), view.strides().data(), view.rank(), expect);
}

// Judge every value of a float (f32/f64) or complex (c32/c64) view against `domain`.
template <typename T>
[[nodiscard]] crd::StridedValueReport probe_tensor_values(const TensorView<T>& view,
                                                          const crd::FloatDomain& domain) noexcept
{
    using U = std::remove_const_t<T>;
    static_assert(std::is_same_v<U, crd::f32> || std::is_same_v<U, crd::f64> || std::is_same_v<U, c32> ||
                      std::is_same_v<U, c64>,
                  "probe_tensor_values: a float or complex view (integer and mask tensors are not float domains)");
    if constexpr (std::is_same_v<U, crd::f32> || std::is_same_v<U, crd::f64>)
    {
        return crd::probe_strided_values(domain, view.data(), view.shape().data(), view.strides().data(), view.rank(),
                                         1U);
    }
    else
    {
        // std::complex<S> is layout-compatible with S[2] (the standard says so): the real lane, then the imaginary.
        using S = typename U::value_type;
        return crd::probe_strided_values(domain, reinterpret_cast<const S*>(view.data()), view.shape().data(), // NOLINT
                                         view.strides().data(), view.rank(), 2U);
    }
}

} // namespace crd::hesap::tensor
