// crd-geometry-spatial — `LooseOctree<T>` impl (Phase 3.1.7 v5b).
//
// Reference: Thatcher Ulrich, "Loose Octrees" in *Game Programming Gems* vol. 1,
// 2000. Header `loose_octree.hpp` documents the design + locked decisions.

#include <crd/geometry/spatial/loose_octree.hpp>

#include <algorithm>
#include <cmath>

namespace crd::geometry::spatial
{
using crd::f32;
using crd::f64;
using crd::i32;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::geometry::primitives::is_finite;
using crd::math::Vec3;

namespace
{

// AABB enclosure: `inner` ⊆ `outer` (componentwise inclusive).
template <MathScalar T>
inline bool aabb_encloses(const AABB3<T>& outer, const AABB3<T>& inner) noexcept
{
    return outer.min.x <= inner.min.x && inner.max.x <= outer.max.x
        && outer.min.y <= inner.min.y && inner.max.y <= outer.max.y
        && outer.min.z <= inner.min.z && inner.max.z <= outer.max.z;
}

template <MathScalar T>
inline Vec3<T> aabb_center(const AABB3<T>& b) noexcept
{
    return Vec3<T>{(b.min.x + b.max.x) * T{0.5},
                    (b.min.y + b.max.y) * T{0.5},
                    (b.min.z + b.max.z) * T{0.5}};
}

template <MathScalar T>
inline Vec3<T> aabb_extent(const AABB3<T>& b) noexcept
{
    return Vec3<T>{b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z};
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

template <MathScalar T>
LooseOctree<T>::LooseOctree(crd::memory::IAllocator* alloc, const OctreeBuildOptions<T>& opts)
    : m_alloc(alloc)
    , m_nodes(alloc)
    , m_objects(alloc)
    , m_cells(alloc)
    , m_root_bounds(opts.root_bounds)
    , m_loosening(opts.loosening)
    , m_leaf_object_threshold(opts.leaf_object_threshold == 0U ? k_octree_leaf_object_threshold
                                                                 : opts.leaf_object_threshold)
    , m_max_depth(opts.max_depth == 0U ? k_octree_max_depth : opts.max_depth)
{
    // Validate root bounds: finite + positive extent on every axis. Builder-
    // reject contract per ADR-0076 §15.
    CRD_ASSERT(is_finite(m_root_bounds));
    CRD_ASSERT(m_root_bounds.max.x > m_root_bounds.min.x);
    CRD_ASSERT(m_root_bounds.max.y > m_root_bounds.min.y);
    CRD_ASSERT(m_root_bounds.max.z > m_root_bounds.min.z);
    CRD_ASSERT(m_loosening >= T{1});

    // Lazily-grown root: created on first insert. (Allocating it here costs a
    // node + a CellObjects entry for trees that may stay empty.)
    m_root = k_null;
}

// =============================================================================
// Node + object pool helpers
// =============================================================================

template <MathScalar T>
u32 LooseOctree<T>::allocate_node(const AABB3<T>& bounds, u32 parent, u8 depth)
{
    u32 idx;
    if (m_node_free_list != k_null)
    {
        idx = m_node_free_list;
        m_node_free_list = m_nodes[idx].children[0]; // free-list link parked in children[0]
        // Reset the recycled node.
        m_nodes[idx] = OctreeNode<T>{};
        // Recycle existing CellObjects entry — capacity retained from prior life.
        m_cells[idx].ids.clear();
    }
    else
    {
        idx = static_cast<u32>(m_nodes.size());
        m_nodes.push_back(OctreeNode<T>{});
        m_cells.push_back(CellObjects{m_alloc});
    }

    OctreeNode<T>& n = m_nodes[idx];
    n.bounds = bounds;
    for (u8 i = 0; i < 8U; ++i) { n.children[i] = k_null; }
    n.parent = parent;
    n.depth  = depth;
    n.flags  = 1U; // bit 0 = allocated
    ++m_allocated_nodes;
    if (depth > m_max_depth_used) { m_max_depth_used = depth; }
    return idx;
}

template <MathScalar T>
void LooseOctree<T>::free_node(u32 idx)
{
    CRD_ASSERT(is_node_alive(idx));
    OctreeNode<T>& n = m_nodes[idx];
    n.flags = 0U;
    n.children[0] = m_node_free_list;
    m_node_free_list = idx;
    --m_allocated_nodes;
    m_cells[idx].ids.clear();
}

template <MathScalar T>
u32 LooseOctree<T>::allocate_object(const AABB3<T>& aabb, u32 payload, u32 cell_node)
{
    u32 idx;
    if (m_object_free_list != k_invalid_node)
    {
        idx = m_object_free_list;
        m_object_free_list = m_objects[idx].next_free;
    }
    else
    {
        idx = static_cast<u32>(m_objects.size());
        m_objects.push_back(ObjectEntry{});
    }
    ObjectEntry& obj = m_objects[idx];
    obj.aabb = aabb;
    obj.payload = payload;
    obj.cell_node = cell_node;
    obj.next_free = k_invalid_node;
    obj.alive = true;
    ++m_object_count;
    return idx;
}

template <MathScalar T>
void LooseOctree<T>::free_object(u32 idx)
{
    CRD_ASSERT(is_object_alive(idx));
    ObjectEntry& obj = m_objects[idx];
    obj.alive = false;
    obj.cell_node = k_invalid_node;
    obj.next_free = m_object_free_list;
    m_object_free_list = idx;
    --m_object_count;
}

// =============================================================================
// Cell ↔ object plumbing
// =============================================================================

template <MathScalar T>
void LooseOctree<T>::cell_add_object(u32 cell_idx, u32 obj_idx)
{
    m_cells[cell_idx].ids.push_back(obj_idx);
    m_objects[obj_idx].cell_node = cell_idx;
}

template <MathScalar T>
void LooseOctree<T>::cell_remove_object(u32 cell_idx, u32 obj_idx)
{
    auto& list = m_cells[cell_idx].ids;
    // Swap-with-last: O(cell_count). Cell counts are bounded by the leaf
    // threshold (typically 8) at non-max-depth cells; max-depth cells may
    // grow larger but that's the worst case (rare in practice).
    for (usize i = 0; i < list.size(); ++i)
    {
        if (list[i] == obj_idx)
        {
            list[i] = list[list.size() - 1U];
            list.resize(list.size() - 1U);
            return;
        }
    }
    CRD_ASSERT(false && "cell_remove_object: obj_idx not in cell list");
}

// =============================================================================
// Octree algorithms — child geometry, octant pick, depth pick
// =============================================================================

template <MathScalar T>
u8 LooseOctree<T>::octant_of_center(const AABB3<T>& cell, const Vec3<T>& center) noexcept
{
    const Vec3<T> mid = aabb_center(cell);
    // bit 0 = X (lower→0, upper→1), bit 1 = Y, bit 2 = Z.
    // `>=` not `>` — "lower octant wins" lex tiebreak when center exactly on midplane.
    const u8 ox = (center.x >= mid.x) ? 1U : 0U;
    const u8 oy = (center.y >= mid.y) ? 1U : 0U;
    const u8 oz = (center.z >= mid.z) ? 1U : 0U;
    return static_cast<u8>(ox | (oy << 1U) | (oz << 2U));
}

template <MathScalar T>
AABB3<T> LooseOctree<T>::child_bounds_of(const AABB3<T>& parent_bounds, u8 octant) noexcept
{
    const Vec3<T> mid = aabb_center(parent_bounds);
    AABB3<T> c = parent_bounds;
    if ((octant & 1U) != 0U) { c.min.x = mid.x; } else { c.max.x = mid.x; }
    if ((octant & 2U) != 0U) { c.min.y = mid.y; } else { c.max.y = mid.y; }
    if ((octant & 4U) != 0U) { c.min.z = mid.z; } else { c.max.z = mid.z; }
    return c;
}

template <MathScalar T>
u8 LooseOctree<T>::target_depth_for(const Vec3<T>& extent) const noexcept
{
    // Pick the deepest cell depth at which the object's tight AABB is GUARANTEED
    // to fit within the cell's loose AABB regardless of WHERE inside the cell
    // the object's center sits. (Ulrich's classical formula `loose × R / extent`
    // is the *centered-fit* depth — only correct when the object's center
    // coincides with the cell center; off-center placement at that depth can
    // place the object's tight AABB partially outside the cell's loose AABB,
    // which would silently lose the object from queries that hit the object's
    // tight AABB but not the cell's loose AABB. Real pathological hazard.)
    //
    // Worst-case off-center: `|offset_from_cell_center| ≤ cell_extent / 2`.
    // Object fits in loose AABB iff
    //   `cell_extent/2 + extent/2 ≤ loosening × cell_extent / 2`
    // ⇒ `extent ≤ (loosening - 1) × cell_extent`
    // ⇒ `cell_extent ≥ extent / (loosening - 1)`
    // ⇒ `2^d ≤ (loosening - 1) × R / extent`
    //
    // For loosening = 2.0 (the default), `(loose - 1) = 1` ⇒ ratio = R / extent.
    // For loosening > 2, deeper placement is allowed (looser cells).
    // For loosening = 1.0, ratio = 0 ⇒ root-only placement (classical octree).
    //
    // This is the GUARANTEED-FAST-PATH formula: any update keeping the object's
    // tight AABB inside its cell's loose AABB after motion takes the fast path,
    // for ANY initial placement and any motion that keeps center in cell.
    const Vec3<T> root_ext = aabb_extent(m_root_bounds);
    const T fit_factor = m_loosening - T{1};

    auto axis_depth = [&](T ext, T root_ext_axis) -> i32 {
        if (ext <= T{0}) { return static_cast<i32>(m_max_depth); } // degenerate axis
        if (fit_factor <= T{0}) { return 0; } // loosening=1 ⇒ classical octree, root only
        const T ratio = fit_factor * root_ext_axis / ext;
        if (!(ratio > T{1})) { return 0; } // root only
        // d = floor(log2(ratio)). std::log2 is a boundary scalar use, OK in
        // builder code (geometry-spatial is not in the crd-no-std-math-check scope).
        const f64 d_f = std::log2(static_cast<f64>(ratio));
        if (d_f >= static_cast<f64>(m_max_depth)) { return static_cast<i32>(m_max_depth); }
        return static_cast<i32>(d_f);
    };

    const i32 dx = axis_depth(extent.x, root_ext.x);
    const i32 dy = axis_depth(extent.y, root_ext.y);
    const i32 dz = axis_depth(extent.z, root_ext.z);
    i32 d = std::min({dx, dy, dz});
    if (d < 0) { d = 0; }
    if (d > static_cast<i32>(m_max_depth)) { d = static_cast<i32>(m_max_depth); }
    return static_cast<u8>(d);
}

template <MathScalar T>
AABB3<T> LooseOctree<T>::loose_aabb_of(const OctreeNode<T>& node) const noexcept
{
    const Vec3<T> c = aabb_center(node.bounds);
    const Vec3<T> half = Vec3<T>{(node.bounds.max.x - node.bounds.min.x) * T{0.5},
                                   (node.bounds.max.y - node.bounds.min.y) * T{0.5},
                                   (node.bounds.max.z - node.bounds.min.z) * T{0.5}};
    const Vec3<T> loose_half{half.x * m_loosening, half.y * m_loosening, half.z * m_loosening};
    return AABB3<T>{Vec3<T>{c.x - loose_half.x, c.y - loose_half.y, c.z - loose_half.z},
                      Vec3<T>{c.x + loose_half.x, c.y + loose_half.y, c.z + loose_half.z}};
}

template <MathScalar T>
u32 LooseOctree<T>::descend_and_insert(u32 obj_idx, const Vec3<T>& center, u8 target_depth)
{
    u32 cell = m_root;
    u8 depth = 0;
    while (depth < target_depth)
    {
        const u8 oct = octant_of_center(m_nodes[cell].bounds, center);
        u32 child = m_nodes[cell].children[oct];
        if (child == k_null)
        {
            const AABB3<T> cb = child_bounds_of(m_nodes[cell].bounds, oct);
            child = allocate_node(cb, cell, static_cast<u8>(depth + 1U));
            // m_nodes may have reallocated — re-fetch parent reference is fine
            // through index access.
            m_nodes[cell].children[oct] = child;
        }
        cell = child;
        ++depth;
    }
    // Sanity: the object MUST fit in the terminal cell's loose AABB. The
    // guaranteed-fast-path formula in `target_depth_for` makes this true by
    // construction; this assert catches future regressions in either the
    // depth-pick formula or the loose-AABB geometry.
    CRD_ASSERT(aabb_encloses(loose_aabb_of(m_nodes[cell]), m_objects[obj_idx].aabb));
    cell_add_object(cell, obj_idx);
    return cell;
}

// =============================================================================
// Public mutators — insert / remove / update
// =============================================================================

template <MathScalar T>
OctreeObjectId LooseOctree<T>::insert(const AABB3<T>& aabb, u32 payload)
{
    // Builder-reject contract — non-finite, out-of-root center, or oversized.
    CRD_ASSERT(is_finite(aabb));
    const Vec3<T> center = aabb_center(aabb);
    const Vec3<T> extent = aabb_extent(aabb);
    CRD_ASSERT(center.x >= m_root_bounds.min.x && center.x <= m_root_bounds.max.x);
    CRD_ASSERT(center.y >= m_root_bounds.min.y && center.y <= m_root_bounds.max.y);
    CRD_ASSERT(center.z >= m_root_bounds.min.z && center.z <= m_root_bounds.max.z);
    [[maybe_unused]] const Vec3<T> root_ext = aabb_extent(m_root_bounds);
    CRD_ASSERT(extent.x <= m_loosening * root_ext.x);
    CRD_ASSERT(extent.y <= m_loosening * root_ext.y);
    CRD_ASSERT(extent.z <= m_loosening * root_ext.z);

    if (m_root == k_null)
    {
        m_root = allocate_node(m_root_bounds, k_null, 0U);
    }

    const u8 td = target_depth_for(extent);

    // Allocate object first (so descend can write its cell_node back-pointer).
    const u32 obj_idx = allocate_object(aabb, payload, k_invalid_node);
    descend_and_insert(obj_idx, center, td);
    return OctreeObjectId{obj_idx};
}

template <MathScalar T>
void LooseOctree<T>::remove(OctreeObjectId id)
{
    CRD_ASSERT(is_object_alive(id.value));
    const u32 cell = m_objects[id.value].cell_node;
    cell_remove_object(cell, id.value);
    free_object(id.value);
    // Cells are NOT auto-collapsed on empty (cheap in steady-state churn;
    // optional `compact()` follow-on if a consumer surfaces).
}

template <MathScalar T>
bool LooseOctree<T>::update(OctreeObjectId id, const AABB3<T>& new_aabb)
{
    CRD_ASSERT(is_object_alive(id.value));
    CRD_ASSERT(is_finite(new_aabb));

    ObjectEntry& obj = m_objects[id.value];
    const u32 cur_cell = obj.cell_node;
    CRD_ASSERT(is_node_alive(cur_cell));

    // Fast path — Ulrich's correctness invariant: the object stays correctly
    // findable as long as its tight AABB fits within its cell's loose AABB.
    // Center can drift outside the cell — only the AABB-fit matters.
    const AABB3<T> loose = loose_aabb_of(m_nodes[cur_cell]);
    if (aabb_encloses(loose, new_aabb))
    {
        obj.aabb = new_aabb;
        return false;
    }

    // Slow path — also re-validate root containment. (Tests + downstream
    // safety: an object whose new center leaves root or whose extent exceeds
    // the root×loosening cap is a precondition violation.)
    const Vec3<T> center = aabb_center(new_aabb);
    const Vec3<T> extent = aabb_extent(new_aabb);
    CRD_ASSERT(center.x >= m_root_bounds.min.x && center.x <= m_root_bounds.max.x);
    CRD_ASSERT(center.y >= m_root_bounds.min.y && center.y <= m_root_bounds.max.y);
    CRD_ASSERT(center.z >= m_root_bounds.min.z && center.z <= m_root_bounds.max.z);
    [[maybe_unused]] const Vec3<T> root_ext = aabb_extent(m_root_bounds);
    CRD_ASSERT(extent.x <= m_loosening * root_ext.x);
    CRD_ASSERT(extent.y <= m_loosening * root_ext.y);
    CRD_ASSERT(extent.z <= m_loosening * root_ext.z);

    cell_remove_object(cur_cell, id.value);
    obj.aabb = new_aabb;
    const u8 td = target_depth_for(extent);
    descend_and_insert(id.value, center, td);
    return true;
}

// =============================================================================
// overlap (Array sink) — convenience wrapper over the templated callback form
// =============================================================================

template <MathScalar T>
void LooseOctree<T>::overlap(const AABB3<T>& query, crd::containers::Array<u32>& out) const
{
    overlap(query, [&](u32 payload) { out.push_back(payload); });
}

// =============================================================================
// raycast — t-near-first descent + best_t pruning + payload-tiebreak
// =============================================================================

template <MathScalar T>
std::optional<crd::geometry::RayHit<u32>>
LooseOctree<T>::raycast(const Ray3<T>& ray, T tmax) const noexcept
{
    if (m_root == k_null) { return std::nullopt; }
    if (tmax <= T{0}) { return std::nullopt; }
    // Defensive NaN guard at the query surface — robust ray-AABB intrinsics
    // can return TRUE for non-finite inputs depending on order of operations
    // (NaN-vs-NaN comparisons are unspecified in the slab math). Symmetric
    // with v5a kd_radius / kd_range_aabb non-finite tolerance.
    if (!is_finite(ray.origin) || !is_finite(ray.direction)) { return std::nullopt; }

    // Ray-AABB precompute (sign + inv direction) — amortise across many AABB tests.
    // Note: precompute is f32-only today; cast for f64 path.
    if constexpr (!std::is_same_v<T, crd::f32>)
    {
        // f64 raycast: scalar slab traversal without precompute. Acceptable
        // until a f64 consumer surfaces (orbital aerospace etc). See debt note.
        struct DummyHit { f64 t; u32 payload; };
        DummyHit best{static_cast<f64>(tmax), 0xFFFFFFFFU};
        bool any = false;

        // Tail-call-style explicit stack (depth bounded by m_max_depth).
        u32 stack[64];
        usize sp = 0;
        stack[sp++] = m_root;
        while (sp > 0)
        {
            const u32 ni = stack[--sp];
            const OctreeNode<T>& node = m_nodes[ni];
            const AABB3<T> loose = loose_aabb_of(node);

            // Slab test (Williams/Ize style) — scalar f64.
            T tmin_loc = T{0};
            T tmax_loc = static_cast<T>(best.t);
            for (int ax = 0; ax < 3; ++ax)
            {
                const T o = ray.origin[static_cast<usize>(ax)];
                const T d = ray.direction[static_cast<usize>(ax)];
                const T lo = loose.min[static_cast<usize>(ax)];
                const T hi = loose.max[static_cast<usize>(ax)];
                if (std::abs(d) < std::numeric_limits<T>::epsilon())
                {
                    if (o < lo || o > hi) { tmin_loc = std::numeric_limits<T>::infinity(); break; }
                }
                else
                {
                    const T inv = T{1} / d;
                    T t1 = (lo - o) * inv;
                    T t2 = (hi - o) * inv;
                    if (t1 > t2) { const T tmp = t1; t1 = t2; t2 = tmp; }
                    if (t1 > tmin_loc) { tmin_loc = t1; }
                    if (t2 < tmax_loc) { tmax_loc = t2; }
                    if (tmin_loc > tmax_loc) { tmin_loc = std::numeric_limits<T>::infinity(); break; }
                }
            }
            if (tmin_loc > tmax_loc || tmin_loc >= static_cast<T>(best.t)) { continue; }

            // Local objects scan
            if (ni < m_cells.size())
            {
                const auto& list = m_cells[ni].ids;
                for (usize i = 0; i < list.size(); ++i)
                {
                    const u32 obj_idx = list[i];
                    const ObjectEntry& obj = m_objects[obj_idx];
                    T t_obj_min = T{0};
                    T t_obj_max = static_cast<T>(best.t);
                    bool hit = true;
                    for (int ax = 0; ax < 3; ++ax)
                    {
                        const T o = ray.origin[static_cast<usize>(ax)];
                        const T d = ray.direction[static_cast<usize>(ax)];
                        const T lo = obj.aabb.min[static_cast<usize>(ax)];
                        const T hi = obj.aabb.max[static_cast<usize>(ax)];
                        if (std::abs(d) < std::numeric_limits<T>::epsilon())
                        {
                            if (o < lo || o > hi) { hit = false; break; }
                        }
                        else
                        {
                            const T inv = T{1} / d;
                            T t1 = (lo - o) * inv;
                            T t2 = (hi - o) * inv;
                            if (t1 > t2) { const T tmp = t1; t1 = t2; t2 = tmp; }
                            if (t1 > t_obj_min) { t_obj_min = t1; }
                            if (t2 < t_obj_max) { t_obj_max = t2; }
                            if (t_obj_min > t_obj_max) { hit = false; break; }
                        }
                    }
                    if (!hit || t_obj_min < T{0}) { continue; }
                    const f64 t_d = static_cast<f64>(t_obj_min);
                    if (t_d < best.t)
                    {
                        best.t = t_d;
                        best.payload = obj.payload;
                        any = true;
                    }
                    else if (t_d == best.t && obj.payload < best.payload)
                    {
                        best.payload = obj.payload;
                        any = true;
                    }
                }
            }

            // Push children unordered (f64 path is reference; t-near ordering is f32-only optimisation).
            for (u8 oct = 0; oct < 8U; ++oct)
            {
                const u32 child = node.children[oct];
                if (child != k_null)
                {
                    CRD_ASSERT(sp < 64U);
                    stack[sp++] = child;
                }
            }
        }
        if (!any) { return std::nullopt; }
        return crd::geometry::RayHit<u32>{static_cast<f32>(best.t), best.payload};
    }
    else
    {
        // f32 path — use the existing robust ray-AABB precompute.
        const auto pre = crd::geometry::primitives::precompute_ray_aabb(ray);

        struct Frame { u32 cell; T t_near; };
        Frame stack[64];
        usize sp = 0;

        T best_t = tmax;
        u32 best_payload = 0xFFFFFFFFU;
        bool any = false;

        // Push root with its t_near computed once.
        T root_t = T{0};
        const AABB3<T> root_loose = loose_aabb_of(m_nodes[m_root]);
        if (!crd::geometry::primitives::intersect_ray_aabb_robust(ray, pre, root_loose, T{0}, best_t, root_t))
        {
            return std::nullopt;
        }
        stack[sp++] = Frame{m_root, root_t};

        while (sp > 0)
        {
            const Frame f = stack[--sp];
            if (f.t_near >= best_t) { continue; }

            const OctreeNode<T>& node = m_nodes[f.cell];

            // Local objects.
            if (f.cell < m_cells.size())
            {
                const auto& list = m_cells[f.cell].ids;
                for (usize i = 0; i < list.size(); ++i)
                {
                    const u32 obj_idx = list[i];
                    const ObjectEntry& obj = m_objects[obj_idx];
                    T t = T{0};
                    if (crd::geometry::primitives::intersect_ray_aabb_robust(ray, pre, obj.aabb, T{0}, best_t, t))
                    {
                        if (t < best_t)
                        {
                            best_t = t;
                            best_payload = obj.payload;
                            any = true;
                        }
                        else if (t == best_t && obj.payload < best_payload)
                        {
                            best_payload = obj.payload;
                            any = true;
                        }
                    }
                }
            }

            // Compute t_near for each allocated child, then push in reverse-
            // order (largest t_near first → smallest popped first).
            Frame child_frames[8];
            u32 nchild = 0;
            for (u8 oct = 0; oct < 8U; ++oct)
            {
                const u32 child = node.children[oct];
                if (child == k_null) { continue; }
                const AABB3<T> child_loose = loose_aabb_of(m_nodes[child]);
                T t_c = T{0};
                if (!crd::geometry::primitives::intersect_ray_aabb_robust(
                        ray, pre, child_loose, T{0}, best_t, t_c))
                {
                    continue;
                }
                child_frames[nchild++] = Frame{child, t_c};
            }
            // Sort by t_near DESCENDING — sp-pop yields ascending (nearest first).
            // 8 elements → insertion sort is the right tool.
            for (u32 i = 1; i < nchild; ++i)
            {
                Frame v = child_frames[i];
                u32 j = i;
                while (j > 0U && child_frames[j - 1U].t_near < v.t_near)
                {
                    child_frames[j] = child_frames[j - 1U];
                    --j;
                }
                child_frames[j] = v;
            }
            for (u32 i = 0; i < nchild; ++i)
            {
                CRD_ASSERT(sp < 64U);
                stack[sp++] = child_frames[i];
            }
        }

        if (!any) { return std::nullopt; }
        return crd::geometry::RayHit<u32>{best_t, best_payload};
    }
}

// =============================================================================
// Explicit instantiations
// =============================================================================

template class LooseOctree<f32>;
template class LooseOctree<f64>;

} // namespace crd::geometry::spatial
