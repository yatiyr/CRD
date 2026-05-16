#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — `UniformGrid<T>`, dense bounded-domain AABB index.
// Phase 3.1.7 v5e. Reference: classical uniform-grid acceleration; Eberly,
// *3D Game Engine Design* §11.6 (uniform grid broadphase). Voxel raycast:
// Amanatides & Woo, *A Fast Voxel Traversal Algorithm for Ray Tracing*,
// Eurographics 1987 — adapted with grid-bounds entry-clip.
//
// **Distinct from `SpatialHash` (v5d)**: dense flat array of cells, no hash,
// no collision penalty, O(cell_count) memory regardless of population. The
// right tool when (a) the spatial domain is bounded + KNOWN at construction,
// (b) the cell count is small enough that O(cell_count) overhead is
// acceptable (rule of thumb: ≤ 1M cells), and (c) max-uniform-density
// pattern (every cell roughly populated) defeats hash's O(1) advantage.
//
// Use cases: cooked-level cell grids, voxel scenes, uniform particle
// emitters in bounded volume, CFD coarse cells, pathfinding occupancy grids.
//
// ── Bounded domain ─────────────────────────────────────────────────────────
//
// Caller specifies AABB `bounds` + `cell_size` at construction. Grid
// dimensions: `(nx, ny, nz) = ceil((bounds.max - bounds.min) / cell_size)`.
// Total cell count = `nx · ny · nz`. Flat cell index = `(z·ny + y)·nx + x`.
//
// Out-of-bounds AABBs are CLAMPED to the grid (insertion silently restricted
// to overlapping cells). An object whose AABB lies wholly outside the grid
// touches no cells — it's still inserted (handle valid, queryable via
// handle accessors), but no query can find it spatially. Queries with
// out-of-bounds query inputs are similarly clamped. This keeps the contract
// simple: domain bounds are an optimisation hint, not a strict membership.
//
// ── Storage ────────────────────────────────────────────────────────────────
//
//   * `m_objects`  — free-list-managed pool of `{aabb, payload, last_query_gen,
//                    next_free, alive}`. Stable handle = pool slot.
//   * `m_cells`    — flat `Array<Array<u32>>` sized `nx·ny·nz`. Each cell
//                    is `Array<u32 obj_idx>` lazily allocated on first insert.
//   * Bounds + cell_size + (nx,ny,nz) stored.
//
// ── Per-query generation counter for dedup (zero-allocation) ──────────────
//
// Same elite trick as `SpatialHash` (v5d): each object has `last_query_gen`;
// query bumps a `mutable u64 m_query_generation`; visiting an object whose
// `last_query_gen == current` means already-emitted-this-query.
//
// ── Thread-safety: TWO API surfaces (same pattern as v5d-fast) ────────────
//
//   1. **Convenience (single-thread)** — `overlap(q, on_hit)` etc. Tree owns
//      `mutable u64` counter. Zero alloc, dead-simple, NOT thread-safe.
//
//   2. **Thread-safe (caller scratch)** — `overlap(q, scratch, on_hit)` etc.
//      Caller provides a `UniformGridScratch` (one per worker fiber/thread).
//      Scratch owns the per-object generation array; tree's mutable state
//      never touched. Multiple fibers each holding their own scratch can
//      query concurrently with no race.
//
// Both surfaces share `*_traverse_` template helpers parameterised over
// `WasVisited`/`MarkVisited` policy lambdas — provably equivalent dedup +
// emission. Same template-policy pattern as v5d-fast.
//
// `find_overlapping_pairs` is purely-read const — already thread-safe.
//
// ── Queries ────────────────────────────────────────────────────────────────
//
//   * `overlap(aabb, on_hit)` — clamp query to grid; canonical (z,y,x)
//     traversal; dedup; AABB-vs-AABB filter.
//   * `radius(point, r, on_hit)` — clamp sphere bbox; dedup; point-aabb-d²
//     filter.
//   * `raycast(ray)` — **grid-bounds-clipped Amanatides-Woo voxel
//     traversal**: clip ray to grid AABB (entry t), walk cells from entry
//     in spatial order, stop when cell index leaves grid OR `best_t`
//     pruning. Lowest-payload tiebreak on equal `t` (§4 pin #11). The
//     grid-bounds clip is the v5e differentiator vs v5d: SpatialHash uses
//     unbounded signed cell coords; UniformGrid bounds the walk by
//     construction.
//   * `find_overlapping_pairs(out)` — dense linear cell scan (no hash
//     overhead); emit `(min, max)` pairs; sort+unique to dedup cross-cell
//     duplicates. The eylem v3 XPBD soft-body broadphase target when the
//     domain is a bounded simulation box.
//
// Builder REJECTS non-finite + inverted AABB (debug `CRD_ASSERT`); queries
// TOLERATE non-finite + zero-direction-ray (defensive `is_finite` short-
// circuit at API surface).
//
// ── Two-layer typing (ADR-0078 §5 D34) ─────────────────────────────────────
// Algorithm body is raw `<MathScalar T>`. Typed wrappers in
// `grid_queries_typed.hpp` strip-compute-retag at the API surface.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>
#include <optional>

