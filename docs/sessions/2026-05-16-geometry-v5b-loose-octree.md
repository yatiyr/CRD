# Session log — 2026-05-16 — geometry v5b: Loose octree

> Phase 3.1.7 v5 `-spatial` cluster continues. Loose octree (Ulrich 2000) —
> dynamic AABB index with overlapping children-bounds (loose factor 2.0
> default). The workhorse for scene spatial culling + eylem broadphase
> + editor selection. Same `engine/geometry-spatial/` module as v5a;
> sister backends v5c R-tree / v5d SpatialHash / v5e UniformGrid follow.

## Scope landed

| Element | Path |
|---|---|
| Header (storage + API + inline overlap template) | `engine/geometry-spatial/include/crd/geometry/spatial/loose_octree.hpp` |
| Implementation (mutators + raycast + helpers) | `engine/geometry-spatial/src/loose_octree.cpp` |
| Typed wrapper layer | `engine/geometry-spatial/include/crd/geometry/spatial/octree_queries_typed.hpp` |
| Umbrella update | `engine/geometry-spatial/include/crd/geometry/spatial/spatial.hpp` |
| Tests | `tests/geometry-spatial/test_loose_octree.cpp`, `test_loose_octree_typed.cpp` |

## API surface

```cpp
namespace crd::geometry::spatial {

struct OctreeObjectId { u32 value; bool valid() const; };

template <MathScalar T> struct OctreeBuildOptions {
    AABB3<T> root_bounds;       // fixed (no auto-grow at v5b)
    T        loosening{2};      // Ulrich's pin
    u32      leaf_object_threshold{8};
    u8       max_depth{8};
};

template <MathScalar T> struct OctreeNode { /* 64 B — pinned */ };
static_assert(sizeof(OctreeNode<f32>) == 64);

template <MathScalar T> class LooseOctree {
public:
    LooseOctree(IAllocator*, const OctreeBuildOptions<T>&);

    [[nodiscard]] OctreeObjectId insert(const AABB3<T>&, u32 payload);
    void remove(OctreeObjectId);
    bool update(OctreeObjectId, const AABB3<T>& new_aabb);  // returns true on slow-path

    template <typename Fn> void overlap(const AABB3<T>& q, Fn&& on_hit) const;
    void overlap(const AABB3<T>& q, Array<u32>& out) const;

    [[nodiscard]] std::optional<RayHit<u32>>
    raycast(const Ray3<T>&, T tmax = +∞) const noexcept;

    // Diagnostics: bounds(), node_count(), object_count(), max_depth_used(),
    //              loose_aabb_of(node) — for tests + viz.
};
} // namespace
```

Typed `octree_queries_typed.hpp` provides `octree_insert<D, T>` /
`octree_update<D, T>` / `octree_overlap<D, T>` / `octree_raycast<D, T>` —
strip-compute-retag wrappers per ADR-0078 §5 D34. Same pattern as
`kd_queries_typed.hpp` (v5a) and `mesh_queries_typed.hpp` (v4).

## Algorithm — elite-tier choices (locked via advisor 2026-05-16)

1. **Loosening factor 2.0** — Ulrich 2000's recommendation. Loose AABB edge =
   2 × tight cell edge. Gives the "single-cell residency" property: an object
   whose tight AABB extent ≤ loose × cell-extent fits in *one* cell's loose
   AABB regardless of midplane proximity. Configurable via
   `OctreeBuildOptions::loosening`.

2. **Update fast-path predicate = AABB-fit only**, NOT center-in-cell.
   Ulrich's correctness invariant: a query touching the cell finds the object
   as long as its tight AABB fits within the cell's loose AABB. Center can
   drift outside the cell — only the AABB-fit matters. ~90%+ of small motions
   take the fast path; the slow path (loose-AABB violation) does
   remove + reinsert and returns `true`.

