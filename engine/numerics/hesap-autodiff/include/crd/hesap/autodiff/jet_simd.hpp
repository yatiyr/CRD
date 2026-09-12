#pragma once

// jet_simd.hpp — Phase 3.1.6 v15-a (crush carrier): the SIMD vector-forward carrier. Partials live in SIMD
// registers (Vec4d for f64, Vec8f for f32), NOT a scalar C-array — arithmetic is single-rounded FMA on whole
// registers with no intermediate materialization and no Eigen. ADR-0097; design + boards:
// docs/research/2026-07-06-v15-forward-ad-crush.md §A, docs/bench/2026-07-06-v15a-forward-carrier.md.
//
//   JetPackD<N> — f64 value + N partials across ceil(N/4) Vec4d registers.
//   JetPackF<N> — f32 value + N partials across ceil(N/8) Vec8f registers.
//
// TWO codegen levers make this crush (measured; the N=4 single-register case beats ALL 7 frontier peers 1.85x, and
// the >1-register case went 34.6->10.5 ns at N=8 — 3.3x — closing on Ceres/Eigen):
//   (1) The partial registers are a RECURSIVE NAMED-MEMBER pack (`RegPack`), NOT a `Vec4d v[kRegs]` ARRAY. GCC/clang
//       SROA-promote named members into YMM registers across an operation chain; they SPILL an array (the array
//       cost 2.5x). The pack is compile-time unrolled (each level is one named Vec4d).
//   (2) The product rule places the CARRIED accumulator's partial as the FMA MULTIPLICAND (single-rounded fma), and
//       the non-carried `a.value * b.partial` term as the addend — the recurrence is then one 4-cycle FMA hop, not
//       mul->fma-addend (8c). This is tuned for the `acc = acc * x` idiom (accumulator on the LEFT), the dominant
//       AD accumulation pattern; a right-carried chain is a cycle longer. Ceres/Eigen has the identical asymmetry
//       (the compiler resolves it silently); we FIX the order explicitly — which ALSO pins it for the determinism
//       moat. The order is load-bearing for BOTH latency and bit-reproducibility — NEVER reorder.
//
// DETERMINISM: single-rounded `crd::math::simd::fma` throughout; the chain rule is per-LANE independent (no
// cross-lane reduction in forward mode), so the packed result is bit-identical regardless of how directions tile
// across registers — the {1..16}/cross-platform moat is automatic. We do NOT reassociate the product chain (that
// would change rounding) — so `crd::math::simd::mul_add` / compiler `-ffp-contract` fusion is deliberately NOT used.
// Results are <=1 ulp from the scalar Jet<T,N> path (fma vs mul+add), gated against it in the tests.
//
// The >1-register regime is competitive but does not yet CRUSH Eigen-Ceres; that remains an OPEN crush target
// (SANITY #9 — a documented gap, never an accepted loss). N>register-width tiles here; the gradient/jacobian
// DRIVERS are v15-d.

#include <crd/core/platform.hpp> // CRD_FORCEINLINE
#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

// no_unique_address: MSVC + clang-cl (MSVC ABI) accept ONLY [[msvc::no_unique_address]] and hard-error on the
// standard spelling under -Werror; gcc/clang-native use the standard one. Select per toolchain.
#if defined(_MSC_VER)
#define CRD_NO_UNIQUE_ADDR [[msvc::no_unique_address]]
#else
#define CRD_NO_UNIQUE_ADDR [[no_unique_address]]
#endif

