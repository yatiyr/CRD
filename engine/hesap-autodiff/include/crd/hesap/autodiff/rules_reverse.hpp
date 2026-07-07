#pragma once

// rules_reverse.hpp — Phase 3.1.6 v16-b: the full scalar VJP RULE LIBRARY. Extends the core reverse `Var` (tape.hpp,
// v16-a: +−×÷ / exp log sin cos sqrt tanh pow) with the COMPLETE crd::math surface + the binary rules + control flow.
// The crush is CORRECTNESS: a VJP is the TRANSPOSE of the JVP, so every local partial is the SAME audited
// `forward::detail` slope the v15 forward mode uses — there is no second rule library to drift or mis-derive. Gated
// 3 ways (reverse ≡ forward-JVP transpose ≡ FD, + complex-step where the complex core exists). Deterministic
// (crd::math cores). ADR-0097.

#include <crd/hesap/autodiff/tape.hpp>

namespace crd::hesap::autodiff::reverse
{

// Unary rule: Var FN(Var) = { value crd::math::FN(v), local partial = the forward slope }. (macro pastes FN as both
// the defined name AND crd::math::FN — not expressible as a plain template.)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CRD_AD_VAR_UNARY(FN, SLOPE)                                                                                    \
    [[nodiscard]] inline Var FN(Var a) noexcept                                                                       \
    {                                                                                                                 \
        return Var{a.tape, a.tape->unary(crd::math::FN(a.val()), a.node, (SLOPE))};                                   \
    }
CRD_AD_VAR_UNARY(asin, fwd_detail::d_asin(a.val()))
CRD_AD_VAR_UNARY(acos, fwd_detail::d_acos(a.val()))
CRD_AD_VAR_UNARY(atan, fwd_detail::d_atan(a.val()))
CRD_AD_VAR_UNARY(sinh, fwd_detail::d_sinh(a.val()))
CRD_AD_VAR_UNARY(cosh, fwd_detail::d_cosh(a.val()))
CRD_AD_VAR_UNARY(asinh, fwd_detail::d_asinh(a.val()))
CRD_AD_VAR_UNARY(acosh, fwd_detail::d_acosh(a.val()))
CRD_AD_VAR_UNARY(atanh, fwd_detail::d_atanh(a.val()))
CRD_AD_VAR_UNARY(exp2, fwd_detail::d_exp2(a.val()))
CRD_AD_VAR_UNARY(exp10, fwd_detail::d_exp10(a.val()))
CRD_AD_VAR_UNARY(expm1, fwd_detail::d_expm1(a.val()))
CRD_AD_VAR_UNARY(log2, fwd_detail::d_log2(a.val()))
CRD_AD_VAR_UNARY(log10, fwd_detail::d_log10(a.val()))
CRD_AD_VAR_UNARY(log1p, fwd_detail::d_log1p(a.val()))
#undef CRD_AD_VAR_UNARY

// tan (sec²), cbrt / rsqrt (reuse the computed value in the slope).
[[nodiscard]] inline Var tan(Var a) noexcept
{
    const crd::f64 c = crd::math::cos(a.val());
    return Var{a.tape, a.tape->unary(crd::math::tan(a.val()), a.node, 1.0 / (c * c))};
}
[[nodiscard]] inline Var cbrt(Var a) noexcept
{
    const crd::f64 c = crd::math::cbrt(a.val());
    return Var{a.tape, a.tape->unary(c, a.node, fwd_detail::d_cbrt(c))};
}
[[nodiscard]] inline Var rsqrt(Var a) noexcept
{
    const crd::f64 r = crd::math::rsqrt(a.val());
    return Var{a.tape, a.tape->unary(r, a.node, fwd_detail::d_rsqrt(a.val(), r))};
}

// ---- binary rules (two operands; the partials combine two tangents in forward, two adjoints in reverse) ----
[[nodiscard]] inline Var atan2(Var y, Var x) noexcept
{
    return Var{y.tape, y.tape->binary(crd::math::atan2(y.val(), x.val()), y.node, fwd_detail::atan2_dy(y.val(), x.val()),
                                      x.node, fwd_detail::atan2_dx(y.val(), x.val()))};
}
[[nodiscard]] inline Var hypot(Var x, Var y) noexcept
{
    const crd::f64 h = crd::math::hypot(x.val(), y.val());
    return Var{x.tape, x.tape->binary(h, x.node, x.val() / h, y.node, y.val() / h)};
}
[[nodiscard]] inline Var pow(Var x, Var y) noexcept // dual exponent — Ceres-faithful edge table via pow_dual
{
    const auto r = fwd_detail::pow_dual(x.val(), y.val());
    return Var{x.tape, x.tape->binary(r.value, x.node, r.dbase, y.node, r.dexp)};
}

// ---- control flow — the taped taken branch carries the derivative (the standard reverse-AD convention) ----
[[nodiscard]] inline Var abs(Var a) noexcept // subgradient at 0 takes the +1 branch
{
    return a.val() >= 0.0 ? Var{a.tape, a.tape->unary(a.val(), a.node, 1.0)}
                          : Var{a.tape, a.tape->unary(-a.val(), a.node, -1.0)};
}
[[nodiscard]] inline Var min(Var a, Var b) noexcept { return a.val() <= b.val() ? a : b; } // tie → a (deterministic)
[[nodiscard]] inline Var max(Var a, Var b) noexcept { return a.val() >= b.val() ? a : b; }
[[nodiscard]] inline Var select(bool cond, Var a, Var b) noexcept { return cond ? a : b; }

// comparisons act on the VALUE (branch by the point, not the tangent — the standard forward/reverse-AD convention).
[[nodiscard]] inline bool operator<(Var a, Var b) noexcept { return a.val() < b.val(); }
[[nodiscard]] inline bool operator<=(Var a, Var b) noexcept { return a.val() <= b.val(); }
[[nodiscard]] inline bool operator>(Var a, Var b) noexcept { return a.val() > b.val(); }
[[nodiscard]] inline bool operator>=(Var a, Var b) noexcept { return a.val() >= b.val(); }
[[nodiscard]] inline bool operator==(Var a, Var b) noexcept { return a.val() == b.val(); }
[[nodiscard]] inline bool operator!=(Var a, Var b) noexcept { return a.val() != b.val(); }

} // namespace crd::hesap::autodiff::reverse
