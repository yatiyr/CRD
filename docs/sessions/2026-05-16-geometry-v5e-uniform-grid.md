# Session log — 2026-05-16 — geometry v5e: UniformGrid

> Phase 3.1.7 v5 `-spatial` cluster's last backend before scene-index
> bringup + queries-extension + close. Dense bounded-domain 3D cell array —
> distinct from v5d SpatialHash (no hash, no collision, O(cell_count)
> memory regardless of population). Same `engine/geometry-spatial/` module.
> **`UniformGridScratch` + scratch overloads + crd-jobs concurrent test
> shipped from day 1** per the elite mandate (no v5e-fast follow-on).

## Scope landed

| Element | Path |
|---|---|
| Header | `engine/geometry-spatial/include/crd/geometry/spatial/uniform_grid.hpp` |
| Implementation | `engine/geometry-spatial/src/uniform_grid.cpp` |
| Typed wrapper | `engine/geometry-spatial/include/crd/geometry/spatial/grid_queries_typed.hpp` |
| Umbrella update | `engine/geometry-spatial/include/crd/geometry/spatial/spatial.hpp` |
| Tests | `tests/geometry-spatial/test_uniform_grid.cpp`, `test_uniform_grid_typed.cpp` |

## API surface

```cpp
namespace crd::geometry::spatial {

struct UniformGridObjectId { u32 value; bool valid() const; };
struct UniformGridPair { u32 a, b; };  // canonical (a < b)

template <MathScalar T>
struct UniformGridConfig {
    AABB3<T> bounds;       // domain — finite, min < max
    T        cell_size{1};
};

class UniformGridScratch {       // one per worker fiber for thread-safe queries
    explicit UniformGridScratch(IAllocator*);
    usize capacity() const;
    u64 prepare_for_query(usize object_capacity);
    bool was_visited(u32, u64) const; void mark_visited(u32, u64);
};

template <MathScalar T> class UniformGrid {
public:
    UniformGrid(IAllocator*, const UniformGridConfig<T>&);

    [[nodiscard]] UniformGridObjectId insert(const AABB3<T>&, u32 payload);
    void remove(UniformGridObjectId);
    bool update(UniformGridObjectId, const AABB3<T>& new_aabb);  // false on fast path

    // Convenience (single-thread, mutable tree state) + scratch (thread-safe)
    template <typename Fn> void overlap(const AABB3<T>&, Fn&&) const;
    template <typename Fn> void overlap(const AABB3<T>&, UniformGridScratch&, Fn&&) const;
    void overlap(const AABB3<T>&, Array<u32>&) const;
    void overlap(const AABB3<T>&, UniformGridScratch&, Array<u32>&) const;

    template <typename Fn> void radius(const Vec3<T>&, T r, Fn&&) const;
    template <typename Fn> void radius(const Vec3<T>&, T r, UniformGridScratch&, Fn&&) const;
    void radius(const Vec3<T>&, T r, Array<u32>&) const;
    void radius(const Vec3<T>&, T r, UniformGridScratch&, Array<u32>&) const;

    [[nodiscard]] std::optional<RayHit<u32>> raycast(const Ray3<T>&, T tmax) const noexcept;
    [[nodiscard]] std::optional<RayHit<u32>> raycast(const Ray3<T>&, UniformGridScratch&, T tmax) const noexcept;

    void find_overlapping_pairs(Array<UniformGridPair>&) const;  // already const-safe — no scratch needed

    // Diagnostics: nx/ny/nz/cell_count/bounds/cell_size/object_aabb/object_payload/max_cell_size/load_factor
};
} // namespace
```

Typed `grid_queries_typed.hpp`: `uniform_grid_insert<D, T>` /
`uniform_grid_overlap<D, T>` / `uniform_grid_radius<D, T>` /
`uniform_grid_raycast<D, T>` strip-compute-retag wrappers per ADR-0078 §5 D34.

## Algorithm — dense bounded grid + Amanatides-Woo with grid-bounds clip

