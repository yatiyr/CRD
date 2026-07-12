#pragma once

// ckir_render.hpp — the RENDER-PATH math (D-007 B8-l): the analytic cores of the two render pipelines + clustered decals, as
// backend-neutral CKIR. ADR-0102's hybrid — deferred opaque + forward transparent — needs three pieces, all pure shader math:
//   • FORWARD+ clustered light culling — a froxel grid (screen tiles × exponential depth slices) + the light-sphere-vs-cluster
//     test that the cull compute pass runs per (light, cluster).
//   • DEFERRED lighting — DECODE the B5 G-buffer MRT back into a surface, then shade it (B8 Cook-Torrance).
//   • CLUSTERED DECALS — project a world point into a decal's box → decal UV + an inside test (works in forward+ AND deferred).
//
// SCOPE: the per-fragment/per-cluster MATH is here + testable now. The renderer LEAVES (B8-m + the post-detour render phase):
// the actual clustered-cull COMPUTE dispatch (atomics building the per-cluster light lists into a set-1 buffer), the G-buffer
// render-to-texture + sampling in the deferred pass, the decal-cull list build, and the frame-graph orchestration
// (shadow → depth-prepass → light/decal-cull → G-buffer/forward → lighting → composite). Those need the compute context +
// structured buffers + render-to-sampled-texture; the shading/culling/projection math they invoke is complete here.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_lighting.hpp>
#include <crd/kir/ckir_material.hpp>

namespace crd::kir::render
{

// ── FORWARD+ : clustered light culling ───────────────────────────────────────────────────────────────────────────────────

// cluster_z_slice — the EXPONENTIAL depth slice (Doom-2016 clustered shading): `slice = floor(log(z/near)/log(far/near)·N)`.
// Exponential slicing keeps froxels roughly cube-shaped in view space (near slices thin, far slices thick).
[[nodiscard]] inline int cluster_z_slice(KGraph& g, int view_z, int near, int far, int num_slices)
{
    const int t = g.binary(KOp::Div, g.unary(KOp::Log, g.binary(KOp::Div, view_z, near)), g.unary(KOp::Log, g.binary(KOp::Div, far, near)));
    return g.unary(KOp::Floor, g.binary(KOp::Mul, t, num_slices));
}
// cluster_coord — the froxel (tile_x, tile_y, z_slice) a fragment falls in: screen tiles × the exponential depth slice. The
// index into the per-cluster light list the cull pass fills.
[[nodiscard]] inline int cluster_coord(KGraph& g, int uv, int view_z, int tiles_x, int tiles_y, int num_slices, int near, int far)
{
    const int tx = g.unary(KOp::Floor, g.binary(KOp::Mul, g.swizzle(uv, 0), tiles_x));
    const int ty = g.unary(KOp::Floor, g.binary(KOp::Mul, g.swizzle(uv, 1), tiles_y));
    return g.vec3(tx, ty, cluster_z_slice(g, view_z, near, far, num_slices));
}
// sphere_aabb_sq_dist — squared distance from a point to an AABB via the closest-point clamp. The core of the cull test.
[[nodiscard]] inline int sphere_aabb_sq_dist(KGraph& g, int center, int aabb_min, int aabb_max)
{
    const int closest = nodes::clamp(g, center, aabb_min, aabb_max);
    const int d       = g.binary(KOp::Sub, center, closest);
    return g.dot(d, d);
}
// light_cluster_cull — 1 if the light SPHERE (center, radius) intersects the cluster AABB, else 0. Branchless (`step`): this is
// the per-(light,cluster) test the Forward+ cull compute pass runs to build each cluster's light list.
[[nodiscard]] inline int light_cluster_cull(KGraph& g, int center, int radius, int aabb_min, int aabb_max)
{
    const int sq = sphere_aabb_sq_dist(g, center, aabb_min, aabb_max);
    return g.binary(KOp::Step, sq, g.binary(KOp::Mul, radius, radius)); // radius² < sq ? 0 : 1  → 1 iff sq ≤ radius²
}

// ── DEFERRED : G-buffer lighting ─────────────────────────────────────────────────────────────────────────────────────────

// deferred_shade — the full-screen deferred lighting pass: DECODE the B5 G-buffer MRT (the `material::pack_gbuffer` layout:
// g0=(base,metallic) · g1=(normal_enc = n·0.5+0.5, roughness) · g2=(emissive,occlusion)) back into a surface, then shade it
// with the B8 Cook-Torrance directional key light + emissive. The scene light ARRAY (set-1, from the Forward+ cull) binds at
// B8-m; here a fixed key light keeps the deferred variant renderable + oracle-checkable.
[[nodiscard]] inline int deferred_shade(KGraph& g, int g0, int g1, int g2, int view, int light_dir, int light_color)
{
    const int base = g.swizzle(g0, 0, 1, 2);
    const int met  = g.swizzle(g0, 3);
    const int nenc = g.swizzle(g1, 0, 1, 2);
    const int rgh  = g.swizzle(g1, 3);
    const int emis = g.swizzle(g2, 0, 1, 2);
    // decode n = enc·2 − 1 — the scalar 2/1 must BROADCAST across the vec3 (use `bin`, not raw `binary`: konst is a scalar, so
    // `binary(Mul, vec3, scalar)` is the shape-mismatch scar — GPU broadcasts, the same-shape oracle reads OOB → wrong normal).
    const int two  = nodes::detail::konst(g, nenc, 2.0);
    const int one  = nodes::detail::konst(g, nenc, 1.0);
    const int nrm  = g.normalize(nodes::detail::bin(g, KOp::Sub, nodes::detail::bin(g, KOp::Mul, nenc, two), one));
    const int lit  = lighting::directional_light(g, base, met, rgh, nrm, view, light_dir, light_color);
    return nodes::clamp01(g, nodes::detail::bin(g, KOp::Add, lit, emis));
}

// ── CLUSTERED DECALS ─────────────────────────────────────────────────────────────────────────────────────────────────────

// decal_project — project a world point into a decal's LOCAL box via the decal's inverse transform (a mat4). Returns
// vec3(decal_uv.x, decal_uv.y, inside): `inside` = 1 iff the point lies within the [−0.5,0.5]³ box; `uv = local.xy + 0.5`
// (∈ [0,1]²) indexes the decal's albedo/normal/metal-rough/emissive atlas. Works in forward+ AND deferred (no prepass).
[[nodiscard]] inline int decal_project(KGraph& g, int world_pos, int decal_inv)
{
    const int  wp4   = g.vec4(g.swizzle(world_pos, 0), g.swizzle(world_pos, 1), g.swizzle(world_pos, 2), nodes::detail::konst(g, g.swizzle(world_pos, 0), 1.0));
    const int  local = g.mat_mul_vec(decal_inv, wp4); // decal-space position (vec4; xyz is the box coordinate)
    const int  lx    = g.swizzle(local, 0);
    const int  ly    = g.swizzle(local, 1);
    const int  lz    = g.swizzle(local, 2);
    const auto in1   = [&](int a) { return g.binary(KOp::Step, g.unary(KOp::Abs, a), nodes::detail::konst(g, a, 0.5)); }; // |a|>0.5 ? 0 : 1  → 1 iff |a| ≤ 0.5
    const int  inside = g.binary(KOp::Mul, g.binary(KOp::Mul, in1(lx), in1(ly)), in1(lz));
    return g.vec3(g.binary(KOp::Add, lx, nodes::detail::konst(g, lx, 0.5)), g.binary(KOp::Add, ly, nodes::detail::konst(g, ly, 0.5)), inside);
}

} // namespace crd::kir::render
