#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — `SpatialHash<T>`, fixed-grid spatial-hash AABB index.
// Phase 3.1.7 v5d. Reference: Teschner, Heidelberger, Müller, Pomeranets,
// Gross, *Optimized Spatial Hashing for Collision Detection of Deformable
// Objects*, VMV 2003. Voxel raycast: Amanatides & Woo, *A Fast Voxel
// Traversal Algorithm for Ray Tracing*, Eurographics 1987.
//
// **Particle / swarm broadphase** + soft-body cloth/fluid neighbour search +
// spatially-uniform large-N point-cloud queries — the tool when the data is
// uniformly distributed and the cell size matches the typical object size
// (Teschner's recommendation: `cell_size ≈ 2 × max_object_radius`).
//
// vs LooseOctree (v5b): SpatialHash is O(1) per cell vs O(log N) per descent;
// wins on uniform-density / particle-system / soft-body workloads. Loses on
// sparse + clustered scenes (every empty cell still costs a hash + bucket
// allocation). vs RTree (v5c): SpatialHash has no tree structure overhead;
// wins on insert/update churn; loses on highly non-uniform spatial extents.
//
// ── Hash function (Teschner 2003 §3.2) ────────────────────────────────────
//
//   `H(ix, iy, iz) = (ix·P1) ⊕ (iy·P2) ⊕ (iz·P3)`, modulo `bucket_count`.
//   * `P1 = 73856093`, `P2 = 19349663`, `P3 = 83492791` (large primes).
//   * `bucket_count` is **power-of-two** so the mod becomes `& (bucket_count-1)`.
//   * Cell coords: `ix = floor(x / cell_size)` — signed-safe via
//     `static_cast<i32>(crd::math::floor)` to handle negative coords correctly.
//
// ── Object storage ────────────────────────────────────────────────────────
//
//   * `m_objects`: free-list-managed pool of `{aabb, payload, last_query_gen,
//     next_free, alive}`. Stable handle = pool slot.
//   * `m_buckets[hash]`: `Array<u32 obj_idx>` — lazily allocated on first
//     insert into that cell.
//   * Object stored to ALL cells its AABB overlaps. Multi-cell membership is
//     the cost of the hash's O(1) cell lookup.
//
// ── Per-query generation counter for dedup (the elite-tier no-alloc trick) ─
//
// An object spans multiple cells, so the same `obj_idx` appears in multiple
// buckets — query-traversal would emit it once per cell. Naïve dedup uses a
// per-query `seen` set (allocates). The elite trick: **bump a `u64`
// generation counter once per query**; when visiting an object, compare its
// per-query gen entry against the counter. If equal → already seen → skip.
// Else → set + emit. **Zero allocation per query.** `noexcept` query
// interface preserved.
//
// ── Thread-safety: TWO API surfaces ───────────────────────────────────────
//
// `SpatialHash` ships **two parallel query API surfaces**:
//
//   1. **Convenience (single-thread)** — `overlap(q, on_hit)` etc. The
//      tree owns a `mutable u64 m_query_generation` + per-object
//      `last_query_gen`. Zero alloc, dead-simple call site, NOT thread-
//      safe for concurrent invocation on the same tree.
//
//   2. **Thread-safe (caller scratch)** — `overlap(q, scratch, on_hit)`
//      etc. Caller provides a `SpatialHashScratch` (one per worker fiber
//      / thread). The scratch owns the per-object generation array; the
//      tree's mutable state is never touched by these overloads. Multiple
//      threads each holding their own scratch can query the same tree
//      concurrently with no race. Zero alloc per query (the scratch is
//      reused across calls; resizes only when the tree's object count
//      grows past the scratch's capacity).
//
// The two paths use **identical traversal + dedup logic** — they're
// semantically equivalent (byte-for-byte same emission order on the same
// inputs, modulo NaN handling which is tolerated by both). Pick (1) for
// per-frame main-thread broadphase; pick (2) for jobified parallel queries
// (eylem v3 XPBD soft-body broadphase target).
//
// `find_overlapping_pairs` is purely-read const (no dedup state) and is
// thread-safe by construction — no scratch overload needed.
//
// **Overflow protection** (both paths): pre-wrap detection at u64::max
// resets the relevant gen array to 0 + the counter to 1. Correctness-
// preserving, once-in-cosmic-time linear scan.
//
// ── Update fast-path ─────────────────────────────────────────────────────
//
// If the object's NEW AABB hashes to the SAME set of cells as the OLD AABB,
// the buckets don't need re-sorting — just update the AABB in place and
// return `false`. Only when the cell range changes do we remove from old
// cells + insert into new cells. ~95%+ of small-motion updates take the
// fast path for objects much smaller than `cell_size`.
//
// ── Queries ──────────────────────────────────────────────────────────────
//
//   * `overlap(aabb, on_hit)` — hash query AABB to overlapping cells, scan
//     each cell, dedup via generation counter, filter by exact AABB-vs-AABB.
//   * `radius(point, r, on_hit)` — hash sphere bbox to cells, dedup, filter
//     by exact `point_to_aabb_dist² ≤ r²`.
//   * `raycast(ray)` — **Amanatides-Woo 1987 voxel traversal**: tDelta + step
//     + tMax per axis, advance to nearest cell boundary, scan cell, check
//     `best_t` pruning. Lowest-payload tiebreak on equal `t` (§4 pin #11).
//   * `find_overlapping_pairs(out)` — broadphase pair query: visit each
//     bucket, emit `(min, max)` AABB-overlapping pairs, sort + unique
//     across the full output (cross-cell duplicates removed). Eylem v3
//     XPBD soft-body broadphase target.
//
// Queries TOLERATE non-finite query inputs (defensive `is_finite` short-
// circuit at the API surface). Builder REJECTS non-finite (debug
// `CRD_ASSERT`). Symmetric with v5a/v5b/v5c + ADR-0076 §15.
//
// ── Two-layer typing (ADR-0078 §5 D34) ───────────────────────────────────
// Algorithm body is raw `<MathScalar T>`. Typed wrappers in
// `hash_queries_typed.hpp`. Same pattern as `kd_queries_typed.hpp` (v5a),
// `octree_queries_typed.hpp` (v5b), `rtree_queries_typed.hpp` (v5c).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
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

