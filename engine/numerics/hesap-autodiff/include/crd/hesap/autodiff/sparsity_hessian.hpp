#pragma once

// sparsity_hessian.hpp — Phase 3.1.6 v15-e: automatic HESSIAN sparsity detection (second order). A scalar-generic
// functor f: R^n → R is traced ONCE on HessPattern; the output carries the set of index PAIRS {(i,j) | ∂²f/∂xᵢ∂xⱼ
// can be nonzero} — the structural Hessian pattern. Hill & Dalle 2025 (arXiv:2501.17737); the propagation is the
// second-order chain rule (Faà di Bruno), VERIFIED by derivation: for z = φ(x,y) with the five scalar flags
// [∂₁φ],[∂₂φ],[∂²₁φ],[∂²₂φ],[∂²₁₂φ] (does that partial's derivative structurally vanish?):
//
//   grad(z) = [∂₁φ]·grad(x) ∨ [∂₂φ]·grad(y)
//   hess(z) = [∂₁φ]·hess(x) ∨ [∂₂φ]·hess(y)                                      (inherited)
//           ∨ [∂²₁φ]·(grad(x)⊗grad(x)) ∨ [∂²₂φ]·(grad(y)⊗grad(y))                (self)
//           ∨ [∂²₁₂φ]·(grad(x)⊗grad(y) ∨ grad(y)⊗grad(x))                        (cross)
//
//   x*y → cross only;  sin(x)/x²/exp(x) → self;  x+y → inherited only (no new pairs).
//
// Pairs are canonicalized i≤j (upper triangle, no double count). grad = a GW-word bitset; hess = an HW-word bitset
// over MaxN² (index i*MaxN+j) — held inline (moderate n; the target of Hessian sparsity). The self/cross outer
// products are O(k²) per op ⇒ the Θ(k²n) total (Hill-Dalle). ⚠ dense-row functions (sum/norm/softmax) blow the
// pattern to O(n²) pairs — expected (the Hessian genuinely IS dense then).

#include <crd/core/types.hpp>

#include <bit> // std::countr_zero

namespace crd::hesap::autodiff::forward
{

template <int MaxN>
struct HessPattern
{
    static constexpr int gw = (MaxN + 63) / 64;
    static constexpr int hw = (MaxN * MaxN + 63) / 64;

    crd::u64 grad[gw];
    crd::u64 hess[hw];

