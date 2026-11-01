#pragma once

// f2c_view.hpp — the shifted array addressing of the Powell ports (COBYLA, NEWUOA, BOBYQA).
//
// Their NLopt references are f2c output, which shifts each array argument before use (`--x;`, `a -= a_offset;`) so
// that x[1] and a[i + j * a_dim1] address the first element. Forming a pointer before its array is undefined in C++,
// and GCC's -Warray-bounds reports it once link-time optimization shows it the array. The ports keep the references'
// subscripts verbatim and apply the shift inside each subscript instead: `const F2cView x{x_base, 1};` makes x[i]
// read x_base[i - 1], so no out-of-range pointer is ever formed and the differential tests stay line-for-line.

namespace crd::hesap::opt::detail
{

template <typename E> struct F2cView
{
    E* base;
    int bias;

    [[nodiscard]] constexpr E& operator[](int i) const noexcept { return base[i - bias]; }
};

template <typename E> F2cView(E*, int) -> F2cView<E>;

} // namespace crd::hesap::opt::detail
