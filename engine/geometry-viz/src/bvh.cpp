#include <crd/draw/shapes.hpp>
#include <crd/geometry/primitives/intersect.hpp> // intersects(Plane, AABB3) — for frustum cull
#include <crd/geometry/viz/bvh.hpp>

namespace crd::geometry::viz
{
namespace
{
using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::math::Vec3f;

// 8-entry depth palette: hue-distinct, avoiding near-greys so contrast
// survives on the engine's neutral debug clear. Indexed by `depth % 8`.
constexpr crd::draw::Color kDepthPalette[8] = {
    crd::draw::Color{255, 80, 80, 220},    // red    — depth 0 (root)
    crd::draw::Color{255, 180, 60, 220},   // orange
    crd::draw::Color{255, 240, 80, 220},   // yellow
    crd::draw::Color{120, 230, 80, 220},   // lime
    crd::draw::Color{60, 220, 200, 220},   // cyan
    crd::draw::Color{80, 140, 255, 220},   // blue
    crd::draw::Color{180, 100, 255, 220},  // violet
    crd::draw::Color{255, 100, 200, 220},  // pink
};

} // namespace

crd::draw::Color depth_color(u32 depth) noexcept
{
    return kDepthPalette[depth % 8U];
}

namespace
{
// Recursive helper for the static BvhTree: walks `node_idx` at depth `d`,
// emits its AABB, and recurses into children (interior nodes only).
void walk_binary(crd::draw::RenderBuffer& buf, const bvh::BvhTree& tree, u32 node_idx, u32 depth, u32 depth_limit,
                 f32 width_px, crd::draw::PrimFlags flags, f32 lifetime_s)
{
    if (depth_limit != 0U && depth > depth_limit)
    {
        return;
    }
    const crd::containers::ConstSpan<bvh::BvhNode> nodes = tree.nodes();
    const bvh::BvhNode& n = nodes[node_idx];
    crd::draw::aabb_wire_to(buf, n.bounds.min, n.bounds.max, depth_color(depth), width_px, flags, lifetime_s);
    if (!n.is_leaf())
    {
        walk_binary(buf, tree, n.left_first, depth + 1U, depth_limit, width_px, flags, lifetime_s);
        walk_binary(buf, tree, n.left_first + 1U, depth + 1U, depth_limit, width_px, flags, lifetime_s);
    }
}

// Recursive helper for Bvh4Tree.
void walk_bvh4(crd::draw::RenderBuffer& buf, const bvh::Bvh4Tree& tree, u32 node_idx, u32 depth, u32 depth_limit,
               f32 width_px, crd::draw::PrimFlags flags, f32 lifetime_s)
{
    if (depth_limit != 0U && depth > depth_limit)
    {
        return;
    }
    const crd::containers::ConstSpan<bvh::Bvh4Node> nodes = tree.nodes();
    const bvh::Bvh4Node& n = nodes[node_idx];
    crd::draw::aabb_wire_to(buf, n.bounds.min, n.bounds.max, depth_color(depth), width_px, flags, lifetime_s);
    // For each child: emit its AABB at depth+1; recurse into interior children.
    for (crd::u8 c = 0; c < n.child_count; ++c)
    {
        const bvh::Bvh4Child& ch = n.children[c];
        crd::draw::aabb_wire_to(buf, ch.bounds.min, ch.bounds.max, depth_color(depth + 1U), width_px, flags,
                                lifetime_s);
        if (!ch.is_leaf())
        {
            walk_bvh4(buf, tree, ch.first, depth + 2U, depth_limit, width_px, flags, lifetime_s);
        }
    }
}
} // namespace

void draw_bvh(crd::draw::RenderBuffer& buf, const bvh::BvhTree& tree,
              crd::containers::ConstSpan<primitives::AABB3<f32>> /*prims*/, u32 depth_limit, f32 width_px,
              crd::draw::PrimFlags flags, f32 lifetime_s)
{
    if (tree.is_empty())
    {
        return;
    }
    walk_binary(buf, tree, tree.root(), 0U, depth_limit, width_px, flags, lifetime_s);
}

void draw_bvh(crd::draw::RenderBuffer& buf, const bvh::Bvh4Tree& tree,
              crd::containers::ConstSpan<primitives::AABB3<f32>> /*prims*/, u32 depth_limit, f32 width_px,
              crd::draw::PrimFlags flags, f32 lifetime_s)
{
    if (tree.is_empty())
    {
        return;
    }
    walk_bvh4(buf, tree, tree.root(), 0U, depth_limit, width_px, flags, lifetime_s);
}

void draw_bvh_bounds(crd::draw::RenderBuffer& buf, const bvh::DynamicBvh& tree, crd::draw::Color color, f32 width_px,
                     crd::draw::PrimFlags flags, f32 lifetime_s)
{
    if (tree.is_empty())
    {
        return;
    }
    // Emit only the tree's outermost union AABB. Per-leaf and per-internal-
    // node iteration on `DynamicBvh` isn't in the public API yet (filed as
    // debt — a `DynamicBvh::for_each_leaf(Fn)` walker the editor slice will
    // need); callers that want per-leaf draws should plug their own
    // `user_data → fat_aabb` side-table through `draw_overlap_pairs_with`
    // or query the tree directly.
    const AABB3<f32> universe = tree.bounds();
    crd::draw::aabb_wire_to(buf, universe.min, universe.max, color, width_px, flags, lifetime_s);
}

void draw_overlap_pairs(crd::draw::RenderBuffer& buf, const bvh::DynamicBvh& /*tree*/, crd::draw::Color /*color*/,
                        f32 /*width_px*/, crd::draw::PrimFlags /*flags*/, f32 /*lifetime_s*/)
{
    // Intentionally empty. `DynamicBvh` carries `user_data` per leaf but no
    // centroid table — there is nothing meaningful for a position-less call
    // to draw. Callers wanting per-pair lines should use the templated
    // `draw_overlap_pairs_with(buf, tree, user_data_to_position, ...)` form
    // and supply a `user_data → Vec3f` lambda over their own position table.
    (void)buf;
}

void draw_frustum_cull(crd::draw::RenderBuffer& buf, const primitives::Frustum<f32>& frustum,
                       const bvh::BvhTree& /*tree*/, crd::containers::ConstSpan<primitives::AABB3<f32>> prims,
                       crd::draw::Color kept_color, crd::draw::Color culled_color, f32 width_px,
                       crd::draw::PrimFlags flags, f32 lifetime_s)
{
    // Per-prim plane-side test: prim is *culled* if every corner sits on
    // the outward side of any one plane. Conservative: a partial-overlap
    // prim counts as kept. For the sandbox viz this matches the visual
    // intent — "would this prim be drawn?".
    for (crd::usize i = 0; i < prims.size(); ++i)
    {
        const AABB3<f32>& a = prims[i];
        bool kept = true;
        for (crd::usize pl = 0; pl < 6; ++pl)
        {
            const primitives::Plane<f32>& p = frustum.planes[pl];
            // Test if the AABB is entirely on the negative (outside) side of `p`.
            // Use the AABB's positive vertex (point pushed along `p.normal`).
            const Vec3f positive_vertex(p.normal.x >= 0.0F ? a.max.x : a.min.x, p.normal.y >= 0.0F ? a.max.y : a.min.y,
                                        p.normal.z >= 0.0F ? a.max.z : a.min.z);
            if (p.normal.x * positive_vertex.x + p.normal.y * positive_vertex.y + p.normal.z * positive_vertex.z + p.d <
                0.0F)
            {
                kept = false;
                break;
            }
        }
        crd::draw::aabb_wire_to(buf, a.min, a.max, kept ? kept_color : culled_color, width_px, flags, lifetime_s);
    }
}

} // namespace crd::geometry::viz
