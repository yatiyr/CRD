#pragma once

// Public scalar Quat type.
//
// **Quat is intentionally NOT SIMD-ified at the per-instance API level.**
// v0e bench measured per-instance Quatf compose under SIMD at 0.65× scalar
// speed — the Hamilton product (16 muls + 12 adds) doesn't amortise the
// SIMD register load/store cost for a single quaternion.
//
// For batched quaternion work (eylem v4 articulation joint composition,
// animation skinning, particle orientations), use `simd::Soa<TChunk>` with
// `Vec8f qx, qy, qz, qw` columns — 8 quaternions per AVX op, zero waste.
//
// See vec.hpp + mat_simd_f32.hpp for the wider SIMD-vs-scalar architectural
// rationale.

#include <crd/math/mat.hpp>

#include <cmath>

namespace crd::math
{
template <MathScalar T> struct Quat
{
    T x = static_cast<T>(0);
    T y = static_cast<T>(0);
    T z = static_cast<T>(0);
    T w = static_cast<T>(1);

    constexpr Quat() noexcept = default;
    constexpr Quat(T x_in, T y_in, T z_in, T w_in) noexcept : x(x_in), y(y_in), z(z_in), w(w_in) {}

    [[nodiscard]] static constexpr Quat identity() noexcept
    {
        return Quat(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
    }
};

template <MathScalar T> [[nodiscard]] constexpr bool operator==(const Quat<T>& lhs, const Quat<T>& rhs) noexcept
{
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

template <MathScalar T> [[nodiscard]] constexpr T dot(const Quat<T>& lhs, const Quat<T>& rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
}

template <MathScalar T> [[nodiscard]] constexpr T length_squared(const Quat<T>& q) noexcept
{
    return dot(q, q);
}

template <MathScalar T> [[nodiscard]] inline T length(const Quat<T>& q) noexcept
{
    return static_cast<T>(std::sqrt(length_squared(q)));
}

template <MathScalar T> [[nodiscard]] constexpr Quat<T> conjugate(const Quat<T>& q) noexcept
{
    return Quat<T>(-q.x, -q.y, -q.z, q.w);
}

template <MathScalar T> [[nodiscard]] inline bool try_normalize(Quat<T>& q, T epsilon = default_epsilon<T>()) noexcept
{
    const T len = length(q);
    if (len <= epsilon)
    {
        return false;
    }
    q.x /= len;
    q.y /= len;
    q.z /= len;
    q.w /= len;
    return true;
}

template <MathScalar T> [[nodiscard]] inline Quat<T> normalized(Quat<T> q) noexcept
{
    const bool ok = try_normalize(q);
    CRD_ASSERT(ok);
    (void)ok;
    return q;
}

template <MathScalar T>
[[nodiscard]] inline bool try_inverse(const Quat<T>& q, Quat<T>& out, T epsilon = default_epsilon<T>()) noexcept
{
    const T len_sq = length_squared(q);
    if (len_sq <= epsilon)
    {
        return false;
    }

    const Quat<T> conj = conjugate(q);
    out = Quat<T>(conj.x / len_sq, conj.y / len_sq, conj.z / len_sq, conj.w / len_sq);
    return true;
}

template <MathScalar T> [[nodiscard]] inline Quat<T> inversed(const Quat<T>& q) noexcept
{
    Quat<T> out{};
    const bool ok = try_inverse(q, out);
    CRD_ASSERT(ok);
    (void)ok;
    return out;
}

template <MathScalar T> [[nodiscard]] constexpr Quat<T> operator*(const Quat<T>& lhs, const Quat<T>& rhs) noexcept
{
    return Quat<T>(lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
                   lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
                   lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
                   lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z);
}

template <MathScalar T> [[nodiscard]] inline Quat<T> from_axis_angle(const Vec3<T>& axis, T radians) noexcept
{
    Vec3<T> unit_axis = normalized(axis);
    const T half = radians * static_cast<T>(0.5);
    const T s = static_cast<T>(std::sin(half));
    const T c = static_cast<T>(std::cos(half));
    return Quat<T>(unit_axis.x * s, unit_axis.y * s, unit_axis.z * s, c);
}

// Euler-angle ordering convention (ADR-0054 cross-domain robustness).
//
// Tait-Bryan angles applied as a sequence of single-axis rotations. The
// enumerator name reads LEFT-TO-RIGHT as the order of multiplications
// applied to a vector — `XYZ_Intrinsic` means "rotate around X, then
// around the new Y, then around the new Z" (object-frame). Extrinsic
// variants apply rotations about WORLD-FIXED axes.
//
// Aerospace conventions: ZYX_Intrinsic = (yaw, pitch, roll) order.
// Robotics: domain-specific; URDF uses ZYX_Intrinsic for joint frames
// but tool-tip orientation typically uses XYZ_Intrinsic.
// Games: XYZ_Intrinsic is the default Unity / Unreal expectation.
//
// All twelve permutations are enumerated; v1j ships the four most-used
// (XYZ_Intrinsic, ZYX_Intrinsic, XYZ_Extrinsic, ZYX_Extrinsic) and
// reserves the remainder for caller-driven need.
enum class EulerOrder : crd::u8
{
    XYZ_Intrinsic = 0,
    ZYX_Intrinsic = 1,
    XYZ_Extrinsic = 2,
    ZYX_Extrinsic = 3,
};

// Quaternion from three Euler angles in the given order. Each angle is in
// radians. Default order is XYZ_Intrinsic (matches Unity / Unreal).
//
// Implementation: composes three from_axis_angle quaternions in the right
// order. Intrinsic = q = q_first * q_second * q_third (applied to vector
// reads right-to-left). Extrinsic = q = q_third * q_second * q_first.
template <MathScalar T>
[[nodiscard]] inline Quat<T> from_euler(T x, T y, T z, EulerOrder order = EulerOrder::XYZ_Intrinsic) noexcept
{
    const Quat<T> qx = from_axis_angle(Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0)), x);
    const Quat<T> qy = from_axis_angle(Vec3<T>(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0)), y);
    const Quat<T> qz = from_axis_angle(Vec3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)), z);
    switch (order)
    {
        case EulerOrder::XYZ_Intrinsic: return qx * qy * qz;
        case EulerOrder::ZYX_Intrinsic: return qz * qy * qx;
        case EulerOrder::XYZ_Extrinsic: return qz * qy * qx; // extrinsic XYZ == intrinsic ZYX
        case EulerOrder::ZYX_Extrinsic: return qx * qy * qz; // extrinsic ZYX == intrinsic XYZ
    }
    return Quat<T>::identity();
}