// Stable handle for an object in a `SpatialHash`.
struct SpatialHashObjectId
{
    static constexpr crd::u32 k_invalid = 0xFFFFFFFFU;
    crd::u32 value{k_invalid};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != k_invalid; }
    [[nodiscard]] friend constexpr bool operator==(SpatialHashObjectId a, SpatialHashObjectId b) noexcept
    {
        return a.value == b.value;
    }
};

// Default bucket count (POW2). 4096 buckets handles 1000-10000 objects with
// reasonable load factor; tunable via `SpatialHashConfig::bucket_count`.
inline constexpr crd::u32 k_spatial_hash_default_bucket_count = 4096U;

// Default cell size — 1.0 metre (SI). Caller almost always overrides to
// `~2 × typical_object_radius` per Teschner §4.1.
template <MathScalar T>
[[nodiscard]] constexpr T k_spatial_hash_default_cell_size() noexcept
{
    return T{1};
}

template <MathScalar T>
struct SpatialHashConfig
{
    T        cell_size{k_spatial_hash_default_cell_size<T>()};
    crd::u32 bucket_count{k_spatial_hash_default_bucket_count}; // MUST be POW2
};

// Caller-owned scratch for the thread-safe query overloads. One scratch per
// worker fiber / thread. Internally: per-object generation counter +
// monotonic `u64 current_gen`. Reused across queries — capacity grows only
// when the tree's object count exceeds the scratch's allocation.
//
// Zero allocation per query once warmed up. The scratch is independent of
// any specific tree — pass the same scratch to multiple trees if their
// object-count maxima are comparable.
class SpatialHashScratch
{
public:
    explicit SpatialHashScratch(crd::memory::IAllocator* alloc) noexcept : m_per_object_gen(alloc) {}

    SpatialHashScratch(const SpatialHashScratch&) = delete;
    SpatialHashScratch& operator=(const SpatialHashScratch&) = delete;
    SpatialHashScratch(SpatialHashScratch&&) noexcept = default;
    SpatialHashScratch& operator=(SpatialHashScratch&&) noexcept = default;
    ~SpatialHashScratch() = default;

