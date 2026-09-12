// pose.cpp — GEO-8 (D-007 row 73): glTF-spec-exact clip sampling + pose composition + palettes. See pose.hpp.

#include <crd/anim/pose.hpp>

#include <crd/hesap/interp/keyframe.hpp>

namespace crd::anim
{

namespace
{

namespace hi = crd::hesap::interp;

// LINEAR rotation = SLERP between the bracketing keys (glTF's rule); Step holds; Cubic is component-wise
// Hermite then normalize (also glTF's rule — sampled per lane through the shared keyframe engine).
[[nodiscard]] crd::math::Quatf sample_rotation(crd::containers::ConstSpan<crd::f32> times,
                                               crd::containers::ConstSpan<crd::f32> values, hi::KeyInterp interp,
                                               crd::f32 t) noexcept
{
    const crd::usize n = times.size();
    const auto       key_quat = [&](crd::usize k) -> crd::math::Quatf {
        const crd::usize span = static_cast<crd::usize>(hi::key_elements(interp)) * 4U;
        const crd::usize base = k * span + (interp == hi::KeyInterp::CubicHermite ? 4U : 0U);
        return {values[base + 0U], values[base + 1U], values[base + 2U], values[base + 3U]};
    };
    if (n == 1U || t <= times[0]) { return crd::math::normalized(key_quat(t <= times[0] ? 0U : n - 1U)); }
    if (t >= times[n - 1U]) { return crd::math::normalized(key_quat(n - 1U)); }

    if (interp == hi::KeyInterp::CubicHermite)
    {
        crd::usize       cache = 0;
        crd::math::Quatf q;
        q.x = hi::sample_track(times, values, 4U, 0U, interp, t, cache);
        q.y = hi::sample_track(times, values, 4U, 1U, interp, t, cache);
        q.z = hi::sample_track(times, values, 4U, 2U, interp, t, cache);
        q.w = hi::sample_track(times, values, 4U, 3U, interp, t, cache);
        return crd::math::normalized(q);
    }

    crd::usize       cache = 0;
    const crd::usize i     = hi::find_segment(times, t, cache);
    if (interp == hi::KeyInterp::Step) { return crd::math::normalized(key_quat(i)); }
    const crd::f32 u = (t - times[i]) / (times[i + 1U] - times[i]);
    return crd::math::slerp(crd::math::normalized(key_quat(i)), crd::math::normalized(key_quat(i + 1U)), u);
}

} // namespace

void sample_clip(const AnimClipResource& clip, const SkeletonResource& skeleton, crd::f32 t,
                 crd::containers::Span<JointPose> out_poses, crd::containers::Span<crd::f32> out_floats) noexcept
{
    const crd::u32 n = skeleton.joint_count();
    if (out_poses.size() < n) { return; }

    // rest pose first — untracked joints (and untracked channels of tracked joints) hold it
    for (crd::u32 j = 0; j < n; ++j)
    {
        const crd::f32* r        = skeleton.rest.data() + static_cast<crd::usize>(j) * kRestFloats;
        out_poses[j].translation = {r[0], r[1], r[2]};
        out_poses[j].rotation    = {r[3], r[4], r[5], r[6]};
        out_poses[j].scale       = {r[7], r[8], r[9]};
    }

    crd::usize float_cursor = 0;
    for (const AnimTrack& track : clip.tracks)
    {
        const auto times  = clip.track_times(track);
        const auto values = clip.track_values(track);
        const auto interp = static_cast<hi::KeyInterp>(track.interp);

        if (track.target == kFreeTrack || static_cast<AnimChannel>(track.channel) == AnimChannel::Float)
        {
            if (out_floats.size() >= float_cursor + track.components)
            {
                crd::usize cache = 0;
                for (crd::u16 c = 0; c < track.components; ++c)
                {
                    out_floats[float_cursor + c] = hi::sample_track(times, values, track.components,
                                                                    static_cast<crd::u32>(c), interp, t, cache);
                }
            }
            float_cursor += track.components;
            continue;
        }
        if (track.target >= n) { continue; } // a foreign track never writes out of range

        JointPose& pose = out_poses[track.target];
        switch (static_cast<AnimChannel>(track.channel))
        {
            case AnimChannel::Translation:
            {
                crd::usize cache = 0;
                pose.translation = {hi::sample_track(times, values, 3U, 0U, interp, t, cache),
                                    hi::sample_track(times, values, 3U, 1U, interp, t, cache),
                                    hi::sample_track(times, values, 3U, 2U, interp, t, cache)};
                break;
            }
            case AnimChannel::Rotation: pose.rotation = sample_rotation(times, values, interp, t); break;
            case AnimChannel::Scale:
            {
                crd::usize cache = 0;
                pose.scale = {hi::sample_track(times, values, 3U, 0U, interp, t, cache),
                              hi::sample_track(times, values, 3U, 1U, interp, t, cache),
                              hi::sample_track(times, values, 3U, 2U, interp, t, cache)};
                break;
            }
            case AnimChannel::Float:
            default: break;
        }
    }
}

void compute_pose_matrices(const SkeletonResource& skeleton, crd::containers::ConstSpan<JointPose> poses,
                           crd::containers::Span<crd::math::Mat4f> out_world) noexcept
{
    const crd::u32 n = skeleton.joint_count();
    if (poses.size() < n || out_world.size() < n) { return; }
    for (crd::u32 j = 0; j < n; ++j) // parents[j] < j — one forward pass, the cook's topological contract
    {
        const crd::math::Mat4f local =
            crd::math::from_trs(poses[j].translation, poses[j].rotation, poses[j].scale);
        const crd::i32 parent = skeleton.parents[j];
        out_world[j]          = parent < 0 ? local : out_world[static_cast<crd::u32>(parent)] * local;
    }
}

void compute_skin_palette(const SkeletonResource& skeleton, crd::containers::ConstSpan<crd::math::Mat4f> world,
                          crd::containers::Span<crd::math::Mat4f> out_palette) noexcept
{
    const crd::u32 n = skeleton.joint_count();
    if (world.size() < n || out_palette.size() < n) { return; }
    for (crd::u32 j = 0; j < n; ++j)
    {
        crd::math::Mat4f ibm;
        const crd::f32*  m = skeleton.inverse_binds.data() + static_cast<crd::usize>(j) * 16U;
        ibm.c0             = {m[0], m[1], m[2], m[3]};
        ibm.c1             = {m[4], m[5], m[6], m[7]};
        ibm.c2             = {m[8], m[9], m[10], m[11]};
        ibm.c3             = {m[12], m[13], m[14], m[15]};
        out_palette[j]     = world[j] * ibm;
    }
}

void palette_to_dual_quats(crd::containers::ConstSpan<crd::math::Mat4f> palette,
                           crd::containers::Span<DualQuat> out) noexcept
{
    const crd::usize n = palette.size() < out.size() ? palette.size() : out.size();
    for (crd::usize i = 0; i < n; ++i)
    {
        const crd::math::Mat4f& m = palette[i];
        crd::math::Mat3f        r3;
        r3.c0 = {m.c0.x, m.c0.y, m.c0.z};
        r3.c1 = {m.c1.x, m.c1.y, m.c1.z};
        r3.c2 = {m.c2.x, m.c2.y, m.c2.z};
        const crd::math::Quatf q = crd::math::normalized(crd::math::from_mat3(r3));
        // dual = ½ · (t as a pure quat) ⊗ real  (Kavan 2007)
        const crd::math::Quatf tq{m.c3.x, m.c3.y, m.c3.z, 0.0F};
        const crd::math::Quatf d = tq * q;
        out[i].real              = {q.x, q.y, q.z, q.w};
        out[i].dual              = {0.5F * d.x, 0.5F * d.y, 0.5F * d.z, 0.5F * d.w};
    }
}

crd::math::Vec3f dual_quat_transform(const DualQuat& dq, const crd::math::Vec3f& p) noexcept
{
    // p' = p + 2·rxyz×(rxyz×p + rw·p) + 2·(rw·dxyz − dw·rxyz + rxyz×dxyz) — the B8-j GPU formula, verbatim
    const crd::math::Vec3f rxyz{dq.real.x, dq.real.y, dq.real.z};
    const crd::f32         rw = dq.real.w;
    const crd::math::Vec3f dxyz{dq.dual.x, dq.dual.y, dq.dual.z};
    const crd::f32         dw    = dq.dual.w;
    const crd::math::Vec3f inner = crd::math::cross(rxyz, p) + p * rw;
    const crd::math::Vec3f rot   = p + crd::math::cross(rxyz, inner) * 2.0F;
    const crd::math::Vec3f trans = (dxyz * rw - rxyz * dw + crd::math::cross(rxyz, dxyz)) * 2.0F;
    return rot + trans;
}

} // namespace crd::anim
