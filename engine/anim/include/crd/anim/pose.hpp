#pragma once

// pose.hpp — GEO-8 (D-007 row 73): clip sampling + pose composition + the skinning palette. glTF-spec-EXACT
// sampling semantics (the bit-stable-resample contract):
//   · scalar lanes (T/S components, float tracks, cubic rotation components) sample through the hesap-interp
//     keyframe engine (`sample_track` — scipy-parity Hermite, deterministic FMUL/FADD);
//   · LINEAR rotation is SLERP between the bracketing keys (shortest-path — the spec's requirement; a component
//     lerp would cut corners);
//   · CUBICSPLINE rotation is component-wise Hermite THEN normalize (the spec's exact rule);
//   · out-of-range time clamps to the boundary key; untracked joints hold the skeleton's REST pose.
//
// Composition is one forward pass (the cook pins topological joint order, parents[i] < i): world[i] =
// world[parent] · local(i); the skin palette is world · inverse_bind — at bind pose the palette is IDENTITY
// (the classic correctness gate). LBS consumes the matrix palette (B8-j's affine-blend formulation); the
// dual-quaternion conversion feeds B8-j's DQS path (Kavan 2007 — volume-preserving twist).

#include <crd/anim/anim_resources.hpp>
#include <crd/containers/span.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/vec.hpp>

namespace crd::anim
{

struct JointPose
{
    crd::math::Vec3f translation{0.0F, 0.0F, 0.0F};
    crd::math::Quatf rotation = crd::math::Quatf::identity();
    crd::math::Vec3f scale{1.0F, 1.0F, 1.0F};
};

// Sample `clip` at time `t` (seconds, clamped to [0, duration]) into `out_poses` (size == joint_count),
// starting from the skeleton's rest pose. Free float tracks (target == kFreeTrack) are sampled into
// `out_floats` when provided (lane-flattened in track order), else skipped.
void sample_clip(const AnimClipResource& clip, const SkeletonResource& skeleton, crd::f32 t,
                 crd::containers::Span<JointPose> out_poses,
                 crd::containers::Span<crd::f32>  out_floats = {}) noexcept;

// world[i] = world[parents[i]] · from_trs(pose[i])  (single forward pass — topological order is the contract)
void compute_pose_matrices(const SkeletonResource& skeleton, crd::containers::ConstSpan<JointPose> poses,
                           crd::containers::Span<crd::math::Mat4f> out_world) noexcept;

// palette[i] = world[i] · inverse_bind[i] — the LBS bone palette (identity at bind pose)
void compute_skin_palette(const SkeletonResource& skeleton, crd::containers::ConstSpan<crd::math::Mat4f> world,
                          crd::containers::Span<crd::math::Mat4f> out_palette) noexcept;

// The dual-quaternion form of a RIGID palette entry (Kavan 2007): real = rotation quat, dual = ½·t⊗real.
// Scale is not representable — DQS consumers pre-normalize or split scale (the standard practice).
struct DualQuat
{
    crd::math::Vec4f real{0.0F, 0.0F, 0.0F, 1.0F}; // xyzw
    crd::math::Vec4f dual{0.0F, 0.0F, 0.0F, 0.0F};
};

void palette_to_dual_quats(crd::containers::ConstSpan<crd::math::Mat4f> palette,
                           crd::containers::Span<DualQuat>              out) noexcept;

// Transform a point by a dual quat (the B8-j GPU formula's CPU oracle — tests pin GPU/CPU agreement).
[[nodiscard]] crd::math::Vec3f dual_quat_transform(const DualQuat& dq, const crd::math::Vec3f& p) noexcept;

} // namespace crd::anim