namespace crd::geometry::spatial
{
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::math::MathScalar;
using crd::math::Vec3;

// Stable handle for an object in a `UniformGrid`.
struct UniformGridObjectId
{
    static constexpr crd::u32 k_invalid = 0xFFFFFFFFU;
    crd::u32 value{k_invalid};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != k_invalid; }
    [[nodiscard]] friend constexpr bool operator==(UniformGridObjectId a, UniformGridObjectId b) noexcept
    {
        return a.value == b.value;
    }
};

template <MathScalar T>
struct UniformGridConfig
{
    AABB3<T> bounds{};       // domain — must be finite + min < max on every axis
    T        cell_size{T{1}}; // edge length per cell
};

// Caller-owned scratch for thread-safe query overloads. One per worker
// fiber / thread. Same design as `SpatialHashScratch`: per-object generation
// counter + monotonic `u64 current_gen` + overflow-protect linear reset.
class UniformGridScratch
{
public:
    explicit UniformGridScratch(crd::memory::IAllocator* alloc) noexcept : m_per_object_gen(alloc) {}

    UniformGridScratch(const UniformGridScratch&) = delete;
    UniformGridScratch& operator=(const UniformGridScratch&) = delete;
    UniformGridScratch(UniformGridScratch&&) noexcept = default;
    UniformGridScratch& operator=(UniformGridScratch&&) noexcept = default;
    ~UniformGridScratch() = default;

    [[nodiscard]] crd::usize capacity() const noexcept { return m_per_object_gen.size(); }

    crd::u64 prepare_for_query(crd::usize object_capacity)
    {
        if (m_per_object_gen.size() < object_capacity)
        {
            const crd::usize old = m_per_object_gen.size();
            m_per_object_gen.resize(object_capacity);
            for (crd::usize i = old; i < object_capacity; ++i) { m_per_object_gen[i] = 0; }
        }
        if (m_current_gen == std::numeric_limits<crd::u64>::max())
        {
            for (crd::usize i = 0; i < m_per_object_gen.size(); ++i) { m_per_object_gen[i] = 0; }
            m_current_gen = 0;
        }
        ++m_current_gen;
        return m_current_gen;
    }

    [[nodiscard]] bool was_visited(crd::u32 obj_idx, crd::u64 gen) const noexcept
    {
        return m_per_object_gen[obj_idx] == gen;
    }

    void mark_visited(crd::u32 obj_idx, crd::u64 gen) noexcept
    {
        m_per_object_gen[obj_idx] = gen;
    }

private:
    crd::containers::Array<crd::u64> m_per_object_gen;
    crd::u64                          m_current_gen{0};
};

struct UniformGridPair
{
    crd::u32 a;
    crd::u32 b;

    [[nodiscard]] friend constexpr bool operator==(UniformGridPair x, UniformGridPair y) noexcept
    {
        return x.a == y.a && x.b == y.b;
    }
    [[nodiscard]] friend constexpr bool operator<(UniformGridPair x, UniformGridPair y) noexcept
    {
        return x.a != y.a ? x.a < y.a : x.b < y.b;
    }
};

template <MathScalar T>
class UniformGrid
{
public:
    UniformGrid(crd::memory::IAllocator* alloc, const UniformGridConfig<T>& cfg);

    UniformGrid(const UniformGrid&) = delete;
    UniformGrid& operator=(const UniformGrid&) = delete;
    UniformGrid(UniformGrid&&) noexcept = default;
    UniformGrid& operator=(UniformGrid&&) noexcept = default;
    ~UniformGrid() = default;

    // ---- mutation -----------------------------------------------------------

    [[nodiscard]] UniformGridObjectId insert(const AABB3<T>& aabb, crd::u32 payload);
    void remove(UniformGridObjectId id);

