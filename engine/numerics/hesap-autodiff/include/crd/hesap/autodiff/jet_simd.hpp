#pragma once

// jet_simd.hpp — Phase 3.1.6 v15-a (crush carrier): the SIMD vector-forward carrier and its gradient and Jacobian
// drivers. Partials live in SIMD registers, NOT a scalar C-array — arithmetic is single-rounded FMA on whole registers
// with no intermediate materialization and no Eigen. ADR-0097; design + boards:
// docs/research/2026-07-06-v15-forward-ad-crush.md §A, docs/bench/2026-07-06-v15a-forward-carrier.md.
//
//   JetPackD<N> — f64 value + N partials in named Vec4d registers (Vec8d for every eight at avx512).
//
// LEVEL (SIMD-DISPATCH, ADR-0134 §5): the carrier lives in the level namespace, so its register layout (alignment,
// kRegs, kLanes) is the level of the unit that compiles it: general code at the baseline, a kernel family's unit at its
// own level (crd/hesap/autodiff/autodiff_kernels.hpp runs the engine's canned functions that way). A caller's jets and
// the code that evaluates them are therefore one unit; no cooked data holds a jet. Every level gives the same bits.
//
// TWO codegen levers make this crush (measured; the N=4 single-register case beats ALL 7 frontier peers 1.85x, and
// the >1-register case went 34.6->10.5 ns at N=8 — 3.3x — closing on Ceres/Eigen):
//   (1) The partial registers are a RECURSIVE NAMED-MEMBER pack (`RegPackD`), NOT a register ARRAY. GCC/clang
//       SROA-promote named members into registers across an operation chain; they SPILL an array (the array cost
//       2.5x). The pack is compile-time unrolled (each level is one named register).
//   (2) The product rule places the CARRIED accumulator's partial as the FMA MULTIPLICAND (single-rounded fma), and
//       the non-carried `a.value * b.partial` term as the addend — the recurrence is then one 4-cycle FMA hop, not
//       mul->fma-addend (8c). This is tuned for the `acc = acc * x` idiom (accumulator on the LEFT), the dominant
//       AD accumulation pattern; a right-carried chain is a cycle longer. Ceres/Eigen has the identical asymmetry
//       (the compiler resolves it silently); we FIX the order explicitly — which ALSO pins it for the determinism
//       moat. The order is load-bearing for BOTH latency and bit-reproducibility — NEVER reorder.
//
// DETERMINISM: single-rounded `crd::math::simd::fma` throughout (hardware at avx2 and avx512, exact emulation at sse2,
// libm fma on portable); the chain rule is per-LANE independent (no cross-lane reduction in forward mode), so the
// packed result is bit-identical regardless of how directions tile across registers or which level runs — the
// {1..16}/cross-platform moat is automatic. We do NOT reassociate the product chain (that would change rounding) — so
// `crd::math::simd::mul_add` / compiler `-ffp-contract` fusion is deliberately NOT used. Results are <=1 ulp from the
// scalar Jet<T,N> path (fma vs mul+add), gated against it in the tests.
//
// The >1-register regime is competitive but does not yet CRUSH Eigen-Ceres; that remains an OPEN crush target
// (SANITY #9 — a documented gap, never an accepted loss).

#include <crd/core/kernel_level.hpp>
#include <crd/core/platform.hpp> // CRD_FORCEINLINE
#include <crd/core/types.hpp>
#include <crd/math/hyperbolic.hpp>
#include <crd/math/power.hpp>
#include <crd/math/select.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8d.hpp>
#include <crd/math/transcendental.hpp>

// no_unique_address: MSVC + clang-cl (MSVC ABI) accept ONLY [[msvc::no_unique_address]] and hard-error on the
// standard spelling under -Werror; gcc/clang-native use the standard one. Select per toolchain.
#if defined(_MSC_VER)
#define CRD_NO_UNIQUE_ADDR [[msvc::no_unique_address]]
#else
#define CRD_NO_UNIQUE_ADDR [[no_unique_address]]
#endif

