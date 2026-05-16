# Session log — 2026-05-16 — geometry v5c: R*-tree

> Phase 3.1.7 v5 `-spatial` cluster continues. Full Beckmann 1990 R*-tree —
> the right index for **static-or-slow-change** AABB data. Includes STR
> (Sort-Tile-Recursive) bulk-load + Hjaltason-Samet 1999 incremental k-NN.
> Same `engine/geometry-spatial/` module as v5a + v5b. **NOT a quadratic-
> split simplification** — the user mandate is elite, so full R*-tree split
> heuristics + forced reinsertion shipped.

## Scope landed

| Element | Path |
|---|---|
| Header (storage + API + inline overlap template) | `engine/geometry-spatial/include/crd/geometry/spatial/rtree.hpp` |
| Implementation (mutators + split + reinsert + STR + queries) | `engine/geometry-spatial/src/rtree.cpp` |
| Typed wrapper layer | `engine/geometry-spatial/include/crd/geometry/spatial/rtree_queries_typed.hpp` |
| Umbrella update | `engine/geometry-spatial/include/crd/geometry/spatial/spatial.hpp` |
| Tests | `tests/geometry-spatial/test_rtree.cpp`, `test_rtree_typed.cpp` |

## API surface

```cpp
namespace crd::geometry::spatial {

struct RTreeLeafId { u32 value; bool valid() const; };

inline constexpr u32 k_rtree_max_entries = 16; // M
inline constexpr u32 k_rtree_min_entries = 5;  // m ≈ 30% of M
inline constexpr u32 k_rtree_reinsert_p  = 4;  // floor(0.3 × M)

template <MathScalar T> struct RTreeEntry { AABB3<T> aabb; u32 payload; u32 handle; };
template <MathScalar T> struct RTreeNode  { RTreeEntry<T>[M]; u32 entry_count; u32 parent; u8 level; ... };

template <MathScalar T> class RTree {
    explicit RTree(IAllocator*);

    [[nodiscard]] RTreeLeafId insert(const AABB3<T>&, u32 payload);
    void remove(RTreeLeafId);

    // STR bulk-load (Leutenegger 1997) — optimal packing for static data.
    void bulk_load(ConstSpan<AABB3<T>>, ConstSpan<u32>, Array<RTreeLeafId>& out_handles);

    template <typename Fn> void overlap(const AABB3<T>& q, Fn&& on_hit) const;
    void overlap(const AABB3<T>& q, Array<u32>& out) const;

    [[nodiscard]] std::optional<RayHit<u32>> raycast(const Ray3<T>&, T tmax = +∞) const noexcept;

    struct Neighbor { u32 payload; T distance_squared; };
    void nearest_n(const Vec3<T>& query, usize k, Array<Neighbor>& out) const noexcept;

    // Diagnostics: bounds(), depth(), node_count(), leaf_count(), entry_aabb(id),
    //              entry_payload(id), validate() (debug structural check).
};
} // namespace
```

Typed `rtree_queries_typed.hpp`: `rtree_insert<D, T>` / `rtree_overlap<D, T>` /
`rtree_raycast<D, T>` / `rtree_nearest_n<D, T>` strip-compute-retag wrappers
per ADR-0078 §5 D34.

## Algorithm — full Beckmann 1990 R*-tree

1. **choose-subtree (§4.1)** — at the level above leaves, minimise **overlap
   enlargement** (lex tiebreak: area enlargement → area → child idx). At
   higher levels, minimise area enlargement. The overlap-enlargement metric
   distinguishes R*-tree from Guttman's base R-tree.

2. **split (§4.2)** — for each axis, sort entries TWICE (by lower bound + by
   upper bound). For each of the (M-2m+2) = 8 distributions per sort, sum
   the perimeters of the two resulting groups. Pick the axis minimising the
   sum-of-perimeters across all distributions (lex tiebreak: x<y<z). Within
   that axis, pick the distribution minimising overlap (lex tiebreak: area).
   Canonical R*-tree split.

3. **forced reinsertion (§4.3)** — on insertion overflow, if this LEVEL has
   not yet been "treated" during this insert call, remove `floor(M × 0.3) = 4`
   entries farthest from node centre and re-insert them from the root in
   distance-ASCENDING order. Mark the level as treated via `m_treated_levels`
   bitmask (one-shot per insert per level). Globally compacts the tree on
   inserts and avoids cascading splits.

4. **delete + condense-tree (Guttman §3.4)** — find leaf, remove entry, walk
   up: if a node underflows below `m`, collect orphans, remove node from
   parent, free node. Reinsert orphans at their ORIGINAL level. Root special
   case: if root has 1 child + isn't a leaf, promote the child.

