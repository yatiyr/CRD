#include <crd/hesap/net/dead_reckoning.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/transcendental.hpp>

namespace crd::hesap::net
{

OrientationXyzw orientation_from_dis_euler(const DisEuler& euler) noexcept
{
    crd::f64 s_psi = 0.0;
    crd::f64 c_psi = 0.0;
    crd::f64 s_theta = 0.0;
    crd::f64 c_theta = 0.0;
    crd::f64 s_phi = 0.0;
    crd::f64 c_phi = 0.0;
    crd::math::sincos(euler.psi * 0.5, s_psi, c_psi);
    crd::math::sincos(euler.theta * 0.5, s_theta, c_theta);
    crd::math::sincos(euler.phi * 0.5, s_phi, c_phi);
    return {
        (c_psi * c_theta * s_phi) - (s_psi * s_theta * c_phi), (c_psi * s_theta * c_phi) + (s_psi * c_theta * s_phi),
        (s_psi * c_theta * c_phi) - (c_psi * s_theta * s_phi), (c_psi * c_theta * c_phi) + (s_psi * s_theta * s_phi)};
}

DisEuler dis_euler_from_orientation(const OrientationXyzw& q) noexcept
{
    // The first row of world to body is (cos theta cos psi, cos theta sin psi, -sin theta).
    const crd::f64 cos_cos = 1.0 - (2.0 * ((q.y * q.y) + (q.z * q.z)));
    const crd::f64 cos_sin = 2.0 * ((q.x * q.y) + (q.w * q.z));
    const crd::f64 sine = 2.0 * ((q.w * q.y) - (q.x * q.z));
    const crd::f64 cosine = crd::math::libm::sqrt((cos_cos * cos_cos) + (cos_sin * cos_sin));
    const crd::f64 theta = crd::math::atan2(sine, cosine);
    // Below this cos(theta) the first row's direction and the last column's are rounding alone.
    constexpr crd::f64 gimbal = 4e-8;
    if (cosine < gimbal)
    {
        return {crd::math::atan2(2.0 * ((q.w * q.z) - (q.x * q.y)), 1.0 - (2.0 * ((q.x * q.x) + (q.z * q.z)))), theta,
                0.0};
    }
    return {crd::math::atan2(cos_sin, cos_cos), theta,
            crd::math::atan2(2.0 * ((q.y * q.z) + (q.w * q.x)), 1.0 - (2.0 * ((q.x * q.x) + (q.y * q.y))))};
}

} // namespace crd::hesap::net