    // Update an object's AABB. If the new AABB clamps to the SAME cell
    // range as the old AABB, the call is a no-op (returns false). Otherwise
    // remove from old cells + insert into new (returns true).
    bool update(UniformGridObjectId id, const AABB3<T>& new_aabb);

    // ---- access -------------------------------------------------------------

    [[nodiscard]] bool is_empty() const noexcept { return m_object_count == 0; }
    [[nodiscard]] crd::usize object_count() const noexcept { return m_object_count; }
    [[nodiscard]] crd::u32 nx() const noexcept { return m_nx; }
    [[nodiscard]] crd::u32 ny() const noexcept { return m_ny; }
    [[nodiscard]] crd::u32 nz() const noexcept { return m_nz; }
    [[nodiscard]] crd::usize cell_count() const noexcept { return m_cells.size(); }
    [[nodiscard]] AABB3<T> bounds() const noexcept { return m_bounds; }
    [[nodiscard]] T cell_size() const noexcept { return m_cell_size; }
    [[nodiscard]] AABB3<T> object_aabb(UniformGridObjectId id) const noexcept;
    [[nodiscard]] crd::u32 object_payload(UniformGridObjectId id) const noexcept;

    // Largest cell occupancy + total entries / cell count.
    [[nodiscard]] crd::usize max_cell_size() const noexcept;
    [[nodiscard]] crd::f32   load_factor() const noexcept;

    // ---- queries — convenience (single-thread) ----

    template <typename Fn> void overlap(const AABB3<T>& query, Fn&& on_hit) const;
    void overlap(const AABB3<T>& query, crd::containers::Array<crd::u32>& out) const;

    template <typename Fn> void radius(const Vec3<T>& point, T r, Fn&& on_hit) const;
    void radius(const Vec3<T>& point, T r, crd::containers::Array<crd::u32>& out) const;

    [[nodiscard]] std::optional<crd::geometry::RayHit<crd::u32>>
    raycast(const Ray3<T>& ray,
            T tmax = std::numeric_limits<T>::infinity()) const noexcept;

    // ---- queries — thread-safe (caller scratch) ----

    template <typename Fn> void overlap(const AABB3<T>& query, UniformGridScratch& scratch, Fn&& on_hit) const;
    void overlap(const AABB3<T>& query, UniformGridScratch& scratch, crd::containers::Array<crd::u32>& out) const;

    template <typename Fn> void radius(const Vec3<T>& point, T r, UniformGridScratch& scratch, Fn&& on_hit) const;
    void radius(const Vec3<T>& point, T r, UniformGridScratch& scratch, crd::containers::Array<crd::u32>& out) const;

    [[nodiscard]] std::optional<crd::geometry::RayHit<crd::u32>>
    raycast(const Ray3<T>& ray, UniformGridScratch& scratch,
            T tmax = std::numeric_limits<T>::infinity()) const noexcept;

    // ---- broadphase pair query (already const-safe — no scratch needed) ---

    void find_overlapping_pairs(crd::containers::Array<UniformGridPair>& out) const;

private:
    static constexpr crd::u32 k_invalid_handle = 0xFFFFFFFFU;

    struct ObjectEntry
    {
        AABB3<T>  aabb{};
        crd::u32  payload{0};
        crd::u64  last_query_gen{0};
        crd::u32  next_free{k_invalid_handle};
        bool      alive{false};
    };

    // Cell coords from world position, clamped to grid extent. Returns
    // half-open range; if the AABB lies wholly outside the grid, returns
    // an empty range (min > max).
    void aabb_cell_range(const AABB3<T>& a, crd::u32& min_x, crd::u32& min_y, crd::u32& min_z,
                                              crd::u32& max_x, crd::u32& max_y, crd::u32& max_z,
                                              bool& empty_range) const noexcept;

    [[nodiscard]] constexpr crd::usize cell_index(crd::u32 x, crd::u32 y, crd::u32 z) const noexcept
    {
        return (static_cast<crd::usize>(z) * m_ny + y) * m_nx + x;
    }

    // Pool helpers.
    [[nodiscard]] crd::u32 allocate_object(const AABB3<T>& aabb, crd::u32 payload);
    void free_object(crd::u32 idx);
    [[nodiscard]] bool is_object_alive(crd::u32 idx) const noexcept
    {
        return idx < m_objects.size() && m_objects[idx].alive;
    }

    void insert_into_cells(crd::u32 obj_idx, const AABB3<T>& aabb);
    void remove_from_cells(crd::u32 obj_idx, const AABB3<T>& aabb);