namespace crd::hesap::autodiff::forward
{

// ============================ f64 carrier: JetPackD<N> (Vec4d named-register pack) ============================
namespace detail
{
using crd::math::simd::Vec4d;

// Recursive named-register pack: `head` (one Vec4d) + `tail` (the rest). Named members => SROA-promotable to YMM
// registers across a chain (an array is not). Compile-time unrolled.
template <int NR>
struct RegPackD
{
    Vec4d head;
    // Empty at NR==1 => zero-size (RegPackD<1> is exactly one Vec4d, no padding bloat) on every toolchain.
    CRD_NO_UNIQUE_ADDR RegPackD<NR - 1> tail;
};
template <>
struct RegPackD<0>
{
};

[[nodiscard]] CRD_FORCEINLINE const Vec4d& unit_d(int lane) noexcept
{
    static const Vec4d u[4] = {Vec4d(1, 0, 0, 0), Vec4d(0, 1, 0, 0), Vec4d(0, 0, 1, 0), Vec4d(0, 0, 0, 1)};
    return u[lane];
}

template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_zero() noexcept
{
    return {Vec4d::zero(), rpd_zero<NR - 1>()};
}
template <>
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_zero<0>() noexcept
{
    return {};
}

// Seed direction k (global slot): register k/4 gets a 1 in lane k%4, all others zero.
template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_seed(int k) noexcept
{
    return (k >= 0 && k < 4) ? RegPackD<NR>{unit_d(k), rpd_zero<NR - 1>()}
                             : RegPackD<NR>{Vec4d::zero(), rpd_seed<NR - 1>(k - 4)};
}
template <>
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_seed<0>(int) noexcept
{
    return {};
}

template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_neg(const RegPackD<NR>& a) noexcept
{
    return {-a.head, rpd_neg(a.tail)};
}
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_neg(const RegPackD<0>&) noexcept { return {}; }

template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_add(const RegPackD<NR>& a, const RegPackD<NR>& b) noexcept
{
    return {a.head + b.head, rpd_add(a.tail, b.tail)};
}
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_add(const RegPackD<0>&, const RegPackD<0>&) noexcept { return {}; }

template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_sub(const RegPackD<NR>& a, const RegPackD<NR>& b) noexcept
{
    return {a.head - b.head, rpd_sub(a.tail, b.tail)};
}
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_sub(const RegPackD<0>&, const RegPackD<0>&) noexcept { return {}; }

// Product rule per register, FMA-ordered: result = av.partial * b.value  +  a.value * bv.partial, with the CARRIED
// (av) partial as the fma multiplicand. `ab`=broadcast(a.value), `bb`=broadcast(b.value).
template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_mul(const RegPackD<NR>& av, Vec4d ab, Vec4d bb,
                                                   const RegPackD<NR>& bv) noexcept
{
    return {crd::math::simd::fma(av.head, bb, ab * bv.head), rpd_mul(av.tail, ab, bb, bv.tail)};
}
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_mul(const RegPackD<0>&, Vec4d, Vec4d, const RegPackD<0>&) noexcept
{
    return {};
}

// Quotient rule per register: (a.partial - (a/b) * b.partial) * (1/b.value). `abv`=broadcast(a/b), `invv`=broadcast(1/b.value).
template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_div(const RegPackD<NR>& av, Vec4d abv, Vec4d invv,
                                                   const RegPackD<NR>& bv) noexcept
{
    return {(av.head - abv * bv.head) * invv, rpd_div(av.tail, abv, invv, bv.tail)};
}
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_div(const RegPackD<0>&, Vec4d, Vec4d, const RegPackD<0>&) noexcept
{
    return {};
}

// Scale every partial by a broadcast derivative (transcendentals + scalar mul).
template <int NR>
[[nodiscard]] CRD_FORCEINLINE RegPackD<NR> rpd_scale(Vec4d d, const RegPackD<NR>& a) noexcept
{
    return {d * a.head, rpd_scale(d, a.tail)};
}
[[nodiscard]] CRD_FORCEINLINE RegPackD<0> rpd_scale(Vec4d, const RegPackD<0>&) noexcept { return {}; }

// Store the first `count` partials (across registers) contiguously.
template <int NR>
CRD_FORCEINLINE void rpd_store(const RegPackD<NR>& a, crd::f64* out, int count) noexcept
{
    if (count >= 4)
    {
        a.head.store(out); // plain (not masked) store for a full register
    }
    else
    {
        a.head.store_partial(out, static_cast<crd::usize>(count));
    }
    rpd_store(a.tail, out + 4, count - 4);
}
CRD_FORCEINLINE void rpd_store(const RegPackD<0>&, crd::f64*, int) noexcept {}
} // namespace detail

template <int N>
struct JetPackD
{
    static constexpr int kWidth    = 4;
    static constexpr int kRegs     = (N + kWidth - 1) / kWidth;
    static constexpr int DIMENSION = N;
    using Scalar                   = crd::f64;

    crd::f64                  a; // value
    detail::RegPackD<kRegs>   v; // partials (recursive named Vec4d registers)

    JetPackD() noexcept = default;
    JetPackD(crd::f64 value) noexcept : a(value), v(detail::rpd_zero<kRegs>()) {} // NOLINT(google-explicit-constructor)
    JetPackD(crd::f64 value, int k) noexcept : a(value), v(detail::rpd_seed<kRegs>(k)) {}
    JetPackD(crd::f64 value, const detail::RegPackD<kRegs>& part) noexcept : a(value), v(part) {}

