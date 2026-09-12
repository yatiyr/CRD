#pragma once

// crd-hesap-motion v13-o — CLOTHOID (Euler spiral): the curve whose curvature varies LINEARLY with arc length,
// κ(s) = κ0 + κ1·s. That linear curvature is exactly what bounds LATERAL JERK in a steering maneuver, which is why
// clothoids are the reference transition curve for highway/rail alignment and self-driving path planning (the wheel
// turns at a constant rate). Position is given in closed form by the FRESNEL integrals (reuses crd-hesap-special;
// SANITY 8) — far more accurate and cheaper than numerically integrating the heading. Verified vs numeric integration.
//
// Moat: determinism (crd::math + the deterministic Fresnel) + allocation-free + closed-form (no per-eval quadrature).

#include <crd/hesap/special/fresnel.hpp>

#include <crd/core/types.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::motion
{

// The pose along a clothoid at arc length s.
template <typename T>
struct ClothoidPose
{
    T x;
    T y;
    T theta; // heading
    T kappa; // curvature κ0 + κ1·s
};

// Evaluate the clothoid starting at (x0,y0) with heading θ0 and curvature κ0, curvature-rate κ1, at arc length s.
// κ1 ≈ 0 degrades gracefully: to a circular arc (κ0 ≠ 0) or a straight line (κ0 ≈ 0). f64 Fresnel internally.
template <typename T>
[[nodiscard]] ClothoidPose<T> clothoid_eval(T x0, T y0, T theta0, T kappa0, T kappa1, T s)
{
    const T theta = theta0 + kappa0 * s + static_cast<T>(0.5) * kappa1 * s * s;
    const T kappa = kappa0 + kappa1 * s;
    const T eps   = static_cast<T>(1e-12);
    if (crd::math::fabs(kappa1) < eps)
    {
        if (crd::math::fabs(kappa0) < eps) // straight line
        {
            return ClothoidPose<T>{x0 + s * crd::math::cos(theta0), y0 + s * crd::math::sin(theta0), theta0, kappa0};
        }
        // circular arc of radius 1/κ0
        const T r = T{1} / kappa0;
        const T x = x0 + r * (crd::math::sin(theta0 + kappa0 * s) - crd::math::sin(theta0));
        const T y = y0 - r * (crd::math::cos(theta0 + kappa0 * s) - crd::math::cos(theta0));
        return ClothoidPose<T>{x, y, theta, kappa};
    }
    // General clothoid via Fresnel. θ(u) = φ + 0.5·κ1·(u + κ0/κ1)², φ = θ0 − κ0²/(2κ1). With τ = w·√(|κ1|/π),
    // ∫_0^s cos/sin(θ(u)) du = a·[ … C(τ),S(τ) … ], a = √(π/|κ1|), sgn = sign(κ1).
    const T sgn   = kappa1 > T{0} ? T{1} : T{-1};
    const T akl   = crd::math::fabs(kappa1);
    const T a     = crd::math::sqrt(static_cast<T>(3.14159265358979323846264338327950288) / akl);
    const T phi   = theta0 - kappa0 * kappa0 / (T{2} * kappa1);
    const T scale = crd::math::sqrt(akl / static_cast<T>(3.14159265358979323846264338327950288));
    const T w0    = kappa0 / kappa1;
    const T w1    = s + kappa0 / kappa1;
    const T t0    = w0 * scale;
    const T t1    = w1 * scale;
    const T cc0   = crd::hesap::special::fresnel_c<T>(t0);
    const T cc1   = crd::hesap::special::fresnel_c<T>(t1);
    const T sc0   = crd::hesap::special::fresnel_s<T>(t0);
    const T sc1   = crd::hesap::special::fresnel_s<T>(t1);
    const T dc    = cc1 - cc0;
    const T ds    = sc1 - sc0;
    const T cphi  = crd::math::cos(phi);
    const T sphi  = crd::math::sin(phi);
    const T x     = x0 + a * (cphi * dc - sgn * sphi * ds);
    const T y     = y0 + a * (sphi * dc + sgn * cphi * ds);
    return ClothoidPose<T>{x, y, theta, kappa};
}

} // namespace crd::hesap::motion