    [[nodiscard]] crd::usize capacity() const noexcept { return m_per_object_gen.size(); }

    // Internal: called by the SpatialHash query overloads at the start of
    // each query. Resizes the per-object array if needed (zero-fills new
    // entries) + bumps the generation. Returns the new generation. Public
    // so consumers can pre-warm: `scratch.prepare_for_query(N)` before a
    // hot loop avoids a first-call resize. Idempotent across hot-path use.
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

// Broadphase pair (always (a, b) with a < b for canonical ordering).
struct SpatialHashPair
{
    crd::u32 a;
    crd::u32 b;

    [[nodiscard]] friend constexpr bool operator==(SpatialHashPair x, SpatialHashPair y) noexcept
    {
        return x.a == y.a && x.b == y.b;
    }
    [[nodiscard]] friend constexpr bool operator<(SpatialHashPair x, SpatialHashPair y) noexcept
    {
        return x.a != y.a ? x.a < y.a : x.b < y.b;
    }
};

template <MathScalar T>
class SpatialHash
{
public:
    SpatialHash(crd::memory::IAllocator* alloc, const SpatialHashConfig<T>& cfg);

    SpatialHash(const SpatialHash&) = delete;
    SpatialHash& operator=(const SpatialHash&) = delete;
    SpatialHash(SpatialHash&&) noexcept = default;
    SpatialHash& operator=(SpatialHash&&) noexcept = default;
    ~SpatialHash() = default;

    // ---- mutation -----------------------------------------------------------

    // Insert an object. Object's AABB is hashed to ALL cells it overlaps.
    // Returns a stable handle. Builder-reject in debug for non-finite AABB.
    [[nodiscard]] SpatialHashObjectId insert(const AABB3<T>& aabb, crd::u32 payload);

    // Remove an object. Handle invalid afterwards (slot may be reused).
    void remove(SpatialHashObjectId id);

    // Update an object's AABB. If the new AABB hashes to the SAME cell range,
    // the call is a no-op (just refresh the AABB) and returns false. Otherwise
    // the object is removed from old cells + inserted into new cells, and
    // returns true. Handle stays valid in both cases.
    bool update(SpatialHashObjectId id, const AABB3<T>& new_aabb);

    // ---- access -------------------------------------------------------------

    [[nodiscard]] bool is_empty() const noexcept { return m_object_count == 0; }
    [[nodiscard]] crd::usize object_count() const noexcept { return m_object_count; }
    [[nodiscard]] crd::usize bucket_count() const noexcept { return m_buckets.size(); }
    [[nodiscard]] T cell_size() const noexcept { return m_cell_size; }

    // Diagnostics: largest bucket size + total entries / bucket count.
    [[nodiscard]] crd::usize max_bucket_size() const noexcept;
    [[nodiscard]] crd::f32   load_factor() const noexcept;

    [[nodiscard]] AABB3<T> object_aabb(SpatialHashObjectId id) const noexcept;
    [[nodiscard]] crd::u32 object_payload(SpatialHashObjectId id) const noexcept;

    // ---- queries ------------------------------------------------------------

    // ---- queries — convenience (single-thread) ----
    //
    // Dedup uses tree-internal `mutable` generation counter — zero alloc, NOT
    // thread-safe. For multi-thread, use the scratch overloads below.

    template <typename Fn> void overlap(const AABB3<T>& query, Fn&& on_hit) const;
    void overlap(const AABB3<T>& query, crd::containers::Array<crd::u32>& out) const;

    template <typename Fn> void radius(const Vec3<T>& point, T r, Fn&& on_hit) const;
    void radius(const Vec3<T>& point, T r, crd::containers::Array<crd::u32>& out) const;

    [[nodiscard]] std::optional<crd::geometry::RayHit<crd::u32>>
    raycast(const Ray3<T>& ray,
            T tmax = std::numeric_limits<T>::infinity()) const noexcept;

