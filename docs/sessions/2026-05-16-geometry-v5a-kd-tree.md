# Session log — 2026-05-16 — geometry v5a: KD-tree

> Phase 3.1.7 v5 `-spatial` cluster opens with the KD-tree substrate. Static
> balanced KD-tree over a point set + three queries (k-NN / radius / AABB-
> window range) + typed Quantity boundary wrappers. New peer module
> `crd-geometry-spatial`; sister modules v5b LooseOctree / v5c R-tree / v5d
> SpatialHash / v5e UniformGrid land in subsequent sessions.

## Scope landed

| Element | Path |
|---|---|
| Module umbrella | `engine/geometry-spatial/include/crd/geometry/spatial/spatial.hpp` |
| Module CMake | `engine/geometry-spatial/CMakeLists.txt` |
| Tree storage + builder | `engine/geometry-spatial/include/crd/geometry/spatial/kd_tree.hpp` + `src/kd_tree.cpp` |
| Radius search | `engine/geometry-spatial/include/crd/geometry/spatial/kd_radius.hpp` + `src/kd_radius.cpp` |
| k-NN | `engine/geometry-spatial/include/crd/geometry/spatial/kd_nearest_n.hpp` + `src/kd_nearest_n.cpp` |
| AABB-window range | `engine/geometry-spatial/include/crd/geometry/spatial/kd_range_aabb.hpp` + `src/kd_range_aabb.cpp` |
| Typed boundary | `engine/geometry-spatial/include/crd/geometry/spatial/kd_queries_typed.hpp` |
| Tests | `tests/geometry-spatial/test_kd_tree_build.cpp`, `test_kd_radius.cpp`, `test_kd_nearest_n.cpp`, `test_kd_range_aabb.cpp`, `test_kd_typed.cpp` |
| Root wiring | `CMakeLists.txt` + `tests/CMakeLists.txt` (one `add_subdirectory` each) |

## API surface

```cpp
namespace crd::geometry::spatial {

template <MathScalar T> struct KdNode {
    T          split_value;
    crd::u32   child_first;     // interior: left child idx; leaf: first slot
    crd::u16   prim_count;      // 0 ⇒ interior; >0 ⇒ leaf
    crd::u8    split_axis;      // 0..2
    crd::u8    pad_;
};
// sizeof(KdNode<f32>) == 12 (5 per cache line); sizeof(KdNode<f64>) == 16 (4 per line) — pinned

template <MathScalar T> class KdTree {
    Array<KdNode<T>> m_nodes;
    Array<crd::u32>  m_point_indices;   // permutation of [0, n)
    AABB3<T>         m_root_bounds;
    crd::u32         m_root;
    // is_empty / node_count / point_count / root / nodes() / point_indices() / bounds()
};

struct KdBuildOptions { crd::u32 leaf_threshold{8}; };

template <MathScalar T>
KdTree<T> kd_build(ConstSpan<Vec3<T>> points, IAllocator*, KdBuildOptions = {});

template <MathScalar T> struct KdRadiusHit { crd::u32 payload; T distance_squared; };
template <MathScalar T> struct KdNeighbor  { crd::u32 payload; T distance_squared; };

template <MathScalar T> void kd_radius     (const KdTree<T>&, ConstSpan<Vec3<T>>,
                                              const Vec3<T>&, T radius,
                                              Array<KdRadiusHit<T>>&) noexcept;
template <MathScalar T> void kd_nearest_n  (const KdTree<T>&, ConstSpan<Vec3<T>>,
                                              const Vec3<T>&, crd::usize k,
                                              Array<KdNeighbor<T>>&) noexcept;
template <MathScalar T> void kd_range_aabb (const KdTree<T>&, ConstSpan<Vec3<T>>,
                                              const AABB3<T>&, Array<crd::u32>&) noexcept;

} // namespace
```

Typed `kd_queries_typed.hpp` adds `Vec3<Length32>` / `Length32` strip-compute-
retag wrappers per ADR-0078 §5 D34. Distance squared is `Quantity<DimMul<D,D>, T>`
at the typed boundary; the raw-`f32` algorithm body is identical to the no-
units build (ADR-0078 §5 D32-D36).

## Algorithm — elite-tier choices

1. **Widest-extent split axis**, NOT canonical round-robin (X, Y, Z, X, …).
   nanoflann/PCL default. Better query depth on skewed inputs (lidar / scene /
   particle clouds). Tiebreak X<Y<Z on equal extent — canonical topology
   across builds.

