# Session log — 2026-05-16 — geometry v5d: SpatialHash

> Phase 3.1.7 v5 `-spatial` cluster continues. Teschner 2003 fixed-grid
> spatial-hash AABB index — particle/swarm broadphase + soft-body cloth/fluid
> neighbour search. Includes Amanatides-Woo 1987 voxel raycast +
> `find_overlapping_pairs` broadphase pair query (eylem v3 XPBD soft-body
> target) + per-query generation-counter dedup. Same `engine/geometry-spatial/`
> module as v5a + v5b + v5c. **Phase doc spec was conservative (radius +
> overlap callbacks); elite mandate widened the slice to add raycast + pair
> query + dedup discipline.**

## Scope landed

| Element | Path |
|---|---|
| Header (storage + API + inline overlap/radius templates) | `engine/geometry-spatial/include/crd/geometry/spatial/spatial_hash.hpp` |
| Implementation (mutators + voxel raycast + pair query + helpers) | `engine/geometry-spatial/src/spatial_hash.cpp` |
| Typed wrapper layer | `engine/geometry-spatial/include/crd/geometry/spatial/hash_queries_typed.hpp` |
| Umbrella update | `engine/geometry-spatial/include/crd/geometry/spatial/spatial.hpp` |
| Tests | `tests/geometry-spatial/test_spatial_hash.cpp`, `test_spatial_hash_typed.cpp` |

## API surface

```cpp
namespace crd::geometry::spatial {

struct SpatialHashObjectId { u32 value; bool valid() const; };
struct SpatialHashPair { u32 a, b; };  // canonical (a < b)

template <MathScalar T>
struct SpatialHashConfig {
    T   cell_size{1};                          // edge length per cell (Teschner: ~2 × max object radius)
    u32 bucket_count{4096};                    // POW2 — bit-mask modulo
};

template <MathScalar T> class SpatialHash {
public:
    SpatialHash(IAllocator*, const SpatialHashConfig<T>&);

    [[nodiscard]] SpatialHashObjectId insert(const AABB3<T>&, u32 payload);
    void remove(SpatialHashObjectId);
    bool update(SpatialHashObjectId, const AABB3<T>& new_aabb);  // false on fast path

    template <typename Fn> void overlap(const AABB3<T>& q, Fn&& on_hit) const;
    void overlap(const AABB3<T>& q, Array<u32>& out) const;

    template <typename Fn> void radius(const Vec3<T>& p, T r, Fn&& on_hit) const;
    void radius(const Vec3<T>& p, T r, Array<u32>& out) const;

    [[nodiscard]] std::optional<RayHit<u32>>
    raycast(const Ray3<T>&, T tmax = +∞) const noexcept;

    void find_overlapping_pairs(Array<SpatialHashPair>& out) const;

    // Diagnostics: object_count(), bucket_count(), cell_size(),
    //              max_bucket_size(), load_factor(), object_aabb(id),
    //              object_payload(id).
};
} // namespace
```

Typed `hash_queries_typed.hpp`: `spatial_hash_insert<D, T>` /
`spatial_hash_overlap<D, T>` / `spatial_hash_radius<D, T>` /
`spatial_hash_raycast<D, T>` strip-compute-retag wrappers per ADR-0078 §5 D34.

## Algorithm — Teschner 2003 + Amanatides-Woo 1987

1. **Hash function** (Teschner §3.2): `H(ix, iy, iz) = (ix·P1) ⊕ (iy·P2) ⊕
   (iz·P3)` mod `bucket_count`. `P1 = 73856093`, `P2 = 19349663`,
   `P3 = 83492791` (large primes). `bucket_count` is **POW2** so mod is a
   bit-mask `& (bucket_count - 1)`. Cell coords from world position via
   signed-safe `static_cast<i32>(std::floor(v / cell_size))` — handles
   negative coords correctly (adjacent objects on either side of x=0 land
   in adjacent cells).

2. **AABB stored in ALL overlapping cells**. Multi-cell membership is the
   cost of O(1) cell lookup. Object pool with free-list + stable
   `SpatialHashObjectId` handle = pool slot. Each bucket = `Array<u32 obj_idx>`,
   allocated lazily on first insert into that cell.