    // ---- queries — thread-safe (caller scratch) ----
    //
    // Dedup state lives on the caller-supplied `SpatialHashScratch`. One
    // scratch per worker thread / fiber. Multiple threads each holding their
    // own scratch can query the same tree concurrently with no race. Zero
    // alloc per query (the scratch is reused; resizes only when the tree's
    // object count exceeds the scratch's capacity).
    //
    // Semantically identical to the single-thread overloads — same dedup,
    // same emission order on the same inputs. `find_overlapping_pairs` is
    // already thread-safe (no dedup state) — no scratch overload needed.

    template <typename Fn> void overlap(const AABB3<T>& query, SpatialHashScratch& scratch, Fn&& on_hit) const;
    void overlap(const AABB3<T>& query, SpatialHashScratch& scratch, crd::containers::Array<crd::u32>& out) const;

    template <typename Fn> void radius(const Vec3<T>& point, T r, SpatialHashScratch& scratch, Fn&& on_hit) const;
    void radius(const Vec3<T>& point, T r, SpatialHashScratch& scratch, crd::containers::Array<crd::u32>& out) const;

    [[nodiscard]] std::optional<crd::geometry::RayHit<crd::u32>>
    raycast(const Ray3<T>& ray, SpatialHashScratch& scratch,
            T tmax = std::numeric_limits<T>::infinity()) const noexcept;

    // Broadphase pair query — every (a, b) pair (with a < b in user-payload
    // order) where the two objects' AABBs overlap. Cross-cell duplicates
    // removed via sort + unique. **The eylem v3 XPBD soft-body broadphase
    // target.**
    void find_overlapping_pairs(crd::containers::Array<SpatialHashPair>& out) const;

private:
    static constexpr crd::u32 k_invalid_handle = 0xFFFFFFFFU;

    struct ObjectEntry
    {
        AABB3<T>  aabb{};
        crd::u32  payload{0};
        crd::u64  last_query_gen{0}; // generation-counter dedup
        crd::u32  next_free{k_invalid_handle};
        bool      alive{false};
    };

    // Hash function — Teschner 2003 §3.2.
    [[nodiscard]] crd::u32 hash_cell(crd::i32 ix, crd::i32 iy, crd::i32 iz) const noexcept;

    // Cell coords from world position (signed-safe via crd::math::floor).
    void aabb_cell_range(const AABB3<T>& a, crd::i32& min_x, crd::i32& min_y, crd::i32& min_z,
                                              crd::i32& max_x, crd::i32& max_y, crd::i32& max_z) const noexcept;

    // Pool helpers.
    [[nodiscard]] crd::u32 allocate_object(const AABB3<T>& aabb, crd::u32 payload);
    void free_object(crd::u32 idx);
    [[nodiscard]] bool is_object_alive(crd::u32 idx) const noexcept
    {
        return idx < m_objects.size() && m_objects[idx].alive;
    }

    // Insert/remove obj_idx into/from every bucket whose cell the AABB overlaps.
    void insert_into_cells(crd::u32 obj_idx, const AABB3<T>& aabb);
    void remove_from_cells(crd::u32 obj_idx, const AABB3<T>& aabb);

    // Bump the per-query generation counter; if this would overflow, reset
    // every object's `last_query_gen` to 0 first. Returns the new generation.
    [[nodiscard]] crd::u64 next_query_generation() const noexcept;

