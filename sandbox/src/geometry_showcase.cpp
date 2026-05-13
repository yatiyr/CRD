// Sandbox geometry showcase — Phase 3.1.7 v1j-b.
//
// See geometry_showcase.hpp for the design contract. The .cpp dispatches on
// `state.mode` to a per-mode `render_*` function; each emits primitives into
// the caller's `RenderBuffer`. ImGui controls live in
// `draw_geometry_showcase_imgui` and dispatch the same way.

#include "geometry_showcase.hpp"

#include <crd/draw/active_buffer.hpp> // ScopedActiveBuffer + active-buffer-style wrappers used by render_draw_showcase
#include <crd/draw/shapes.hpp>
#include <crd/draw/types.hpp>
#include <crd/geometry/bvh/bvh.hpp>
#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/intersect.hpp>
#include <crd/geometry/primitives/signed_distance.hpp>
#include <crd/geometry/queries.hpp>
#include <crd/geometry/viz/viz.hpp>
#include <crd/math/quat.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>

namespace crd::sandbox
{
namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Capsule3;
using crd::geometry::primitives::Cylinder3;
using crd::geometry::primitives::Frustum;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::Segment3;
using crd::geometry::primitives::Sphere;
using crd::geometry::primitives::Tetrahedron;
using crd::geometry::primitives::Triangle3;
using crd::math::Vec3f;
namespace viz = crd::geometry::viz;
namespace gprim = crd::geometry::primitives;

// SplitMix64 RNG — deterministic across runs given a seed.
struct Rng
{
    crd::u64 state;
    explicit Rng(crd::u64 seed) : state(seed) {}
    crd::u64 next()
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    f32 unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1U << 24); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }
};

AABB3<f32> random_box(Rng& rng, f32 world, f32 max_size)
{
    const Vec3f c(rng.range(-world, world), rng.range(-world, world), rng.range(-world, world));
    const f32 hx = rng.range(0.1F, max_size);
    const f32 hy = rng.range(0.1F, max_size);
    const f32 hz = rng.range(0.1F, max_size);
    return AABB3<f32>(Vec3f(c.x - hx, c.y - hy, c.z - hz), Vec3f(c.x + hx, c.y + hy, c.z + hz));
}

Vec3f aabb_centroid(const AABB3<f32>& a) noexcept
{
    return Vec3f((a.min.x + a.max.x) * 0.5F, (a.min.y + a.max.y) * 0.5F, (a.min.z + a.max.z) * 0.5F);
}

// Convert Euler XYZ (in degrees) → quaternion → 3x3 orientation.
crd::math::Mat3f euler_to_mat3(const Vec3f& deg) noexcept
{
    const f32 to_rad = static_cast<f32>(std::numbers::pi) / 180.0F;
    const crd::math::Quatf qx = crd::math::from_axis_angle(Vec3f(1.0F, 0.0F, 0.0F), deg.x * to_rad);
    const crd::math::Quatf qy = crd::math::from_axis_angle(Vec3f(0.0F, 1.0F, 0.0F), deg.y * to_rad);
    const crd::math::Quatf qz = crd::math::from_axis_angle(Vec3f(0.0F, 0.0F, 1.0F), deg.z * to_rad);
    return crd::math::to_mat3(crd::math::normalized(qz * qy * qx));
}

Vec3f normalize_or(const Vec3f& v, const Vec3f& fallback) noexcept
{
    const f32 l2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (l2 <= 1e-20F)
    {
        return fallback;
    }
    const f32 inv = 1.0F / std::sqrt(l2);
    return Vec3f(v.x * inv, v.y * inv, v.z * inv);
}

// Build a 6-plane frustum from a position, a look direction, FOV (degrees),
// aspect ratio, and near/far. Plane normals point inward. Used by both the
// primitive viewer's Frustum case and the BVH viewer's frustum cull toggle.
Frustum<f32> build_view_frustum(const Vec3f& pos, const Vec3f& look_in, f32 fov_deg, f32 aspect, f32 near_z, f32 far_z)
{
    const Vec3f fwd = normalize_or(look_in, Vec3f(0.0F, 0.0F, 1.0F));
    const Vec3f world_up = (std::abs(fwd.y) < 0.95F) ? Vec3f(0.0F, 1.0F, 0.0F) : Vec3f(1.0F, 0.0F, 0.0F);
    const Vec3f right = normalize_or(crd::math::cross(fwd, world_up), Vec3f(1.0F, 0.0F, 0.0F));
    const Vec3f up = crd::math::cross(right, fwd);

    const f32 to_rad = static_cast<f32>(std::numbers::pi) / 180.0F;
    const f32 half_h = std::tan(fov_deg * 0.5F * to_rad);
    const f32 half_w = half_h * aspect;

    // Outward-facing normals would be the opposite; we want inward for the
    // "inside the frustum" half-space test. Each plane's `d` is computed so
    // `dot(n, x) + d >= 0` ⇔ x is on the inside.
    auto plane_from_normal_through_point = [](const Vec3f& n_in, const Vec3f& point_on_plane) -> Plane<f32> {
        const Vec3f n = normalize_or(n_in, Vec3f(0.0F, 1.0F, 0.0F));
        const f32 d = -(n.x * point_on_plane.x + n.y * point_on_plane.y + n.z * point_on_plane.z);
        return Plane<f32>(n, d);
    };

    Frustum<f32> f;
    // Near / Far (n axis = forward; near plane normal = +fwd from pos+near).
    f.planes[4] = plane_from_normal_through_point(fwd, Vec3f(pos.x + fwd.x * near_z, pos.y + fwd.y * near_z, pos.z + fwd.z * near_z));
    f.planes[5] = plane_from_normal_through_point(Vec3f(-fwd.x, -fwd.y, -fwd.z),
                                                  Vec3f(pos.x + fwd.x * far_z, pos.y + fwd.y * far_z, pos.z + fwd.z * far_z));
    // Left / Right (slanted out from `pos` toward the frustum side edges).
    const Vec3f left_n_raw(fwd.x + right.x * half_w, fwd.y + right.y * half_w, fwd.z + right.z * half_w);
    const Vec3f right_n_raw(fwd.x - right.x * half_w, fwd.y - right.y * half_w, fwd.z - right.z * half_w);
    f.planes[0] = plane_from_normal_through_point(normalize_or(left_n_raw, fwd), pos);
    f.planes[1] = plane_from_normal_through_point(normalize_or(right_n_raw, fwd), pos);
    // Bottom / Top (slanted up/down).
    const Vec3f bot_n_raw(fwd.x + up.x * half_h, fwd.y + up.y * half_h, fwd.z + up.z * half_h);
    const Vec3f top_n_raw(fwd.x - up.x * half_h, fwd.y - up.y * half_h, fwd.z - up.z * half_h);
    f.planes[2] = plane_from_normal_through_point(normalize_or(bot_n_raw, fwd), pos);
    f.planes[3] = plane_from_normal_through_point(normalize_or(top_n_raw, fwd), pos);
    return f;
}

