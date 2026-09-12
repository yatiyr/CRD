#pragma once

// complex_dual.hpp — Phase 3.1.6 v15-h: COMPLEX / WIRTINGER forward AD. The holomorphic dual is just
// `Dual<std::complex<T>>` — value AND tangent both complex — so a holomorphic op propagates ẇ = f'(z)·ż with a
// COMPLEX multiply using the IDENTICAL real-dual code (dual.hpp), no new rules, for +−×÷ / exp/log/sqrt/pow/sin/cos/
// tan/tanh and the linear FFT. This is the JAX `jvp` (un-conjugated pushforward) convention; gradient-conjugation is
// a v16 reverse-mode concern.
//
// NON-HOLOMORPHIC ops (conj / Re / Im / |z| / |z|²) are NOT of the form f'(z)·ż — their single-tangent pushforward is
// ℝ-linear (t_ż = a·ż + b·conj(ż)) with a=∂f/∂z, b=∂f/∂z̄. We define them here. The full WIRTINGER PAIR (∂/∂z, ∂/∂z̄)
// is RECONSTRUCTED by seeding two tangents ż=1 and ż=i (t1=a+b, ti=i(a−b) ⇒ ∂/∂z=(t1−i·ti)/2, ∂/∂z̄=(t1+i·ti)/2).
// Holomorphic ⟺ ∂/∂z̄=0 ⟺ Cauchy-Riemann — that is the gate. Determinism preserved (crd::math::complex cores).
//
// LIMIT (encode it): complex-step CANNOT validate these rules (it co-opts the imaginary axis) — validate with a
// 2×2-real-Jacobian FD instead (tests). ADR-0097.

#include <crd/hesap/autodiff/dual.hpp>

#include <crd/core/types.hpp>
#include <crd/math/complex.hpp>

#include <complex>

namespace crd::hesap::autodiff::forward
{

// The holomorphic dual: value + complex tangent. (Holomorphic ops come free from dual.hpp via complex arithmetic.)
template <typename T>
using CDual = Dual<std::complex<T>>;

// Seed a complex variable z with a tangent direction (ż=1 for the holomorphic derivative; ż=i for the second CR seed).
template <typename T>
[[nodiscard]] inline CDual<T> cdual_var(std::complex<T> z) noexcept
{
    return CDual<T>{z, std::complex<T>(T(1), T(0))};
}
template <typename T>
[[nodiscard]] inline CDual<T> cdual_seed(std::complex<T> z, std::complex<T> dz) noexcept
{
    return CDual<T>{z, dz};
}

// ---- non-holomorphic pushforwards (single complex tangent) --------------------------------------------------
// conj: ∂/∂z=0, ∂/∂z̄=1 ⇒ ẇ = conj(ż).
template <typename T>
[[nodiscard]] inline CDual<T> conj(const CDual<T>& x) noexcept
{
    return CDual<T>{crd::math::conj(x.v), crd::math::conj(x.d)};
}
// Re: ẇ = Re(ż) (embedded on the real axis). Im: ẇ = Im(ż).
template <typename T>
[[nodiscard]] inline CDual<T> real_part(const CDual<T>& x) noexcept
{
    return CDual<T>{std::complex<T>(x.v.real(), T(0)), std::complex<T>(x.d.real(), T(0))};
}
template <typename T>
[[nodiscard]] inline CDual<T> imag_part(const CDual<T>& x) noexcept
{
    return CDual<T>{std::complex<T>(x.v.imag(), T(0)), std::complex<T>(x.d.imag(), T(0))};
}
// |z| : real-valued; ẇ = Re(conj(z)·ż)/|z|.
template <typename T>
[[nodiscard]] inline CDual<T> abs(const CDual<T>& x) noexcept
{
    const T m  = crd::math::abs(x.v);
    const T dt = (x.v.real() * x.d.real() + x.v.imag() * x.d.imag()) / m; // Re(z̄·ż)/|z|
    return CDual<T>{std::complex<T>(m, T(0)), std::complex<T>(dt, T(0))};
}
// |z|² = z·conj(z) : real-valued; ẇ = 2·Re(conj(z)·ż).
template <typename T>
[[nodiscard]] inline CDual<T> norm(const CDual<T>& x) noexcept
{
    const T n  = crd::math::norm(x.v);
    const T dt = T(2) * (x.v.real() * x.d.real() + x.v.imag() * x.d.imag());
    return CDual<T>{std::complex<T>(n, T(0)), std::complex<T>(dt, T(0))};
}

// ---- Wirtinger reconstruction + holomorphy gate -------------------------------------------------------------
template <typename T>
struct Wirtinger
{
    std::complex<T> dz;    // ∂f/∂z
    std::complex<T> dzbar; // ∂f/∂z̄  (≈0 ⟺ holomorphic)
};

// Reconstruct (∂f/∂z, ∂f/∂z̄) for a scalar-generic complex functor f(CDual<T>)→CDual<T> at the point z: seed ż=1 and
// ż=i, read the two output tangents, solve the ℝ-linear pushforward. Two evaluations, EXACT (no FD step, no cancel).
template <typename T, class F>
[[nodiscard]] inline Wirtinger<T> wirtinger(const F& f, std::complex<T> z) noexcept
{
    const std::complex<T> t1 = f(cdual_seed(z, std::complex<T>(T(1), T(0)))).d;
    const std::complex<T> ti = f(cdual_seed(z, std::complex<T>(T(0), T(1)))).d;
    const std::complex<T> im(T(0), T(1));
    return Wirtinger<T>{(t1 - im * ti) * T(0.5), (t1 + im * ti) * T(0.5)};
}

// Holomorphic ⟺ ∂f/∂z̄ ≈ 0 (Cauchy-Riemann). Returns |∂f/∂z̄| — a caller thresholds it.
template <typename T, class F>
[[nodiscard]] inline T holomorphy_defect(const F& f, std::complex<T> z) noexcept
{
    return crd::math::abs(wirtinger<T>(f, z).dzbar);
}

} // namespace crd::hesap::autodiff::forward