3. **Per-query generation counter for dedup** (the elite-tier trick): an
   object spans multiple cells, so the same `obj_idx` appears in multiple
   buckets — query traversal would emit it once per cell. Bump
   `mutable u64 m_query_generation` once per query; when visiting an object,
   compare `obj.last_query_gen` vs `m_query_generation`. Equal ⇒ already
   seen this query ⇒ skip. Else ⇒ set + emit. **Zero allocation per query.**
   `mutable` + `noexcept` query interface preserved.

4. **Update fast-path**: compute the cell range of OLD aabb + NEW aabb. If
   identical, just refresh the in-memory AABB (return false). Otherwise,
   remove from old cells + insert into new cells (return true). ~95%+ of
   small-motion updates take the fast path for objects much smaller than
   `cell_size`.

5. **Raycast — Amanatides-Woo 1987 voxel traversal** (NOT recursive descent):
   - Per axis: `step ∈ {-1, 0, +1}`, `tDelta = |cell_size / dir|`,
     `tMax = parametric t at next cell boundary`.
   - Walk: scan cell, check `best_t` pruning, advance to axis with smallest
     `tMax`. Stop when `t_next > best_t` (no closer hit possible) or
     `t_next > tmax`.
   - Per-cell scan uses dedup generation counter (an object spanning N
     cells along the ray is tested once, not N times).
   - Cap iterations at 2^20 cells to defend against pathological inputs
     (degenerate ray near cell boundary, etc.).
   - Lowest-payload tiebreak on equal `t` (ADR-0076 §4 pin #11).

6. **`find_overlapping_pairs(out)`** — broadphase pair query: visit each
   bucket, emit `(min(a.payload, b.payload), max(...))` pairs for every
   AABB-overlapping in-bucket pair. Cross-cell duplicates removed via
   `crd::containers::sort` + manual unique. **The eylem v3 XPBD soft-body
   broadphase target.**

7. **Builder REJECTS non-finite + inverted AABB** (debug `CRD_ASSERT`).
   **Queries TOLERATE non-finite** (defensive `is_finite(query)` short-
   circuit at `overlap`/`radius`/`raycast` API surfaces; zero-direction ray
   short-circuits in raycast). Symmetric with v5a/v5b/v5c + ADR-0076 §15.

## Locked design choices

| # | Decision | Rationale |
|---|---|---|
| 1 | Teschner Pi1×Pi2×Pi3 hash + POW2 bucket count + bit-mask modulo | Standard since 2003; uniform distribution for spatially-correlated coords; bit-mask faster than `%` |
| 2 | Cells visited in canonical (z, y, x) loop order | Deterministic emission across runs |
| 3 | AABB stored to ALL overlapping cells (multi-cell membership) | O(1) cell lookup is the hash's killer feature; multi-cell-store is the cost |
| 4 | Per-query `mutable u64 m_query_generation` + per-object `last_query_gen` for dedup | Zero allocation per query (`seen`-set-free); preserves `noexcept` query interface |
| 5 | Update fast-path = same-cell-range detection, no rebucketing | ~95%+ small motions skip the bucket touch entirely |
| 6 | Stable handle = pool slot; recompute cell range on remove from current AABB | Removes per-object cell-list metadata; aabb is already known |
| 7 | Hash collision tolerated (different cells share buckets) | Statistically rare; query filter (aabb-vs-aabb / point-aabb-d²/ ray-aabb) is exact |
| 8 | Raycast = full Amanatides-Woo (NOT recursive descent) | The right algorithm for a uniform grid; O(cells_along_ray) traversal |
| 9 | `find_overlapping_pairs` first-class — NOT deferred | Eylem v3 broadphase target; cross-cell dedup via sort+unique |
| 10 | Lowest-payload tiebreak on equal raycast `t` | ADR-0076 §4 pin #11 |
| 11 | Builder REJECTS non-finite + inverted; queries TOLERATE non-finite + zero-direction-ray | Symmetric with v5a/v5b/v5c; defensive `is_finite` at API surface |

## Tests — 21 cases / 5-config DoD PASS

Suite breakdown:

| File | Cases | Coverage |
|---|---|---|
| `test_spatial_hash.cpp` | 18 | empty / POW2 bucket-count accept / single insert / **multi-cell object findable from any overlapping cell** / brute-force overlap × 20 query boxes / brute-force radius × 5 trials × 3 radii / **Amanatides-Woo voxel raycast: nearest hit + diagonal direction (cells along (1,1,1)) + negative direction + lowest-payload tiebreak / miss returns nullopt** / **update fast-path** (same cell ⇒ no rebucketing, returns false) / **update slow path** (cell change ⇒ rebucketed) / insert/remove cycle handle stability / **`find_overlapping_pairs` brute-force xval** / negative coords work / NaN tolerance (overlap + radius + raycast + zero-direction) / **dedup: many-cell object (8000 cells) emitted once per query** |
| `test_spatial_hash_typed.cpp` | 3 | typed insert+overlap round-trip / typed radius round-trip / typed raycast returns typed `Quantity<Length, f32>` `t` |

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 2033 / 2033 ctest |
| win-asan | PASS | full project ctest |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 2028 / 2028 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean (intentional `mt19937` constant seeds + `std::optional` `REQUIRE`-then-deref idioms — non-blocking, same as v5a/v5b/v5c) |

`scripts/per-slice-check.ps1` gates the first 4; win-shipping-profile run
separately. Total full-project ctest: **2012 → 2033 win-debug** (+21 cases
from this slice).

## One debugging pass en route

1. **Non-ASCII `⇒` em-arrow in test name** (5th-time-pin per slice — same
   bug class that bit v3b/v3c/v5b/v5c). `crd-no-non-ascii-test-names`
   guard caught it; per-slice-check.ps1 runs ctest so the guard fires
   per-slice now.

## Comparison vs other v5 backends

| Property | KdTree (v5a) | LooseOctree (v5b) | RTree (v5c) | SpatialHash (v5d) |
|---|---|---|---|---|
| Best for | Static point set + k-NN | Dynamic broadphase + per-tick update | Static AABB + cooked levels | Particle/swarm broadphase + uniform density |
| Insert | Immutable (kd_build) | O(depth) + lazy cell alloc | O(depth) + occasional split/reinsert | O(cells_overlapped) per object |
| Update | N/A | **Fast-path** (Ulrich invariant) | No fast path; remove + reinsert | **Fast-path** (same-cell-range) |
| Bulk-load | kd_build at construction | No | **STR** (~7× faster) | Simple loop over inserts |
| Per-query alloc | None | None | k-NN PQ (caller scratch) | None (gen-counter dedup) |
| k-NN | YES (caller-heap) | No | YES (Hjaltason-Samet) | No (use radius then iterate) |
| Pair query | No | No | No | YES (`find_overlapping_pairs`) |
| Raycast | No | t-near BVH-style | t-near BVH-style | **Amanatides-Woo voxel** |
| Memory per object | tight | 24 + handle table | 32 + indirection table | ObjectEntry + per-cell index |

Choose:
* **KdTree** for k-NN over points (lidar, particle nearest-neighbor).
* **LooseOctree** for dynamic broadphase (eylem rigid v1c+ + scene cull).
* **RTree** for cooked static scenes + spatial-query-heavy + k-NN over AABBs.
* **SpatialHash** for particle systems + soft-body neighbour search +
  uniform-density broadphase pair queries (eylem v3 XPBD).

## Decisions locked (for ADR-0076 §20 v5-close amendment)

All 11 decisions from the table above carry forward. Of cross-substrate
significance:
* **Per-query generation counter** — the zero-allocation dedup pattern.
  Reusable for any structure where the same object is referenced from
  multiple cells/leaves. v5e UniformGrid will likely reuse it.
* **Amanatides-Woo voxel traversal** — the canonical algorithm for any
  uniform-grid raycast. Reusable substrate; once written, applies to
  v5e UniformGrid + future octree-grid hybrids.

## Self-review correctness fixes shipped after first DoD pass

After the first 5-config DoD passed, advisor self-review caught three
real concerns. All THREE shipped inline (the third was originally documented
as deferred, then promoted to v5d-fast in the same session per the user's
"elite, no deferring" mandate — see "v5d-fast caller-scratch overload"
below).

1. **Amanatides-Woo corner-tie advance bug (real correctness gap)** —
   strict `<` chain `tmax_x < tmax_y && tmax_x < tmax_z` falls through to
   the `else` and advances Z when `tmax_x == tmax_y` (ray exactly grazes a
   cell-edge or cell-corner). Per Amanatides-Woo, ALL axes whose `tMax`
   equals the minimum must advance — otherwise corner-grazing rays skip
   cells. Fixed: `t_min = min({tmax_x, tmax_y, tmax_z})`; advance every
   axis with `tMax == t_min`. Added test
   `SpatialHash raycast handles exact corner-grazing rays (Amanatides-Woo
   correctness)` — un-normalised dir `(1,1,1)` ray hitting an object at
   the diagonal cell `(3,3,3)`. PASS.

2. **`u64` generation overflow (cosmically rare but real)** — at
   1B queries/sec the counter wraps in ~585 years, but a long-running
   session could theoretically reach `2^64` queries. The wrap-to-0 case
   would mark all objects as "already seen" on the next gen-0 query.
   Fixed via `next_query_generation()` helper: pre-wrap detection +
   reset every object's `last_query_gen` to 0 + restart at 1.
   Once-in-cosmic-time linear scan; correctness-preserving.

3. **Thread-safety: full caller-scratch overload shipped as v5d-fast** —
   originally documented as a deferred follow-on. User reviewed and
   directed "do it now, elite level" before moving on to v5e. Shipped
   in the same session — see "v5d-fast caller-scratch overload" below.

5-config DoD re-run after the hardening — all green again.

## v5d-fast caller-scratch overload (shipped same session)

Added a parallel API surface for thread-safe concurrent queries. The
convenience single-thread API stays — both APIs use semantically
identical traversal, only the dedup-state owner differs.

### API surface

```cpp
class SpatialHashScratch {
public:
    explicit SpatialHashScratch(IAllocator*);
    [[nodiscard]] usize capacity() const noexcept;
    u64 prepare_for_query(usize object_capacity);  // resize + bump gen + overflow-protect
    bool was_visited(u32 obj_idx, u64 gen) const noexcept;
    void mark_visited(u32 obj_idx, u64 gen) noexcept;
private:
    Array<u64> m_per_object_gen;  // resized on tree growth
    u64        m_current_gen{0};
};

// Convenience (single-thread, mutable tree state)
template <typename Fn> void overlap(const AABB3<T>&, Fn&&) const;
template <typename Fn> void radius(const Vec3<T>&, T r, Fn&&) const;
[[nodiscard]] std::optional<RayHit<u32>> raycast(const Ray3<T>&, T tmax) const noexcept;

// Thread-safe (caller scratch — one per worker fiber)
template <typename Fn> void overlap(const AABB3<T>&, SpatialHashScratch&, Fn&&) const;
template <typename Fn> void radius(const Vec3<T>&, T r, SpatialHashScratch&, Fn&&) const;
[[nodiscard]] std::optional<RayHit<u32>> raycast(const Ray3<T>&, SpatialHashScratch&, T tmax) const noexcept;
```

`find_overlapping_pairs` is purely-read const (no dedup state) — already
thread-safe by construction; no scratch overload needed.

### Implementation

Both API surfaces share `overlap_traverse_` / `radius_traverse_` /
`raycast_traverse_` template helpers parameterised over a `WasVisited` /
`MarkVisited` policy pair. Convenience overloads bind to lambdas writing
through the tree's `mutable` field via `const_cast`; scratch overloads
bind to lambdas writing through the caller's scratch. **Same dedup logic,
same emission order, same result on the same inputs** — provably
equivalent because both bodies share the exact same template instantiation
modulo the policy lambdas.

### Concurrent test via crd-jobs fiber pool (the whole point)

`SpatialHashJobsListener` (Catch2 listener pattern from
`test_bvh_parallel.cpp`'s `BvhJobsListener`) calls `crd::jobs::init` once
at test-binary startup with 4 workers + a 16 MB frame arena. The
concurrent test uses **`crd::jobs::parallel_for`** to fan out 400 query
tasks (16 distinct query boxes × 25 iterations each) across 16 jobs running
on the fiber pool. Each task constructs its own `TlsfAllocator` +
`SpatialHashScratch` — **maximally adversarial isolation**, fresh per query
— and asserts the result matches a single-thread reference computed
before fan-out.

Atomic `std::atomic<u32> mismatches` counter aggregates results across all
fibers; test asserts `mismatches == 0`. With 4 worker threads × 16 jobs ×
25 iterations under win-asan, any data race in the scratch path would
either produce a mismatch (functional check) or fire ASan's race
detection (instrumentation check). Both paths are clean.

### Tests added (6 cases / 1 fiber-job concurrent stress)

| Case | Coverage |
|---|---|
| scratch overlap byte-identical to single-thread overload | 20 trials × 300 random AABBs — set equality |
| scratch radius byte-identical | 5 trials × 3 radii × 200 AABBs |
| scratch raycast byte-identical | 10 trials × 100 random objects |
| **concurrent queries via crd-jobs fiber pool** | 16 queries × 25 iters = 400 fan-out tasks across 16 jobs / 4 worker fibers; per-task isolated scratch + allocator; atomic mismatch counter == 0 under win-asan |
| scratch reuses across queries | Verifies gen-counter bump, not array reset, drives dedup correctness across back-to-back queries |
| scratch grows as tree grows | Insert 5 → query → insert 50 → query; verify `scratch.capacity()` resized correctly |

Total v5d test count: **22 → 28** (+6). Project ctest: 2033 → 2040 win-debug.

### Locked decisions (carry into ADR-0076 §20 v5-close)

| # | Decision | Rationale |
|---|---|---|
| 12 | Two parallel API surfaces (convenience + scratch) | Clean single-thread call site; opt-in zero-alloc thread-safe path for jobified consumers |
| 13 | Scratch owns per-object generation array; tree never touched by scratch overloads | Provably race-free: each scratch is fiber-local |
| 14 | Both API surfaces share `*_traverse_` template helpers | Single dedup/traversal logic — no chance of divergence between paths |
| 15 | Test concurrent path via `crd::jobs::parallel_for` (NOT std::thread) | Cerid's substrate IS the fiber pool; tests must use the substrate they ship for |

### 5-config DoD post-hardening — all green

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 2040 / 2040 ctest |
| win-asan | PASS | full project ctest — race detection clean across the 400-task fiber-jobified test |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 2035 / 2035 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean |

## Open follow-ons (deferred — not v5d blockers)

- ~~**Caller-scratch overload for parallel queries**~~ ✅ **shipped same
  session as v5d-fast** — see "v5d-fast caller-scratch overload" above.
- **`find_overlapping_pairs` scratch param** — currently allocates the
  output Array per call. For eylem v3 hot-path: caller-supplied scratch
  (`DynamicBvhPairScratch` pattern in `crd-geometry-bvh`). Defer until
  eylem v3 surfaces.
- **f64 path performance** — uses the same scalar slab raycast as f32
  inner loop; precompute optimisation deferred until consumer surfaces
  (orbital-scale aerospace).
- **Dynamic bucket resize** — `bucket_count` is fixed at construction.
  Could grow on load-factor threshold (HashMap pattern). Defer until a
  consumer needs it.
- **Sparse hash variant** — empty buckets still consume 24-byte
  `Array<u32>` overhead. For ultra-large bucket counts, could use a
  `HashMap<u32, Array<u32>>` instead. Defer.

## Next

v5e — Dense `UniformGrid` (3D cell array; no hash, O(cell_count) memory).
For small bounded domains where hash collisions waste cycles. ~2 days.
Same module + same per-slice DoD (5 configs).
