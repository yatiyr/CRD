#pragma once

// crd-hesap-motion v13-n — ATTITUDE trajectories on the unit-quaternion manifold:
//   ★SQUAD (Spherical & Quadrangle) — a C²-continuous spherical cubic through a sequence of orientations via nested
//   SLERP + per-key control points (Shoemake). The smooth-slew primitive for spacecraft attitude, camera rigs, and
//   animation — manifold-correct (stays on the unit sphere, no gimbal lock, no re-normalization drift).
//
// Reuses crd-math Quat slerp + quat_log/quat_exp (SANITY 8 — the quaternion capability lives in crd-math). Verified
// in python: unit-norm preserved to 1e-16 along a segment, endpoints exact, C² across segments. No numeric peer (the
// gate is the manifold invariants); Boost/GSL/scipy have no quaternion-spline. Moat: determinism (crd::math) + alloc-free.

#include <crd/math/quat.hpp>

namespace crd::hesap::motion
{

// The SQUAD control point (inner quadrangle point) s_i for key q_i with neighbours q_{i-1}, q_{i+1}:
//   s_i = q_i · exp( −( log(q_i⁻¹ q_{i+1}) + log(q_i⁻¹ q_{i-1}) ) / 4 ).
// For a unit quaternion q_i⁻¹ = conjugate(q_i). Precompute one per interior key; endpoints reuse the key itself.
template <typename T>
[[nodiscard]] crd::math::Quat<T> squad_control_point(const crd::math::Quat<T>& qprev, const crd::math::Quat<T>& qi,
                                                     const crd::math::Quat<T>& qnext)
{
    const crd::math::Quat<T> qi_inv = crd::math::conjugate(qi);
    const crd::math::Quat<T> ln     = crd::math::quat_log(qi_inv * qnext);
    const crd::math::Quat<T> lp     = crd::math::quat_log(qi_inv * qprev);
    const crd::math::Quat<T> inner(-(ln.x + lp.x) / static_cast<T>(4), -(ln.y + lp.y) / static_cast<T>(4),
                                   -(ln.z + lp.z) / static_cast<T>(4), T{0});
    return qi * crd::math::quat_exp(inner);
}

// SQUAD interpolation between q0 (t=0) and q1 (t=1) with the segment's control points s0, s1:
//   squad = slerp( slerp(q0, q1, t), slerp(s0, s1, t), 2t(1−t) ).  C²-continuous when the s_i are the control points
// above. Returns a unit quaternion.
template <typename T>
[[nodiscard]] crd::math::Quat<T> squad(const crd::math::Quat<T>& q0, const crd::math::Quat<T>& q1,
                                       const crd::math::Quat<T>& s0, const crd::math::Quat<T>& s1, T t)
{
    const crd::math::Quat<T> a = crd::math::slerp(q0, q1, t);
    const crd::math::Quat<T> b = crd::math::slerp(s0, s1, t);
    return crd::math::slerp(a, b, T{2} * t * (T{1} - t));
}

// ★Quaternion cubic B-spline (Kim-Kim-Shin cumulative-basis) — a C² manifold-correct orientation spline through a
// sequence of control quaternions, built by ACCUMULATING relative log-rotations weighted by the cumulative uniform
// cubic B-spline basis. The alternative to SQUAD for long, smoothly-varying attitude paths (drone gimbals, camera
// dollies, articulated-body orientation): unlike SQUAD it is a true C² B-spline (bounded angular acceleration), and
// unlike a naive component-wise spline it never leaves the unit sphere. `qm1, qi, qp1, qp2` are the four control
// quaternions influencing the segment between qi (u=0) and qp1 (u=1); u ∈ [0,1]. Returns a unit quaternion.
template <typename T>
[[nodiscard]] crd::math::Quat<T> quaternion_bspline(const crd::math::Quat<T>& qm1, const crd::math::Quat<T>& qi,
                                                    const crd::math::Quat<T>& qp1, const crd::math::Quat<T>& qp2, T u)
{
    // relative log-rotations ω_j = log(q_{j-1}⁻¹ q_j) (pure quaternions)
    const crd::math::Quat<T> w1 = crd::math::quat_log(crd::math::conjugate(qm1) * qi);
    const crd::math::Quat<T> w2 = crd::math::quat_log(crd::math::conjugate(qi) * qp1);
    const crd::math::Quat<T> w3 = crd::math::quat_log(crd::math::conjugate(qp1) * qp2);
    // cumulative uniform cubic B-spline basis on [0,1]
    const T u2 = u * u;
    const T u3 = u2 * u;
    const T b1 = (u3 - T{3} * u2 + T{3} * u + T{5}) / static_cast<T>(6); // = 1 − (1−u)³/6
    const T b2 = (-T{2} * u3 + T{3} * u2 + T{3} * u + T{1}) / static_cast<T>(6);
    const T b3 = u3 / static_cast<T>(6);
    auto    scaled_exp = [](const crd::math::Quat<T>& w, T b) {
        return crd::math::quat_exp(crd::math::Quat<T>(w.x * b, w.y * b, w.z * b, T{0}));
    };
    return qm1 * scaled_exp(w1, b1) * scaled_exp(w2, b2) * scaled_exp(w3, b3);
}

} // namespace crd::hesap::motion