// HSV → RGBA8 packed for the SDF heatmap. `t` ∈ [-1, +1]:
//   t = -1 → deep blue   (inside, far)
//   t =  0 → bright green (surface)
//   t = +1 → deep red    (outside, far)
crd::draw::Color sdf_color(f32 t)
{
    const f32 c = std::clamp(t, -1.0F, 1.0F);
    if (c < 0.0F)
    {
        // -1 → (0, 0, 255), 0 → (0, 255, 0)
        const f32 a = -c;
        const crd::u8 g = static_cast<crd::u8>((1.0F - a) * 255.0F);
        const crd::u8 b = static_cast<crd::u8>(a * 255.0F);
        return crd::draw::Color{0U, g, b, 220U};
    }
    // 0 → (0, 255, 0), +1 → (255, 0, 0)
    const crd::u8 r = static_cast<crd::u8>(c * 255.0F);
    const crd::u8 g = static_cast<crd::u8>((1.0F - c) * 255.0F);
    return crd::draw::Color{r, g, 0U, 220U};
}

// ===========================================================================
// 1. PRIMITIVE VIEWER
// ===========================================================================

void render_primitive_viewer(GeometryShowcaseState& s, crd::draw::RenderBuffer& buf)
{
    const crd::draw::Color shape_color = crd::draw::kWhite;
    const crd::draw::Color query_color = crd::draw::kOrange;
    const f32              w           = s.line_width;

    // The `viz::draw(buf, Shape, ...)` overloads all pin width_px somewhere in
    // their positional argument list (3rd-7th depending on shape — Sphere has
    // extra lat/lon args, Capsule has segments, etc.). Pass `w` explicitly to
    // every call so the ImGui line-width slider actually drives line thickness.
    auto draw_cp_overlay = [&](const Vec3f& cp) {
        // Internal "endpoint_size" stays 0.1 (a small cross at the query); we
        // only thread the line width through.
        viz::draw_closest_point(buf, s.prim_query, cp, query_color, 0.1F, w);
    };

    switch (s.primitive)
    {
        case ShowcasePrimitive::Sphere:
        {
            const Sphere<f32> sph(s.prim_center, s.prim_radius);
            viz::draw(buf, sph, shape_color, /*lat=*/8U, /*lon=*/12U, w);
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(sph, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Aabb:
        {
            const AABB3<f32> box(Vec3f(s.prim_center.x - s.prim_half_extents.x, s.prim_center.y - s.prim_half_extents.y,
                                       s.prim_center.z - s.prim_half_extents.z),
                                 Vec3f(s.prim_center.x + s.prim_half_extents.x, s.prim_center.y + s.prim_half_extents.y,
                                       s.prim_center.z + s.prim_half_extents.z));
            viz::draw(buf, box, shape_color, w);
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(box, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Obb:
        {
            const OBB3<f32> obb(s.prim_center, s.prim_half_extents, euler_to_mat3(s.prim_obb_euler));
            viz::draw(buf, obb, shape_color, w);
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(obb, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Capsule:
        {
            const Capsule3<f32> cap(s.prim_axis_a, s.prim_axis_b, s.prim_radius);
            viz::draw(buf, cap, shape_color, /*segments=*/16U, w);
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(cap, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Cylinder:
        {
            const Cylinder3<f32> cyl(s.prim_axis_a, s.prim_axis_b, s.prim_radius);
            viz::draw(buf, cyl, shape_color, /*segments=*/16U, w);
            // v1-close debt paid: closest_point(Cylinder3) now exists.
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(cyl, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Plane:
        {
            const Plane<f32> p(normalize_or(s.prim_plane_normal, Vec3f(0.0F, 1.0F, 0.0F)), s.prim_plane_d);
            viz::draw(buf, p, s.prim_center, s.prim_plane_patch_size, 6U, crd::draw::kGrey, w);
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(p, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Triangle:
        {
            const Triangle3<f32> tri(s.prim_axis_a, s.prim_axis_b, s.prim_axis_c);
            viz::draw(buf, tri, shape_color, w);
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(tri, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Tetrahedron:
        {
            const Tetrahedron<f32> tet(s.prim_axis_a, s.prim_axis_b, s.prim_axis_c, s.prim_axis_d);
            viz::draw(buf, tet, shape_color, w);
            // v1-close debt paid: closest_point(Tetrahedron) now exists (barycentric inside
            // test + per-face min if outside).
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(tet, s.prim_query));
            }
            break;
        }
        case ShowcasePrimitive::Frustum:
        {
            const Frustum<f32> f = build_view_frustum(s.prim_frustum_pos, s.prim_frustum_look, s.prim_frustum_fov_deg,
                                                     s.prim_frustum_aspect, s.prim_frustum_near, s.prim_frustum_far);
            viz::draw(buf, f, crd::draw::kYellow, w);
            break;
        }
        case ShowcasePrimitive::Ray:
        {
            const Ray3<f32> ray(s.prim_axis_a, normalize_or(s.prim_axis_b, Vec3f(1.0F, 0.0F, 0.0F)));
            viz::draw(buf, ray, /*length=*/10.0F, crd::draw::kYellow, w);
            break;
        }
        case ShowcasePrimitive::Segment:
        {
            const Segment3<f32> seg(s.prim_axis_a, s.prim_axis_b);
            viz::draw(buf, seg, shape_color, w);
            if (s.prim_show_query)
            {
                draw_cp_overlay(gprim::closest_point(seg, s.prim_query));
            }
            break;
        }
    }
}

// ===========================================================================
// 2. QUERY SHOWCASE
// ===========================================================================

void render_query_showcase(GeometryShowcaseState& s, crd::draw::RenderBuffer& buf, crd::memory::IAllocator& alloc)
{
    const f32 w = s.line_width;
    // Build a deterministic small scene.
    Rng rng(s.qs_seed);
    crd::containers::Array<AABB3<f32>> prims(&alloc);
    prims.reserve(s.qs_prim_count);
    for (u32 i = 0; i < s.qs_prim_count; ++i)
    {
        prims.push_back(random_box(rng, s.qs_world_size, 1.0F));
    }
    const crd::containers::ConstSpan<AABB3<f32>> pspan(prims.data(), prims.size());
    const crd::geometry::bvh::BvhTree tree = crd::geometry::bvh::bvh_build(pspan, &alloc);

    // Emit every prim AABB in a neutral colour as the backdrop.
    for (usize i = 0; i < prims.size(); ++i)
    {
        viz::draw(buf, prims[i], crd::draw::kGrey, w);
    }

    switch (s.qs_mode)
    {
        case ShowcaseQuery::Raycast:
        {
            const Ray3<f32> ray(s.qs_ray_origin, normalize_or(s.qs_ray_dir, Vec3f(1.0F, 0.0F, 0.0F)));
            const auto hit = crd::geometry::raycast(tree, pspan, ray, s.qs_ray_tmax);
            if (hit)
            {
                // Highlight the hit primitive and emit ray-hit visual.
                viz::draw(buf, prims[hit->payload], crd::draw::kRed, w);
                viz::draw_ray_hit(buf, ray, hit->t, Vec3f(0, 0, 0), 0.5F, crd::draw::kYellow, crd::draw::kRed,
                                  crd::draw::kCyan, w);
            }
            else
            {
                viz::draw(buf, ray, s.qs_ray_tmax, crd::draw::kYellow, w);
            }
            break;
        }
        case ShowcaseQuery::Overlap:
        {
            const AABB3<f32> q(s.qs_overlap_min, s.qs_overlap_max);
            crd::containers::Array<u32> hits(&alloc);
            crd::geometry::overlap(tree, pspan, q, hits);
            for (usize i = 0; i < hits.size(); ++i)
            {
                viz::draw(buf, prims[hits[i]], crd::draw::kGreen, w);
            }
            viz::draw(buf, q, crd::draw::kOrange, w);
            break;
        }
        case ShowcaseQuery::ClosestPoint:
        {
            const auto cp = crd::geometry::closest_point(tree, pspan, s.qs_closest_query);
            if (cp)
            {
                viz::draw(buf, prims[cp->payload], crd::draw::kCyan, w);
                viz::draw_closest_point(buf, s.qs_closest_query, cp->point, crd::draw::kOrange, 0.1F, w);
            }
            break;
        }
        case ShowcaseQuery::SphereCast:
        {
            const Sphere<f32> sph(s.qs_sweep_center, s.qs_sweep_radius);
            const Vec3f dir = normalize_or(s.qs_sweep_dir, Vec3f(1.0F, 0.0F, 0.0F));
            viz::draw(buf, sph, crd::draw::kCyan, 8U, 12U, w);
            const auto hit = crd::geometry::cast_sphere(tree, pspan, sph, dir, s.qs_sweep_tmax);
            if (hit)
            {
                viz::draw(buf, prims[hit->payload], crd::draw::kRed, w);
                // Sphere at impact position.
                const Vec3f impact_center(sph.center.x + dir.x * hit->t, sph.center.y + dir.y * hit->t,
                                          sph.center.z + dir.z * hit->t);
                viz::draw(buf, Sphere<f32>(impact_center, sph.radius), crd::draw::kRed, 8U, 12U, w);
                viz::draw_ray_hit(buf, Ray3<f32>(sph.center, dir), hit->t, Vec3f(0, 0, 0), 0.5F, crd::draw::kYellow,
                                  crd::draw::kRed, crd::draw::kCyan, w);
            }
            else
            {
                // Render the swept volume's end position so the user sees where it would land.
                const Vec3f end(sph.center.x + dir.x * s.qs_sweep_tmax, sph.center.y + dir.y * s.qs_sweep_tmax,
                                sph.center.z + dir.z * s.qs_sweep_tmax);
                viz::draw(buf, Sphere<f32>(end, sph.radius), crd::draw::kYellow, 8U, 12U, w);
                viz::draw(buf, Ray3<f32>(sph.center, dir), s.qs_sweep_tmax, crd::draw::kYellow, w);
            }
            break;
        }
    }
}

// ===========================================================================
// 3. BVH VIEWER
// ===========================================================================

// SplitMix64 hash mixer — used for the BVH viewer's cache fingerprint. Hashes
// the slider tuple (N, seed, world_size, max_box_size, tree_kind) into one u64
// the cache compares per frame to decide rebuild-or-reuse.
[[nodiscard]] crd::u64 mix64(crd::u64 z) noexcept
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

[[nodiscard]] crd::u64 bvh_viewer_fingerprint(const GeometryShowcaseState& s) noexcept
{
    crd::u64 h = 0xCBF29CE484222325ULL; // FNV offset basis
    h ^= mix64(static_cast<crd::u64>(s.bv_n) * 0x100000001B3ULL);
    h ^= mix64(static_cast<crd::u64>(s.bv_seed) * 0xC4CEB9FE1A85EC53ULL);
    crd::u32 ws_bits = 0;
    crd::u32 mb_bits = 0;
    std::memcpy(&ws_bits, &s.bv_world_size, sizeof(ws_bits));
    std::memcpy(&mb_bits, &s.bv_max_box_size, sizeof(mb_bits));
    h ^= mix64(static_cast<crd::u64>(ws_bits) * 0x9E3779B97F4A7C15ULL);
    h ^= mix64(static_cast<crd::u64>(mb_bits) * 0xBF58476D1CE4E5B9ULL);
    h ^= mix64(static_cast<crd::u64>(s.bv_tree_kind));
    if (h == 0)
    {
        h = 1; // 0 sentinel = "no cache"
    }
    return h;
}

void render_bvh_viewer(GeometryShowcaseState& s, crd::draw::RenderBuffer& buf, crd::memory::IAllocator& alloc,
                       BvhViewerCache& cache)
{
    const f32 w = s.line_width;

    // Cache fingerprint — rebuild only when slider tuple changes. Lifts the
    // per-frame O(N) build cost so the viewer can show large N at 60Hz.
    const crd::u64 fp = bvh_viewer_fingerprint(s);
    if (cache.fingerprint != fp)
    {
        // Rebuild. Drop previous structs first so their allocators don't
        // double-pin storage in the eylem TLSF heap.
        cache.binary.reset();
        cache.quad.reset();
        cache.dynamic.reset();
        cache.prims.clear();
        cache.centroids.clear();

        Rng rng(s.bv_seed);
        cache.prims.reserve(s.bv_n);
        for (u32 i = 0; i < s.bv_n; ++i)
        {
            cache.prims.push_back(random_box(rng, s.bv_world_size, s.bv_max_box_size));
        }
        const crd::containers::ConstSpan<AABB3<f32>> pspan(cache.prims.data(), cache.prims.size());

        if (s.bv_tree_kind == ShowcaseTreeKind::Binary)
        {
            cache.binary = std::make_unique<crd::geometry::bvh::BvhTree>(crd::geometry::bvh::bvh_build(pspan, cache.alloc));
        }
        else if (s.bv_tree_kind == ShowcaseTreeKind::Quad)
        {
            cache.binary = std::make_unique<crd::geometry::bvh::BvhTree>(crd::geometry::bvh::bvh_build(pspan, cache.alloc));
            cache.quad = std::make_unique<crd::geometry::bvh::Bvh4Tree>(
                crd::geometry::bvh::bvh4_collapse(*cache.binary, cache.alloc));
        }
        else // Dynamic
        {
            cache.dynamic = std::make_unique<crd::geometry::bvh::DynamicBvh>(cache.alloc);
            cache.centroids.reserve(cache.prims.size());
            for (usize i = 0; i < cache.prims.size(); ++i)
            {
                (void)cache.dynamic->insert(cache.prims[i], static_cast<u32>(i));
                cache.centroids.push_back(aabb_centroid(cache.prims[i]));
            }
            // Build a static binary tree alongside for the frustum-cull overlay.
            cache.binary = std::make_unique<crd::geometry::bvh::BvhTree>(crd::geometry::bvh::bvh_build(pspan, cache.alloc));
        }
        cache.fingerprint = fp;
    }
    (void)alloc; // Cache uses its own captured allocator; per-call alloc reserved for future modes.

    const crd::containers::ConstSpan<AABB3<f32>> pspan(cache.prims.data(), cache.prims.size());

    if (s.bv_tree_kind == ShowcaseTreeKind::Binary)
    {
        viz::draw_bvh(buf, *cache.binary, pspan, s.bv_depth_limit, w);
        if (s.bv_show_frustum_cull)
        {
            const Frustum<f32> fr = build_view_frustum(s.bv_frustum_pos, Vec3f(0.0F, 0.0F, 1.0F), s.bv_frustum_fov_deg,
                                                      s.bv_frustum_aspect, s.bv_frustum_near, s.bv_frustum_far);
            viz::draw_frustum_cull(buf, fr, *cache.binary, pspan, crd::draw::kGreen, crd::draw::kRed, w);
            viz::draw(buf, fr, crd::draw::kYellow, w);
        }
    }
    else if (s.bv_tree_kind == ShowcaseTreeKind::Quad)
    {
        viz::draw_bvh(buf, *cache.quad, pspan, s.bv_depth_limit, w);
        if (s.bv_show_frustum_cull)
        {
            const Frustum<f32> fr = build_view_frustum(s.bv_frustum_pos, Vec3f(0.0F, 0.0F, 1.0F), s.bv_frustum_fov_deg,
                                                      s.bv_frustum_aspect, s.bv_frustum_near, s.bv_frustum_far);
            viz::draw_frustum_cull(buf, fr, *cache.binary, pspan, crd::draw::kGreen, crd::draw::kRed, w);
            viz::draw(buf, fr, crd::draw::kYellow, w);
        }
    }
    else // Dynamic
    {
        viz::draw_bvh_bounds(buf, *cache.dynamic, crd::draw::kAabb, w);
        const AABB3<f32> universe = cache.dynamic->bounds();
        cache.dynamic->query(universe, [&](u32 ud) {
            if (ud < cache.prims.size())
            {
                viz::draw(buf, cache.prims[ud], crd::draw::kBodyDynamic, w);
            }
        });
        if (s.bv_show_overlap_pairs)
        {
            const Vec3f* cdata = cache.centroids.data();
            const usize ccount = cache.centroids.size();
            viz::draw_overlap_pairs_with(
                buf, *cache.dynamic,
                [cdata, ccount](u32 ud) -> Vec3f { return (ud < ccount) ? cdata[ud] : Vec3f(0.0F, 0.0F, 0.0F); },
                crd::draw::kOrange, w);
        }
        if (s.bv_show_frustum_cull)
        {
            const Frustum<f32> fr = build_view_frustum(s.bv_frustum_pos, Vec3f(0.0F, 0.0F, 1.0F), s.bv_frustum_fov_deg,
                                                      s.bv_frustum_aspect, s.bv_frustum_near, s.bv_frustum_far);
            viz::draw(buf, fr, crd::draw::kYellow, w);
            viz::draw_frustum_cull(buf, fr, *cache.binary, pspan, crd::draw::kGreen, crd::draw::kRed, w);
        }
    }
}

// ===========================================================================
// 4. SDF HEATMAP
// ===========================================================================

f32 evaluate_sdf(const GeometryShowcaseState& s, const Vec3f& p_world)
{
    // All sd_* functions operate in shape-local space. The showcase places
    // each shape centred at the origin; world = local here.
    switch (s.sdf_kind)
    {
        case ShowcaseSdfKind::Sphere:
            return gprim::sd_sphere(p_world, s.sdf_sphere_r);
        case ShowcaseSdfKind::Box:
            return gprim::sd_box(p_world, s.sdf_box_b);
        case ShowcaseSdfKind::RoundBox:
            return gprim::sd_round_box(p_world, s.sdf_box_b, s.sdf_round_r);
        case ShowcaseSdfKind::Torus:
            return gprim::sd_torus(p_world, crd::math::Vec2f(s.sdf_torus_t.x, s.sdf_torus_t.y));
        case ShowcaseSdfKind::Octahedron:
            return gprim::sd_octahedron(p_world, s.sdf_octa_s);
        case ShowcaseSdfKind::Capsule:
            return gprim::sd_capsule(p_world, s.sdf_capsule_a, s.sdf_capsule_b, s.sdf_capsule_r);
        case ShowcaseSdfKind::Cone:
        {
            const f32 half_angle = std::atan2(s.sdf_cone_radius, s.sdf_cone_height);
            const crd::math::Vec2f c(std::sin(half_angle), std::cos(half_angle));
            return gprim::sd_cone(p_world, c, s.sdf_cone_height);
        }
        case ShowcaseSdfKind::BoxFrame:
            return gprim::sd_box_frame(p_world, s.sdf_box_b, s.sdf_frame_e);
        case ShowcaseSdfKind::Cylinder:
        {
            // Upright cylinder spanning [-h/2, +h/2] on Y; radius = sdf_box_b.x.
            const f32 half_h = s.sdf_cylinder_h * 0.5F;
            return gprim::sd_cylinder(p_world, Vec3f(0.0F, -half_h, 0.0F), Vec3f(0.0F, half_h, 0.0F), s.sdf_box_b.x);
        }
    }
    return 0.0F;
}

void render_sdf_heatmap(GeometryShowcaseState& s, crd::draw::RenderBuffer& buf)
{
    // The crd-draw renderer ships line + triangle pipelines only (no point
    // pipeline today). Emitting `add_point_to` records would silently drop
    // them. We render each grid sample as a tiny 3-axis cross (3 line
    // emissions) — visible at the sandbox's default camera distance and
    // budget-safe given the `sdf_max_distance` skip filter.
    const u32 res = std::clamp(s.sdf_grid_res, 4U, 32U);
    const f32 ext = s.sdf_grid_extent;
    const f32 step = (2.0F * ext) / static_cast<f32>(res - 1U);
    const f32 normaliser = s.sdf_max_distance > 0.0F ? s.sdf_max_distance : 1.0F;

    // Cross-size scales with the grid step so the dots look right at any
    // resolution. ~30% of the cell so neighbours don't overlap visually.
    const f32 cross_size = step * 0.3F;

    for (u32 ix = 0; ix < res; ++ix)
    {
        for (u32 iy = 0; iy < res; ++iy)
        {
            for (u32 iz = 0; iz < res; ++iz)
            {
                const Vec3f p(-ext + step * static_cast<f32>(ix), -ext + step * static_cast<f32>(iy),
                              -ext + step * static_cast<f32>(iz));
                const f32 d = evaluate_sdf(s, p);
                // Skip far-outside points to keep the buffer manageable
                // (4096-line per-frame cap in crd-draw; 32³ = 32k samples
                // × 3 lines/cross = 98k lines if everything is kept — far
                // over budget. The skip filter is what makes this usable).
                if (d > s.sdf_max_distance)
                {
                    continue;
                }
                const crd::draw::Color col = sdf_color(d / normaliser);
                crd::draw::cross_3d_to(buf, p, cross_size, col, s.line_width, crd::draw::kDefaultFlags, 0.0F);
            }
        }
    }
}

// ===========================================================================
// IMGUI controls
// ===========================================================================

void imgui_primitive_viewer(GeometryShowcaseState& s)
{
    static const char* k_prim_names[] = {"Sphere",     "AABB",  "OBB",     "Capsule3",   "Cylinder3", "Plane",
                                          "Triangle3", "Tetrahedron", "Frustum", "Ray3",       "Segment3"};
    int idx = static_cast<int>(s.primitive);
    if (ImGui::Combo("Primitive", &idx, k_prim_names, IM_ARRAYSIZE(k_prim_names)))
    {
        s.primitive = static_cast<ShowcasePrimitive>(idx);
    }
    ImGui::Separator();

    switch (s.primitive)
    {
        case ShowcasePrimitive::Sphere:
            ImGui::DragFloat3("center", &s.prim_center.x, 0.05F);
            ImGui::DragFloat("radius", &s.prim_radius, 0.05F, 0.01F, 10.0F);
            break;
        case ShowcasePrimitive::Aabb:
            ImGui::DragFloat3("center", &s.prim_center.x, 0.05F);
            ImGui::DragFloat3("half-extents", &s.prim_half_extents.x, 0.05F, 0.01F, 10.0F);
            break;
        case ShowcasePrimitive::Obb:
            ImGui::DragFloat3("center", &s.prim_center.x, 0.05F);
            ImGui::DragFloat3("half-extents", &s.prim_half_extents.x, 0.05F, 0.01F, 10.0F);
            ImGui::DragFloat3("euler deg (XYZ)", &s.prim_obb_euler.x, 1.0F, -180.0F, 180.0F);
            break;
        case ShowcasePrimitive::Capsule:
        case ShowcasePrimitive::Cylinder:
            ImGui::DragFloat3("a", &s.prim_axis_a.x, 0.05F);
            ImGui::DragFloat3("b", &s.prim_axis_b.x, 0.05F);
            ImGui::DragFloat("radius", &s.prim_radius, 0.05F, 0.01F, 5.0F);
            break;
        case ShowcasePrimitive::Plane:
            ImGui::DragFloat3("normal", &s.prim_plane_normal.x, 0.02F, -1.0F, 1.0F);
            ImGui::DragFloat("d", &s.prim_plane_d, 0.05F);
            ImGui::DragFloat3("anchor", &s.prim_center.x, 0.05F);
            ImGui::DragFloat("patch size", &s.prim_plane_patch_size, 0.1F, 0.5F, 20.0F);
            break;
        case ShowcasePrimitive::Triangle:
            ImGui::DragFloat3("a", &s.prim_axis_a.x, 0.05F);
            ImGui::DragFloat3("b", &s.prim_axis_b.x, 0.05F);
            ImGui::DragFloat3("c", &s.prim_axis_c.x, 0.05F);
            break;
        case ShowcasePrimitive::Tetrahedron:
            ImGui::DragFloat3("a", &s.prim_axis_a.x, 0.05F);
            ImGui::DragFloat3("b", &s.prim_axis_b.x, 0.05F);
            ImGui::DragFloat3("c", &s.prim_axis_c.x, 0.05F);
            ImGui::DragFloat3("d", &s.prim_axis_d.x, 0.05F);
            break;
        case ShowcasePrimitive::Frustum:
            ImGui::DragFloat3("position", &s.prim_frustum_pos.x, 0.1F);
            ImGui::DragFloat3("look dir", &s.prim_frustum_look.x, 0.02F, -1.0F, 1.0F);
            ImGui::DragFloat("FOV (deg)", &s.prim_frustum_fov_deg, 0.5F, 10.0F, 170.0F);
            ImGui::DragFloat("aspect", &s.prim_frustum_aspect, 0.05F, 0.1F, 5.0F);
            ImGui::DragFloat("near", &s.prim_frustum_near, 0.05F, 0.01F, 100.0F);
            ImGui::DragFloat("far", &s.prim_frustum_far, 0.5F, 0.1F, 200.0F);
            break;
        case ShowcasePrimitive::Ray:
            ImGui::DragFloat3("origin", &s.prim_axis_a.x, 0.05F);
            ImGui::DragFloat3("direction", &s.prim_axis_b.x, 0.02F);
            break;
        case ShowcasePrimitive::Segment:
            ImGui::DragFloat3("a", &s.prim_axis_a.x, 0.05F);
            ImGui::DragFloat3("b", &s.prim_axis_b.x, 0.05F);
            break;
    }

    ImGui::Separator();
    ImGui::Checkbox("show closest-point query", &s.prim_show_query);
    if (s.prim_show_query)
    {
        ImGui::DragFloat3("query point", &s.prim_query.x, 0.05F);
    }
}

void imgui_query_showcase(GeometryShowcaseState& s)
{
    bool dirty = false;
    int n = static_cast<int>(s.qs_prim_count);
    if (ImGui::SliderInt("prim count", &n, 2, 64))
    {
        s.qs_prim_count = static_cast<u32>(n);
        dirty = true;
    }
    if (ImGui::DragFloat("world size", &s.qs_world_size, 0.1F, 1.0F, 50.0F))
    {
        dirty = true;
    }
    int seed = static_cast<int>(s.qs_seed);
    if (ImGui::DragInt("seed", &seed, 1.0F))
    {
        s.qs_seed = static_cast<u32>(seed);
        dirty = true;
    }
    (void)dirty; // rebuilt every frame — render fn handles it

    static const char* k_query_names[] = {"Raycast", "Overlap", "Closest point", "Sphere cast"};
    int q = static_cast<int>(s.qs_mode);
    if (ImGui::Combo("Query", &q, k_query_names, IM_ARRAYSIZE(k_query_names)))
    {
        s.qs_mode = static_cast<ShowcaseQuery>(q);
    }
    ImGui::Separator();

    switch (s.qs_mode)
    {
        case ShowcaseQuery::Raycast:
            ImGui::DragFloat3("origin", &s.qs_ray_origin.x, 0.05F);
            ImGui::DragFloat3("direction", &s.qs_ray_dir.x, 0.02F);
            ImGui::DragFloat("tmax", &s.qs_ray_tmax, 0.1F, 1.0F, 500.0F);
            break;
        case ShowcaseQuery::Overlap:
            ImGui::DragFloat3("min", &s.qs_overlap_min.x, 0.05F);
            ImGui::DragFloat3("max", &s.qs_overlap_max.x, 0.05F);
            break;
        case ShowcaseQuery::ClosestPoint:
            ImGui::DragFloat3("query", &s.qs_closest_query.x, 0.05F);
            break;
        case ShowcaseQuery::SphereCast:
            ImGui::DragFloat3("center", &s.qs_sweep_center.x, 0.05F);
            ImGui::DragFloat3("direction", &s.qs_sweep_dir.x, 0.02F);
            ImGui::DragFloat("radius", &s.qs_sweep_radius, 0.02F, 0.01F, 5.0F);
            ImGui::DragFloat("tmax", &s.qs_sweep_tmax, 0.1F, 1.0F, 500.0F);
            break;
    }
}

void imgui_bvh_viewer(GeometryShowcaseState& s)
{
    int n = static_cast<int>(s.bv_n);
    if (ImGui::SliderInt("N (random AABBs)", &n, 4, 5000))
    {
        s.bv_n = static_cast<u32>(n);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("BvhViewerCache reuses the built tree across frames; only the depth-"
                          "coloured walk runs each frame (~12*N line emissions). Slider changes "
                          "force a rebuild via the fingerprint check (v1-close debt paid).");
    }
    int seed = static_cast<int>(s.bv_seed);
    if (ImGui::DragInt("seed", &seed, 1.0F))
    {
        s.bv_seed = static_cast<u32>(seed);
    }
    ImGui::DragFloat("world size", &s.bv_world_size, 0.1F, 1.0F, 100.0F);
    ImGui::DragFloat("max box size", &s.bv_max_box_size, 0.05F, 0.1F, 10.0F);

    static const char* k_tree_names[] = {"BvhTree", "Bvh4Tree", "DynamicBvh"};
    int t = static_cast<int>(s.bv_tree_kind);
    if (ImGui::Combo("Tree kind", &t, k_tree_names, IM_ARRAYSIZE(k_tree_names)))
    {
        s.bv_tree_kind = static_cast<ShowcaseTreeKind>(t);
    }
    int depth = static_cast<int>(s.bv_depth_limit);
    if (ImGui::SliderInt("depth limit (0 = all)", &depth, 0, 30))
    {
        s.bv_depth_limit = static_cast<u32>(depth);
    }
    if (s.bv_tree_kind == ShowcaseTreeKind::Dynamic)
    {
        ImGui::Checkbox("show overlap pairs", &s.bv_show_overlap_pairs);
    }
    ImGui::Checkbox("show frustum cull", &s.bv_show_frustum_cull);
    if (s.bv_show_frustum_cull)
    {
        ImGui::DragFloat3("frustum pos", &s.bv_frustum_pos.x, 0.1F);
        ImGui::DragFloat("FOV (deg)", &s.bv_frustum_fov_deg, 0.5F, 10.0F, 170.0F);
        ImGui::DragFloat("near", &s.bv_frustum_near, 0.05F, 0.01F, 100.0F);
        ImGui::DragFloat("far", &s.bv_frustum_far, 0.5F, 0.1F, 200.0F);
        ImGui::DragFloat("aspect", &s.bv_frustum_aspect, 0.05F, 0.1F, 5.0F);
    }
}

void imgui_sdf_heatmap(GeometryShowcaseState& s)
{
    static const char* k_sdf_names[] = {"Sphere",      "Box",       "RoundBox",   "Torus", "Octahedron",
                                        "Capsule",     "Cone",      "BoxFrame",   "Cylinder"};
    int k = static_cast<int>(s.sdf_kind);
    if (ImGui::Combo("SDF type", &k, k_sdf_names, IM_ARRAYSIZE(k_sdf_names)))
    {
        s.sdf_kind = static_cast<ShowcaseSdfKind>(k);
    }
    int res = static_cast<int>(s.sdf_grid_res);
    if (ImGui::SliderInt("grid resolution", &res, 4, 48))
    {
        s.sdf_grid_res = static_cast<u32>(res);
    }
    ImGui::DragFloat("grid half-extent", &s.sdf_grid_extent, 0.1F, 0.5F, 10.0F);
    ImGui::DragFloat("max distance shown", &s.sdf_max_distance, 0.05F, 0.05F, 10.0F);
    ImGui::Separator();
    switch (s.sdf_kind)
    {
        case ShowcaseSdfKind::Sphere:
            ImGui::DragFloat("radius", &s.sdf_sphere_r, 0.05F, 0.05F, 5.0F);
            break;
        case ShowcaseSdfKind::Box:
        case ShowcaseSdfKind::Cylinder:
            ImGui::DragFloat3("half-extents", &s.sdf_box_b.x, 0.05F, 0.05F, 5.0F);
            break;
        case ShowcaseSdfKind::RoundBox:
            ImGui::DragFloat3("half-extents", &s.sdf_box_b.x, 0.05F, 0.05F, 5.0F);
            ImGui::DragFloat("round radius", &s.sdf_round_r, 0.02F, 0.01F, 2.0F);
            break;
        case ShowcaseSdfKind::Torus:
            ImGui::DragFloat("major", &s.sdf_torus_t.x, 0.05F, 0.05F, 5.0F);
            ImGui::DragFloat("minor", &s.sdf_torus_t.y, 0.02F, 0.01F, 2.0F);
            break;
        case ShowcaseSdfKind::Octahedron:
            ImGui::DragFloat("size", &s.sdf_octa_s, 0.05F, 0.05F, 5.0F);
            break;
        case ShowcaseSdfKind::Capsule:
            ImGui::DragFloat3("a", &s.sdf_capsule_a.x, 0.05F);
            ImGui::DragFloat3("b", &s.sdf_capsule_b.x, 0.05F);
            ImGui::DragFloat("radius", &s.sdf_capsule_r, 0.02F, 0.01F, 2.0F);
            break;
        case ShowcaseSdfKind::Cone:
            ImGui::DragFloat("height", &s.sdf_cone_height, 0.05F, 0.05F, 5.0F);
            ImGui::DragFloat("base radius", &s.sdf_cone_radius, 0.05F, 0.05F, 5.0F);
            break;
        case ShowcaseSdfKind::BoxFrame:
            ImGui::DragFloat3("half-extents", &s.sdf_box_b.x, 0.05F, 0.05F, 5.0F);
            ImGui::DragFloat("frame thickness", &s.sdf_frame_e, 0.01F, 0.005F, 1.0F);
            break;
    }
}

} // namespace

void render_geometry_showcase(GeometryShowcaseState& state, crd::draw::RenderBuffer& buf,
                              crd::memory::IAllocator& alloc, BvhViewerCache& bvh_cache)
{
    // Optional origin triad to anchor every showcase scene visually. User
    // toggle (default ON); width tracks the line_width slider so the triad
    // matches whatever line thickness the user has dialled in.
    if (state.show_origin_triad)
    {
        crd::draw::axis_triad_to(buf, crd::math::Mat4f::identity(), state.origin_triad_size, state.line_width);
    }

    switch (state.mode)
    {
        case GeometryShowcaseMode::PrimitiveViewer:
            render_primitive_viewer(state, buf);
            break;
        case GeometryShowcaseMode::QueryShowcase:
            render_query_showcase(state, buf, alloc);
            break;
        case GeometryShowcaseMode::BvhViewer:
            render_bvh_viewer(state, buf, alloc, bvh_cache);
            break;
        case GeometryShowcaseMode::SdfHeatmap:
            render_sdf_heatmap(state, buf);
            break;
    }
}

// ===========================================================================
// CRD-DRAW API SHOWCASE — historic v1a-draw d0d demo (moved out of
// `render_scene` and gated on `SandboxScene::DrawShowcase`). Exists as a
// scene of its own so the geometry showcase has a clean canvas.
// ===========================================================================

void render_draw_showcase(const GeometryShowcaseState& state, crd::draw::RenderBuffer& buf)
{
    const f32 w = state.line_width;
    crd::draw::ScopedActiveBuffer scoped_buf{&buf};

    // World-axis triad at origin (3 arrows, RGB convention).
    if (state.show_origin_triad)
    {
        crd::draw::axis_triad(crd::math::Mat4f::identity(), 1.0F, w);
    }

    // Box: wire + translucent solid fill at (2, 0, 0).
    crd::math::Mat4f box_world = crd::math::Mat4f::identity();
    box_world.c3.x = 2.0F;
    crd::draw::box_wire(box_world, {0.5F, 0.5F, 0.5F}, crd::draw::kBodyDynamic, w);
    crd::draw::box_solid(box_world, {0.5F, 0.5F, 0.5F}, crd::draw::Color{200, 200, 100, 80});

    // Sphere: wire + translucent solid at (-2, 0, 0).
    crd::draw::sphere_wire({-2.0F, 0.0F, 0.0F}, 0.6F, crd::draw::kCyan, /*lat=*/8U, /*lon=*/12U, w);
    crd::draw::sphere_solid({-2.0F, 0.0F, 0.0F}, 0.6F, crd::draw::Color{0, 255, 255, 60});

    // Capsule: wire + translucent solid at (0, 0, 2).
    crd::draw::capsule_wire({0.0F, -0.4F, 2.0F}, {0.0F, 0.4F, 2.0F}, 0.4F, crd::draw::kBodyKinematic,
                            /*segments=*/16U, w);
    crd::draw::capsule_solid({0.0F, -0.4F, 2.0F}, {0.0F, 0.4F, 2.0F}, 0.4F,
                             crd::draw::Color{80, 200, 240, 70});

    // Velocity-style arrow at (0, 1.5, 0) pointing +X.
    crd::draw::arrow({0.0F, 1.5F, 0.0F}, {1.0F, 0.0F, 0.0F}, 1.0F, crd::draw::kVelocityArrow, 0.2F, 0.4F, w);

    // 3D cross marker (contact-point-style indicator).
    crd::draw::cross_3d({0.0F, 0.0F, -2.0F}, 0.3F, crd::draw::kContactPoint, w);

    // Joint-limit-style arc (90 degree sweep around Y axis).
    crd::draw::arc({0.0F, 0.5F, -2.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 0.5F, 0.0F, 1.5707963F,
                   crd::draw::kJointFrame, 32U, w);
}

void draw_geometry_showcase_imgui(GeometryShowcaseState& state)
{
    static const char* k_mode_names[] = {"Primitive viewer", "Query showcase", "BVH viewer", "SDF heatmap"};
    int m = static_cast<int>(state.mode);
    if (ImGui::Combo("Showcase mode", &m, k_mode_names, IM_ARRAYSIZE(k_mode_names)))
    {
        state.mode = static_cast<GeometryShowcaseMode>(m);
    }
    ImGui::Separator();
    switch (state.mode)
    {
        case GeometryShowcaseMode::PrimitiveViewer:
            imgui_primitive_viewer(state);
            break;
        case GeometryShowcaseMode::QueryShowcase:
            imgui_query_showcase(state);
            break;
        case GeometryShowcaseMode::BvhViewer:
            imgui_bvh_viewer(state);
            break;
        case GeometryShowcaseMode::SdfHeatmap:
            imgui_sdf_heatmap(state);
            break;
    }
}

} // namespace crd::sandbox