    [[nodiscard]] crd::u64 next_query_generation() const noexcept;

    // Shared traversal templates — same policy-lambda pattern as SpatialHash.
    template <typename WasVisited, typename MarkVisited, typename Fn>
    void overlap_traverse_(const AABB3<T>& query,
                            WasVisited& was_visited, MarkVisited& mark_visited,
                            Fn&& on_hit) const;
    template <typename WasVisited, typename MarkVisited, typename Fn>
    void radius_traverse_(const Vec3<T>& point, T r,
                           WasVisited& was_visited, MarkVisited& mark_visited,
                           Fn&& on_hit) const;
    template <typename WasVisited, typename MarkVisited>
    [[nodiscard]] std::optional<crd::geometry::RayHit<crd::u32>>
    raycast_traverse_(const Ray3<T>& ray, T tmax,
                       WasVisited& was_visited, MarkVisited& mark_visited) const noexcept;

    // ---- storage ------------------------------------------------------------
    crd::memory::IAllocator*                                  m_alloc{nullptr};
    crd::containers::Array<ObjectEntry>                       m_objects;
    crd::containers::Array<crd::containers::Array<crd::u32>>  m_cells;
    AABB3<T>                                                  m_bounds{};
    T                                                          m_cell_size{T{1}};
    T                                                          m_inv_cell_size{T{1}};
    crd::u32                                                   m_nx{0};
    crd::u32                                                   m_ny{0};
    crd::u32                                                   m_nz{0};
    crd::u32                                                   m_object_free_list{k_invalid_handle};
    crd::usize                                                 m_object_count{0};
    mutable crd::u64                                           m_query_generation{0};
};

// ---- inline templates -------------------------------------------------------

template <MathScalar T>
template <typename Fn>
void UniformGrid<T>::overlap(const AABB3<T>& query, Fn&& on_hit) const
{
    if (m_object_count == 0) { return; }
    if (!crd::geometry::primitives::is_finite(query)) { return; }
    if (query.min.x > query.max.x || query.min.y > query.max.y || query.min.z > query.max.z) { return; }
    const crd::u64 gen = next_query_generation();
    auto was_visited = [this, gen](crd::u32 obj_idx) noexcept -> bool {
        return m_objects[obj_idx].last_query_gen == gen;
    };
    auto mark_visited = [this, gen](crd::u32 obj_idx) noexcept {
        const_cast<ObjectEntry&>(m_objects[obj_idx]).last_query_gen = gen;
    };
    overlap_traverse_(query, was_visited, mark_visited, static_cast<Fn&&>(on_hit));
}

template <MathScalar T>
template <typename Fn>
void UniformGrid<T>::overlap(const AABB3<T>& query, UniformGridScratch& scratch, Fn&& on_hit) const
{
    if (m_object_count == 0) { return; }
    if (!crd::geometry::primitives::is_finite(query)) { return; }
    if (query.min.x > query.max.x || query.min.y > query.max.y || query.min.z > query.max.z) { return; }
    const crd::u64 gen = scratch.prepare_for_query(m_objects.size());
    auto was_visited = [&scratch, gen](crd::u32 obj_idx) noexcept -> bool {
        return scratch.was_visited(obj_idx, gen);
    };
    auto mark_visited = [&scratch, gen](crd::u32 obj_idx) noexcept {
        scratch.mark_visited(obj_idx, gen);
    };
    overlap_traverse_(query, was_visited, mark_visited, static_cast<Fn&&>(on_hit));
}

template <MathScalar T>
template <typename Fn>
void UniformGrid<T>::radius(const Vec3<T>& point, T r, Fn&& on_hit) const
{
    if (m_object_count == 0 || r < T{0}) { return; }
    if (!crd::geometry::primitives::is_finite(point)) { return; }
    const crd::u64 gen = next_query_generation();
    auto was_visited = [this, gen](crd::u32 obj_idx) noexcept -> bool {
        return m_objects[obj_idx].last_query_gen == gen;
    };
    auto mark_visited = [this, gen](crd::u32 obj_idx) noexcept {
        const_cast<ObjectEntry&>(m_objects[obj_idx]).last_query_gen = gen;
    };
    radius_traverse_(point, r, was_visited, mark_visited, static_cast<Fn&&>(on_hit));
}

