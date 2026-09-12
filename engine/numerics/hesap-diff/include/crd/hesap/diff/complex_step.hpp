#pragma once

// crd-hesap-diff v13-m — ★★ COMPLEX-STEP differentiation: f'(x) = Im[f(x + i·h)] / h, MACHINE-EXACT.
//
// The killer property: a finite difference (f(x+h)−f(x−h))/(2h) subtracts two nearly-equal numbers ⇒ catastrophic
// cancellation ⇒ you lose ~half (forward) to ~⅓ (central) of your digits and must tune h. The complex step has NO
// subtraction: the derivative rides in the IMAGINARY part, recovered exactly for ANY tiny h (h² just underflows to
// zero, harmlessly). Verified bit-against JAX autodiff (float64): agreement to 0 / 1e-16 — i.e. as accurate as
// algorithmic differentiation, with none of its machinery. The modern best gradient for satellite/aero/MDO
// sensitivity and robot IK Jacobians when f can be written generically over a complex scalar type.
//
// Requirement: f must be callable on std::complex<T> (write the integrand generically over its scalar type — it then
// works for both the real evaluation and the complex-step derivative). Moat: determinism (crd::math complex cores via
// std::complex arithmetic, fixed FP order) + allocation-free.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

#include <complex>
#include <limits>
#include <utility>

namespace crd::hesap::diff
{

// The default complex-step size: sqrt(smallest-normal) — tiny enough that h² underflows (no real-part perturbation)
// yet well within the normal range (no denormal hazard). f64 ~ 1.5e-154; f32 ~ 1.1e-19.
template <typename T>
[[nodiscard]] inline T complex_step_default_h() noexcept
{
    return crd::math::sqrt(std::numeric_limits<T>::min());
}

// Machine-exact first derivative of a real-analytic f at x via the complex step. f: callable std::complex<T> -> std::complex<T>.
template <typename T, typename F>
[[nodiscard]] T derivative_complex_step(F&& f, T x, T h = complex_step_default_h<T>())
{
    const std::complex<T> r = f(std::complex<T>(x, h));
    return r.imag() / h;
}

// Machine-exact gradient of a scalar field f: R^n -> R via the complex step (one complex evaluation per component).
// `x` is the (mutable) complex work vector (real parts = the point; imaginary parts must be 0 on entry and are
// restored on exit). f: callable (ConstSpan<std::complex<T>>) -> std::complex<T>. grad_out has length n.
template <typename T, typename F>
void gradient_complex_step(F&& f, crd::containers::Span<std::complex<T>> x, T* grad_out,
                           T h = complex_step_default_h<T>())
{
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        x[static_cast<crd::usize>(i)] = std::complex<T>(x[static_cast<crd::usize>(i)].real(), h);
        const std::complex<T> v       = f(crd::containers::ConstSpan<std::complex<T>>{x.data(), x.size()});
        grad_out[i]                   = v.imag() / h;
        x[static_cast<crd::usize>(i)] = std::complex<T>(x[static_cast<crd::usize>(i)].real(), T{0});
    }
}

// Machine-exact Jacobian of a vector field f: R^n -> R^m via the complex step. `x` is the mutable complex work vector
// (length n, imaginary parts 0 on entry); `fx` is a scratch output vector (length m); jac_out is row-major m×n
// (jac_out[r*n + c] = ∂f_r/∂x_c). f: callable (ConstSpan<std::complex<T>> in, Span<std::complex<T>> out) -> void.
template <typename T, typename F>
void jacobian_complex_step(F&& f, crd::containers::Span<std::complex<T>> x, crd::containers::Span<std::complex<T>> fx,
                           T* jac_out, T h = complex_step_default_h<T>())
{
    const int n = static_cast<int>(x.size());
    const int m = static_cast<int>(fx.size());
    for (int c = 0; c < n; ++c)
    {
        x[static_cast<crd::usize>(c)] = std::complex<T>(x[static_cast<crd::usize>(c)].real(), h);
        f(crd::containers::ConstSpan<std::complex<T>>{x.data(), x.size()}, fx);
        for (int r = 0; r < m; ++r)
        {
            jac_out[r * n + c] = fx[static_cast<crd::usize>(r)].imag() / h;
        }
        x[static_cast<crd::usize>(c)] = std::complex<T>(x[static_cast<crd::usize>(c)].real(), T{0});
    }
}

} // namespace crd::hesap::diff