    constexpr HessPattern() noexcept : grad{}, hess{} {}
    constexpr HessPattern(crd::f64 /*constant*/) noexcept : grad{}, hess{} {} // NOLINT — a constant depends on nothing
    [[nodiscard]] static constexpr HessPattern seed(int i) noexcept
    {
        HessPattern p;
        p.grad[i >> 6] = crd::u64{1} << (i & 63);
        return p;
    }
    [[nodiscard]] constexpr bool has_grad(int i) const noexcept { return (grad[i >> 6] >> (i & 63)) & crd::u64{1}; }
    [[nodiscard]] constexpr bool has_pair(int i, int j) const noexcept // ∂²/∂xᵢ∂xⱼ structurally nonzero?
    {
        const int a = i <= j ? i : j;
        const int b = i <= j ? j : i;
        const int idx = a * MaxN + b;
        return (hess[idx >> 6] >> (idx & 63)) & crd::u64{1};
    }
};

namespace detail
{
template <int MaxN>
constexpr void hp_or_grad(HessPattern<MaxN>& r, const HessPattern<MaxN>& a) noexcept
{
    for (int w = 0; w < HessPattern<MaxN>::gw; ++w) { r.grad[w] |= a.grad[w]; }
}
template <int MaxN>
constexpr void hp_or_hess(HessPattern<MaxN>& r, const HessPattern<MaxN>& a) noexcept
{
    for (int w = 0; w < HessPattern<MaxN>::hw; ++w) { r.hess[w] |= a.hess[w]; }
}
template <int MaxN>
constexpr void hp_set_pair(HessPattern<MaxN>& r, int i, int j) noexcept
{
    const int a   = i <= j ? i : j;
    const int b   = i <= j ? j : i;
    const int idx = a * MaxN + b;
    r.hess[idx >> 6] |= crd::u64{1} << (idx & 63);
}
// self: all pairs (i,j), i≤j, i,j ∈ grad(a).
template <int MaxN>
inline void hp_add_self(HessPattern<MaxN>& r, const HessPattern<MaxN>& a) noexcept
{
    for (int wi = 0; wi < HessPattern<MaxN>::gw; ++wi)
    {
        crd::u64 bi = a.grad[wi];
        while (bi)
        {
            const int i = (wi << 6) + std::countr_zero(bi);
            bi &= bi - 1;
            for (int wj = wi; wj < HessPattern<MaxN>::gw; ++wj)
            {
                crd::u64 bj = a.grad[wj] & (wj == wi ? ~((crd::u64{1} << (i & 63)) - 1) : ~crd::u64{0});
                while (bj)
                {
                    const int j = (wj << 6) + std::countr_zero(bj);
                    bj &= bj - 1;
                    hp_set_pair(r, i, j);
                }
            }
        }
    }
}
// cross: all pairs (i,j) with i∈grad(a), j∈grad(b) (canonicalized) — symmetric, so it also covers b×a.
template <int MaxN>
inline void hp_add_cross(HessPattern<MaxN>& r, const HessPattern<MaxN>& a, const HessPattern<MaxN>& b) noexcept
{
    for (int wi = 0; wi < HessPattern<MaxN>::gw; ++wi)
    {
        crd::u64 bi = a.grad[wi];
        while (bi)
        {
            const int i = (wi << 6) + std::countr_zero(bi);
            bi &= bi - 1;
            for (int wj = 0; wj < HessPattern<MaxN>::gw; ++wj)
            {
                crd::u64 bj = b.grad[wj];
                while (bj)
                {
                    const int j = (wj << 6) + std::countr_zero(bj);
                    bj &= bj - 1;
                    hp_set_pair(r, i, j);
                }
            }
        }
    }
}
} // namespace detail

// z = x + y / x - y : grad ∪, hess inherited only (all 2nd-order flags zero).
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator+(const HessPattern<MaxN>& x, const HessPattern<MaxN>& y) noexcept
{
    HessPattern<MaxN> r = x;
    detail::hp_or_grad(r, y);
    detail::hp_or_hess(r, y);
    return r;
}
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator-(const HessPattern<MaxN>& x, const HessPattern<MaxN>& y) noexcept
{
    return x + y;
}
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator-(const HessPattern<MaxN>& x) noexcept { return x; }

// z = x * y : grad ∪, hess = inherited ∪ CROSS (∂²₁₂ = 1).
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator*(const HessPattern<MaxN>& x, const HessPattern<MaxN>& y) noexcept
{
    HessPattern<MaxN> r = x;
    detail::hp_or_grad(r, y);
    detail::hp_or_hess(r, y);
    detail::hp_add_cross(r, x, y);
    return r;
}
// z = x / y : both partials nonlinear ⇒ self(x)?, self(y), cross — conservatively self(x)∪self(y)∪cross.
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator/(const HessPattern<MaxN>& x, const HessPattern<MaxN>& y) noexcept
{
    HessPattern<MaxN> r = x;
    detail::hp_or_grad(r, y);
    detail::hp_or_hess(r, y);
    detail::hp_add_self(r, y);      // ∂²/∂y² of x/y ≠ 0
    detail::hp_add_cross(r, x, y);  // ∂²/∂x∂y ≠ 0
    return r;
}

// Nonlinear unary f(x): grad = grad(x); hess = hess(x) ∪ SELF (∂²₁ = f'' ≠ 0).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CRD_HP_UNARY(FN)                                                                                             \
    template <int MaxN>                                                                                             \
    [[nodiscard]] inline HessPattern<MaxN> FN(const HessPattern<MaxN>& x) noexcept                                 \
    {                                                                                                             \
        HessPattern<MaxN> r = x;                                                                                  \
        detail::hp_add_self(r, x);                                                                                \
        return r;                                                                                                 \
    }
CRD_HP_UNARY(sin)
CRD_HP_UNARY(cos)
CRD_HP_UNARY(tan)
CRD_HP_UNARY(exp)
CRD_HP_UNARY(log)
CRD_HP_UNARY(sqrt)
CRD_HP_UNARY(tanh)
CRD_HP_UNARY(sinh)
CRD_HP_UNARY(cosh)
CRD_HP_UNARY(asin)
CRD_HP_UNARY(atan)
CRD_HP_UNARY(exp2)
CRD_HP_UNARY(expm1)
CRD_HP_UNARY(log1p)
#undef CRD_HP_UNARY

// abs: piecewise-linear ⇒ f''=0 ⇒ grad copied, NO new pairs (inherited hess only).
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> abs(const HessPattern<MaxN>& x) noexcept { return x; }

// Mixed scalar/pattern: a scalar is inert (linear in the pattern) — grad + hess unchanged, no new pairs.
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator+(crd::f64, const HessPattern<MaxN>& x) noexcept { return x; }
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator+(const HessPattern<MaxN>& x, crd::f64) noexcept { return x; }
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator-(crd::f64, const HessPattern<MaxN>& x) noexcept { return x; }
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator-(const HessPattern<MaxN>& x, crd::f64) noexcept { return x; }
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator*(crd::f64, const HessPattern<MaxN>& x) noexcept { return x; }
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator*(const HessPattern<MaxN>& x, crd::f64) noexcept { return x; }
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator/(const HessPattern<MaxN>& x, crd::f64) noexcept { return x; }
// c / x : nonlinear in x ⇒ self.
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> operator/(crd::f64, const HessPattern<MaxN>& x) noexcept
{
    HessPattern<MaxN> r = x;
    detail::hp_add_self(r, x);
    return r;
}
// pow(x, c): nonlinear ⇒ self.
template <int MaxN>
[[nodiscard]] inline HessPattern<MaxN> pow(const HessPattern<MaxN>& x, crd::f64) noexcept
{
    HessPattern<MaxN> r = x;
    detail::hp_add_self(r, x);
    return r;
}

// Trace the Hessian sparsity of a scalar functor f: R^n → R. `scratch` (>= n) is the seed workspace.
template <int MaxN, class F>
inline void trace_hessian(const F& f, int n, HessPattern<MaxN>* scratch, HessPattern<MaxN>& out) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = HessPattern<MaxN>::seed(i);
    }
    out = f(scratch, n);
}

// Build the UPPER-TRIANGLE CSR (row_ptr[n+1], col_idx[nnz], each row's cols j ≥ i) from a Hessian pattern; returns nnz.
template <int MaxN>
[[nodiscard]] inline int build_hess_csr(const HessPattern<MaxN>& h, int n, int* row_ptr, int* col_idx) noexcept
{
    int nnz    = 0;
    row_ptr[0] = 0;
    for (int i = 0; i < n; ++i)
    {
        for (int j = i; j < n; ++j) // upper triangle incl diagonal
        {
            if (h.has_pair(i, j))
            {
                col_idx[nnz++] = j;
            }
        }
        row_ptr[i + 1] = nnz;
    }
    return nnz;
}

} // namespace crd::hesap::autodiff::forward
