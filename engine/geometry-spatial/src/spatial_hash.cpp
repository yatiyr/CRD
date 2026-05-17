// crd-geometry-spatial â€” `SpatialHash<T>` impl (Phase 3.1.7 v5d).
//
// Reference: Teschner, Heidelberger, MÃ¼ller, Pomeranets, Gross, *Optimized
// Spatial Hashing for Collision Detection of Deformable Objects*, VMV 2003.
// Voxel raycast: Amanatides & Woo, *A Fast Voxel Traversal Algorithm for
// Ray Tracing*, Eurographics 1987.
//
// Header `spatial_hash.hpp` documents the design + locked decisions.

#include <crd/geometry/spatial/spatial_hash.hpp>

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

// Teschner 2003 Â§3.2 â€” three large primes. The XOR mix ensures the bucket
// distribution is ~uniform for spatially-correlated cell coords.
constexpr u32 kP1 = 73856093U;
constexpr u32 kP2 = 19349663U;
constexpr u32 kP3 = 83492791U;

[[nodiscard, maybe_unused]] constexpr bool is_pow2(u32 x) noexcept
{
    return x != 0 && (x & (x - 1U)) == 0U;
}

// Signed-safe floor-and-cast: converts world coord â†’ cell coord. Negative
// coords floor TOWARDS -âˆ (canonical mathematical floor) so that adjacent
// objects on either side of x=0 land in adjacent cells.
template <MathScalar T>
inline i32 floor_to_i32(T v) noexcept
{
    return static_cast<i32>(std::floor(static_cast<f64>(v)));
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

template <MathScalar T>
SpatialHash<T>::SpatialHash(crd::memory::IAllocator* alloc, const SpatialHashConfig<T>& cfg)
    : m_alloc(alloc)
    , m_objects(alloc)
    , m_buckets(alloc)
    , m_cell_size(cfg.cell_size)
{
    CRD_ASSERT(cfg.cell_size > T{0} && "cell_size must be positive");
    CRD_ASSERT(is_pow2(cfg.bucket_count) && "bucket_count must be a power of two");

    m_buckets.resize(cfg.bucket_count);
    for (usize i = 0; i < cfg.bucket_count; ++i)
    {
        m_buckets[i] = crd::containers::Array<u32>{alloc};
    }
    m_bucket_mask = cfg.bucket_count - 1U;
}

// =============================================================================
// Hash function + cell math
// =============================================================================

template <MathScalar T>
u32 SpatialHash<T>::hash_cell(i32 ix, i32 iy, i32 iz) const noexcept
{
    // Cast through unsigned to silence signed-overflow UB on the multiply.
    // The operation IS deterministic across compilers (mod 2^32 multiply).
    const u32 h = (static_cast<u32>(ix) * kP1)
                ^ (static_cast<u32>(iy) * kP2)
                ^ (static_cast<u32>(iz) * kP3);
    return h & m_bucket_mask;
}

template <MathScalar T>
void SpatialHash<T>::aabb_cell_range(const AABB3<T>& a, i32& min_x, i32& min_y, i32& min_z,
                                                          i32& max_x, i32& max_y, i32& max_z) const noexcept
{
    const T inv_cs = T{1} / m_cell_size;
    min_x = floor_to_i32(a.min.x * inv_cs);
    min_y = floor_to_i32(a.min.y * inv_cs);
    min_z = floor_to_i32(a.min.z * inv_cs);
    max_x = floor_to_i32(a.max.x * inv_cs);
    max_y = floor_to_i32(a.max.y * inv_cs);
    max_z = floor_to_i32(a.max.z * inv_cs);
}

// =============================================================================
// Object pool
// =============================================================================

template <MathScalar T>
u32 SpatialHash<T>::allocate_object(const AABB3<T>& aabb, u32 payload)
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
void SpatialHash<T>::free_object(u32 idx)
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
void SpatialHash<T>::insert_into_cells(u32 obj_idx, const AABB3<T>& aabb)
{
    i32 min_x;
    i32 min_y;
    i32 min_z;
    i32 max_x;
    i32 max_y;
    i32 max_z;
    aabb_cell_range(aabb, min_x, min_y, min_z, max_x, max_y, max_z);

    for (i32 iz = min_z; iz <= max_z; ++iz)
    for (i32 iy = min_y; iy <= max_y; ++iy)
    for (i32 ix = min_x; ix <= max_x; ++ix)
    {
        const u32 h = hash_cell(ix, iy, iz);
        m_buckets[h].push_back(obj_idx);
    }
}

template <MathScalar T>
void SpatialHash<T>::remove_from_cells(u32 obj_idx, const AABB3<T>& aabb)
{
    i32 min_x;
    i32 min_y;
    i32 min_z;
    i32 max_x;
    i32 max_y;
    i32 max_z;
    aabb_cell_range(aabb, min_x, min_y, min_z, max_x, max_y, max_z);

    for (i32 iz = min_z; iz <= max_z; ++iz)
    for (i32 iy = min_y; iy <= max_y; ++iy)
    for (i32 ix = min_x; ix <= max_x; ++ix)
    {
        const u32 h = hash_cell(ix, iy, iz);
        auto& bucket = m_buckets[h];
        // Note: hash collisions can put OTHER objects in the same bucket
        // whose AABBs don't overlap our cell. We must scan the full bucket
        // and remove only OUR obj_idx (swap-with-last). Since we visit each
        // cell once in our cell range, we want to remove exactly ONE
        // occurrence of obj_idx per visit (a hash collision could put us
        // in this same bucket from multiple cells; remove first match).
        for (usize i = 0; i < bucket.size(); ++i)
        {
            if (bucket[i] == obj_idx)
            {
                bucket[i] = bucket[bucket.size() - 1];
                bucket.resize(bucket.size() - 1);
                break;
            }
        }
    }
}

// =============================================================================
// Public mutators
// =============================================================================

template <MathScalar T>
SpatialHashObjectId SpatialHash<T>::insert(const AABB3<T>& aabb, u32 payload)
{
    CRD_ASSERT(crd::geometry::primitives::is_finite(aabb));
    CRD_ASSERT(aabb.min.x <= aabb.max.x && aabb.min.y <= aabb.max.y && aabb.min.z <= aabb.max.z);

    const u32 idx = allocate_object(aabb, payload);
    insert_into_cells(idx, aabb);
    return SpatialHashObjectId{idx};
}

template <MathScalar T>
void SpatialHash<T>::remove(SpatialHashObjectId id)
{
    CRD_ASSERT(is_object_alive(id.value));
    const AABB3<T> old_aabb = m_objects[id.value].aabb;
    remove_from_cells(id.value, old_aabb);
    free_object(id.value);
}

template <MathScalar T>
bool SpatialHash<T>::update(SpatialHashObjectId id, const AABB3<T>& new_aabb)
{
    CRD_ASSERT(is_object_alive(id.value));
    CRD_ASSERT(crd::geometry::primitives::is_finite(new_aabb));
    CRD_ASSERT(new_aabb.min.x <= new_aabb.max.x);

    ObjectEntry& obj = m_objects[id.value];

    // Compute old + new cell ranges; if they're equal, fast path.
    i32 omin_x;
    i32 omin_y;
    i32 omin_z;
    i32 omax_x;
    i32 omax_y;
    i32 omax_z;
    aabb_cell_range(obj.aabb, omin_x, omin_y, omin_z, omax_x, omax_y, omax_z);
    i32 nmin_x;
    i32 nmin_y;
    i32 nmin_z;
    i32 nmax_x;
    i32 nmax_y;
    i32 nmax_z;
    aabb_cell_range(new_aabb, nmin_x, nmin_y, nmin_z, nmax_x, nmax_y, nmax_z);

    if (omin_x == nmin_x && omin_y == nmin_y && omin_z == nmin_z
        && omax_x == nmax_x && omax_y == nmax_y && omax_z == nmax_z)
    {
        obj.aabb = new_aabb;
        return false;
    }

    // Slow path â€” different cell range. Remove + reinsert.
    remove_from_cells(id.value, obj.aabb);
    obj.aabb = new_aabb;
    insert_into_cells(id.value, new_aabb);
    return true;
}

// =============================================================================
// Generation counter (overflow-safe)
// =============================================================================

template <MathScalar T>
u64 SpatialHash<T>::next_query_generation() const noexcept
{
    // Pre-wrap detection: if the next bump would overflow u64 to 0, reset
    // every object's last_query_gen to 0 + restart at 1. Cosmically rare
    // (one query/ns Ã— 585 years to wrap u64) but correctness-preserving.
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
// Diagnostics
// =============================================================================

template <MathScalar T>
usize SpatialHash<T>::max_bucket_size() const noexcept
{
    usize m = 0;
    for (usize i = 0; i < m_buckets.size(); ++i)
    {
        if (m_buckets[i].size() > m) { m = m_buckets[i].size(); }
    }
    return m;
}

template <MathScalar T>
f32 SpatialHash<T>::load_factor() const noexcept
{
    if (m_buckets.size() == 0) { return 0.0F; }
    usize total = 0;
    for (usize i = 0; i < m_buckets.size(); ++i) { total += m_buckets[i].size(); }
    return static_cast<f32>(total) / static_cast<f32>(m_buckets.size());
}

template <MathScalar T>
AABB3<T> SpatialHash<T>::object_aabb(SpatialHashObjectId id) const noexcept
{
    CRD_ASSERT(is_object_alive(id.value));
    return m_objects[id.value].aabb;
}

template <MathScalar T>
u32 SpatialHash<T>::object_payload(SpatialHashObjectId id) const noexcept
{
    CRD_ASSERT(is_object_alive(id.value));
    return m_objects[id.value].payload;
}

// =============================================================================
// overlap (Array sink) + radius (Array sink)
// =============================================================================

template <MathScalar T>
void SpatialHash<T>::overlap(const AABB3<T>& query, crd::containers::Array<u32>& out) const
{
    overlap(query, [&](u32 payload) { out.push_back(payload); });
}

template <MathScalar T>
void SpatialHash<T>::overlap(const AABB3<T>& query, SpatialHashScratch& scratch,
                              crd::containers::Array<u32>& out) const
{
    overlap(query, scratch, [&](u32 payload) { out.push_back(payload); });
}

template <MathScalar T>
void SpatialHash<T>::radius(const Vec3<T>& point, T r, crd::containers::Array<u32>& out) const
{
    radius(point, r, [&](u32 payload) { out.push_back(payload); });
}

template <MathScalar T>
void SpatialHash<T>::radius(const Vec3<T>& point, T r, SpatialHashScratch& scratch,
                             crd::containers::Array<u32>& out) const
{
    radius(point, r, scratch, [&](u32 payload) { out.push_back(payload); });
}

// =============================================================================
// raycast â€” Amanatides-Woo 1987 voxel traversal
// =============================================================================
//
// Setup per ray (per axis):
//   * stepX = sign(direction.x) âˆˆ {-1, 0, +1}; tDeltaX = |cell_size / direction.x|;
//     tMaxX = parametric t at which ray crosses the next x-cell boundary.
// Loop:
//   * scan cell (ix, iy, iz) â€” for each object in its bucket (dedup via gen),
//     raycast vs object AABB, update best_t.
//   * advance to next cell along axis with smallest tMax.
//   * stop when tMax > best_t (no closer hit possible).
//
// Lowest-payload tiebreak on equal t per ADR-0076 Â§4 pin #11.

template <MathScalar T>
std::optional<crd::geometry::RayHit<u32>>
SpatialHash<T>::raycast(const Ray3<T>& ray, T tmax) const noexcept
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
SpatialHash<T>::raycast(const Ray3<T>& ray, SpatialHashScratch& scratch, T tmax) const noexcept
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
SpatialHash<T>::raycast_traverse_(const Ray3<T>& ray, T tmax,
                                     WasVisited& was_visited,
                                     MarkVisited& mark_visited) const noexcept
{
    const T inv_cs = T{1} / m_cell_size;

    // Start cell.
    i32 ix = floor_to_i32(ray.origin.x * inv_cs);
    i32 iy = floor_to_i32(ray.origin.y * inv_cs);
    i32 iz = floor_to_i32(ray.origin.z * inv_cs);

    // Step direction per axis. Zero direction component => no traversal on that axis.
    auto sign_step = [](T d) noexcept -> i32 {
        if (d > T{0}) { return 1; }
        if (d < T{0}) { return -1; }
        return 0;
    };
    const i32 step_x = sign_step(ray.direction.x);
    const i32 step_y = sign_step(ray.direction.y);
    const i32 step_z = sign_step(ray.direction.z);

    // tDelta per axis = parametric distance to traverse one cell along that axis.
    constexpr T kInf = std::numeric_limits<T>::infinity();
    const T tdelta_x = (step_x != 0) ? std::abs(m_cell_size / ray.direction.x) : kInf;
    const T tdelta_y = (step_y != 0) ? std::abs(m_cell_size / ray.direction.y) : kInf;
    const T tdelta_z = (step_z != 0) ? std::abs(m_cell_size / ray.direction.z) : kInf;

    // tMax per axis = parametric t at which ray crosses the NEXT cell boundary.
    // For step_x > 0: next boundary at (ix+1)*cs; for step_x < 0: at ix*cs.
    auto initial_tmax = [&](T origin_a, T dir_a, i32 ia, i32 step_a) -> T {
        if (step_a == 0) { return kInf; }
        const T boundary = (step_a > 0) ? static_cast<T>(ia + 1) * m_cell_size
                                          : static_cast<T>(ia) * m_cell_size;
        return (boundary - origin_a) / dir_a;
    };
    T tmax_x = initial_tmax(ray.origin.x, ray.direction.x, ix, step_x);
    T tmax_y = initial_tmax(ray.origin.y, ray.direction.y, iy, step_y);
    T tmax_z = initial_tmax(ray.origin.z, ray.direction.z, iz, step_z);

    T best_t = tmax;
    u32 best_payload = 0xFFFFFFFFU;
    bool any = false;

    // Per-object slab raycast (scalar â€” same as v5b's f64 path).
    auto ray_aabb = [&](const AABB3<T>& a, T& out_t) noexcept -> bool {
        T tmin = T{0};
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
                if (t1 > tmin) { tmin = t1; }
                if (t2 < tcur_max) { tcur_max = t2; }
                if (tmin > tcur_max) { return false; }
            }
        }
        if (tmin < T{0}) { return false; }
        out_t = tmin;
        return true;
    };

    auto scan_cell = [&]() noexcept {
        const u32 h = hash_cell(ix, iy, iz);
        const auto& bucket = m_buckets[h];
        for (usize i = 0; i < bucket.size(); ++i)
        {
            const u32 obj_idx = bucket[i];
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

    // Walk cells. Cap iterations to avoid runaway on degenerate input.
    constexpr usize kMaxVoxelSteps = 1U << 20; // 1M cells worst case
    usize steps = 0;
    while (steps++ < kMaxVoxelSteps)
    {
        scan_cell();

        // Stop if we've left the best_t-bounded region.
        const T t_next = std::min({tmax_x, tmax_y, tmax_z});
        if (t_next > best_t) { break; }
        if (t_next > tmax)   { break; }

        // Advance ALL axes whose tMax equals the minimum. Strict-less-only
        // chains skip cells when the ray exactly grazes a cell corner (two
        // or three tMax values tie); advancing every tied axis preserves
        // Amanatides-Woo correctness on corner-grazing rays.
        const T t_min = std::min({tmax_x, tmax_y, tmax_z});
        if (tmax_x == t_min) { ix += step_x; tmax_x += tdelta_x; }
        if (tmax_y == t_min) { iy += step_y; tmax_y += tdelta_y; }
        if (tmax_z == t_min) { iz += step_z; tmax_z += tdelta_z; }
    }

    if (!any) { return std::nullopt; }
    return crd::geometry::RayHit<u32>{static_cast<f32>(best_t), best_payload};
}

// =============================================================================
// find_overlapping_pairs â€” broadphase
// =============================================================================
//
// Per cell: for each pair (a, b) of objects in the bucket, if their AABBs
// overlap, emit (min(a.payload, b.payload), max(a.payload, b.payload)).
// Cross-cell duplicates handled via final sort + unique.

template <MathScalar T>
void SpatialHash<T>::find_overlapping_pairs(crd::containers::Array<SpatialHashPair>& out) const
{
    out.clear();
    if (m_object_count < 2) { return; }

    auto aabb_isect = [](const AABB3<T>& a, const AABB3<T>& b) noexcept {
        return a.min.x <= b.max.x && a.max.x >= b.min.x
            && a.min.y <= b.max.y && a.max.y >= b.min.y
            && a.min.z <= b.max.z && a.max.z >= b.min.z;
    };

    for (usize bi = 0; bi < m_buckets.size(); ++bi)
    {
        const auto& bucket = m_buckets[bi];
        const usize n = bucket.size();
        if (n < 2) { continue; }
        for (usize i = 0; i < n; ++i)
        {
            const u32 ai = bucket[i];
            const ObjectEntry& a = m_objects[ai];
            for (usize j = i + 1; j < n; ++j)
            {
                const u32 bi_inner = bucket[j];
                if (ai == bi_inner) { continue; } // hash collision: same obj in same bucket twice (impossible by construction)
                const ObjectEntry& b = m_objects[bi_inner];
                if (!aabb_isect(a.aabb, b.aabb)) { continue; }
                const u32 lo = a.payload < b.payload ? a.payload : b.payload;
                const u32 hi = a.payload < b.payload ? b.payload : a.payload;
                out.push_back(SpatialHashPair{lo, hi});
            }
        }
    }

    // Sort + unique. The same pair (a, b) can appear from multiple cells
    // when both objects span those cells â€” dedup at the output stage.
    crd::containers::sort(out.data(), out.data() + out.size(),
                            [](const SpatialHashPair& x, const SpatialHashPair& y) { return x < y; });
    // Manual unique â€” std::unique would work but `crd::containers` doesn't
    // ship one yet; keep deterministic in-place.
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

template class SpatialHash<f32>;
template class SpatialHash<f64>;

} // namespace crd::geometry::spatial