namespace crd::hesap::autodiff::forward::inline CRD_MATH_LEVEL_NS
{

// ============================ f64 carrier: JetPackD<N> (named-register pack) ============================
namespace pack_detail
{
using crd::math::simd::Vec4d;
using crd::math::simd::Vec8d;

// The register that holds the next L partials: a Vec8d while more than four remain at avx512, else a Vec4d.
template <int L> struct PackHead
{
    using Type = Vec4d;
};
#if CRD_SIMD_HAS_AVX512
template <int L>
    requires(L > 4)
struct PackHead<L>
{
    using Type = Vec8d;
};
#endif
template <int L> using Head = typename PackHead<L>::Type;
template <int L> inline constexpr int kHeadLanes = static_cast<int>(sizeof(Head<L>) / sizeof(crd::f64));
template <int L> inline constexpr int kTailLanes = L > kHeadLanes<L> ? L - kHeadLanes<L> : 0;

// Recursive named-register pack for L partials: `head` (one register) + `tail` (the rest). Named members =>
// SROA-promotable to registers across a chain (an array is not). Compile-time unrolled.
template <int L> struct RegPackD
{
    Head<L> head;
    // Empty at the last register => zero-size (RegPackD<4> is exactly one Vec4d, no padding bloat) on every toolchain.
    CRD_NO_UNIQUE_ADDR RegPackD<kTailLanes<L>> tail;
};
template <> struct RegPackD<0>
{
};

template <int L> inline constexpr int kPackRegs = 1 + kPackRegs<kTailLanes<L>>;
template <> inline constexpr int kPackRegs<0> = 0;
template <int L> inline constexpr int kPackLanes = kHeadLanes<L> + kPackLanes<kTailLanes<L>>;
template <> inline constexpr int kPackLanes<0> = 0;

// The register with a 1 in lane k and zeros elsewhere (all zeros when k is outside it).
template <class V> [[nodiscard]] CRD_FORCEINLINE V unit(int k) noexcept
{
    if constexpr (sizeof(V) > sizeof(Vec4d))
    {
        return Vec8d(unit<Vec4d>(k), unit<Vec4d>(k - 4));
    }
    else
    {
        return Vec4d(k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0, k == 3 ? 1.0 : 0.0);
    }
}

template <int L> [[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_zero() noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        return {Head<L>::zero(), rpd_zero<kTailLanes<L>>()};
    }
}

// Seed direction k (global slot): its register gets a 1 in its lane, all others zero.
template <int L> [[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_seed(int k) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        return {unit<Head<L>>(k), rpd_seed<kTailLanes<L>>(k - kHeadLanes<L>)};
    }
}

// The pack of p[0..L).
template <int L> [[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_load(const crd::f64* p) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else if constexpr (L >= kHeadLanes<L>)
    {
        return {Head<L>::load(p), rpd_load<kTailLanes<L>>(p + kHeadLanes<L>)};
    }
    else
    {
        return {Head<L>::load_partial(p, static_cast<crd::usize>(L)), {}};
    }
}

// Store the L partials contiguously (a plain store for a full register, a masked one for the last).
template <int L> CRD_FORCEINLINE void rpd_store(const RegPackD<L>& a, crd::f64* out) noexcept
{
    if constexpr (L >= kHeadLanes<L>)
    {
        a.head.store(out);
        rpd_store(a.tail, out + kHeadLanes<L>);
    }
    else if constexpr (L > 0)
    {
        a.head.store_partial(out, static_cast<crd::usize>(L));
    }
}

template <int L> [[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_neg(const RegPackD<L>& a) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        return {-a.head, rpd_neg(a.tail)};
    }
}

template <int L> [[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_add(const RegPackD<L>& a, const RegPackD<L>& b) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        return {a.head + b.head, rpd_add(a.tail, b.tail)};
    }
}

template <int L> [[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_sub(const RegPackD<L>& a, const RegPackD<L>& b) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        return {a.head - b.head, rpd_sub(a.tail, b.tail)};
    }
}

// Product rule per register, FMA-ordered: result = av.partial * b.value  +  a.value * bv.partial, with the CARRIED
// (av) partial as the fma multiplicand.
template <int L>
[[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_mul(const RegPackD<L>& av, crd::f64 a, crd::f64 b,
                                                  const RegPackD<L>& bv) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        using V = Head<L>;
        return {crd::math::simd::fma(av.head, V(b), V(a) * bv.head), rpd_mul(av.tail, a, b, bv.tail)};
    }
}

// Quotient rule per register: (a.partial - (a/b) * b.partial) * (1/b.value).
template <int L>
[[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_div(const RegPackD<L>& av, crd::f64 a_by_b, crd::f64 inv,
                                                  const RegPackD<L>& bv) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        using V = Head<L>;
        return {(av.head - V(a_by_b) * bv.head) * V(inv), rpd_div(av.tail, a_by_b, inv, bv.tail)};
    }
}

// Scale every partial by a broadcast derivative (transcendentals + scalar mul).
template <int L> [[nodiscard]] CRD_FORCEINLINE RegPackD<L> rpd_scale(crd::f64 d, const RegPackD<L>& a) noexcept
{
    if constexpr (L == 0)
    {
        return {};
    }
    else
    {
        return {Head<L>(d) * a.head, rpd_scale(d, a.tail)};
    }
}
} // namespace pack_detail

template <int N> struct JetPackD
{
    static constexpr int kRegs = pack_detail::kPackRegs<N>;   // registers holding the partials at this level
    static constexpr int kLanes = pack_detail::kPackLanes<N>; // their lanes (N rounded up to the registers)
    static constexpr int kDimension = N;
    using Scalar = crd::f64;

    crd::f64 a;                 // value
    pack_detail::RegPackD<N> v; // partials (recursive named registers)

    JetPackD() noexcept = default;
    JetPackD(crd::f64 value) noexcept
        : a(value), v(pack_detail::rpd_zero<N>()) {} // NOLINT(google-explicit-constructor)
    JetPackD(crd::f64 value, int k) noexcept : a(value), v(pack_detail::rpd_seed<N>(k)) {}
    JetPackD(crd::f64 value, const pack_detail::RegPackD<N>& part) noexcept : a(value), v(part) {}
    // Value plus the N partials partials[0..N) (an arbitrary tangent seed).
    JetPackD(crd::f64 value, const crd::f64* partials) noexcept : a(value), v(pack_detail::rpd_load<N>(partials)) {}

    [[nodiscard]] CRD_FORCEINLINE JetPackD operator-() const noexcept { return {-a, pack_detail::rpd_neg(v)}; }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator+(const JetPackD& b) const noexcept
    {
        return {a + b.a, pack_detail::rpd_add(v, b.v)};
    }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator-(const JetPackD& b) const noexcept
    {
        return {a - b.a, pack_detail::rpd_sub(v, b.v)};
    }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator*(const JetPackD& b) const noexcept
    {
        // `this` (left) is the carried accumulator in `acc = acc * x` — its partials are the fma multiplicand.
        return {a * b.a, pack_detail::rpd_mul(v, a, b.a, b.v)};
    }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator/(const JetPackD& b) const noexcept
    {
        const crd::f64 inv = 1.0 / b.a;
        const crd::f64 a_by_b = a * inv;
        return {a_by_b, pack_detail::rpd_div(v, a_by_b, inv, b.v)};
    }

    CRD_FORCEINLINE JetPackD& operator+=(const JetPackD& b) noexcept { return *this = *this + b; }
    CRD_FORCEINLINE JetPackD& operator-=(const JetPackD& b) noexcept { return *this = *this - b; }
    CRD_FORCEINLINE JetPackD& operator*=(const JetPackD& b) noexcept { return *this = *this * b; }
    CRD_FORCEINLINE JetPackD& operator/=(const JetPackD& b) noexcept { return *this = *this / b; }

    [[nodiscard]] bool operator==(const JetPackD& b) const noexcept { return a == b.a; }
    [[nodiscard]] bool operator!=(const JetPackD& b) const noexcept { return a != b.a; }
    [[nodiscard]] bool operator<(const JetPackD& b) const noexcept { return a < b.a; }
    [[nodiscard]] bool operator<=(const JetPackD& b) const noexcept { return a <= b.a; }
    [[nodiscard]] bool operator>(const JetPackD& b) const noexcept { return a > b.a; }
    [[nodiscard]] bool operator>=(const JetPackD& b) const noexcept { return a >= b.a; }

    // Fill out[0..N) with the partials.
    CRD_FORCEINLINE void store_partials(crd::f64* out) const noexcept { pack_detail::rpd_store(v, out); }
    [[nodiscard]] crd::f64 partial(int k) const noexcept
    {
        crd::f64 tmp[kLanes];
        pack_detail::rpd_store(v, tmp);
        return tmp[k];
    }
};

// Transcendentals (ADL; sin/cos fuse via sincos — one range reduction).
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> sin(const JetPackD<N>& x) noexcept
{
    crd::f64 s = 0.0;
    crd::f64 c = 0.0;
    crd::math::sincos(x.a, s, c);
    return {s, pack_detail::rpd_scale(c, x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> cos(const JetPackD<N>& x) noexcept
{
    crd::f64 s = 0.0;
    crd::f64 c = 0.0;
    crd::math::sincos(x.a, s, c);
    return {c, pack_detail::rpd_scale(-s, x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> tan(const JetPackD<N>& x) noexcept
{
    const crd::f64 c = crd::math::cos(x.a);
    return {crd::math::tan(x.a), pack_detail::rpd_scale(1.0 / (c * c), x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> exp(const JetPackD<N>& x) noexcept
{
    const crd::f64 e = crd::math::exp(x.a);
    return {e, pack_detail::rpd_scale(e, x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> log(const JetPackD<N>& x) noexcept
{
    return {crd::math::log(x.a), pack_detail::rpd_scale(1.0 / x.a, x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> sqrt(const JetPackD<N>& x) noexcept
{
    const crd::f64 s = crd::math::sqrt(x.a);
    return {s, pack_detail::rpd_scale(1.0 / (2.0 * s), x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> tanh(const JetPackD<N>& x) noexcept
{
    const crd::f64 t = crd::math::tanh(x.a);
    return {t, pack_detail::rpd_scale(1.0 - t * t, x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> abs(const JetPackD<N>& x) noexcept
{
    return x.a >= 0.0 ? x : -x;
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> pow(const JetPackD<N>& x, crd::f64 p) noexcept
{
    return {crd::math::pow(x.a, p), pack_detail::rpd_scale(p * crd::math::pow(x.a, p - 1.0), x.v)};
}

// Mixed scalar/pack.
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator+(crd::f64 s, const JetPackD<N>& x) noexcept
{
    return {s + x.a, x.v};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator+(const JetPackD<N>& x, crd::f64 s) noexcept
{
    return s + x;
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator-(crd::f64 s, const JetPackD<N>& x) noexcept
{
    return {s - x.a, pack_detail::rpd_neg(x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator-(const JetPackD<N>& x, crd::f64 s) noexcept
{
    return {x.a - s, x.v};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator*(crd::f64 s, const JetPackD<N>& x) noexcept
{
    return {s * x.a, pack_detail::rpd_scale(s, x.v)};
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator*(const JetPackD<N>& x, crd::f64 s) noexcept
{
    return s * x;
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator/(const JetPackD<N>& x, crd::f64 s) noexcept
{
    const crd::f64 inv = 1.0 / s;
    return {x.a * inv, pack_detail::rpd_scale(inv, x.v)};
}

template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> select(bool cond, const JetPackD<N>& a, const JetPackD<N>& b) noexcept
{
    return cond ? a : b;
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> min(const JetPackD<N>& a, const JetPackD<N>& b) noexcept
{
    return a.a <= b.a ? a : b;
}
template <int N> [[nodiscard]] CRD_FORCEINLINE JetPackD<N> max(const JetPackD<N>& a, const JetPackD<N>& b) noexcept
{
    return a.a >= b.a ? a : b;
}

// ============================ drivers (Phase 3.1.6 v15-d) ============================
//
// RUNTIME-n TILING: an arbitrary-length gradient is strip-mined into ceil(n/W) SIMD passes — each pass seeds W input
// directions into the pack, evaluates f once, and reads W partials; the ragged final tile seeds only its valid
// directions (the rest carry a zero tangent and are ignored). W is a COMPILE-TIME tile width (the scratch's JetPackD
// width; 8 is two Vec4d registers, one Vec8d at avx512); the dimension n is a RUNTIME argument. This separates the two
// things Ceres conflates (compile-time N vs a driver over arbitrary n), and needs no Eigen.
//
// ALLOCATION-FREE: the caller owns the jet workspace (`scratch`, at least n jets). The drivers take raw pointers, so a
// kernel unit can run them. DETERMINISM MOAT: each output partial ∂f/∂x_k is computed in its OWN SIMD lane by a
// per-direction chain that never mixes with other lanes, so the result is BIT-IDENTICAL across the tile width W,
// across {1..16} parallel workers, across levels and across platforms (single-rounded `fma`, fixed order). It matches
// the scalar Jet<T,N> driver to <= 1 ulp (fma vs mul+add — the correctness oracle).

// ∇f of a SCALAR functor f: R^n → R at x[0..n), into g[0..n). Functor: `T f(const T* x, int n)`.
template <int W, class F>
inline void gradient(const F& f, const crd::f64* x, int n, crd::f64* g, JetPackD<W>* scratch) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = JetPackD<W>(x[i]); // every input a constant (zero tangent)
    }
    crd::f64 part[W];
    for (int base = 0; base < n; base += W)
    {
        const int cnt = (n - base < W) ? (n - base) : W;
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k], k); // seed direction k of this tile
        }
        const JetPackD<W> y = f(static_cast<const JetPackD<W>*>(scratch), n);
        y.store_partials(part);
        for (int k = 0; k < cnt; ++k)
        {
            g[base + k] = part[k]; // ∂f/∂x_{base+k}
        }
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k]); // reset to constant for the next tile
        }
    }
}

// J of a VECTOR functor f: R^n → R^m at x[0..n), into row-major jac[m*n]. Functor: `void f(const T* x, int n, T* y,
// int m)` (writes y[0..m)). `scratch` (>= n) and `yscratch` (>= m) are the caller's.
template <int W, class F>
inline void jacobian(const F& f, const crd::f64* x, int n, int m, crd::f64* jac, JetPackD<W>* scratch,
                     JetPackD<W>* yscratch) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = JetPackD<W>(x[i]);
    }
    crd::f64 part[W];
    for (int base = 0; base < n; base += W)
    {
        const int cnt = (n - base < W) ? (n - base) : W;
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k], k);
        }
        f(static_cast<const JetPackD<W>*>(scratch), n, yscratch, m);
        for (int j = 0; j < m; ++j)
        {
            yscratch[j].store_partials(part);
            for (int k = 0; k < cnt; ++k)
            {
                jac[(j * n) + base + k] = part[k]; // ∂f_j/∂x_{base+k}
            }
        }
        for (int k = 0; k < cnt; ++k)
        {
            scratch[base + k] = JetPackD<W>(x[base + k]);
        }
    }
}

} // namespace crd::hesap::autodiff::forward::inline CRD_MATH_LEVEL_NS

#undef CRD_NO_UNIQUE_ADDR