1. **Construction**: `(nx, ny, nz) = ceil((bounds.max - bounds.min) / cell_size)`.
   Total cells = `nx · ny · nz`. Sanity cap at 256M cells (debug `CRD_ASSERT`)
   — UniformGrid is the wrong tool past that point; use `SpatialHash` for
   sparse/unbounded domains.

2. **Cell indexing**: flat `cell_idx = (z·ny + y)·nx + x`. No hash.

3. **Out-of-bounds AABBs CLAMP to grid** (insertion silently restricted to
   overlapping cells). Object whose AABB lies wholly outside is still
   inserted (handle valid, queryable via accessors), but no spatial query
   can reach it. Documented contract: domain bounds are an optimisation
   hint, not a strict membership.

4. **Per-cell `Array<u32>`**, lazily allocated on first push_back. ~32 B
   overhead per cell from the `Array` itself even when empty — the
   O(cell_count) memory cost the docs flag.

5. **Update fast-path** = same cell range (or both wholly-outside) ⇒
   refresh AABB only, return `false`. Slow path = remove from old cells +
   insert into new.

6. **Per-query generation-counter dedup** identical to v5d SpatialHash —
   `mutable u64 m_query_generation` for the convenience API; per-scratch
   counter for the thread-safe API.

7. **Raycast = grid-bounds-clipped Amanatides-Woo voxel traversal**:
   - Slab-test ray vs grid AABB → `(t_entry, t_exit_grid)`. Miss ⇒ nullopt.
   - Clamp `t_entry` to ≥ 0 (rays starting inside the grid).
   - Compute entry point in grid-relative coords + start cell (clamped to
     `[0, n-1]` per axis to handle the boundary case `entry == grid_max`).
   - Per-axis `step`, `tDelta`, `tMax` (relative to ray's world origin).
   - Walk: scan cell, prune by `best_t`, **advance ALL axes whose `tMax`
     ties for minimum** (corner-grazing safe — same fix as v5d), stop
     when cell idx leaves grid OR `t_next > best_t` OR `t_next > tmax` OR
     step cap (2²² cells).
   - Lowest-payload tiebreak on equal `t` (§4 pin #11).

8. **`find_overlapping_pairs`** — dense linear cell scan (no hash overhead):
   for each cell, emit `(min, max)` AABB-overlapping pairs; cross-cell
   duplicates removed via `crd::containers::sort` + manual unique. Eylem
   v3 XPBD broadphase target when the simulation domain is a bounded box.

9. **Two API surfaces from day 1** (NOT a v5e-fast follow-on):
   convenience (single-thread, `mutable` tree state) + scratch (thread-safe).
   Both share `*_traverse_` template helpers parameterised over
   `WasVisited`/`MarkVisited` policy lambdas — provably equivalent.

10. **Builder REJECTS** non-finite + inverted AABB (debug `CRD_ASSERT`);
    queries TOLERATE non-finite + zero-direction ray (defensive `is_finite`
    short-circuit at API surface).

## Locked design choices

| # | Decision | Rationale |
|---|---|---|
| 1 | Dense flat `Array<Array<u32>>` sized `nx·ny·nz` (no hash) | O(1) cell lookup with no collision overhead; the v5e differentiator vs v5d |
| 2 | `(nx, ny, nz) = ceil(extent/cell_size)`; flat idx `(z·ny+y)·nx+x` | Standard dense-grid layout |
| 3 | Out-of-bounds AABBs CLAMP to grid (no rejection) | Domain bounds are an optimisation hint, not a strict membership; matches user expectation |
| 4 | Per-cell `Array<u32>` allocated lazily on first push_back | Empty cells cost only `sizeof(Array<u32>)` ~32 B; non-empty cells pay normal capacity-growth |
| 5 | 256M cell sanity cap (debug `CRD_ASSERT`) | Past 256M cells the cells-array overhead alone exceeds 8 GB — the wrong tool, document the consumer should use `SpatialHash` |
| 6 | Update fast-path = same-cell-range detection | ~95%+ of small motions skip rebucketing |
| 7 | Per-query gen-counter dedup (convenience) + per-scratch gen counter (thread-safe) | Zero allocation per query in both paths |
| 8 | Raycast = grid-bounds-clipped Amanatides-Woo (NOT recursive) | Right algorithm for uniform grid; clip-then-walk handles ray-starts-outside cleanly |
| 9 | Advance ALL axes whose `tMax` ties for minimum | Corner-grazing rays preserved (same fix as v5d) |
| 10 | Lowest-payload tiebreak on equal raycast `t` / k-NN distance | ADR-0076 §4 pin #11 |
| 11 | Two API surfaces (convenience + scratch) shipped from day 1 | User mandate "elite, no deferring"; thread-safe path is first-class |
| 12 | Test concurrent path via `crd::jobs::parallel_for` (NOT std::thread) | Cerid's substrate IS the fiber pool; tests must use the substrate they ship for (locked at v5d-fast) |

## Tests — 26 cases / 5-config DoD PASS

Suite breakdown:

| File | Cases | Coverage |
|---|---|---|
| `test_uniform_grid.cpp` | 23 | empty / cell-counts via ceil / single insert / **out-of-bounds clamping** / brute-force overlap × 20 / brute-force radius × 5×3 / **grid-bounds-clipped Amanatides-Woo raycast** (nearest + lowest-payload tiebreak + negative direction + corner-grazing diagonal + miss-no-objects + ray-starts-outside-grid + slab-rejects-grid-miss) / update fast/slow paths / insert/remove cycle handle stability / `find_overlapping_pairs` brute-force xval / NaN tolerance / dedup-on-many-cell-object / **scratch parity** for overlap+radius+raycast / **concurrent queries via `crd::jobs::parallel_for` fiber pool** (16 queries × 25 iters = 400 fan-out tasks across 16 jobs / 4 worker fibers, per-task isolated `TlsfAllocator` + `UniformGridScratch`, atomic mismatches == 0 under win-asan) |
| `test_uniform_grid_typed.cpp` | 3 | typed insert+overlap / typed radius / typed raycast returns typed `Quantity<Length, f32>` `t` |

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 2066 / 2066 ctest |
| win-asan | PASS | full project ctest — race detection clean across the 400-task fiber-jobified test |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 2061 / 2061 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean |

`scripts/per-slice-check.ps1` gates the first 4; win-shipping-profile run
separately. Total full-project ctest: **2040 → 2066 win-debug** (+26 cases
from this slice).

## One sizing pass en route

The first DoD pass surfaced 16 OOM failures on the test fixture's 4 MB
`TlsfAllocator`. Diagnosis: `default_cfg(half=50, cs=1)` creates a 100³ =
1M-cell grid; `Array<Array<u32>>` storage alone is ~32 MB at ~32 B per
empty `Array<u32>` overhead. Fix two-pronged: (a) bump `AllocFixture` to
64 MB, (b) shrink `default_cfg`'s default to `half=10, cs=1` ⇒ 20³ = 8000
cells (~256 KB cells overhead). Tests requiring larger spatial extent use
explicit configs documented inline. Concurrency corpus's nested allocator
also bumped from 4 MB → 32 MB to fit its 50³ cell grid + 400 inserted
objects + 16 reference Arrays.

The empty-test's `nx == 100` expectation also needed to track the new
default (`nx == 20`); two reach-tests assumed AABBs at coords ±25 fit the
grid (now ±10) — fixed by either (i) using a 50-half grid for those
specific tests OR (ii) moving coords inside the smaller default. Documented
the user contract explicitly: bounds matter, choose them sized for your
data.

## Comparison vs other v5 backends

| Property | KdTree (v5a) | LooseOctree (v5b) | RTree (v5c) | SpatialHash (v5d) | UniformGrid (v5e) |
|---|---|---|---|---|---|
| Best for | Static point set + k-NN | Dynamic broadphase | Static AABB + cooked levels | Particle/swarm broadphase | Bounded uniform-density domains |
| Cell lookup | tree descent | tree descent | tree descent | hash + bucket | flat array idx |
| Memory | tight | per-node + per-object | per-node + indirection | bucket Array<u32> only for non-empty | **per-cell Array<u32> always** (O(cell_count)) |
| Hash collision penalty | n/a | n/a | n/a | yes (foreign objects in same bucket) | **none** |
| Bounded domain required | no | no | no | no | **yes** (configured at construction) |
| Pair query | no | no | no | yes (sort+unique) | yes (dense linear cell scan) |
| Raycast | no | t-near BVH-style | t-near BVH-style | Amanatides-Woo (unbounded) | **Amanatides-Woo + grid-bounds clip** |
| Thread-safe scratch | no (k-NN out-param only) | no | no | yes (v5d-fast) | **yes (day 1)** |

Choose UniformGrid when:
- The simulation domain is a bounded box AND known at construction.
- Cell count is small enough that O(cell_count) Array overhead is acceptable
  (rule of thumb: ≤ 1M cells = ~32 MB cells overhead).
- Density is roughly uniform (every cell roughly populated) — defeats hash's
  O(1) advantage; dense lookup wins on cache locality.
- Common cases: voxel scenes, bounded particle emitters, CFD coarse cells,
  pathfinding occupancy grids, cooked-level uniform-detail areas.

## Decisions locked (for ADR-0076 §20 v5-close amendment)

All 12 decisions from the table above. Of cross-substrate significance:
* **Out-of-bounds CLAMPING (NOT rejection)** is the dense-grid-specific
  policy — caller-provided bounds are an optimisation hint. v5e is the
  first substrate to formalise this; future grids (v8 voxel-Delaunay)
  may inherit.
* **Two API surfaces from day 1** (vs v5d-fast retrofit) is the new
  default for any future spatial substrate that has dedup state. Locked
  in the §20 amendment.

## Open follow-ons (deferred — not v5e blockers)

- **`find_overlapping_pairs` scratch param** — currently allocates the
  output Array per call. Eylem v3 broadphase hot path will want
  caller-supplied scratch (`DynamicBvhPairScratch` pattern). Defer.
- **`crd::containers::Array<bool>` bitset variant of scratch** — for
  domains where 8 B/object is too much, the 1 B/object bitset trades
  O(N) per-query reset for tighter memory. Defer until consumer surfaces.
- **Sparse-storage variant** for very-large bounded domains — combine
  dense indexing with hash-allocated buckets. Defer to a future
  "HybridGrid" if a consumer needs it (10M-cell scenes are the threshold).

## Phase 3.1.7 v5 `-spatial` cluster — 5/7 backends shipped 2026-05-16

| Backend | Status | LOC engine | LOC tests |
|---|---|---|---|
| v5a KdTree | ✅ 2026-05-16 | ~700 | ~750 |
| v5b LooseOctree | ✅ 2026-05-16 | ~750 | ~530 |
| v5c RTree (R*-tree) | ✅ 2026-05-16 | ~1400 | ~570 |
| v5d SpatialHash + v5d-fast scratch | ✅ 2026-05-16 | ~850 | ~750 |
| **v5e UniformGrid (with scratch from day 1)** | ✅ 2026-05-16 | **~700** | **~700** |

Total v5 backend code: ~4400 LOC engine + ~3300 LOC tests. The five
backends share patterns established in v5a (lex-tuple comparators, raw-
algorithm + typed-wrapper boundary), v5b (loose-AABB invariant +
guaranteed-fast-path placement), v5c (Beckmann split + STR + Hjaltason-
Samet + indirection-table handles), v5d (Teschner hash + Amanatides-Woo
voxel traversal + per-query gen-counter dedup), v5d-fast (caller-scratch
overload pattern + crd-jobs concurrent test).

## Next

Phase 3.1.7 v5 `-spatial` cluster continues:
* **v5-index-bringup** — realize `scene::SpatialBvhIndex` reserved-shell
  (default backend = LooseOctree). The first non-reserved spatial index
  in `crd-scene`. ~2-3 days.
* **v5-queries-extension** — extend `crd/geometry/queries.hpp` compile-
  time-overload facade to dispatch over all 5 v5 backends. ~1 day.
* **v5-close** — ADR-0076 §20 amendment + `docs/systems/geometry-spatial.md`
  + 18-config full sweep + ROADMAP/context/MEMORY sync. ~1 day.

After v5-close, Phase 3.1.7 v6 `-polygon` cluster opens (Vatti + CDT +
Bentley-Ottmann + ear clipping).
