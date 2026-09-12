#pragma once

// sparsity.hpp — Phase 3.1.6 v15-e: automatic SPARSITY DETECTION for Jacobians (Hessian: sparsity_hessian.hpp).
// An operator-overloading pass that carries an INDEX SET (which inputs each quantity depends on) instead of a value:
// seed input i with the singleton {i}; `+`,`-`,`*`,`/` return union(a,b); a nonlinear unary op copies its operand's
// set; a constant / zero-derivative op is ∅. One abstract evaluation of a scalar-generic functor yields, for each
// output, the set of input columns where its Jacobian row can be nonzero — the STRUCTURAL (global) pattern, valid
// for ALL inputs and cacheable. Hill & Dalle 2025 (arXiv:2501.17737); the frontier is Julia (SparseConnectivity
// Tracer.jl) — there is no C++ incumbent. ADR-0097.
//
// SET = a FIXED-WIDTH BITSET of W `u64` words (W = ceil(maxN/64)) held INLINE in the tracer value: union = word-wise
// OR (branch-free, O(N/64), zero alloc, WCET-bounded, DETERMINISTIC — unlike ColPack/Julia hash-set iteration). The
// value is a plain POD, so intermediates live on the stack. (A sorted-index-run policy for huge, ultra-sparse N is a
// later refinement — this is the dense-bitset core.) Branchy functors need the LOCAL tracer (value + set) — later in
// this header; the bare pattern here is branch-free (physics residuals — the target — are).

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

// Jacobian sparsity tracer over W u64 words (up to W*64 inputs). Carries ONLY the dependency set (no value).
template <int W>
struct JacPattern
{
    crd::u64 bits[W];

    constexpr JacPattern() noexcept : bits{} {}                                     // ∅ (a constant depends on nothing)
    constexpr JacPattern(crd::f64 /*constant*/) noexcept : bits{} {}                // NOLINT — lift a constant to ∅
    [[nodiscard]] static constexpr JacPattern seed(int i) noexcept                  // input i → {i}
    {
        JacPattern p;
        p.bits[i >> 6] = crd::u64{1} << (i & 63);
        return p;
    }
    [[nodiscard]] constexpr bool has(int i) const noexcept { return (bits[i >> 6] >> (i & 63)) & crd::u64{1}; }
};

// union (the workhorse — word-wise OR).
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator|(const JacPattern<W>& a, const JacPattern<W>& b) noexcept
{
    JacPattern<W> r;
    for (int w = 0; w < W; ++w)
    {
        r.bits[w] = a.bits[w] | b.bits[w];
    }
    return r;
}