5. **STR bulk load (Leutenegger 1997)** — for N entries with M-capacity
   leaves: `L = ceil(N/M)` total leaves, `S = ceil(sqrt(L))` vertical slabs.
   Sort entries by x-midpoint, partition into S slabs. Sort each slab by
   y-midpoint, partition into leaves of M each. Recursively pack levels
   above. Optimal packing for cooked-level data; the right tool when
   consumer authors a static scene.

6. **k-NN — Hjaltason-Samet 1999 incremental NN** — priority queue ordered
   by min-distance from query point to AABB. Pop closest item; if leaf
   entry, emit; if node, expand children into PQ. Stops when k results
   emitted. Optimal — no extra work beyond what's needed. Lex tiebreak:
   `(distance², payload)` for stable equal-distance behaviour. PQ uses
   `crd::containers::push_heap` / `pop_heap` (deterministic across
   compilers).

## Locked design choices (advisor 2026-05-16)

| # | Decision | Rationale |
|---|---|---|
| 1 | M=16, m=5 (≈30%), p=4 forced-reinsert | Beckmann §6 + Boost.Geometry default; m/M=0.3 hits the published Beckmann sweet spot |
| 2 | Full R*-tree split (NOT Guttman quadratic) | Min-perimeter axis pick + min-overlap distribution = the killer R*-tree differentiator |
| 3 | Forced reinsertion (NOT split-only) on first overflow per level per insert | Beckmann §4.3 — globally compacts tree, avoids cascading splits |
| 4 | choose-subtree: leaf-parent uses overlap-enlargement, higher uses area-enlargement | Beckmann §4.1 |
| 5 | Lex tiebreak everywhere: split (x<y<z, then area, then payload), choose-subtree (4-tuple), reinsert-distance (then payload), STR (xc → yc → zc → payload) | Cross-compiler bit-identical given a fixed input order |
| 6 | Stable handle = u32 slot in `m_locations[handle] -> {node, entry}`, updated whenever split/reinsert/condense moves the entry | R*-tree splits move entries — handle-as-position would invalidate; indirection table is the canonical R-tree handle stability technique |
| 7 | Two-field RTreeEntry: `payload` (user) + `handle` (stable ref). 32 B per entry | Inline `overlap` template emits raw user payload at zero cost (no indirection); split bookkeeping uses `handle` field |
| 8 | STR bulk-load constructor — first-class API, NOT deferred | User mandate "elite, no deferring"; ~7× faster than sequential insert for static data |
| 9 | Hjaltason-Samet 1999 k-NN with `crd::containers::push_heap`/`pop_heap` priority queue | Optimal incremental algorithm; deterministic heap algos cross-compiler |
| 10 | Lowest-payload tiebreak on equal raycast `t` + equal k-NN distance | ADR-0076 §4 pin #11 |
| 11 | Builder REJECTS non-finite (debug `CRD_ASSERT(is_finite(aabb))`); queries TOLERATE non-finite (defensive `is_finite` short-circuit at API surface) | Symmetric with v5a/v5b + ADR-0076 §15 |
| 12 | `validate()` walks the tree asserting structural invariants (parent links, AABB enclosure-by-construction, leaf level == 0, size in [m, M] except root, handle locations match) | Caught the v5c-3 storage/lookup field-mixup bug at first run |

## The R*-tree split correctness mini-proof in code

The advisor's chief concern with R*-tree implementations is the split
correctness. The split picks (axis, sort, k) tuple. For elite quality I
verified the chain:

* **Axis pick** uses sum-of-perimeters (across both sorts × all distributions)
  as the per-axis score. Min-axis wins (lex tiebreak x<y<z).
* **Distribution pick** within chosen axis uses min-overlap-area (lex tiebreak
  min-total-area).
* **Distribution materialisation** — re-sort on the chosen (axis, sort_kind),
  take entries `[0..k)` to original node, `[k..N)` to new sibling.

Determinism: every sort is `crd::containers::sort` with a 4-tuple lex
comparator. All ties broken by `payload` (the user data) for cross-compiler
reproducibility.

## Tests — 17 cases / 5-config DoD PASS

Suite breakdown:

| File | Cases | Coverage |
|---|---|---|
| `test_rtree.cpp` | 15 | empty / single insert / depth tracking / **first overflow ⇒ reinsert** / **second overflow ⇒ split** / brute-force overlap × 20 query boxes / raycast nearest / equal-`t` lowest-payload tiebreak / brute-force k-NN k∈{1,5,20} / insert/remove cycle handle stability / **STR bulk-load brute-force xval × 10 query boxes** / STR depth ≤ sequential / **permutation determinism: insert order changes tree but result SET matches** / NaN tolerance (overlap + raycast + k-NN) / sizing pin |
| `test_rtree_typed.cpp` | 2 | typed insert+overlap round-trip / typed raycast returns typed `Quantity<Length, f32>` `t` |