2. **Leaf bucket size = 8** (`k_kd_leaf_threshold`). nanoflann ≈ 10, PCL ≈ 15.
   Cuts node count ~8× vs leaf=1, kills per-node prune overhead. Configurable
   via `KdBuildOptions::leaf_threshold`.

3. **Median pick via `crd::containers::nth_element`** with a lex-tuple
   comparator `(coord_value, original_input_index)`. No two elements compare
   equal — original indices are unique by construction — so the partition
   is *fully determined* by the comparator. The resulting tree topology is
   **designed for** byte-identical output across MSVC / GCC / clang;
   Windows MSVC + clang-cl verified at v5a; full GCC / Linux verification
   lands at v5-close 18-config sweep. (`std::nth_element` partitions equal-
   keyed elements implementation-definedly — that's the determinism trap on
   KD-trees that the tuple comparator closes.) Same algorithm `crd-eylem` /
   `crd-hesap` will use when their `crd-no-std-sort` scope expands.

4. **Pre-allocate both children before pushing build frames** — the BVH-builder
   trick. Right child sits at `left_index + 1` in the node array regardless
   of left subtree depth. Maintains the "child_first + 1 = right child"
   invariant traversal relies on.

5. **Builder rejects non-finite input** (`CRD_ASSERT(all_finite(points))` in
   debug). Queries TOLERATE non-finite query points / boxes: every finite-vs-
   NaN comparison is false, so the AABB-prune kills every subtree at the
   root and the query returns no hits. Symmetric with ADR-0076 §15
   `crd-geometry-bvh` builder-reject / query-tolerate pin.