// Arithmetic: every operation whose derivative w.r.t. an operand can be structurally nonzero UNIONS the operands'
// sets (global/structural — `x*0` keeps grad(x) because the coefficient is not known to be zero at trace time).
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator+(const JacPattern<W>& a, const JacPattern<W>& b) noexcept { return a | b; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator-(const JacPattern<W>& a, const JacPattern<W>& b) noexcept { return a | b; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator*(const JacPattern<W>& a, const JacPattern<W>& b) noexcept { return a | b; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator/(const JacPattern<W>& a, const JacPattern<W>& b) noexcept { return a | b; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator-(const JacPattern<W>& a) noexcept { return a; } // −x: same deps

// Mixed scalar/pattern: a scalar has ∅ deps, so the result is the pattern's deps unchanged.
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator+(crd::f64, const JacPattern<W>& a) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator+(const JacPattern<W>& a, crd::f64) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator-(crd::f64, const JacPattern<W>& a) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator-(const JacPattern<W>& a, crd::f64) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator*(crd::f64, const JacPattern<W>& a) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator*(const JacPattern<W>& a, crd::f64) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator/(const JacPattern<W>& a, crd::f64) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> operator/(crd::f64, const JacPattern<W>& a) noexcept { return a; } // 1/x: dep on x

// Nonlinear unary functions: derivative is (generically) nonzero, so the dependency set is COPIED through. One macro
// covers the whole cmath surface — the sparsity pattern of f(x) is exactly the pattern of x.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — pastes FN as the ADL-found name; not expressible as a function
#define CRD_SP_UNARY(FN)                                                                                              \
    template <int W>                                                                                                 \
    [[nodiscard]] constexpr JacPattern<W> FN(const JacPattern<W>& a) noexcept                                        \
    {                                                                                                               \
        return a;                                                                                                   \
    }
CRD_SP_UNARY(sin)
CRD_SP_UNARY(cos)
CRD_SP_UNARY(tan)
CRD_SP_UNARY(exp)
CRD_SP_UNARY(exp2)
CRD_SP_UNARY(expm1)
CRD_SP_UNARY(log)
CRD_SP_UNARY(log2)
CRD_SP_UNARY(log1p)
CRD_SP_UNARY(sqrt)
CRD_SP_UNARY(cbrt)
CRD_SP_UNARY(tanh)
CRD_SP_UNARY(sinh)
CRD_SP_UNARY(cosh)
CRD_SP_UNARY(asin)
CRD_SP_UNARY(acos)
CRD_SP_UNARY(atan)
CRD_SP_UNARY(asinh)
CRD_SP_UNARY(acosh)
CRD_SP_UNARY(atanh)
CRD_SP_UNARY(abs)
#undef CRD_SP_UNARY

// pow(x, c) and pow(c, x): depend on the varying operand.
template <int W>
[[nodiscard]] constexpr JacPattern<W> pow(const JacPattern<W>& a, crd::f64) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> pow(crd::f64, const JacPattern<W>& a) noexcept { return a; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> pow(const JacPattern<W>& a, const JacPattern<W>& b) noexcept { return a | b; }
// hypot / atan2: depend on both.
template <int W>
[[nodiscard]] constexpr JacPattern<W> hypot(const JacPattern<W>& a, const JacPattern<W>& b) noexcept { return a | b; }
template <int W>
[[nodiscard]] constexpr JacPattern<W> atan2(const JacPattern<W>& a, const JacPattern<W>& b) noexcept { return a | b; }

// ============================ LOCAL tracer: value + set (point-dependent, sparser) ===========================
// Carries a live VALUE alongside the dependency set, so it (1) resolves branches / min / max / abs on the value,
// and (2) drops a dependency when the local partial is numerically zero (`x*0`, `0*x`, `sin(0*x)` — value-gated
// multiply). The result is the pattern AT THIS x — SPARSER than the global one, but point-dependent: NEVER cache it
// across inputs (a previously-dead entry going live at another x would be a silently wrong Jacobian).
template <int W>
struct JacLocal
{
    crd::f64 v;       // live value
    crd::u64 bits[W]; // dependency set at this point

    constexpr JacLocal() noexcept : v(0), bits{} {}
    constexpr JacLocal(crd::f64 value) noexcept : v(value), bits{} {} // NOLINT — a constant: value, ∅ deps
    [[nodiscard]] static constexpr JacLocal seed(crd::f64 value, int i) noexcept
    {
        JacLocal r;
        r.v            = value;
        r.bits[i >> 6] = crd::u64{1} << (i & 63);
        return r;
    }
    [[nodiscard]] constexpr bool has(int i) const noexcept { return (bits[i >> 6] >> (i & 63)) & crd::u64{1}; }
    [[nodiscard]] constexpr bool operator<(const JacLocal& b) const noexcept { return v < b.v; }
    [[nodiscard]] constexpr bool operator>(const JacLocal& b) const noexcept { return v > b.v; }
    [[nodiscard]] constexpr bool operator<=(const JacLocal& b) const noexcept { return v <= b.v; }
    [[nodiscard]] constexpr bool operator>=(const JacLocal& b) const noexcept { return v >= b.v; }
    [[nodiscard]] constexpr bool operator==(const JacLocal& b) const noexcept { return v == b.v; }
};

namespace detail
{
template <int W>
constexpr void jl_or(JacLocal<W>& r, const JacLocal<W>& a, const JacLocal<W>& b) noexcept
{
    for (int w = 0; w < W; ++w) { r.bits[w] = a.bits[w] | b.bits[w]; }
}
template <int W>
constexpr void jl_gated(JacLocal<W>& r, const JacLocal<W>& a, crd::f64 da, const JacLocal<W>& b, crd::f64 db) noexcept
{
    for (int w = 0; w < W; ++w)
    {
        r.bits[w] = (da != 0.0 ? a.bits[w] : crd::u64{0}) | (db != 0.0 ? b.bits[w] : crd::u64{0});
    }
}
template <int W>
constexpr void jl_copy(JacLocal<W>& r, const JacLocal<W>& a) noexcept
{
    for (int w = 0; w < W; ++w) { r.bits[w] = a.bits[w]; }
}
} // namespace detail

template <int W>
[[nodiscard]] constexpr JacLocal<W> operator+(const JacLocal<W>& a, const JacLocal<W>& b) noexcept
{
    JacLocal<W> r;
    r.v = a.v + b.v;
    detail::jl_or(r, a, b);
    return r;
}
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator-(const JacLocal<W>& a, const JacLocal<W>& b) noexcept
{
    JacLocal<W> r;
    r.v = a.v - b.v;
    detail::jl_or(r, a, b);
    return r;
}
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator-(const JacLocal<W>& a) noexcept
{
    JacLocal<W> r;
    r.v = -a.v;
    detail::jl_copy(r, a);
    return r;
}
// value-gated product: ∂(x·y)/∂x = y, ∂/∂y = x → drop x's deps if y==0 (and vice versa). Kills x*0 / 0*x locally.
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator*(const JacLocal<W>& a, const JacLocal<W>& b) noexcept
{
    JacLocal<W> r;
    r.v = a.v * b.v;
    detail::jl_gated(r, a, b.v, b, a.v);
    return r;
}
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator/(const JacLocal<W>& a, const JacLocal<W>& b) noexcept
{
    JacLocal<W> r;
    r.v = a.v / b.v;
    detail::jl_gated(r, a, 1.0, b, a.v); // ∂/∂a = 1/b (≠0); ∂/∂b = −a/b² (0 iff a==0)
    return r;
}
// mixed scalar
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator+(crd::f64 s, const JacLocal<W>& a) noexcept { JacLocal<W> r = a; r.v = s + a.v; return r; }
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator+(const JacLocal<W>& a, crd::f64 s) noexcept { JacLocal<W> r = a; r.v = a.v + s; return r; }
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator-(const JacLocal<W>& a, crd::f64 s) noexcept { JacLocal<W> r = a; r.v = a.v - s; return r; }
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator*(crd::f64 s, const JacLocal<W>& a) noexcept // gated: s==0 kills deps
{
    JacLocal<W> r;
    r.v = s * a.v;
    for (int w = 0; w < W; ++w) { r.bits[w] = (s != 0.0) ? a.bits[w] : crd::u64{0}; }
    return r;
}
template <int W>
[[nodiscard]] constexpr JacLocal<W> operator*(const JacLocal<W>& a, crd::f64 s) noexcept { return s * a; }

// min/max/abs resolve on the VALUE (the local advantage) — carry the active branch's deps.
template <int W>
[[nodiscard]] constexpr JacLocal<W> min(const JacLocal<W>& a, const JacLocal<W>& b) noexcept { return a.v <= b.v ? a : b; }
template <int W>
[[nodiscard]] constexpr JacLocal<W> max(const JacLocal<W>& a, const JacLocal<W>& b) noexcept { return a.v >= b.v ? a : b; }
template <int W>
[[nodiscard]] constexpr JacLocal<W> abs(const JacLocal<W>& a) noexcept { return a.v >= 0.0 ? a : -a; }

// nonlinear unary: value via crd::math, deps copied (generic f'≠0).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CRD_SL_UNARY(FN)                                                                                             \
    template <int W>                                                                                                \
    [[nodiscard]] inline JacLocal<W> FN(const JacLocal<W>& a) noexcept                                              \
    {                                                                                                              \
        JacLocal<W> r;                                                                                             \
        r.v = crd::math::FN(a.v);                                                                                  \
        detail::jl_copy(r, a);                                                                                     \
        return r;                                                                                                  \
    }
CRD_SL_UNARY(sin)
CRD_SL_UNARY(cos)
CRD_SL_UNARY(exp)
CRD_SL_UNARY(log)
CRD_SL_UNARY(sqrt)
CRD_SL_UNARY(tanh)
CRD_SL_UNARY(atan)
#undef CRD_SL_UNARY

} // namespace crd::hesap::autodiff::forward
