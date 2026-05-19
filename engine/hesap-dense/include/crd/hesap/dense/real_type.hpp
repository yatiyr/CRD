#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>

#include <type_traits>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// real_type<T> — the real scalar associated with a (possibly complex) T.
//
// Used by BLAS L1 norms and absolute sums where the input is `T` but the
// output is the underlying real type (nrm2 / asum return real-valued
// magnitudes even for complex inputs).
//
//   real_type<f32>::type        == f32
//   real_type<f64>::type        == f64
//   real_type<Complex32>::type  == f32
//   real_type<Complex64>::type  == f64
//
// `RealType<T>` is the convenience alias.
// -----------------------------------------------------------------------

// NOLINTBEGIN(readability-identifier-naming) — std-style trait names (mirrors
// `std::is_floating_point` / `std::is_floating_point_v` conventions).
template <typename T>
struct real_type
{
    using type = T;
};

template <typename T>
struct real_type<Complex<T>>
{
    using type = T;
};

template <typename T>
using RealType = typename real_type<T>::type;

// Companion trait: `is_complex<T>` true iff T is a Complex<U>.
template <typename T>
struct is_complex : std::false_type
{
};

template <typename T>
struct is_complex<Complex<T>> : std::true_type
{
};

template <typename T>
inline constexpr bool is_complex_v = is_complex<T>::value;
// NOLINTEND(readability-identifier-naming)

} // namespace crd::hesap::dense