Every test using random data uses a TLSF allocator + named (not default)
allocator per `feedback_named_allocators_in_tests.md`.

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 2012 / 2012 ctest |
| win-asan | PASS | full project ctest |
| win-shipping | PASS | LTCG-clean (after `[[maybe_unused]]` fix on `validate()` debug-only `h` lookup) |
| win-shipping-profile | PASS | 2007 / 2007 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean |

`scripts/per-slice-check.ps1` gates the first 4; win-shipping-profile run
separately. Total full-project ctest: **1996 → 2012 win-debug** (+16 cases
from this slice).

## Three debugging passes en route

1. **Storage refactor** — initial design used `entry.payload = handle` with
   parallel `m_user_payloads[handle]` lookup. Cleaner: widen `RTreeEntry` to
   carry both `payload` (user) and `handle` (stable ref). Inline `overlap`
   template now emits raw user payload at zero cost.

2. **`validate()` field-mixup** — the structural-validate function looked up
   handles via `entry.payload` (the OLD layout). Fixed to `entry.handle`.
   The bug surfaced because validate() asserted at the bulk-load corpus
   (200+ entries → many leaf nodes → several handles failed lookup).

3. **Non-ASCII `—` em-dash + `→` in test name** — same bug class that bit
   v3b/v3c/v5b. Fixed (`:`); per-slice-check runs ctest so the
   `crd-no-non-ascii-test-names` guard catches it.

4. **Shipping-mode unused-variable** — `validate()` keeps `h` as a local
   for assertion clarity; `CRD_ASSERT` strips in NDEBUG → MSVC `/W4 /WX`
   flags `h` unused. `[[maybe_unused]]` fix.

## Self-review correctness fix shipped after first DoD pass

After the first 5-config DoD passed, advisor self-review caught a latent
worst-case stack-overflow risk. The iterative DFS query stacks pushed up
to M=16 child frames per interior node visit, so worst-case `sp` grew by
M-1 per descent step. For depth-`D` tree with full branching, max
`sp = D × (M-1)`. Original `k_rtree_max_stack = 32` was safe only for
depth ~2; a 500-entry tree (depth 3) could approach 48 frames — within
tolerance NOW (R*-tree's well-balanced property + best_t pruning kept it
under threshold) but **brittle**. Bumped to:

* **256** for raycast / nearest_n (covers depth 16 at full M branching →
  ~10^19 entries — cosmic).
* **256** for the inline `overlap` template (was 64).
* **1024** for `validate()` (paranoia — only used in debug builds).

5-config DoD re-run after the hardening — all green again. A v5c-fast
follow-on can introduce caller-supplied scratch (`crd::containers::Array`)
for hot-path consumers — same `DynamicBvhPairScratch` pattern used in
`crd-geometry-bvh`. Not a v5c blocker.

## Comparison vs LooseOctree (v5b)

| Property | LooseOctree (v5b) | RTree (v5c) |
|---|---|---|
| Best for | Highly dynamic broadphase (per-tick update) | Static-or-slow-change AABB data + cooked levels |
| Update fast-path | YES — Ulrich invariant (~90%+ small motions) | No fast path; update = remove + reinsert |
| Insert O(?) | O(depth) | O(depth) + occasional split/reinsert (amortised) |
| Tree quality | Loose factor 2 = looser cells | Tight cells, no overlap inflation |
| Bulk-load | No | YES — STR (~7× faster than sequential) |
| k-NN | No (use overlap with growing query box) | YES — Hjaltason-Samet incremental |
| Memory per node | 64 B (pinned) | ~528 B (M=16 entries × 32 B + header) |

Choose LooseOctree for eylem broadphase + scene culling; RTree for cooked
levels + editor selection + spatial-query-heavy queries.

## Decisions locked (for ADR-0076 §20 v5-close amendment)

All 12 decisions from the table above carry forward to the §20 amendment.
Key ones for cross-substrate consistency:
* Indirection-table handles (vs slot-position handles) — the right approach
  for any tree where the stored entry can move (R-tree, B-tree, etc.). The
  free-list-of-handles + `m_locations[handle] -> {node, entry}` pattern is
  the template; v5d/v5e may use it if their entries also relocate.
* Lowest-payload tiebreak on equal-distance / equal-`t` is now consistent
  across BVH (v1) / Mesh (v4) / KdTree (v5a) / LooseOctree (v5b) / RTree
  (v5c). The §4 pin #11 holds engine-wide.

## Next

v5d — Spatial hash (Teschner 2003). Fixed-grid spatial hash with configurable
cell size; particle/swarm broadphase. ~2 days. Same module + same per-slice
DoD (5 configs).
