// The jet probe's operations and drivers (jet_level_probe.hpp): kernels/jet_level_probe.cpp runs them on each level's
// JetPackD, test_jet_levels.cpp on general code's and, through ops_with, on the scalar Jet<f64, N> oracle. They live
// in the level namespace (ADR-0134 §5) because both instantiate them.

#pragma once

#include "jet_level_probe.hpp"

#include <crd/core/types.hpp>
#include <crd/hesap/autodiff/jet_simd.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::test::jet_probe::inline CRD_MATH_LEVEL_NS
{

// emit(result) for every operation of the carrier on the operands a and b and the scalar s: kOpCount results.
template <class J, class Emit> void run_ops(const J& a, const J& b, crd::f64 s, const Emit& emit)
{
    emit(-a);
    emit(a + b);
    emit(a - b);
    emit(a * b);
    emit(a / b);
    emit(sin(a));
    emit(cos(a));
    emit(tan(a));
    emit(exp(a));
    emit(log(a));
    emit(sqrt(a));
    emit(tanh(a));
    emit(abs(a));
    emit(pow(a, 2.5));
    emit(pow(a, -1.5));
    emit(s + a);
    emit(a + s);
    emit(s - a);
    emit(a - s);
    emit(s * a);
    emit(a * s);
    emit(a / s);
    emit(min(a, b));
    emit(max(a, b));
    emit(select(b < a, a, b));
    J c = a;
    c += b;
    c *= a;
    c -= b;
    c /= b;
    emit(c);
    J acc = a; // the carried product chain (acc = acc * x)
    for (int i = 0; i < 4; ++i)
    {
        acc = acc * b;
        acc = acc * a;
    }
    emit(acc);
    emit(exp(sin(a)) + a * log(1.0 + b * b));
    emit(sqrt(a * a + b * b) / (b - a));
}

// f: R^n → R and g: R^n → R^m for the drivers.
struct Scalar
{
    template <class T> T operator()(const T* x, int n) const
    {
        T acc = x[0] * x[0];
        for (int i = 0; i < n; ++i)
        {
            acc = acc + sin(x[i]) * x[(i + 1) % n] + exp(x[i] * 0.1);
        }
        return acc;
    }
};
struct Vector
{
    template <class T> void operator()(const T* x, int n, T* y, int m) const
    {
        for (int j = 0; j < m; ++j)
        {
            y[j] = sin(x[j % n]) * x[(j + 1) % n] + x[j % n] * x[(j + 2) % n];
        }
    }
};

// Every operation on the carrier operands a and b of width N into out (jet_level_probe.hpp's layout).
template <int N, class J> crd::usize ops_with(const J& a, const J& b, crd::f64 s, crd::f64* out) noexcept
{
    crd::usize at = 0;
    run_ops(a, b, s,
            [&](const J& r)
            {
                out[at] = r.a;
                if constexpr (requires { r.store_partials(out); })
                {
                    r.store_partials(out + at + 1);
                }
                else
                {
                    for (int k = 0; k < N; ++k)
                    {
                        out[at + 1 + static_cast<crd::usize>(k)] = r.v[k];
                    }
                }
                at += 1 + N + kGuard;
            });
    return at;
}

template <int... N> crd::usize pack_ops(const Input& in, crd::f64* out) noexcept
{
    using crd::hesap::autodiff::forward::JetPackD;
    crd::usize at = 0;
    ((at += ops_with<N>(JetPackD<N>(in.a, in.da), JetPackD<N>(in.b, in.db), in.s, out + at)), ...);
    return at;
}

// Table::ops on this level's JetPackD.
inline crd::usize ops(const Input& in, crd::f64* out) noexcept
{
    return pack_ops<1, 3, 4, 5, 7, 8, 9, 15, 16, 17, 32>(in, out);
}

template <int W> crd::usize gradient_at(const crd::f64* x, int n, crd::f64* out) noexcept
{
    crd::hesap::autodiff::forward::JetPackD<W> scratch[kMaxN];
    crd::hesap::autodiff::forward::gradient(Scalar{}, x, n, out, scratch);
    return static_cast<crd::usize>(n) + kGuard;
}

template <int W> crd::usize jacobian_at(const crd::f64* x, int n, int m, crd::f64* out) noexcept
{
    crd::hesap::autodiff::forward::JetPackD<W> scratch[kMaxN];
    crd::hesap::autodiff::forward::JetPackD<W> y[8];
    crd::hesap::autodiff::forward::jacobian(Vector{}, x, n, m, out, scratch, y);
    return static_cast<crd::usize>(n * m) + kGuard;
}

// Table::drivers on this level's JetPackD: ragged and full tiles of 3, 4, 5, 8 and 16 directions.
inline crd::usize drivers(const crd::f64* x, crd::f64* out) noexcept
{
    crd::usize at = 0;
    at += gradient_at<8>(x, 1, out + at);
    at += gradient_at<8>(x, 7, out + at);
    at += gradient_at<8>(x, 8, out + at);
    at += gradient_at<8>(x, 9, out + at);
    at += gradient_at<8>(x, 17, out + at);
    at += gradient_at<8>(x, 32, out + at);
    at += gradient_at<4>(x, 13, out + at);
    at += gradient_at<5>(x, 13, out + at);
    at += gradient_at<16>(x, 19, out + at);
    at += jacobian_at<4>(x, 9, 3, out + at);
    at += jacobian_at<8>(x, 17, 5, out + at);
    at += jacobian_at<3>(x, 32, 8, out + at);
    return at;
}

// One JetPackD<N>'s layout at this level.
template <int N> constexpr Layout layout() noexcept
{
    using J = crd::hesap::autodiff::forward::JetPackD<N>;
    return {sizeof(J), alignof(J), J::kRegs, J::kLanes};
}

} // namespace crd::test::jet_probe::inline CRD_MATH_LEVEL_NS
