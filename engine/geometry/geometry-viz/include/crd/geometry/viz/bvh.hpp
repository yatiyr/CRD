#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-viz — BVH traversal visualisations (Phase 3.1.7 v1j-a).
//
// Walks every node of a `BvhTree` / `Bvh4Tree` / `DynamicBvh` and emits
// depth-coloured AABB wireframes via `crd::draw::aabb_wire_to`. Plus the
// composite helpers `draw_overlap_pairs` (lines between overlapping leaf
// centroids — wraps `find_overlapping_pairs`) and `draw_frustum_cull`
// (two-colour kept/culled visualisation).
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp> // add_line_to used by the inline draw_overlap_pairs_with template
#include <crd/draw/types.hpp>
#include <crd/geometry/bvh/bvh4.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/bvh/dynamic_bvh.hpp>
#include <crd/geometry/primitives/primitives.hpp>

namespace crd::geometry::viz
{
// Default 8-entry HSV-around-the-wheel palette used when the caller doesn't
// supply one; depth `d` cycles `palette[d % 8]`. The palette intentionally
// avoids near-greys so the depth contrast survives on the engine's neutral
// debug clear.
[[nodiscard]] crd::draw::Color depth_color(crd::u32 depth) noexcept;

// `draw_bvh(BvhTree, prims)` walks every node and emits an AABB wireframe
// per node, coloured by depth via `depth_color()`. `depth_limit > 0` caps
// traversal at that depth (root = 0); `depth_limit = 0` walks the whole
// tree. The `prims` span is unused today (leaf bounds are stored inside the
// node array) but kept in the signature to mirror the query-side calls
// `bvh_raycast` / `bvh_overlap` use.
void draw_bvh(crd::draw::RenderBuffer& buf, const bvh::BvhTree& tree,
              crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims, crd::u32 depth_limit = 0U,
              crd::f32 width_px = 1.0F, crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
              crd::f32 lifetime_s = 0.0F);

void draw_bvh(crd::draw::RenderBuffer& buf, const bvh::Bvh4Tree& tree,
              crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims, crd::u32 depth_limit = 0U,
              crd::f32 width_px = 1.0F, crd::draw::PrimFlags flags = crd::draw::kDefaultFlags,
              crd::f32 lifetime_s = 0.0F);

// `DynamicBvh` doesn't expose per-leaf / per-internal-node iteration
// publicly today, so `draw_bvh_bounds` only emits the tree's root AABB
// (the outermost union). The name reflects what it does — it draws the
// tree's bounds, not its structure. Per-leaf visualisation needs a public
// `DynamicBvh::for_each_leaf(Fn)` walker (filed as debt). Sandbox callers
// that want per-leaf draws should use `draw_overlap_pairs_with` (gives
// pair-edges through a position lookup) or maintain their own
// `user_data → fat_aabb` mapping side-table.
void draw_bvh_bounds(crd::draw::RenderBuffer& buf, const bvh::DynamicBvh& tree,
                     crd::draw::Color color = crd::draw::kAabb, crd::f32 width_px = 1.0F,
                     crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Lines between centroids of every overlapping leaf pair, found via the
// v1i-c `find_overlapping_pairs(DynamicBvh)`. Uses caller-owned allocator
// for the scratch buffer (matching v1i-c+ pattern); if a future hot-path
// caller wants per-frame reuse, pass the same scratch across calls — but
// for a sandbox viz the per-call alloc is fine.
//
// `user_data_to_position` resolves a leaf's `user_data` to its centroid;
// callers pass a lambda over their own per-leaf state (the DynamicBvh only
// stores user_data, not positions). If the caller's `user_data` is already a
// world centroid index, the lambda is `[](u32){ return positions[ud]; }`.
template <typename Fn>
void draw_overlap_pairs_with(crd::draw::RenderBuffer& buf, const bvh::DynamicBvh& tree, Fn&& user_data_to_position,
                             crd::draw::Color color = crd::draw::kOrange, crd::f32 width_px = 1.0F,
                             crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    tree.find_overlapping_pairs(
        [&](crd::u32 a, crd::u32 b)
        {
            const crd::math::Vec3f pa = user_data_to_position(a);
            const crd::math::Vec3f pb = user_data_to_position(b);
            crd::draw::add_line_to(buf, pa, pb, color, width_px, flags, lifetime_s);
        });
}

// No-position-table convenience overload: emits NOTHING. `DynamicBvh`
// stores only `user_data` per leaf, no centroid table — there is nothing
// meaningful for a position-less call to draw. The function exists so
// callers can write `viz::draw_overlap_pairs(buf, tree);` symmetrically
// with the other `viz::*` calls, get back nothing, and then realise they
// need `draw_overlap_pairs_with(buf, tree, ud→pos)` to supply positions.
// (An earlier v1j-a draft tried to emit a count-indicator line — it
// looked like a misalignment, was untestable, and advisor flagged it as
// a future-you "what's this?" bug-report; cut.)
void draw_overlap_pairs(crd::draw::RenderBuffer& buf, const bvh::DynamicBvh& tree,
                        crd::draw::Color color = crd::draw::kOrange, crd::f32 width_px = 1.0F,
                        crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

// Two-colour cull visualisation: `kept_color` (default green) for prim
// AABBs whose centroid satisfies every frustum plane; `culled_color`
// (default red) for the rest. Walks `prims` directly — the BvhTree is
// taken for parity with future tree-aware culling (today it's the same
// per-prim test the brute-force does).
void draw_frustum_cull(crd::draw::RenderBuffer& buf, const primitives::Frustum<crd::f32>& frustum,
                       const bvh::BvhTree& tree, crd::containers::ConstSpan<primitives::AABB3<crd::f32>> prims,
                       crd::draw::Color kept_color = crd::draw::kGreen,
                       crd::draw::Color culled_color = crd::draw::kRed, crd::f32 width_px = 1.0F,
                       crd::draw::PrimFlags flags = crd::draw::kDefaultFlags, crd::f32 lifetime_s = 0.0F);

} // namespace crd::geometry::viz