6. **Lowest-payload-index tiebreak on equal squared distance** (ADR-0076 §4
   pin #11). Same rule `bvh_closest_point` / `mesh_closest_point` / GJK /
   Quickhull use.

## k-NN — heap design

Caller-allocated max-heap over the result `Array`, manipulated via
`crd::containers::push_heap` / `pop_heap` (deterministic across MSVC / GCC /
clang). The heap top is the WORST distance still in the candidate set.

* `MaxByDistance` comparator orders `(distance², payload)` lexicographically:
  on tied distance, higher payload sits on top. When the heap is full and
  a new candidate ties the top, the *higher-payload* current top is evicted
  in favour of the *lower-payload* candidate — the §4 pin #11 tiebreak.

* Pruning: at an interior node, the lower-bound dq² accumulates as we cross
  splitting planes (the "loose ancestor AABB" bound). Prune if
  `lower_dsq > worst` AND heap is full. Strict `>` (not `>=`): a tied
  lower-bound subtree may contain a tied-distance point with a lower
  payload that wins the tiebreak — must descend.

* Final ascending sort by `(distance², payload)` via `crd::containers::sort`
  so callers get a stable, reproducible result without their own post-sort.

**API note:** `k` is passed *explicitly*, not via `out.capacity()`.
`Array::reserve(n)` is a "≥ n" hint and may allocate more (rounds up to a
minimum-block boundary — caught at first run with a 1-vs-8 mismatch). Lesson
captured: the contract should be the data, not the container's capacity.

## Storage — sizeof pin

`sizeof(KdNode<f32>) == 12` and `sizeof(KdNode<f64>) == 16`, pinned via
`static_assert`. The `f32` variant fits 5 nodes per 64-byte cache line; the
`f64` variant fits 4. (My first draft asserted 16 / 24 — wrong; the natural
layout already packs to 12 / 16. Caught at first build.)

## Tests — 24 cases, brute-force cross-validation

Suite breakdown:

| File | Cases | Coverage |
|---|---|---|
| `test_kd_tree_build.cpp` | 7 | empty / 1-pt / coincident / colinear / widest-axis pick / **permutation determinism (5 shuffles)** / leaf-threshold respected / large-coord stability |
| `test_kd_radius.cpp` | 5 | brute-force cross-val × 5 radii × 8 trials / zero-radius coincident / empty tree / negative radius / squared-distance integrity |
| `test_kd_nearest_n.cpp` | 6 | brute-force cross-val k∈{1,5,20,100} × 5 trials / k=1 closest / k > N returns all / empty tree / k=0 / **equal-distance tiebreak** |
| `test_kd_range_aabb.cpp` | 4 | brute-force cross-val × 16 boxes / inverted box / on-boundary inclusive / empty tree |
| `test_kd_typed.cpp` | 2 | typed-wrapper round-trip vs raw call (radius + k-NN) — bit-identical |

**Brute-force reference** is the "no two answers should disagree" gold:
linear scan + sort by `(distance², payload)`, take top-k or filter by radius.
Cross-validates over 1000-pt random clouds with 5 distinct trial seeds per
configuration — catches every algorithmic bug a unit test could.

**Permutation determinism**: shuffle a 200-pt cloud 5× with distinct seeds,
verify the **set of positions** in the resulting tree is identical (sort by
lex order of position, compare elementwise). Stronger than "topology
identical" — proves the lex-tuple builder is order-invariant on the input
SET, not just node arrangement.

**Large-coord stability**: build the tree at origin AND at +1e6, verify
both trees retain every input point + are queryable. NOT byte-identical
topology — f32 ULP at 1e6 is ~0.0625, sub-ULP point separations near
origin can validly quantize together at 1e6, changing the comparator's
ordering. The right invariant is "no input lost", not "topology preserved".
(My first draft over-asserted topology preservation. Lesson: write tests
to the algorithm's real contract, not to a stricter aspirational one.)

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 1977 / 1977 ctest |
| win-asan | PASS | full project ctest |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 1972 / 1972 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean (after dropping 3 unused using-decls + splitting `bool a, b` into two declarations) |

`scripts/per-slice-check.ps1` gates the first 4; `win-shipping-profile` was
run separately. Total full-project ctest: **1952 → 1977 win-debug
(+25 cases)**.

## Two latent items dropped en route

1. `mt19937 rng(7);` deliberate-constant-seed warning from
   `bugprone-random-generator-seed` — INTENTIONAL for test reproducibility.
   Tidy WARN, not ERROR; suppression deferred to a future hygiene pass.

2. `constexpr u32 n = 200U;` flagged by `readability-identifier-naming` as
   "should be `kN`". Project rule (CLAUDE.md) says local `constexpr` is
   `lower_case` (e.g. `default_capacity`); only *global* constants get the
   `k` prefix. The tidy default rule is too aggressive for project style.
   Tidy WARN, not ERROR; rule scoping is a config-level concern (out of
   scope for this slice).

## Decisions locked (for ADR-0076 §20 v5-close amendment)

| # | Decision | Rationale |
|---|---|---|
| 1 | Widest-extent split, X<Y<Z tiebreak (NOT round-robin) | Better query depth on skewed clouds; deterministic topology across builds |
| 2 | Leaf bucket size = 8 default | nanoflann-style; ~8× fewer nodes vs leaf=1; configurable per-call |
| 3 | `crd::containers::nth_element` + lex-tuple `(coord, original_index)` | Cross-compiler bit-identical partition; same algo eylem/hesap will inherit |
| 4 | Pre-allocate both children before subtree recursion | Maintains "right = left+1" invariant; same trick `bvh_build` uses |
| 5 | `KdNode<f32>` = 12 B, `KdNode<f64>` = 16 B, both `static_assert`-pinned | 5 / 4 per 64 B cache line; field bloat fails CI |
| 6 | k-NN takes `k` as explicit parameter, not via `out.capacity()` | `Array::reserve` is "≥ n" not exact; capacity-based contract is unreliable |
| 7 | k-NN max-heap orders `(distance², payload)` lex; pruning is strict `>` | Tied lower-bound subtree may hold a tied-distance lower-payload tiebreak winner; must descend |
| 8 | Builder REJECTS non-finite input (debug); queries TOLERATE | Symmetric with ADR-0076 §15 `crd-geometry-bvh` pin |
| 9 | Lowest-payload-index tiebreak on equal squared distance | ADR-0076 §4 pin #11; same as bvh / mesh / GJK / Quickhull |
| 10 | Result order: tree-DFS for radius / range_aabb (deterministic given fixed tree, but coordinate-ordered NOT payload-ordered — caller sorts if specific order needed); ascending `(distance², payload)` sort for k-NN | Reproducible across runs; k-NN sort is the only post-pass needed by the documented API |

These will fold into the §20 amendment at v5-close.

## Next

v5b — Loose octree (Ulrich 2000). Dynamic AABB index with overlapping
children-bounds, the workhorse for scene spatial culling. Insert / remove /
update + overlap + raycast. ~3-4 days. Same module
(`crd-geometry-spatial`); same per-slice DoD (5 configs).
