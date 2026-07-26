// csm.cpp — REN-3.2-b: stabilized cascade fitting. See csm.hpp for why stabilization IS the problem.

#include <crd/scenerender/csm.hpp>

#include <crd/math/mat.hpp>
#include <crd/math/cmath.hpp>   // floor / sqrt / abs
#include <crd/math/power.hpp>   // pow (the practical-split log term)

namespace crd::scenerender
{
namespace
{

// A right-handed orthographic projection mapping z to [0,1] (the Vulkan/DX depth range this engine targets).
// View space looks down -Z, so a point at view-space z = -n lands on depth 0 and z = -f on depth 1.
[[nodiscard]] crd::math::Mat4f ortho_rh_zo(float l, float r, float b, float t, float n, float f) noexcept
{
    crd::math::Mat4f m = crd::math::Mat4f::identity();
    m.c0.x = 2.0F / (r - l);
    m.c1.y = 2.0F / (t - b);
    m.c2.z = -1.0F / (f - n);
    m.c3.x = -(r + l) / (r - l);
    m.c3.y = -(t + b) / (t - b);
    m.c3.z = -n / (f - n);
    return m;
}

[[nodiscard]] crd::math::Vec3f xform_point(const crd::math::Mat4f& m, const crd::math::Vec3f& p) noexcept
{
    return {m.c0.x * p.x + m.c1.x * p.y + m.c2.x * p.z + m.c3.x,
            m.c0.y * p.x + m.c1.y * p.y + m.c2.y * p.z + m.c3.y,
            m.c0.z * p.x + m.c1.z * p.y + m.c2.z * p.z + m.c3.z};
}

} // namespace

float csm_split_practical_cpu(float near_p, float far_p, float lambda, crd::u32 i, crd::u32 count)
{
    if (count == 0U) { return far_p; }
    const float si = static_cast<float>(i + 1U) / static_cast<float>(count);
    // near*(far/near)^si  —  the logarithmic (perspective-correct) term
    const float lg = near_p * crd::math::pow(far_p / near_p, si);
    // near + (far-near)*si  —  the uniform term
    const float un = near_p + (far_p - near_p) * si;
    return lambda * lg + (1.0F - lambda) * un;
}

CsmCascades compute_csm_cascades(const crd::math::Mat4f& view, const crd::math::Mat4f& proj,
                                 const crd::math::Vec3f& light_dir, const CsmConfig& cfg)
{
    CsmCascades out;
    // clamped, never truncated silently — a caller asking for 8 cascades gets 4 and can observe it in `count`
    crd::u32 want = cfg.cascade_count;
    if (want < 1U) { want = 1U; }
    if (want > kMaxCascades) { want = kMaxCascades; }
    out.count = want;
    const crd::u32 map_size = cfg.map_size < 1U ? 1U : cfg.map_size;

    // ── the camera's half-extents at unit view distance, straight out of the projection ─────────────────────
    // proj.c0.x = 1/(aspect*tan(fovy/2)) and |proj.c1.y| = 1/tan(fovy/2) for ANY z convention, so reading the
    // frustum shape this way is immune to reverse-Z / infinite-far / clip-space Y flips. Reconstructing corners
    // from inverse(proj*view) instead would silently depend on all three.
    const float px = crd::math::abs(proj.c0.x);
    const float py = crd::math::abs(proj.c1.y);
    const float tx = px > 1.0e-6F ? 1.0F / px : 1.0F; // aspect * tan(fovy/2)
    const float ty = py > 1.0e-6F ? 1.0F / py : 1.0F; // tan(fovy/2)
    const float k2 = tx * tx + ty * ty;

    // light basis: normalize, and pick an up hint that is never parallel to it
    crd::math::Vec3f ld = light_dir;
    const float      ll = crd::math::sqrt(ld.x * ld.x + ld.y * ld.y + ld.z * ld.z);
    if (ll < 1.0e-6F) { ld = {0.0F, -1.0F, 0.0F}; } // degenerate input -> straight down, never NaN
    else { ld = {ld.x / ll, ld.y / ll, ld.z / ll}; }
    const crd::math::Vec3f up_hint =
        crd::math::abs(ld.y) > 0.99F ? crd::math::Vec3f{0.0F, 0.0F, 1.0F} : crd::math::Vec3f{0.0F, 1.0F, 0.0F};

    const crd::math::Mat4f inv_view = crd::math::inverse(view);

    float near_i = cfg.near_plane;
    for (crd::u32 i = 0; i < out.count; ++i)
    {
        const float far_i = csm_split_practical_cpu(cfg.near_plane, cfg.far_plane, cfg.lambda, i, out.count);
        out.split_far[i]  = far_i;

        // ── STABILIZATION 1: the ANALYTIC bounding sphere of this frustum slice. ────────────────────────────
        // Solving |corner(near)-c| == |corner(far)-c| for a centre `c` on the view axis gives
        //     c = (near + far)(k^2 + 1) / 2,   k^2 = tx^2 + ty^2
        // which depends ONLY on the split distances and the FOV — not on where the camera is or which way it
        // points. That is exactly the invariance a tight per-frame box fit destroys, and the reason a rotating
        // camera does not resize the cascade. When the centre would fall beyond the far plane the far corners
        // dominate, so it is clamped there.
        float centre_dist = (near_i + far_i) * (k2 + 1.0F) * 0.5F;
        if (centre_dist > far_i) { centre_dist = far_i; }
        const float dx     = tx * far_i;
        const float dy     = ty * far_i;
        const float ddz    = far_i - centre_dist;
        float       radius = crd::math::sqrt(dx * dx + dy * dy + ddz * ddz);
        // near corners can be the wider pair for a clamped centre - take whichever is larger
        const float nx      = tx * near_i;
        const float ny      = ty * near_i;
        const float ndz     = near_i - centre_dist;
        const float r_near  = crd::math::sqrt(nx * nx + ny * ny + ndz * ndz);
        if (r_near > radius) { radius = r_near; }
        if (radius < 1.0e-4F) { radius = 1.0e-4F; } // never a zero-extent ortho

        // the slice's sphere centre in WORLD space (view space looks down -Z)
        const crd::math::Vec3f centre_ws = xform_point(inv_view, {0.0F, 0.0F, -centre_dist});

        // ── STABILIZATION 2: TEXEL SNAP (Valient). ─────────────────────────────────────────────────────────
        // Build the light view first, express the centre in LIGHT space, then quantize x/y to whole shadow
        // texels. The ortho extent is already rotation-invariant (step 1), so quantizing the centre makes the
        // projection move only in exact texel increments — a world point keeps landing on the SAME texel until
        // it crosses a full one, which is what removes crawling under camera translation.
        // ⛔ Snapping in world space instead would be wrong: the grid must be aligned to the LIGHT's axes,
        // which is where the shadow map's texels actually live.
        const float texel = (2.0F * radius) / static_cast<float>(map_size);

        // ⛔ The light view must be ORIENTATION-ONLY, anchored at the world origin — NOT aimed at the cascade
        // centre. Building it with `look_at(centre - ld*d, centre, up)` places the centre at light-space (0,0)
        // BY CONSTRUCTION, so there is nothing left to quantize and the snap silently does nothing. The first
        // run of this gate failed exactly that way: sub-texel phase swept the full [0,1) range (0.99 spread)
        // while the code "had" a snap. A fixed light basis is what gives the centre real coordinates to round.
        // ⛔ The shadow camera looks along the direction light TRAVELS, which is -light_dir: `light_dir` points
        // FROM the surface TOWARD the light (the convention the forward FS uses for dot(N, L)). Aiming it at
        // +light_dir points the camera at the sky, inverting the depth ordering so the LOWEST surface is treated
        // as nearest the light — every shadow then comes from the wrong side and the light appears to be
        // nowhere in particular. This was the first CSM bug the sandbox exposed.
        const crd::math::Vec3f fwd = {-ld.x, -ld.y, -ld.z};
        const crd::math::Mat4f view0 = crd::math::look_at(crd::math::Vec3f{0.0F, 0.0F, 0.0F}, fwd, up_hint);
        const crd::math::Vec3f c_ls  = xform_point(view0, centre_ws);
        const float            snap_x = crd::math::floor(c_ls.x / texel + 0.5F) * texel;
        const float            snap_y = crd::math::floor(c_ls.y / texel + 0.5F) * texel;

        // The ortho window is centred on the SNAPPED position, so it can only ever slide in whole texels. Depth
        // spans the sphere, pulled back by `caster_extrusion` so geometry behind the slice still casts into it.
        const float cz = -c_ls.z; // distance along the light's forward axis
        const crd::math::Mat4f ortho = ortho_rh_zo(snap_x - radius, snap_x + radius, snap_y - radius,
                                                   snap_y + radius, cz - radius - cfg.caster_extrusion,
                                                   cz + radius);

        out.light_vp[i]    = ortho * view0;
        out.texel_world[i] = texel;
        near_i             = far_i;
    }
    return out;
}

namespace
{
// The camera's VIEW matrix + the x/y projection scales, recovered EXACTLY from a combined view_proj. Factored out
// because REN-37.3 needs the camera POSITION from the same reconstruction the cascade fit uses — two independent
// derivations of "where is the camera" would be two things to keep in agreement, and this repo has already paid
// for that once (the shadow camera aimed at the sky).
struct RecoveredCamera
{
    crd::math::Mat4f view = crd::math::Mat4f::identity();
    float            sx   = 1.0F; // |proj.c0.x|
    float            sy   = 1.0F; // |proj.c1.y|
};

[[nodiscard]] RecoveredCamera recover_camera(const crd::math::Mat4f& view_proj)
{
    // Recover the pieces the fit actually needs, without fabricating a view/proj split:
    //  · the projection's x/y half-extent scales are the lengths of view_proj's first two ROWS restricted to the
    //    rotation part — because view_proj = P*V and V's upper 3x3 is ORTHONORMAL, which preserves row length.
    //    So |row0.xyz| = |P.c0.x| and |row1.xyz| = |P.c1.y|, exactly.
    //  · the inverse VIEW is then inverse(view_proj) composed with that projection; but the fit only uses
    //    inverse(view) to place a point on the view axis, and inverse(view_proj) maps NDC -> world directly, so
    //    the camera position and forward axis are read straight out of it.
    const float r0 = crd::math::sqrt(view_proj.c0.x * view_proj.c0.x + view_proj.c1.x * view_proj.c1.x
                                     + view_proj.c2.x * view_proj.c2.x);
    const float r1 = crd::math::sqrt(view_proj.c0.y * view_proj.c0.y + view_proj.c1.y * view_proj.c1.y
                                     + view_proj.c2.y * view_proj.c2.y);

    // The caller rebuilds a minimal equivalent projection carrying just those scales (the fit reads ONLY
    // proj.c0.x / proj.c1.y), and this recovers a view whose rotation/translation match the real camera — so the
    // pair reproduces the original exactly rather than approximating it.
    //
    // view = P^-1 * (P*V) with our reconstructed P (diagonal in x/y, identity elsewhere): dividing the first two
    // rows by their scales leaves the orthonormal view rows, and the z/w rows come along unchanged.
    crd::math::Mat4f view = view_proj;
    if (r0 > 1.0e-9F)
    {
        view.c0.x /= r0;
        view.c1.x /= r0;
        view.c2.x /= r0;
        view.c3.x /= r0;
    }
    if (r1 > 1.0e-9F)
    {
        view.c0.y /= r1;
        view.c1.y /= r1;
        view.c2.y /= r1;
        view.c3.y /= r1;
    }
    // the z row of a perspective matrix is not the view's — restore the affine bottom rows
    view.c0.z = -(view_proj.c0.w);
    view.c1.z = -(view_proj.c1.w);
    view.c2.z = -(view_proj.c2.w);
    view.c3.z = -(view_proj.c3.w);
    view.c0.w = 0.0F;
    view.c1.w = 0.0F;
    view.c2.w = 0.0F;
    view.c3.w = 1.0F;

    RecoveredCamera out;
    out.view = view;
    out.sx   = r0;
    out.sy   = r1;
    return out;
}
} // namespace

CsmCascades compute_csm_cascades_from_vp(const crd::math::Mat4f& view_proj, const crd::math::Vec3f& light_dir,
                                         const CsmConfig& cfg)
{
    const RecoveredCamera cam  = recover_camera(view_proj);
    crd::math::Mat4f      proj = crd::math::Mat4f::identity();
    proj.c0.x                  = cam.sx;
    proj.c1.y                  = cam.sy;
    return compute_csm_cascades(cam.view, proj, light_dir, cfg);
}

crd::math::Vec3f camera_position_from_vp(const crd::math::Mat4f& view_proj)
{
    // view = [R | t] with R ORTHONORMAL, so inverse(view) = [R^T | -R^T t] and the camera's world position is
    // exactly -R^T t. No matrix inverse and no near-singular case: R being orthonormal is the same property the
    // whole reconstruction above already rests on.
    const crd::math::Mat4f v = recover_camera(view_proj).view;
    const crd::math::Vec3f t{v.c3.x, v.c3.y, v.c3.z};
    return crd::math::Vec3f{-((v.c0.x * t.x) + (v.c0.y * t.y) + (v.c0.z * t.z)),
                            -((v.c1.x * t.x) + (v.c1.y * t.y) + (v.c1.z * t.z)),
                            -((v.c2.x * t.x) + (v.c2.y * t.y) + (v.c2.z * t.z))};
}

} // namespace crd::scenerender