    // Shared traversal helpers for the convenience + scratch overloads. The
    // dedup policy is parameterised: `was_visited(obj_idx) -> bool` +
    // `mark_visited(obj_idx)`. Both overloads of `overlap` / `radius` use
    // these — semantically identical traversal, only the dedup-state owner
    // differs (tree mutable field vs caller scratch).
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
    crd::memory::IAllocator*                              m_alloc{nullptr};
    crd::containers::Array<ObjectEntry>                   m_objects;
    crd::containers::Array<crd::containers::Array<crd::u32>> m_buckets;
    T                                                      m_cell_size{T{1}};
    crd::u32                                               m_bucket_mask{0}; // bucket_count - 1
    crd::u32                                               m_object_free_list{k_invalid_handle};
    crd::usize                                             m_object_count{0};
    // mutable so const queries can bump it.
    mutable crd::u64                                       m_query_generation{0};
};

// ---- inline templates -------------------------------------------------------
//
// Both the convenience (single-thread) and scratch (thread-safe) overloads
// share identical traversal logic, parameterised over a `Visited` policy
// supplying `was_visited(obj_idx) -> bool` + `mark_visited(obj_idx)`. Each
// overload constructs the appropriate policy and calls the shared
// `overlap_traverse_` / `radius_traverse_` template.

template <MathScalar T>
template <typename Fn>
void SpatialHash<T>::overlap(const AABB3<T>& query, Fn&& on_hit) const
{
    if (m_object_count == 0) { return; }
    if (!crd::geometry::primitives::is_finite(query)) { return; }
    if (query.min.x > query.max.x || query.min.y > query.max.y || query.min.z > query.max.z) { return; }

    const crd::u64 gen = next_query_generation();
    // Tree-state visited policy — writes through `mutable` field via
    // const_cast (single-thread contract).
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
void SpatialHash<T>::overlap(const AABB3<T>& query, SpatialHashScratch& scratch, Fn&& on_hit) const
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
void SpatialHash<T>::radius(const Vec3<T>& point, T r, Fn&& on_hit) const
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
void SpatialHash<T>::radius(const Vec3<T>& point, T r, SpatialHashScratch& scratch, Fn&& on_hit) const
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

// ---- shared traversal templates --------------------------------------------

template <MathScalar T>
template <typename WasVisited, typename MarkVisited, typename Fn>
void SpatialHash<T>::overlap_traverse_(const AABB3<T>& query,
                                          WasVisited& was_visited,
                                          MarkVisited& mark_visited,
                                          Fn&& on_hit) const
{
    crd::i32 min_x, min_y, min_z, max_x, max_y, max_z;
    aabb_cell_range(query, min_x, min_y, min_z, max_x, max_y, max_z);

    auto aabb_isect = [](const AABB3<T>& a, const AABB3<T>& b) noexcept {
        return a.min.x <= b.max.x && a.max.x >= b.min.x
            && a.min.y <= b.max.y && a.max.y >= b.min.y
            && a.min.z <= b.max.z && a.max.z >= b.min.z;
    };

    // Canonical visit order: z, y, x — deterministic emission across runs.
    for (crd::i32 iz = min_z; iz <= max_z; ++iz)
    for (crd::i32 iy = min_y; iy <= max_y; ++iy)
    for (crd::i32 ix = min_x; ix <= max_x; ++ix)
    {
        const crd::u32 h = hash_cell(ix, iy, iz);
        const auto& bucket = m_buckets[h];
        for (crd::usize i = 0; i < bucket.size(); ++i)
        {
            const crd::u32 obj_idx = bucket[i];
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
void SpatialHash<T>::radius_traverse_(const Vec3<T>& point, T r,
                                         WasVisited& was_visited,
                                         MarkVisited& mark_visited,
                                         Fn&& on_hit) const
{
    const T r2 = r * r;
    const AABB3<T> sphere_bbox{
        Vec3<T>{point.x - r, point.y - r, point.z - r},
        Vec3<T>{point.x + r, point.y + r, point.z + r}};

    crd::i32 min_x, min_y, min_z, max_x, max_y, max_z;
    aabb_cell_range(sphere_bbox, min_x, min_y, min_z, max_x, max_y, max_z);

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

    for (crd::i32 iz = min_z; iz <= max_z; ++iz)
    for (crd::i32 iy = min_y; iy <= max_y; ++iy)
    for (crd::i32 ix = min_x; ix <= max_x; ++ix)
    {
        const crd::u32 h = hash_cell(ix, iy, iz);
        const auto& bucket = m_buckets[h];
        for (crd::usize i = 0; i < bucket.size(); ++i)
        {
            const crd::u32 obj_idx = bucket[i];
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

using SpatialHashf = SpatialHash<crd::f32>;
using SpatialHashd = SpatialHash<crd::f64>;

extern template class SpatialHash<crd::f32>;
extern template class SpatialHash<crd::f64>;

} // namespace crd::geometry::spatial