template <MathScalar T>
template <typename Fn>
void UniformGrid<T>::radius(const Vec3<T>& point, T r, UniformGridScratch& scratch, Fn&& on_hit) const
{
    if (m_object_count == 0 || r < T{0}) { return; }
    if (!crd::geometry::primitives::is_finite(point)) { return; }
    const crd::u64 gen = scratch.prepare_for_query(m_objects.size());
    auto was_visited = [&scratch, gen](crd::u32 obj_idx) noexcept -> bool {
        return scratch.was_visited(obj_idx, gen);
    };
    auto mark_visited = [&scratch, gen](crd::u32 obj_idx) noexcept {
        scratch.mark_visited(obj_idx, gen);
    };
    radius_traverse_(point, r, was_visited, mark_visited, static_cast<Fn&&>(on_hit));
}

// Shared traversal bodies.

template <MathScalar T>
template <typename WasVisited, typename MarkVisited, typename Fn>
void UniformGrid<T>::overlap_traverse_(const AABB3<T>& query,
                                          WasVisited& was_visited,
                                          MarkVisited& mark_visited,
                                          Fn&& on_hit) const
{
    crd::u32 min_x, min_y, min_z, max_x, max_y, max_z;
    bool empty_range = false;
    aabb_cell_range(query, min_x, min_y, min_z, max_x, max_y, max_z, empty_range);
    if (empty_range) { return; }

    auto aabb_isect = [](const AABB3<T>& a, const AABB3<T>& b) noexcept {
        return a.min.x <= b.max.x && a.max.x >= b.min.x
            && a.min.y <= b.max.y && a.max.y >= b.min.y
            && a.min.z <= b.max.z && a.max.z >= b.min.z;
    };

    for (crd::u32 iz = min_z; iz <= max_z; ++iz)
    for (crd::u32 iy = min_y; iy <= max_y; ++iy)
    for (crd::u32 ix = min_x; ix <= max_x; ++ix)
    {
        const auto& cell = m_cells[cell_index(ix, iy, iz)];
        for (crd::usize i = 0; i < cell.size(); ++i)
        {
            const crd::u32 obj_idx = cell[i];
            if (was_visited(obj_idx)) { continue; }
            mark_visited(obj_idx);
            const ObjectEntry& obj = m_objects[obj_idx];
            if (aabb_isect(obj.aabb, query))
            {
                on_hit(obj.payload);
            }
        }
    }
}

template <MathScalar T>
template <typename WasVisited, typename MarkVisited, typename Fn>
void UniformGrid<T>::radius_traverse_(const Vec3<T>& point, T r,
                                         WasVisited& was_visited,
                                         MarkVisited& mark_visited,
                                         Fn&& on_hit) const
{
    const T r2 = r * r;
    const AABB3<T> sphere_bbox{
        Vec3<T>{point.x - r, point.y - r, point.z - r},
        Vec3<T>{point.x + r, point.y + r, point.z + r}};

    crd::u32 min_x, min_y, min_z, max_x, max_y, max_z;
    bool empty_range = false;
    aabb_cell_range(sphere_bbox, min_x, min_y, min_z, max_x, max_y, max_z, empty_range);
    if (empty_range) { return; }

    auto pt_aabb_d2 = [](const Vec3<T>& p, const AABB3<T>& a) noexcept -> T {
        T d2 = T{0};
        for (int i = 0; i < 3; ++i)
        {
            const T v = p[static_cast<crd::usize>(i)];
            const T lo = a.min[static_cast<crd::usize>(i)];
            const T hi = a.max[static_cast<crd::usize>(i)];
            if (v < lo)      { const T d = lo - v; d2 += d * d; }
            else if (v > hi) { const T d = v - hi; d2 += d * d; }
        }
        return d2;
    };

    for (crd::u32 iz = min_z; iz <= max_z; ++iz)
    for (crd::u32 iy = min_y; iy <= max_y; ++iy)
    for (crd::u32 ix = min_x; ix <= max_x; ++ix)
    {
        const auto& cell = m_cells[cell_index(ix, iy, iz)];
        for (crd::usize i = 0; i < cell.size(); ++i)
        {
            const crd::u32 obj_idx = cell[i];
            if (was_visited(obj_idx)) { continue; }
            mark_visited(obj_idx);
            const ObjectEntry& obj = m_objects[obj_idx];
            if (pt_aabb_d2(point, obj.aabb) <= r2)
            {
                on_hit(obj.payload);
            }
        }
    }
}

using UniformGridf = UniformGrid<crd::f32>;
using UniformGridd = UniformGrid<crd::f64>;

extern template class UniformGrid<crd::f32>;
extern template class UniformGrid<crd::f64>;

} // namespace crd::geometry::spatial