    [[nodiscard]] CRD_FORCEINLINE JetPackD operator-() const noexcept { return {-a, detail::rpd_neg(v)}; }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator+(const JetPackD& b) const noexcept
    {
        return {a + b.a, detail::rpd_add(v, b.v)};
    }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator-(const JetPackD& b) const noexcept
    {
        return {a - b.a, detail::rpd_sub(v, b.v)};
    }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator*(const JetPackD& b) const noexcept
    {
        // `this` (left) is the carried accumulator in `acc = acc * x` — its partials are the fma multiplicand.
        return {a * b.a, detail::rpd_mul(v, detail::Vec4d(a), detail::Vec4d(b.a), b.v)};
    }
    [[nodiscard]] CRD_FORCEINLINE JetPackD operator/(const JetPackD& b) const noexcept
    {
        const crd::f64 inv    = 1.0 / b.a;
        const crd::f64 a_by_b = a * inv;
        return {a_by_b, detail::rpd_div(v, detail::Vec4d(a_by_b), detail::Vec4d(inv), b.v)};
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
    CRD_FORCEINLINE void store_partials(crd::f64* out) const noexcept { detail::rpd_store(v, out, N); }
    [[nodiscard]] crd::f64 partial(int k) const noexcept
    {
        crd::f64 tmp[kRegs * 4];
        detail::rpd_store(v, tmp, N);
        return tmp[k];
    }
};

// Transcendentals (ADL; sin/cos fuse via sincos — one range reduction).
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> sin(const JetPackD<N>& x) noexcept
{
    crd::f64 s = 0.0;
    crd::f64 c = 0.0;
    crd::math::sincos(x.a, s, c);
    return {s, detail::rpd_scale(detail::Vec4d(c), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> cos(const JetPackD<N>& x) noexcept
{
    crd::f64 s = 0.0;
    crd::f64 c = 0.0;
    crd::math::sincos(x.a, s, c);
    return {c, detail::rpd_scale(detail::Vec4d(-s), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> tan(const JetPackD<N>& x) noexcept
{
    const crd::f64 c = crd::math::cos(x.a);
    return {crd::math::tan(x.a), detail::rpd_scale(detail::Vec4d(1.0 / (c * c)), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> exp(const JetPackD<N>& x) noexcept
{
    const crd::f64 e = crd::math::exp(x.a);
    return {e, detail::rpd_scale(detail::Vec4d(e), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> log(const JetPackD<N>& x) noexcept
{
    return {crd::math::log(x.a), detail::rpd_scale(detail::Vec4d(1.0 / x.a), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> sqrt(const JetPackD<N>& x) noexcept
{
    const crd::f64 s = crd::math::sqrt(x.a);
    return {s, detail::rpd_scale(detail::Vec4d(1.0 / (2.0 * s)), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> tanh(const JetPackD<N>& x) noexcept
{
    const crd::f64 t = crd::math::tanh(x.a);
    return {t, detail::rpd_scale(detail::Vec4d(1.0 - t * t), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> abs(const JetPackD<N>& x) noexcept
{
    return x.a >= 0.0 ? x : -x;
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> pow(const JetPackD<N>& x, crd::f64 p) noexcept
{
    return {crd::math::pow(x.a, p), detail::rpd_scale(detail::Vec4d(p * crd::math::pow(x.a, p - 1.0)), x.v)};
}

// Mixed scalar/pack.
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator+(crd::f64 s, const JetPackD<N>& x) noexcept
{
    return {s + x.a, x.v};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator+(const JetPackD<N>& x, crd::f64 s) noexcept { return s + x; }
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator-(crd::f64 s, const JetPackD<N>& x) noexcept
{
    return {s - x.a, detail::rpd_neg(x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator-(const JetPackD<N>& x, crd::f64 s) noexcept
{
    return {x.a - s, x.v};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator*(crd::f64 s, const JetPackD<N>& x) noexcept
{
    return {s * x.a, detail::rpd_scale(detail::Vec4d(s), x.v)};
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator*(const JetPackD<N>& x, crd::f64 s) noexcept { return s * x; }
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> operator/(const JetPackD<N>& x, crd::f64 s) noexcept
{
    const crd::f64 inv = 1.0 / s;
    return {x.a * inv, detail::rpd_scale(detail::Vec4d(inv), x.v)};
}

template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> select(bool cond, const JetPackD<N>& a, const JetPackD<N>& b) noexcept
{
    return cond ? a : b;
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> min(const JetPackD<N>& a, const JetPackD<N>& b) noexcept
{
    return a.a <= b.a ? a : b;
}
template <int N>
[[nodiscard]] CRD_FORCEINLINE JetPackD<N> max(const JetPackD<N>& a, const JetPackD<N>& b) noexcept
{
    return a.a >= b.a ? a : b;
}

} // namespace crd::hesap::autodiff::forward

#undef CRD_NO_UNIQUE_ADDR