// Shortest-arc rotation that rotates direction `from` to direction `to`.
// Both vectors are normalized internally; magnitudes are ignored. When
// `from` and `to` are anti-parallel, picks an arbitrary perpendicular
// axis (preferring world-X, falling back to world-Y when from is along
// X) so the result is well-defined.
template <MathScalar T>
[[nodiscard]] inline Quat<T> from_to_rotation(const Vec3<T>& from, const Vec3<T>& to) noexcept
{
    const Vec3<T> a = normalized(from);
    const Vec3<T> b = normalized(to);
    const T d = dot(a, b);
    // Same direction → identity.
    if (d > static_cast<T>(1) - default_epsilon<T>())
    {
        return Quat<T>::identity();
    }
    // Anti-parallel → 180° rotation around any axis perpendicular to a.
    if (d < static_cast<T>(-1) + default_epsilon<T>())
    {
        // Pick world-X unless `a` is itself along X.
        Vec3<T> axis_seed(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
        if (std::abs(a.x) > static_cast<T>(0.9))
        {
            axis_seed = Vec3<T>(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0));
        }
        const Vec3<T> axis = normalized(cross(a, axis_seed));
        return Quat<T>(axis.x, axis.y, axis.z, static_cast<T>(0));
    }
    const Vec3<T> c = cross(a, b);
    const T s = static_cast<T>(std::sqrt((static_cast<T>(1) + d) * static_cast<T>(2)));
    const T inv_s = static_cast<T>(1) / s;
    return Quat<T>(c.x * inv_s, c.y * inv_s, c.z * inv_s, s * static_cast<T>(0.5));
}

template <MathScalar T> [[nodiscard]] inline Vec3<T> rotate_vector(const Quat<T>& q_in, const Vec3<T>& v) noexcept
{
    const Quat<T> q = normalized(q_in);
    const Vec3<T> qv(q.x, q.y, q.z);
    const Vec3<T> t = static_cast<T>(2) * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

template <MathScalar T> [[nodiscard]] inline Mat3<T> to_mat3(const Quat<T>& q_in) noexcept
{
    const Quat<T> q = normalized(q_in);
    const T xx = q.x * q.x;
    const T yy = q.y * q.y;
    const T zz = q.z * q.z;
    const T xy = q.x * q.y;
    const T xz = q.x * q.z;
    const T yz = q.y * q.z;
    const T wx = q.w * q.x;
    const T wy = q.w * q.y;
    const T wz = q.w * q.z;

    return Mat3<T>(Vec3<T>(static_cast<T>(1) - static_cast<T>(2) * (yy + zz), static_cast<T>(2) * (xy + wz),
                           static_cast<T>(2) * (xz - wy)),
                   Vec3<T>(static_cast<T>(2) * (xy - wz), static_cast<T>(1) - static_cast<T>(2) * (xx + zz),
                           static_cast<T>(2) * (yz + wx)),
                   Vec3<T>(static_cast<T>(2) * (xz + wy), static_cast<T>(2) * (yz - wx),
                           static_cast<T>(1) - static_cast<T>(2) * (xx + yy)));
}

template <MathScalar T> [[nodiscard]] inline Mat4<T> to_mat4(const Quat<T>& q) noexcept
{
    const Mat3<T> m3 = to_mat3(q);
    return Mat4<T>(Vec4<T>(m3.c0, static_cast<T>(0)), Vec4<T>(m3.c1, static_cast<T>(0)),
                   Vec4<T>(m3.c2, static_cast<T>(0)),
                   Vec4<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)));
}

