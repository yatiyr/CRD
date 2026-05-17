// crd-geometry-spatial â€” `UniformGrid<T>` impl (Phase 3.1.7 v5e).
//
// Reference: classical uniform-grid acceleration; Eberly *3D Game Engine
// Design* Â§11.6. Voxel raycast: Amanatides & Woo 1987 + grid-bounds entry-
// clip via slab test.
//
// Header `uniform_grid.hpp` documents the design + locked decisions.

#include <crd/geometry/spatial/uniform_grid.hpp>

#include <crd/containers/sort.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::geometry::spatial
{
using crd::f32;
using crd::f64;
using crd::i32;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::Vec3;

namespace
{

// Signed-safe floor + cast (handles f64 source for f32 grids).
template <MathScalar T>
inline T floor_t(T v) noexcept
{
    return static_cast<T>(std::floor(static_cast<f64>(v)));
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

template <MathScalar T>
UniformGrid<T>::UniformGrid(crd::memory::IAllocator* alloc, const UniformGridConfig<T>& cfg)
    : m_alloc(alloc)
    , m_objects(alloc)
    , m_cells(alloc)
    , m_bounds(cfg.bounds)
    , m_cell_size(cfg.cell_size)
    , m_inv_cell_size(T{1} / cfg.cell_size)
{
    CRD_ASSERT(crd::geometry::primitives::is_finite(m_bounds));
    CRD_ASSERT(m_bounds.max.x > m_bounds.min.x);
    CRD_ASSERT(m_bounds.max.y > m_bounds.min.y);
    CRD_ASSERT(m_bounds.max.z > m_bounds.min.z);
    CRD_ASSERT(m_cell_size > T{0});

    // Compute (nx, ny, nz) via ceil((bounds_max - bounds_min) / cell_size).
    auto ceil_div = [&](T extent) -> u32 {
        const f64 raw = static_cast<f64>(extent) * static_cast<f64>(m_inv_cell_size);
        const u32 rounded = static_cast<u32>(std::ceil(raw));
        return rounded == 0U ? 1U : rounded;
    };
    m_nx = ceil_div(m_bounds.max.x - m_bounds.min.x);
    m_ny = ceil_div(m_bounds.max.y - m_bounds.min.y);
    m_nz = ceil_div(m_bounds.max.z - m_bounds.min.z);

    // Sanity cap: prevent accidental cosmic allocations. 1B cells Ã— 24 B
    // (Array<u32> overhead) = 24 GB â€” never intentional.
    const usize cell_count = static_cast<usize>(m_nx) * m_ny * m_nz;
    CRD_ASSERT(cell_count <= (1ULL << 28) && "UniformGrid: cell_count > 256M â€” pick a coarser cell_size or use SpatialHash for sparse domains");

    // Allocate flat cell array â€” each cell is a default-constructed empty
    // Array<u32> bound to our allocator. push_back into a cell allocates
    // its first capacity lazily on first insert into that cell.
    m_cells.resize(cell_count);
    for (usize i = 0; i < cell_count; ++i)
    {
        m_cells[i] = crd::containers::Array<u32>{alloc};
    }
}

// =============================================================================
// Cell range â€” clamped to grid bounds
// =============================================================================
//
// Returns the inclusive (min, max) cell range covering the AABB's intersection
// with the grid. Sets `empty_range = true` if the AABB lies wholly outside.

template <MathScalar T>
void UniformGrid<T>::aabb_cell_range(const AABB3<T>& a,
                                        u32& min_x, u32& min_y, u32& min_z,
                                        u32& max_x, u32& max_y, u32& max_z,
                                        bool& empty_range) const noexcept
{
    empty_range = false;

    auto compute_axis = [&](T amin, T amax, T bmin, T bmax, u32 n,
                             u32& out_min, u32& out_max) -> bool {
        // Clip to grid extent on this axis.
        const T lo = std::max(amin, bmin);
        const T hi = std::min(amax, bmax);
        if (lo > hi) { return false; } // wholly outside

        // Cell index of lo / hi relative to grid origin.
        const T rel_lo = lo - bmin;
        const T rel_hi = hi - bmin;
        i32 i_lo = static_cast<i32>(floor_t<T>(rel_lo * m_inv_cell_size));
        i32 i_hi = static_cast<i32>(floor_t<T>(rel_hi * m_inv_cell_size));
        // Clamp to [0, n-1] â€” handles boundary case where hi == grid max
        // (cell index would be n; collapse to n-1).
        if (i_lo < 0) { i_lo = 0; }
        if (i_hi < 0) { i_hi = 0; }
        if (i_lo >= static_cast<i32>(n)) { i_lo = static_cast<i32>(n) - 1; }
        if (i_hi >= static_cast<i32>(n)) { i_hi = static_cast<i32>(n) - 1; }
        out_min = static_cast<u32>(i_lo);
        out_max = static_cast<u32>(i_hi);
        return true;
    };

    if (!compute_axis(a.min.x, a.max.x, m_bounds.min.x, m_bounds.max.x, m_nx, min_x, max_x))
    {
        empty_range = true; return;
    }
    if (!compute_axis(a.min.y, a.max.y, m_bounds.min.y, m_bounds.max.y, m_ny, min_y, max_y))
    {
        empty_range = true; return;
    }
    if (!compute_axis(a.min.z, a.max.z, m_bounds.min.z, m_bounds.max.z, m_nz, min_z, max_z))
    {
        empty_range = true; return;
    }
}

// =============================================================================
// Generation counter (overflow-safe)
// =============================================================================

template <MathScalar T>
u64 UniformGrid<T>::next_query_generation() const noexcept
{
    if (m_query_generation == std::numeric_limits<u64>::max())
    {
        for (usize i = 0; i < m_objects.size(); ++i)
        {
            const_cast<ObjectEntry&>(m_objects[i]).last_query_gen = 0;
        }
        m_query_generation = 0;
    }
    ++m_query_generation;
    return m_query_generation;
}

// =============================================================================
// Object pool
// =============================================================================

template <MathScalar T>
u32 UniformGrid<T>::allocate_object(const AABB3<T>& aabb, u32 payload)
{
    u32 idx;
    if (m_object_free_list != k_invalid_handle)
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
    obj.last_query_gen = 0;
    obj.next_free = k_invalid_handle;
    obj.alive = true;
    ++m_object_count;
    return idx;
}

template <MathScalar T>
void UniformGrid<T>::free_object(u32 idx)
{
    CRD_ASSERT(is_object_alive(idx));
    ObjectEntry& obj = m_objects[idx];
    obj.alive = false;
    obj.next_free = m_object_free_list;
    m_object_free_list = idx;
    --m_object_count;
}

// =============================================================================
// Cell â†” object plumbing
// =============================================================================

template <MathScalar T>
void UniformGrid<T>::insert_into_cells(u32 obj_idx, const AABB3<T>& aabb)
{
    u32 min_x;
    u32 min_y;
    u32 min_z;
    u32 max_x;
    u32 max_y;
    u32 max_z;
    bool empty_range = false;
    aabb_cell_range(aabb, min_x, min_y, min_z, max_x, max_y, max_z, empty_range);
    if (empty_range) { return; }

    for (u32 iz = min_z; iz <= max_z; ++iz)
    for (u32 iy = min_y; iy <= max_y; ++iy)
    for (u32 ix = min_x; ix <= max_x; ++ix)
    {
        m_cells[cell_index(ix, iy, iz)].push_back(obj_idx);
    }
}

template <MathScalar T>
void UniformGrid<T>::remove_from_cells(u32 obj_idx, const AABB3<T>& aabb)
{
    u32 min_x;
    u32 min_y;
    u32 min_z;
    u32 max_x;
    u32 max_y;
    u32 max_z;
    bool empty_range = false;
    aabb_cell_range(aabb, min_x, min_y, min_z, max_x, max_y, max_z, empty_range);
    if (empty_range) { return; }

    for (u32 iz = min_z; iz <= max_z; ++iz)
    for (u32 iy = min_y; iy <= max_y; ++iy)
    for (u32 ix = min_x; ix <= max_x; ++ix)
    {
        auto& cell = m_cells[cell_index(ix, iy, iz)];
        // Swap-with-last removal â€” bounded by per-cell occupancy.
        for (usize i = 0; i < cell.size(); ++i)
        {
            if (cell[i] == obj_idx)
            {
                cell[i] = cell[cell.size() - 1];
                cell.resize(cell.size() - 1);
                break;
            }
        }
    }
}

// =============================================================================
// Public mutators
// =============================================================================

template <MathScalar T>
UniformGridObjectId UniformGrid<T>::insert(const AABB3<T>& aabb, u32 payload)
{
    CRD_ASSERT(crd::geometry::primitives::is_finite(aabb));
    CRD_ASSERT(aabb.min.x <= aabb.max.x && aabb.min.y <= aabb.max.y && aabb.min.z <= aabb.max.z);

    const u32 idx = allocate_object(aabb, payload);
    insert_into_cells(idx, aabb);
    return UniformGridObjectId{idx};
}

template <MathScalar T>
void UniformGrid<T>::remove(UniformGridObjectId id)
{
    CRD_ASSERT(is_object_alive(id.value));
    const AABB3<T> old_aabb = m_objects[id.value].aabb;
    remove_from_cells(id.value, old_aabb);
    free_object(id.value);
}

template <MathScalar T>
bool UniformGrid<T>::update(UniformGridObjectId id, const AABB3<T>& new_aabb)
{
    CRD_ASSERT(is_object_alive(id.value));
    CRD_ASSERT(crd::geometry::primitives::is_finite(new_aabb));
    CRD_ASSERT(new_aabb.min.x <= new_aabb.max.x);

    ObjectEntry& obj = m_objects[id.value];

    u32 omin_x;
    u32 omin_y;
    u32 omin_z;
    u32 omax_x;
    u32 omax_y;
    u32 omax_z;
    bool oempty = false;
    aabb_cell_range(obj.aabb, omin_x, omin_y, omin_z, omax_x, omax_y, omax_z, oempty);
    u32 nmin_x;
    u32 nmin_y;
    u32 nmin_z;
    u32 nmax_x;
    u32 nmax_y;
    u32 nmax_z;
    bool nempty = false;
    aabb_cell_range(new_aabb, nmin_x, nmin_y, nmin_z, nmax_x, nmax_y, nmax_z, nempty);

    // Same cell range (or both empty / both wholly outside) â‡’ fast path.
    if (oempty == nempty
        && (oempty
            || (omin_x == nmin_x && omin_y == nmin_y && omin_z == nmin_z
                && omax_x == nmax_x && omax_y == nmax_y && omax_z == nmax_z)))
    {
        obj.aabb = new_aabb;
        return false;
    }

    remove_from_cells(id.value, obj.aabb);
    obj.aabb = new_aabb;
    insert_into_cells(id.value, new_aabb);
    return true;
}

// =============================================================================
// Diagnostics + access
// =============================================================================

template <MathScalar T>
usize UniformGrid<T>::max_cell_size() const noexcept
{
    usize m = 0;
    for (usize i = 0; i < m_cells.size(); ++i)
    {
        if (m_cells[i].size() > m) { m = m_cells[i].size(); }
    }
    return m;
}

template <MathScalar T>
f32 UniformGrid<T>::load_factor() const noexcept
{
    if (m_cells.size() == 0) { return 0.0F; }
    usize total = 0;
    for (usize i = 0; i < m_cells.size(); ++i) { total += m_cells[i].size(); }
    return static_cast<f32>(total) / static_cast<f32>(m_cells.size());
}

template <MathScalar T>
AABB3<T> UniformGrid<T>::object_aabb(UniformGridObjectId id) const noexcept
{
    CRD_ASSERT(is_object_alive(id.value));
    return m_objects[id.value].aabb;
}

template <MathScalar T>
u32 UniformGrid<T>::object_payload(UniformGridObjectId id) const noexcept
{
    CRD_ASSERT(is_object_alive(id.value));
    return m_objects[id.value].payload;
}

// =============================================================================
// overlap / radius â€” Array sink overloads (delegate to inline templates)
// =============================================================================

template <MathScalar T>
void UniformGrid<T>::overlap(const AABB3<T>& query, crd::containers::Array<u32>& out) const
{
    overlap(query, [&](u32 payload) { out.push_back(payload); });
}

template <MathScalar T>
void UniformGrid<T>::overlap(const AABB3<T>& query, UniformGridScratch& scratch,
                              crd::containers::Array<u32>& out) const
{
    overlap(query, scratch, [&](u32 payload) { out.push_back(payload); });
}

template <MathScalar T>
void UniformGrid<T>::radius(const Vec3<T>& point, T r, crd::containers::Array<u32>& out) const
{
    radius(point, r, [&](u32 payload) { out.push_back(payload); });
}

template <MathScalar T>
void UniformGrid<T>::radius(const Vec3<T>& point, T r, UniformGridScratch& scratch,
                              crd::containers::Array<u32>& out) const
{
    radius(point, r, scratch, [&](u32 payload) { out.push_back(payload); });
}

// =============================================================================
// raycast â€” Amanatides-Woo voxel traversal with grid-bounds entry-clip
// =============================================================================
//
// Setup:
//   1. Slab-test the ray vs the grid AABB. If miss, return nullopt.
//   2. tEntry = max(0, t at which ray enters grid).
//   3. Entry point P = origin + tEntry * direction.
//   4. Start cell (ix, iy, iz) = floor((P - bounds.min) / cell_size), clamped.
//   5. step / tDelta / tMax per axis as in Amanatides-Woo.
// Walk:
//   6. Scan cell at (ix, iy, iz). Update best_t.
//   7. Advance ALL axes whose tMax ties for minimum (corner-grazing safe).
//   8. Stop when (ix, iy, iz) leaves grid OR best_t < t_next OR step cap.

template <MathScalar T>
std::optional<crd::geometry::RayHit<u32>>
UniformGrid<T>::raycast(const Ray3<T>& ray, T tmax) const noexcept
{
    if (m_object_count == 0) { return std::nullopt; }
    if (tmax <= T{0}) { return std::nullopt; }
    if (!crd::geometry::primitives::is_finite(ray.origin)
        || !crd::geometry::primitives::is_finite(ray.direction))
    {
        return std::nullopt;
    }
    if (ray.direction.x == T{0} && ray.direction.y == T{0} && ray.direction.z == T{0})
    {
        return std::nullopt;
    }
    const u64 gen = next_query_generation();
    auto was_visited = [this, gen](u32 obj_idx) noexcept -> bool {
        return m_objects[obj_idx].last_query_gen == gen;
    };
    auto mark_visited = [this, gen](u32 obj_idx) noexcept {
        const_cast<ObjectEntry&>(m_objects[obj_idx]).last_query_gen = gen;
    };
    return raycast_traverse_(ray, tmax, was_visited, mark_visited);
}

template <MathScalar T>
std::optional<crd::geometry::RayHit<u32>>
UniformGrid<T>::raycast(const Ray3<T>& ray, UniformGridScratch& scratch, T tmax) const noexcept
{
    if (m_object_count == 0) { return std::nullopt; }
    if (tmax <= T{0}) { return std::nullopt; }
    if (!crd::geometry::primitives::is_finite(ray.origin)
        || !crd::geometry::primitives::is_finite(ray.direction))
    {
        return std::nullopt;
    }
    if (ray.direction.x == T{0} && ray.direction.y == T{0} && ray.direction.z == T{0})
    {
        return std::nullopt;
    }
    const u64 gen = scratch.prepare_for_query(m_objects.size());
    auto was_visited = [&scratch, gen](u32 obj_idx) noexcept -> bool {
        return scratch.was_visited(obj_idx, gen);
    };
    auto mark_visited = [&scratch, gen](u32 obj_idx) noexcept {
        scratch.mark_visited(obj_idx, gen);
    };
    return raycast_traverse_(ray, tmax, was_visited, mark_visited);
}

template <MathScalar T>
template <typename WasVisited, typename MarkVisited>
std::optional<crd::geometry::RayHit<u32>>
UniformGrid<T>::raycast_traverse_(const Ray3<T>& ray, T tmax,
                                     WasVisited& was_visited,
                                     MarkVisited& mark_visited) const noexcept
{
    constexpr T kInf = std::numeric_limits<T>::infinity();

    // Slab test ray vs grid bounds. Compute (t_entry, t_exit_grid).
    T t_entry_grid = T{0};
    T t_exit_grid = tmax;
    for (int ax = 0; ax < 3; ++ax)
    {
        const T o = ray.origin[static_cast<usize>(ax)];
        const T d = ray.direction[static_cast<usize>(ax)];
        const T lo = m_bounds.min[static_cast<usize>(ax)];
        const T hi = m_bounds.max[static_cast<usize>(ax)];
        if (std::abs(d) < std::numeric_limits<T>::epsilon())
        {
            if (o < lo || o > hi) { return std::nullopt; }
        }
        else
        {
            const T inv = T{1} / d;
            T t1 = (lo - o) * inv;
            T t2 = (hi - o) * inv;
            if (t1 > t2) { const T tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > t_entry_grid) { t_entry_grid = t1; }
            if (t2 < t_exit_grid)  { t_exit_grid = t2; }
            if (t_entry_grid > t_exit_grid) { return std::nullopt; }
        }
    }
    if (t_entry_grid >= tmax || t_exit_grid <= T{0}) { return std::nullopt; }
    // Clamp entry to non-negative (rays starting inside the grid have t_entry < 0
    // from the slab math; we want to start scanning at the origin's cell).
    const T t_entry = std::max(T{0}, t_entry_grid);

    // Entry point in grid-relative coords.
    const Vec3<T> entry{
        ray.origin.x + t_entry * ray.direction.x - m_bounds.min.x,
        ray.origin.y + t_entry * ray.direction.y - m_bounds.min.y,
        ray.origin.z + t_entry * ray.direction.z - m_bounds.min.z};

    // Start cell. Clamp to [0, n-1] â€” entry can be exactly at grid_max which
    // would yield cell n; pull back to n-1 (we still scan that cell on entry).
    auto clamp_cell = [&](T v, u32 n) -> i32 {
        i32 c = static_cast<i32>(floor_t<T>(v * m_inv_cell_size));
        if (c < 0) { c = 0; }
        if (c >= static_cast<i32>(n)) { c = static_cast<i32>(n) - 1; }
        return c;
    };
    i32 ix = clamp_cell(entry.x, m_nx);
    i32 iy = clamp_cell(entry.y, m_ny);
    i32 iz = clamp_cell(entry.z, m_nz);

    // Step direction per axis.
    auto sign_step = [](T d) noexcept -> i32 {
        if (d > T{0}) { return 1; }
        if (d < T{0}) { return -1; }
        return 0;
    };
    const i32 step_x = sign_step(ray.direction.x);
    const i32 step_y = sign_step(ray.direction.y);
    const i32 step_z = sign_step(ray.direction.z);

    // tDelta per axis = parametric distance to traverse one cell along that axis.
    const T tdelta_x = (step_x != 0) ? std::abs(m_cell_size / ray.direction.x) : kInf;
    const T tdelta_y = (step_y != 0) ? std::abs(m_cell_size / ray.direction.y) : kInf;
    const T tdelta_z = (step_z != 0) ? std::abs(m_cell_size / ray.direction.z) : kInf;

    // tMax per axis = parametric t at which ray crosses NEXT cell boundary
    // (relative to the world origin, NOT t_entry). For step_x > 0: next x
    // boundary at bounds.min.x + (ix+1) * cell_size; for step_x < 0:
    // bounds.min.x + ix * cell_size.
    auto initial_tmax = [&](T origin_a, T dir_a, T bmin_a, i32 ia, i32 step_a) -> T {
        if (step_a == 0) { return kInf; }
        const T boundary = (step_a > 0) ? bmin_a + static_cast<T>(ia + 1) * m_cell_size
                                          : bmin_a + static_cast<T>(ia) * m_cell_size;
        return (boundary - origin_a) / dir_a;
    };
    T tmax_x = initial_tmax(ray.origin.x, ray.direction.x, m_bounds.min.x, ix, step_x);
    T tmax_y = initial_tmax(ray.origin.y, ray.direction.y, m_bounds.min.y, iy, step_y);
    T tmax_z = initial_tmax(ray.origin.z, ray.direction.z, m_bounds.min.z, iz, step_z);

    T best_t = tmax;
    u32 best_payload = 0xFFFFFFFFU;
    bool any = false;

    // Per-object slab raycast (scalar â€” same as v5b/v5d's f64 path).
    auto ray_aabb = [&](const AABB3<T>& a, T& out_t) noexcept -> bool {
        T tmin_loc = T{0};
        T tcur_max = best_t;
        for (int ax = 0; ax < 3; ++ax)
        {
            const T o = ray.origin[static_cast<usize>(ax)];
            const T d = ray.direction[static_cast<usize>(ax)];
            const T lo = a.min[static_cast<usize>(ax)];
            const T hi = a.max[static_cast<usize>(ax)];
            if (std::abs(d) < std::numeric_limits<T>::epsilon())
            {
                if (o < lo || o > hi) { return false; }
            }
            else
            {
                const T inv = T{1} / d;
                T t1 = (lo - o) * inv;
                T t2 = (hi - o) * inv;
                if (t1 > t2) { const T tmp = t1; t1 = t2; t2 = tmp; }
                if (t1 > tmin_loc) { tmin_loc = t1; }
                if (t2 < tcur_max) { tcur_max = t2; }
                if (tmin_loc > tcur_max) { return false; }
            }
        }
        if (tmin_loc < T{0}) { return false; }
        out_t = tmin_loc;
        return true;
    };

    auto scan_cell = [&]() noexcept {
        const auto& cell = m_cells[cell_index(static_cast<u32>(ix), static_cast<u32>(iy), static_cast<u32>(iz))];
        for (usize i = 0; i < cell.size(); ++i)
        {
            const u32 obj_idx = cell[i];
            if (was_visited(obj_idx)) { continue; }
            mark_visited(obj_idx);
            const ObjectEntry& obj = m_objects[obj_idx];
            T t = T{0};
            if (!ray_aabb(obj.aabb, t)) { continue; }
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
    };

    // Walk cells, bounded by grid + step cap.
    constexpr usize kMaxVoxelSteps = 1U << 22; // 4M cells worst case
    usize steps = 0;
    while (steps++ < kMaxVoxelSteps)
    {
        scan_cell();

        const T t_next = std::min({tmax_x, tmax_y, tmax_z});
        if (t_next > best_t) { break; }
        if (t_next > tmax)   { break; }

        // Advance ALL axes whose tMax ties for minimum (corner-grazing safe).
        const T t_min = t_next;
        if (tmax_x == t_min) { ix += step_x; tmax_x += tdelta_x; }
        if (tmax_y == t_min) { iy += step_y; tmax_y += tdelta_y; }
        if (tmax_z == t_min) { iz += step_z; tmax_z += tdelta_z; }

        // Exited grid bounds on any axis â‡’ stop.
        if (ix < 0 || ix >= static_cast<i32>(m_nx)) { break; }
        if (iy < 0 || iy >= static_cast<i32>(m_ny)) { break; }
        if (iz < 0 || iz >= static_cast<i32>(m_nz)) { break; }
    }

    if (!any) { return std::nullopt; }
    return crd::geometry::RayHit<u32>{static_cast<f32>(best_t), best_payload};
}

// =============================================================================
// find_overlapping_pairs â€” broadphase
// =============================================================================

template <MathScalar T>
void UniformGrid<T>::find_overlapping_pairs(crd::containers::Array<UniformGridPair>& out) const
{
    out.clear();
    if (m_object_count < 2) { return; }

    auto aabb_isect = [](const AABB3<T>& a, const AABB3<T>& b) noexcept {
        return a.min.x <= b.max.x && a.max.x >= b.min.x
            && a.min.y <= b.max.y && a.max.y >= b.min.y
            && a.min.z <= b.max.z && a.max.z >= b.min.z;
    };

    for (usize ci = 0; ci < m_cells.size(); ++ci)
    {
        const auto& cell = m_cells[ci];
        const usize n = cell.size();
        if (n < 2) { continue; }
        for (usize i = 0; i < n; ++i)
        {
            const u32 ai = cell[i];
            const ObjectEntry& a = m_objects[ai];
            for (usize j = i + 1; j < n; ++j)
            {
                const u32 bj = cell[j];
                const ObjectEntry& b = m_objects[bj];
                if (!aabb_isect(a.aabb, b.aabb)) { continue; }
                const u32 lo = a.payload < b.payload ? a.payload : b.payload;
                const u32 hi = a.payload < b.payload ? b.payload : a.payload;
                out.push_back(UniformGridPair{lo, hi});
            }
        }
    }

    crd::containers::sort(out.data(), out.data() + out.size(),
                            [](const UniformGridPair& x, const UniformGridPair& y) { return x < y; });
    if (out.size() > 1)
    {
        usize w = 1;
        for (usize r = 1; r < out.size(); ++r)
        {
            if (!(out[r] == out[r - 1U]))
            {
                if (w != r) { out[w] = out[r]; }
                ++w;
            }
        }
        out.resize(w);
    }
}

// =============================================================================
// Explicit instantiations
// =============================================================================

template class UniformGrid<f32>;
template class UniformGrid<f64>;

} // namespace crd::geometry::spatial
