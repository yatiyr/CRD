#pragma once

// ---------------------------------------------------------------------------
// crd-units — compile-time dimension tag + arithmetic (Phase 3.1.7.5 v0a-1).
//
// Dim<L, M, T, I, Th, N, J, A> carries 8 signed integer exponents on the
// base dimensions:
//
//   L  = Length
//   M  = Mass
//   T  = Time
//   I  = Current
//   Th = Thermodynamic temperature
//   N  = Amount of substance
//   J  = Luminous intensity
//   A  = Angle (tagged as a distinct 8th base — strict SI has it as
//        dimensionless m/m, but tagging avoids silent `Length + Angle`
//        bugs at compile time per mp-units P1935 convention).
//
// DimMul / DimDiv / DimInv / DimPow operate on Dim<...> at compile time.
// All operations are constexpr / pure type-level — zero runtime cost.
//
// Per the Strategic Execution Plan 2026-05-15 + ADR-0078 (candidate; mints
// at v0a close): this is the substrate foundation for the Cerid-wide units
// system. SI base everywhere internally; precision tier (f32/f64) orthogonal
// to dimension.
//
// **What is NOT in this header** (lands later in v0a-1 / v0a-2 / v0a-3):
//   - Named base + derived dimension aliases (dim::Length, dim::Velocity,
//     dim::Force, etc.) → dim_aliases.hpp (v0a-1).
//   - `Quantity<D, T>` zero-overhead wrapper → quantity.hpp (v0a-1).
//   - LinearUnit<Dim, std::ratio> + ~120 named units → units_si.hpp (v0a-2).
//   - UnitMul/UnitDiv compound auto-derivation → units_compound.hpp (v0a-2).
//   - AffineUnit + Temperature/TemperatureDelta → units_affine.hpp (v0a-3).
//   - NonLinearUnit + dB family → units_nonlinear.hpp (v0a-3).
//   - UDLs → literals.hpp (v0a-3).
//   - Vec/Mat<Quantity> wrappers → vec_quantity.hpp (v0a-3).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>

namespace crd::units
{

// ===========================================================================
// Dim<L, M, T, I, Th, N, J, A>
// ===========================================================================
//
// 8-exponent compile-time tag. Each parameter is the integer exponent on
// the corresponding base dimension. Operations on Dim<...> work purely at
// the type level via DimMul/DimDiv/DimInv/DimPow below.
//
// **Future extension (NOT v0a):** if Cerid grows a 9th base dimension
// (e.g. data-storage byte-count, or radioactivity Becquerel for medical),
// extend the template parameter list. All existing code recompiles
// unchanged (added exponent defaults to 0 in existing instantiations
// after Dim<...> is updated). Pinned as a future amendment.
template <crd::i32 L,
          crd::i32 M,
          crd::i32 T,
          crd::i32 I,
          crd::i32 Th,
          crd::i32 N,
          crd::i32 J,
          crd::i32 A>
struct Dim
{
    static constexpr crd::i32 length             = L;
    static constexpr crd::i32 mass               = M;
    static constexpr crd::i32 time               = T;
    static constexpr crd::i32 current            = I;
    static constexpr crd::i32 temperature        = Th;
    static constexpr crd::i32 amount             = N;
    static constexpr crd::i32 luminous_intensity = J;
    static constexpr crd::i32 angle              = A;
};

// ===========================================================================
// dim_equal_v
// ===========================================================================
//
// True iff every exponent on D1 matches the corresponding exponent on D2.
// Used by STATIC_REQUIRE / static_assert to prove that derived dimensions
// like dim::Velocity match the algebraic combination DimDiv<dim::Length,
// dim::Time>.
//
// NOT used at runtime — Dim<...> is purely a type tag.
template <typename D1, typename D2>
inline constexpr bool dim_equal_v =
    (D1::length             == D2::length)             &&
    (D1::mass               == D2::mass)               &&
    (D1::time               == D2::time)               &&
    (D1::current            == D2::current)            &&
    (D1::temperature        == D2::temperature)        &&
    (D1::amount             == D2::amount)             &&
    (D1::luminous_intensity == D2::luminous_intensity) &&
    (D1::angle              == D2::angle);

// ===========================================================================
// DimMul / DimDiv / DimInv / DimPow — type-level arithmetic
// ===========================================================================

namespace detail
{

template <typename D1, typename D2>
struct DimMulImpl
{
    using type = Dim<D1::length             + D2::length,
                     D1::mass               + D2::mass,
                     D1::time               + D2::time,
                     D1::current            + D2::current,
                     D1::temperature        + D2::temperature,
                     D1::amount             + D2::amount,
                     D1::luminous_intensity + D2::luminous_intensity,
                     D1::angle              + D2::angle>;
};

template <typename D1, typename D2>
struct DimDivImpl
{
    using type = Dim<D1::length             - D2::length,
                     D1::mass               - D2::mass,
                     D1::time               - D2::time,
                     D1::current            - D2::current,
                     D1::temperature        - D2::temperature,
                     D1::amount             - D2::amount,
                     D1::luminous_intensity - D2::luminous_intensity,
                     D1::angle              - D2::angle>;
};

template <typename D>
struct DimInvImpl
{
    using type = Dim<-D::length,
                     -D::mass,
                     -D::time,
                     -D::current,
                     -D::temperature,
                     -D::amount,
                     -D::luminous_intensity,
                     -D::angle>;
};

template <typename D, crd::i32 N>
struct DimPowImpl
{
    using type = Dim<D::length             * N,
                     D::mass               * N,
                     D::time               * N,
                     D::current            * N,
                     D::temperature        * N,
                     D::amount             * N,
                     D::luminous_intensity * N,
                     D::angle              * N>;
};

} // namespace detail

template <typename D1, typename D2>
using DimMul = typename detail::DimMulImpl<D1, D2>::type;

template <typename D1, typename D2>
using DimDiv = typename detail::DimDivImpl<D1, D2>::type;

template <typename D>
using DimInv = typename detail::DimInvImpl<D>::type;

template <typename D, crd::i32 N>
using DimPow = typename detail::DimPowImpl<D, N>::type;

} // namespace crd::units