template <MathScalar T> [[nodiscard]] inline Quat<T> from_mat3(const Mat3<T>& m) noexcept
{
    const T trace = m.c0.x + m.c1.y + m.c2.z;
    if (trace > static_cast<T>(0))
    {
        const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(trace + static_cast<T>(1)));
        return normalized(
            Quat<T>((m.c1.z - m.c2.y) / s, (m.c2.x - m.c0.z) / s, (m.c0.y - m.c1.x) / s, static_cast<T>(0.25) * s));
    }
    if (m.c0.x > m.c1.y && m.c0.x > m.c2.z)
    {
        const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(static_cast<T>(1) + m.c0.x - m.c1.y - m.c2.z));
        return normalized(
            Quat<T>(static_cast<T>(0.25) * s, (m.c1.x + m.c0.y) / s, (m.c2.x + m.c0.z) / s, (m.c1.z - m.c2.y) / s));
    }
    if (m.c1.y > m.c2.z)
    {
        const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(static_cast<T>(1) + m.c1.y - m.c0.x - m.c2.z));
        return normalized(
            Quat<T>((m.c1.x + m.c0.y) / s, static_cast<T>(0.25) * s, (m.c2.y + m.c1.z) / s, (m.c2.x - m.c0.z) / s));
    }

    const T s = static_cast<T>(2) * static_cast<T>(std::sqrt(static_cast<T>(1) + m.c2.z - m.c0.x - m.c1.y));
    return normalized(
        Quat<T>((m.c2.x + m.c0.z) / s, (m.c2.y + m.c1.z) / s, static_cast<T>(0.25) * s, (m.c0.y - m.c1.x) / s));
}

template <MathScalar T> [[nodiscard]] inline Quat<T> nlerp(Quat<T> a, Quat<T> b, T t) noexcept
{
    if (dot(a, b) < static_cast<T>(0))
    {
        b = Quat<T>(-b.x, -b.y, -b.z, -b.w);
    }

    return normalized(
        Quat<T>(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
}

template <MathScalar T>
[[nodiscard]] inline Quat<T> slerp(Quat<T> a, Quat<T> b, T t, T epsilon = default_epsilon<T>()) noexcept
{
    T cos_theta = dot(a, b);
    if (cos_theta < static_cast<T>(0))
    {
        b = Quat<T>(-b.x, -b.y, -b.z, -b.w);
        cos_theta = -cos_theta;
    }

    if (cos_theta > static_cast<T>(1) - epsilon)
    {
        return nlerp(a, b, t);
    }

    cos_theta = clamp(cos_theta, static_cast<T>(-1), static_cast<T>(1));
    const T theta = static_cast<T>(std::acos(cos_theta));
    const T sin_theta = static_cast<T>(std::sin(theta));
    const T w0 = static_cast<T>(std::sin((static_cast<T>(1) - t) * theta)) / sin_theta;
    const T w1 = static_cast<T>(std::sin(t * theta)) / sin_theta;
    return Quat<T>(a.x * w0 + b.x * w1, a.y * w0 + b.y * w1, a.z * w0 + b.z * w1, a.w * w0 + b.w * w1);
}

// Quaternion logarithm of a UNIT quaternion q = (v, w): log(q) = (θ·v̂, 0), a PURE quaternion where θ = atan2(|v|, w)
// is the half-rotation-angle and v̂ = v/|v|. The manifold-correct tangent used by SQUAD / quaternion-B-spline control
// points. Small-angle safe (|v|→0 ⇒ log→(v,0)).
template <MathScalar T> [[nodiscard]] inline Quat<T> quat_log(const Quat<T>& q, T epsilon = default_epsilon<T>()) noexcept
{
    const T vlen = static_cast<T>(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z));
    if (vlen < epsilon)
    {
        return Quat<T>(q.x, q.y, q.z, static_cast<T>(0)); // small angle: log ≈ v
    }
    const T theta = static_cast<T>(std::atan2(vlen, q.w));
    const T k     = theta / vlen;
    return Quat<T>(q.x * k, q.y * k, q.z * k, static_cast<T>(0));
}

// Quaternion exponential of a PURE quaternion q = (v, 0): exp(q) = (sin|v|·v̂, cos|v|), a UNIT quaternion. Inverse of
// quat_log. Small-angle safe (|v|→0 ⇒ exp→(v, 1)).
template <MathScalar T> [[nodiscard]] inline Quat<T> quat_exp(const Quat<T>& q, T epsilon = default_epsilon<T>()) noexcept
{
    const T vlen = static_cast<T>(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z));
    if (vlen < epsilon)
    {
        return Quat<T>(q.x, q.y, q.z, static_cast<T>(1)); // small angle: exp ≈ (v, 1)
    }
    const T s = static_cast<T>(std::sin(vlen)) / vlen;
    return Quat<T>(q.x * s, q.y * s, q.z * s, static_cast<T>(std::cos(vlen)));
}

