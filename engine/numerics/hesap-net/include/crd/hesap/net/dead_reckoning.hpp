// Predicting an entity from its last update (HESAP-NET, capability numerics.motion-sampling, owner crd-hesap-net): the
// nine dead-reckoning algorithms of IEEE 1278.1 and Murphy's projective velocity blending. The batches run dispatched;
// nothing allocates. What goes on the wire, when an update is due and which entity uses which algorithm belong to
// crd-net and its callers.
//
// An entity's columns: position in world coordinates; velocity and acceleration in world coordinates for the W
// algorithms and in body coordinates for the B ones; rate, the angular velocity in body coordinates in radians per
// second (as DIS sends it for every algorithm); orientation, the unit quaternion x y z w taking body to world; dt, this
// entity's seconds since its update, which may be negative. An entity's update times differ, so each has its own dt.
//
//   Algorithm  Position                                  Writes                Reads
//   Static     unchanged                                 position              position
//   Fpw        p + v dt                                  position              + velocity, dt
//   Rpw        p + v dt, turning                         position, orientation + orientation, rate
//   Rvw        p + v dt + a dt^2 / 2, turning            + velocity
//   Fvw        p + v dt + a dt^2 / 2                     position, velocity    + acceleration
//   Fpb        p + q(v dt)                               position              + orientation
//   Rpb        p + q(R1 v), turning                      position, orientation + rate
//   Rvb        p + q(R1 v + R2 a), turning               + velocity
//   Fvb        p + q(v dt + a dt^2 / 2)                  position, velocity
//
// A column an algorithm does not read or write may be null. Turning is q * (the rotation by |rate| dt about rate), the
// quaternion of the standard's DR * (world to body) matrix. A velocity written is v + a dt, in the frame it came in.
// Output columns may be the input columns exactly; any other overlap is the caller's error.
//
// R1 and R2 are the integrals over the span of the rotation, and of the rotation times the time since the update, so an
// entity holding its body velocity and body acceleration a follows the true curve: a is the rate of change of the body
// velocity's components, d(v_body)/dt. The closed forms cancel for a slow turn, so up to |rate dt| = 1 their Taylor
// series answers instead and the result keeps its precision as the rate goes to zero. Fixed algorithms ignore the rate.
//
// Every level gives the same bits. Finite inputs give finite outputs while their products stay finite; a rate too large
// for |rate dt|^2 to be finite, or not finite, is taken as no rotation, and a position, velocity or orientation that
// is not finite stays so.
//
// projective_blend moves one column of each entity from the path it was on when an update arrived (`from_position`,
// `from_velocity`, its values at that moment) onto the updated path (`position`, `velocity`, `acceleration`, the
// update's values, which may be null for none), at `elapsed` seconds of a `span` the caller expects until the next
// update (Curtiss Murphy, "Believable Dead Reckoning for Networked Games", Game Engine Gems 2, 2011):
//
//   f = elapsed / span cut to [0, 1]; a span that is not positive gives f = 1
//   blended = from_velocity + (velocity - from_velocity) f
//   out     = lerp(from_position + blended elapsed + acceleration elapsed^2 / 2,
//                  position + velocity elapsed + acceleration elapsed^2 / 2, f)
//
// It writes that position and its derivative with respect to elapsed, so the next update's blend starts from where and
// how fast the entity really moves: that derivative is the only velocity the blend has, and past the span it is the
// updated path's own. The derivative steps by (position - from_position) / span when f reaches 1, which is the method's
// own shape. Outputs may be inputs exactly.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/net/motion_dispatch.hpp>
#include <crd/hesap/net/motion_kernels.hpp>

namespace crd::hesap::net
{

// True for the numbers the Dead Reckoning Algorithm field assigns a motion: 1 to 9. 0 is "other", and any other number
// is a private or unknown one.
[[nodiscard]] constexpr bool dead_reckoning_algorithm_known(crd::u8 number) noexcept
{
    return number >= static_cast<crd::u8>(DeadReckoningAlgorithm::Static) &&
           number <= static_cast<crd::u8>(DeadReckoningAlgorithm::Fvb);
}

inline void dead_reckon(DeadReckoningAlgorithm algorithm, const DeadReckoningInput& in, crd::usize count,
                        const DeadReckoningOutput& out) noexcept
{
    motion_kernels().table->dead_reckon(algorithm, in, count, out);
}

inline void projective_blend(const ProjectiveBlendSpan<crd::f32>& span, crd::usize count, crd::f32* position,
                             crd::f32* velocity) noexcept
{
    motion_kernels().table->projective_blend_f32(span, count, position, velocity);
}

inline void projective_blend(const ProjectiveBlendSpan<crd::f64>& span, crd::usize count, crd::f64* position,
                             crd::f64* velocity) noexcept
{
    motion_kernels().table->projective_blend_f64(span, count, position, velocity);
}

// DIS's Euler angles, a wire form of orientation: psi about z, then theta about the new y, then phi about the new x, in
// radians, taking world to body. The quaternion takes body to world.
struct DisEuler
{
    crd::f64 psi;
    crd::f64 theta;
    crd::f64 phi;
};

struct OrientationXyzw
{
    crd::f64 x;
    crd::f64 y;
    crd::f64 z;
    crd::f64 w;
};

[[nodiscard]] OrientationXyzw orientation_from_dis_euler(const DisEuler& euler) noexcept;

// theta in [-pi/2, pi/2], psi and phi in [-pi, pi]. Pitched straight up or down only psi - phi or psi + phi has a
// meaning, so phi is 0 and psi carries it. The quaternion is taken to be of unit length.
[[nodiscard]] DisEuler dis_euler_from_orientation(const OrientationXyzw& q) noexcept;

} // namespace crd::hesap::net