3. **Builder rejects** non-finite AABB, center outside root, extent >
   `loosening × root extent` (debug `CRD_ASSERT`). Queries TOLERATE non-finite
   query input — defensive `is_finite(ray.origin) && is_finite(ray.direction)`
   at `raycast` entry; loose-AABB-vs-NaN prune handles `overlap` symmetrically.
   Symmetric with v5a `kd_*` + ADR-0076 §15 BVH pin.

4. **Octant tiebreak via `>=`** — bit `i` of octant index =
   `(center[i] >= midplane[i]) ? 1 : 0`. "Lower octant wins" lex tiebreak
   when the center sits exactly on a midplane (analogous to v5a's X<Y<Z
   tiebreak on equal coord).

5. **Raycast = t-near-first child descent + `best_t` pruning**, NOT Morton.
   Pinned in the header: emission order is **NOT** part of the API contract —
   only the nearest result is. (BVH pattern, ADR-0076 §16 pin #2.) Lowest-
   payload-index tiebreak on equal `t`.

6. **Overlap = Morton-order child traversal** (000, 001, …, 111). Deterministic
   emission. No early-out → t-near-ordering would buy nothing.

7. **Object pool with separate stable handle**. `ObjectEntry { aabb, payload,
   cell_node, next_free, alive }` lives in a free-list-managed pool;
   `OctreeObjectId.value` = pool slot. Cell stores `Array<u32 obj_idx>`
   (allocated on first insert, capacity retained on `clear()` for reuse).
   Removing an object from a cell is `O(cell_count)` swap-with-last —
   bounded by `leaf_object_threshold` in non-max-depth cells.

8. **`leaf_object_threshold` is a SPLIT TRIGGER, not a hard cap.** Cells at
   `max_depth` retain all assigned objects (no deeper cell to push them to —
   correct by construction). Documented in the header.

9. **64-byte cell node, `static_assert`-pinned**. AABB(24) + 8×u32 children(32)
   + parent(4) + depth(1) + flags(1) + pad(2) = 64. One node per cache line.

10. **Lazy root** — root cell allocated on first insert. Empty trees cost
    nothing past the `LooseOctree` member fields. Same node + cell pools
    (Array<OctreeNode>, Array<CellObjects>) parallel — both grow together via
    `allocate_node`; freeing recycles via slot index in both.

## The fast-path test — Ulrich's invariant verification

The advisor flagged this as **the elite-quality discriminator** that brute-
force cross-validation can't catch. Without it, you've built a regular
octree that gets renamed.

```cpp
TEST_CASE("LooseOctree update fast-path: small motion within loose AABB causes ZERO restructure") {
    LooseOctree<f32> tree{&alloc, default_opts()};
    // Insert 100 objects (half_extent 0.1 → cell extent at depth 8 ≈ 0.78
    // → loose half ≈ 0.78 → fast-path window 0.68 > cell radius 0.39).
    // Every cell-resident object has fast-path room ≥ cell-radius by
    // construction → 100% fast-path under any cell-resident motion.
    auto snap_nodes = tree.node_count();
    auto snap_objects = tree.object_count();
    // Move every object by ±0.05.
    u32 fast_path_hits = 0;
    for (handle in handles) {
        bool restructured = tree.update(handle, slightly_moved_aabb);
        if (!restructured) ++fast_path_hits;
    }
    REQUIRE(tree.node_count()  == snap_nodes);
    REQUIRE(tree.object_count() == snap_objects);
    REQUIRE(fast_path_hits == 100U);  // 100% — by sizing arithmetic
}
```

**Sizing math**: cell at depth 8 has full extent 200/256 ≈ 0.78. Loose
factor 2 → loose half-extent ≈ 0.78. Object full extent 0.2 → fast-path
window `(loose_full - object_full) / 2 = (1.56 - 0.2) / 2 = 0.68`. Cell
radius = 0.39 < 0.68 → **any** cell-resident object survives the fast path
even if its center is at the cell's edge.

Companion test `LooseOctree update slow path triggers when AABB leaves loose
cell` verifies the slow path fires when needed (move object 50 units —
must restructure).

## Tests — 17 cases, brute-force cross-validation + fast-path test + edge cases

Suite breakdown:

| File | Cases | Coverage |
|---|---|---|
| `test_loose_octree.cpp` | 15 | empty / single-object / large-cloud overlap (brute-force × 20 query boxes) / raycast nearest / equal-t lowest-payload tiebreak / **fast-path 100%** / slow-path triggers when needed / loosening 1.5/2.0/3.0 result-count invariance / determinism under permuted insert order / insert/remove cycle stability / NaN tolerance / all-coincident / sizing pin |
| `test_loose_octree_typed.cpp` | 2 | typed insert+overlap round-trip / typed raycast returns typed `Quantity<Length, f32>` `t` |

**Brute-force overlap reference** is `O(N)` AABB-vs-AABB scan. Cross-validates
on 200-AABB random clouds × 20 query boxes; sorted-payload set equality.

**Insert/remove cycle**: insert 10 objects → remove evens → reinsert 5 new
(reusing free pool slots) → verify all surviving handles still queryable +
counts correct. Catches handle-stability bugs.

**Determinism under permutation**: build the tree with `[0..99]` insert
order vs `[99..0]` insert order, query with the same box, sort results,
compare elementwise. Verifies the algorithm is order-invariant on the
result set (the tree topology can differ if cells get split at different
times, but the result set must not).

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 1996 / 1996 ctest |
| win-asan | PASS | full project ctest |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 1991 / 1991 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean (warnings on `optional` unchecked-access in REQUIRE-then-deref idioms + intentional `mt19937` constant seeds — non-blocking, same as v5a) |

`scripts/per-slice-check.ps1` gates the first 4; win-shipping-profile run
separately. Total full-project ctest: **1979 → 1996 win-debug** (+17 cases
from this slice).

## Two debugging passes en route

1. **Fast-path test failed first run** — initial object half-extent 0.5 with
   max_depth 8 cells (~0.78 extent) gives fast-path window only 0.28; many
   randomly-placed objects sit > 0.28 from cell center, so 0.05 motion
   pushes them past the loose AABB. Diagnosed via the sizing arithmetic
   above; fix = shrink object half_extent to 0.1 so fast-path window 0.68
   exceeds cell radius 0.39 always. The test now asserts 100% fast-path
   hits (a stronger invariant than "≥ 80%" — by sizing-construction).

2. **NaN tolerance test failed first run** on raycast — `intersect_ray_aabb_robust`
   can return TRUE for non-finite ray inputs (NaN-vs-NaN comparisons
   unspecified in the slab math). Defensive `is_finite(ray.origin) && is_finite(
   ray.direction)` short-circuit at the raycast entry. Cleaner than
   patching the inner intrinsic; same pattern as the v5a kd_radius NaN
   handling (early prune).

3. **Non-ASCII `→` in test name** ("misses everything → nullopt") triggered
   `crd-no-non-ascii-test-names` guard — the same bug class that bit
   v3b/v3c. Fixed (`->`); per `feedback_per_slice_run_ctest.md`,
   `per-slice-check.ps1` runs ctest, so the guard fires per-slice now.

## Decisions locked (for ADR-0076 §20 v5-close amendment)

| # | Decision | Rationale |
|---|---|---|
| 1 | Loosening factor 2.0 default | Ulrich 2000's pin; loose AABB = 2× tight cell |
| 2 | Update fast-path = AABB-fit only (NOT center-in-cell) | Ulrich's correctness invariant; ~90%+ of small motions |
| 2.5 | `target_depth_for` uses GUARANTEED-FAST-PATH formula `(loose - 1) × R / extent` (NOT Ulrich's classical centered-fit `loose × R / extent`) + placement-fit `CRD_ASSERT` in `descend_and_insert` | Advisor 2026-05-16 caught the centered-fit-vs-off-center hazard. Corrected formula guarantees object fits the loose AABB regardless of cell-resident placement; assertion catches future regressions |
| 3 | Insert REJECTS non-finite, out-of-root, oversized; queries TOLERATE non-finite | Symmetric with ADR-0076 §15 BVH + v5a kd_*; defensive `is_finite` at raycast entry |
| 4 | Octant tiebreak `>=` (lower-octant-wins on midplane) | Same lex-tiebreak family as v5a's X<Y<Z |
| 5 | Raycast = t-near-first child descent (NOT Morton); emission order NOT part of API | BVH pattern — only nearest hit is the contract |
| 6 | Overlap = Morton-order child traversal (000…111) | Deterministic emission; no early-out incentive |
| 7 | Lowest-payload-index tiebreak on equal raycast `t` | ADR-0076 §4 pin #11 |
| 8 | Object pool with `OctreeObjectId.value = pool slot`, free-list, no generation counter | DynamicBvh precedent (handle invalid after remove, by API contract) |
| 9 | Cell stores `Array<u32 obj_idx>`, allocated on first insert | Matches advisor recommendation; better cell traversal locality than intrusive list |
| 10 | `leaf_object_threshold` = split TRIGGER, NOT hard cap | Max-depth cells retain all assigned objects (correct by construction) |
| 11 | 64-byte cell node, `static_assert`-pinned | One per cache line; field bloat fails CI |
| 12 | Lazy root allocation | Empty trees cost nothing past the `LooseOctree` fields |
| 13 | Cells NOT auto-collapsed on empty | Cheap in steady-state churn; optional `compact()` follow-on if a consumer surfaces |

These will fold into the §20 amendment at v5-close.

## Elite-tier correctness fix shipped after second advisor pass

Initial `target_depth_for` used Ulrich's classical *centered-fit* formula
`loose × R / extent`. Advisor 2026-05-16 caught a real but subtle correctness
hazard: that formula gives the deepest depth at which the object fits when
its center is *exactly* at cell center. For off-center placement (the common
case), the object's tight AABB can extend partially outside the cell's loose
AABB. Then a query overlapping the object's tight AABB but missing the cell's
loose AABB silently fails to find the object — pathological adversarial cases
could lose objects.

**Switched to the GUARANTEED-FAST-PATH formula** `(loosening - 1) × R / extent`:
worst-case off-center placement at `|offset| = cell_extent/2`, fits when
`extent ≤ (loose - 1) × cell_extent`, gives `2^d ≤ (loose - 1) × R / extent`.
For loose = 2 (the default) this collapses to `R / extent`. For loose = 1
(classical octree, no overlap) it correctly gives root-only placement.

**Added a defensive placement assertion** in `descend_and_insert`:
`CRD_ASSERT(aabb_encloses(loose_aabb_of(terminal_cell), object.aabb))`. Any
future regression in either the depth formula or the loose-AABB geometry
fails CI immediately.

5-config DoD re-run after the fix — all green; brute-force overlap +
fast-path 100% tests still pass (the corrected formula gives strictly more
generous fast-path windows than the original).

## Open follow-ons (deferred — not v5b blockers)

- **f64 raycast precompute** — robust ray-AABB precompute is `f32`-only today;
  the f64 path uses a scalar slab traversal without ordering optimisation.
  Acceptable until an f64 consumer (orbital-scale aerospace) surfaces.
- **`compact()`** — collapse empty subtrees after churn. Useful for editor
  long-running edit sessions. Defer to v5b-fast / consumer surfacing.
- **`f64` raycast t-near ordering** — currently unordered DFS for f64;
  performance fine for typical scenes, optimisation follow-on if needed.

## Next

v5c — R-tree (Beckmann 1990 R*-tree variant). Static-or-slow-change AABB
index; better packing than octree for cooked/static level data. May split
into v5c-a (build + overlap) + v5c-b (k-NN + delete). ~4 days. Same
`crd-geometry-spatial` module; same per-slice DoD (5 configs).