// ---- TRS Mat4 build / decompose (lives here because it bridges Quat + Mat) ---
//
// from_trs / to_trs encode the canonical column-major M = T * R * S.

// Build a TRS Mat4 from translation + quaternion rotation + per-axis scale.
template <MathScalar T>
[[nodiscard]] inline Mat4<T> from_trs(const Vec3<T>& translation, const Quat<T>& rotation,
                                      const Vec3<T>& scale) noexcept
{
    const Mat3<T> r = to_mat3(rotation);
    Mat4<T>       m;
    m.c0 = Vec4<T>(r.c0 * scale.x, static_cast<T>(0));
    m.c1 = Vec4<T>(r.c1 * scale.y, static_cast<T>(0));
    m.c2 = Vec4<T>(r.c2 * scale.z, static_cast<T>(0));
    m.c3 = Vec4<T>(translation, static_cast<T>(1));
    return m;
}

// Convenience overload: accepts a dimensional translation
// `Vec3<crd::units::Quantity<dim::Length, T>>` and strips the unit at the
// boundary. Lets scene::Transform-shaped consumers call from_trs without
// each having to spell `to_raw_vec(translation)` inline (Phase 3.1.7.5 v0b-3).
template <typename D, typename T>
[[nodiscard]] inline Mat4<T> from_trs(const Vec3<crd::units::Quantity<D, T>>& translation,
                                      const Quat<T>& rotation,
                                      const Vec3<T>& scale) noexcept
{
    return from_trs(to_raw_vec(translation), rotation, scale);
}

// Decompose a TRS Mat4 into translation + rotation quat + per-axis scale.
//
// Returns false ONLY when any column has near-zero length (singular). In
// that case the outputs are left UNCHANGED — callers' prior values are
// preserved.
//
// Negative-determinant matrices succeed with the X-scale axis negated
// (mirror-handedness preserved). This is the standard CAD/robotics-URDF
// convention; documented per v1j cross-domain pin (advisor decision #4).
// Callers needing "all positive scale" check `s_out.x * s_out.y * s_out.z
// > 0` after the call.
//
// Skewed matrices (columns non-orthogonal after scale-removal) decompose
// best-effort via from_mat3; the recovered rotation loses the skew.
// True skew handling via polar decomposition is reserved as a v1j+1
// follow-up — see docs/debt.md.
template <MathScalar T>
[[nodiscard]] inline bool to_trs(const Mat4<T>& m, Vec3<T>& t_out, Quat<T>& r_out, Vec3<T>& s_out,
                                 T epsilon = default_epsilon<T>()) noexcept
{
    const Vec3<T> col0(m.c0.x, m.c0.y, m.c0.z);
    const Vec3<T> col1(m.c1.x, m.c1.y, m.c1.z);
    const Vec3<T> col2(m.c2.x, m.c2.y, m.c2.z);

    T       sx = static_cast<T>(std::sqrt(dot(col0, col0)));
    const T sy = static_cast<T>(std::sqrt(dot(col1, col1)));
    const T sz = static_cast<T>(std::sqrt(dot(col2, col2)));

    if (sx <= epsilon || sy <= epsilon || sz <= epsilon)
    {
        return false;
    }

    const Vec3<T> rcol0_raw = col0 * (static_cast<T>(1) / sx);
    const Vec3<T> rcol1_raw = col1 * (static_cast<T>(1) / sy);
    const Vec3<T> rcol2_raw = col2 * (static_cast<T>(1) / sz);
    const T       det_check = dot(rcol0_raw, cross(rcol1_raw, rcol2_raw));

    Vec3<T> rcol0 = rcol0_raw;
    if (det_check < static_cast<T>(0))
    {
        sx    = -sx;
        rcol0 = -rcol0;
    }

    const Mat3<T> rot_mat(rcol0, rcol1_raw, rcol2_raw);
    t_out = Vec3<T>(m.c3.x, m.c3.y, m.c3.z);
    r_out = from_mat3(rot_mat);
    s_out = Vec3<T>(sx, sy, sz);
    return true;
}

using Quatf = Quat<crd::f32>;
using Quatd = Quat<crd::f64>;
} // namespace crd::math
