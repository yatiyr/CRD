# crd-geometry-bvh

The bounding-volume-hierarchy sub-module of `crd-geometry` (ADR-0076 §1, the
second sub-module after `-primitives`). Builds and queries a BVH over a set of
axis-aligned boxes — the spatial accelerator that eylem broadphase, the
renderer's frustum cull, `crd-scene::SpatialBVHIndex` (the ADR-0053 reserved
shell), `crd-sdf`'s mesh-bake closest-point search, and the audio path-tracer
all need.

Module: `engine/geometry-bvh/`, target `crd-geometry-bvh`, namespace
`crd::geometry::bvh`. Depends on `crd-core` + `crd-math` + `crd-containers` +
`crd-geometry-primitives` (uses `AABB3<f32>`, `Ray3<f32>`, and the v0f
precomputed Williams/Ize robust ray-AABB slab).

## Status

| Slice | Scope | State |
|---|---|---|
| v1a | `BvhTree` container + binned-SAH binary builder (`bvh_build`) + ordered nearest-hit raycast (`bvh_raycast`) + AABB-overlap query (`bvh_overlap`), over `AABB3<f32>` boxes. Functional API form (ADR-0076 §11) — `bvh_build(span, alloc) → BvhTree`, not a `BvhTree::build` member. Determinism: ADR-0076 §5.2 — SAH split tiebreak pinned X→Y→Z, lower bin index first; `crd::containers` only (no `std::sort`); single-threaded build. | ✅ 2026-05-13 |
| v1b | O(n) bottom-up refit (`bvh_refit`, `crd/geometry/bvh/bvh_update.hpp`) — recompute every node's bounds from the current `prims`, topology untouched. Catto GDC 2019. | ✅ 2026-05-13 |
| v1c | `DynamicBvh` (`crd/geometry/bvh/dynamic_bvh.hpp`) — the incrementally-updatable AABB tree: a binary tree of fat AABBs with parent pointers + free list, `insert` / `remove` / `update` with height-balanced tree rotations, a fat-AABB margin so small motion is a no-op, `query`/`raycast` over fat AABBs (broadphase), `validate()` + `sah_cost()` + `max_depth()` diagnostics. Catto GDC 2019 / Box2D v3 `b2DynamicTree`; the form eylem v1c broadphase wraps. | ✅ 2026-05-13 |
| v1d | `Bvh4Tree` (`crd/geometry/bvh/bvh4.hpp`) — the 4-wide topology variant: `bvh4_collapse(BvhTree)` widens a built binary tree into ≤4-child nodes (open the largest interior child, repeat); `bvh4_raycast` / `bvh4_overlap` over it. Same query results as the binary tree it was collapsed from. v1g switched `bvh4_raycast`'s per-node test to a `Vec4f` ray-vs-4-AABB kernel. | ✅ 2026-05-13 |
| v1e | BVH-accelerated closest-point query (`bvh_closest_point` — branch-and-bound: per-node AABB distance is a lower bound on every leaf below, so a node ≥ the current best is pruned; nearer child descended first; `max_dist` cutoff). | ✅ 2026-05-13 |
| v1f | `bvh_build_parallel` (`crd/geometry/bvh/bvh_build_parallel.hpp`) — jobs-parallel binned-SAH build: fans the per-node centroid-bounds + bin-histogram reductions over `crd::jobs::parallel_for` for nodes ≥ a threshold, **bit-identical to the serial `bvh_build`** (the reductions are min/max + integer adds — exact, commutative). Off-by-default Embree-comparison benchmark (`tests/bench/test_bench_bvh.cpp`, `[!benchmark]`). Also fixed a latent `aabb_merge` bug (the empty-box sentinel `{+∞,−∞}` corrupted merges) — see "What you get today". | ✅ 2026-05-13 |
| v1g | `crd/geometry/bvh/bvh4_simd.hpp` — the `Vec4f` ray-vs-4-AABB kernel (`ray_vs_4_aabb` — one `Vec4f` min/max slab chain over the four children, the Ize 2013 conservative `tmax` widening per lane, out-of-line in `src/bvh4_simd.cpp` — a real TU); `bvh4_raycast` now uses it for the per-node test (bit-identical, for finite/well-formed inputs, to the four scalar slab tests it replaced). BVH4 is now the recommended traversal form for static, query-heavy data. `BvhBuildOptions::topology` + the `BvhTopology` enum removed — `bvh_build` is binary, `bvh4_collapse` is the BVH4 path. | ✅ 2026-05-13 |

## What you get today (v1a + v1b + v1c + v1d + v1e + v1f + v1g)

### `BvhTree` — `crd/geometry/bvh/bvh_tree.hpp`

A flat array of 32-byte `BvhNode`s plus a primitive-index array (the leaf
order — a permutation of `[0, n)`). The tree does **not** own the input AABBs:
the builder records each leaf node's union bounds and reorders the index array;
the query helpers take the same `ConstSpan<AABB3<f32>>` back. (Per ADR-0076 §11
— consumers stash trees in their own allocators alongside the primitive data.)

- `BvhNode { AABB3<f32> bounds; u32 left_first; u16 prim_count; u8 split_axis; u8 pad_; }`
  — 32 bytes, two per cache line. `prim_count == 0` ⇒ interior (children at
  `left_first`, `left_first+1`; `split_axis` ∈ {0,1,2} is the axis the SAH cut
  on, recorded so traversal visits the near child first). Otherwise leaf (owns
  prim-index slots `[left_first, left_first+prim_count)`).
- `BvhTree(IAllocator*)` — move-only; `is_empty()`, `node_count()`,
  `prim_count()`, `root()`, `nodes()` / `prim_indices()` (const spans),
  `bounds()` (root AABB, or identity-empty for an empty tree).
- `inline constexpr usize k_max_bvh_depth = 64` — the build asserts recursion
  never exceeds this; the query stacks are sized to match.

### Build — `crd/geometry/bvh/bvh_build.hpp`

- `BvhBuildOptions { u32 sah_bins = 16; u16 max_leaf_prims = 4; }`
  — `sah_bins` is clamped to `[2, 64]`; `max_leaf_prims` to `≥ 1`.
- `BvhTree bvh_build(ConstSpan<AABB3<f32>> prims, IAllocator* alloc, const BvhBuildOptions& = {})`
  — Wald 2007 binned SAH: per node, bucket primitive centroids into `sah_bins`
  bins per axis, sweep the bin boundaries for the minimum-cost split (cost =
  `Nₗ·halfArea(boxₗ) + Nᵣ·halfArea(boxᵣ)`), recurse. A node with
  `≤ max_leaf_prims` prims becomes a leaf; a node whose centroids all coincide
  (no spatial split exists) falls back to a deterministic median-by-index split
  so leaves stay capped. An empty span yields an empty tree. Single-threaded.
- `f32 bvh_sah_cost(const BvhTree&)` — Σ over leaves of `prim_count · halfArea(leaf.bounds)`,
  divided by `halfArea(root.bounds)`. A build-quality metric (lower is better;
  ~`prim_count` for a single leaf, much less for a good split); `0` for an empty
  tree. Used by the tests.
- `BvhTree bvh_build_parallel(ConstSpan<AABB3<f32>> prims, IAllocator* alloc, const BvhBuildOptions& = {}, u32 num_jobs = 0, u32 parallel_threshold = 8192)`
  (`crd/geometry/bvh/bvh_build_parallel.hpp`, v1f) — the jobs-parallel build.
  For nodes with ≥ `parallel_threshold` primitives it fans the two O(count)
  per-node reductions (the centroid bounds; the 3-axis bin histograms) out over
  `crd::jobs::parallel_for` — each worker reduces its chunk into a private
  partial (on the tree's allocator, reused per node — never the frame arena),
  the main thread folds the partials in fixed job order. Because those
  reductions are min/max (componentwise box union) + integer adds — exact and
  commutative — the result is **byte-for-byte equal to `bvh_build`** regardless
  of `num_jobs`; the SAH split, the partition, and the node layout are all the
  serial code. `num_jobs == 0` ⇒ `jobs::num_workers()`; `num_jobs ≤ 1` or an
  input below `parallel_threshold` ⇒ it just calls the serial `bvh_build`.
  Requires the job system (`jobs::init()`); uses `jobs::frame_alloc` (via
  `parallel_for`) only for the small per-call `JobDecl` arrays. The Embree
  comparison benchmark lives in `tests/bench/test_bench_bvh.cpp` (`[!benchmark]`,
  not in ctest).

**Determinism (ADR-0076 §5.2 — pinned):** the SAH split is evaluated axis
X → Y → Z, and within an axis the lower bin index wins a cost tie (the
implementation only replaces the running best on a *strictly* lower cost, so the
first split found at the minimum cost is kept — exactly X-then-Y-then-Z,
lower-bin-first). The leaf-order permutation is produced by a stable two-pass
partition into scratch — no `std::sort`. `bvh_build_parallel` (v1f) is
bit-identical to the serial build for any `num_jobs` (verified by a memcmp test
across {1, 2, 4, 8}). → identical trees and query results bit-for-bit across
configs (verified by the deterministic-replay test).

> **Fixed in v1f — a latent `aabb_merge` bug.** The internal `aabb_merge(a, b)`
> used to do `aabb_include_point(a, b.min); aabb_include_point(a, b.max);` —
> correct for two well-formed boxes, but the "empty" sentinel (`{min = +∞,
> max = −∞}`) has `min > max`, so including its `min` (`+∞`) as a point pushed
> `a.max` to `+∞` (and its `max` pushed `a.min` to `−∞`) — turning `a` into the
> *universe* box instead of leaving it unchanged. This corrupted any merge of an
> empty bin (the `sweep_for_split` prefix bounds when a middle bin is empty; the
> parallel histogram fold when a chunk has no prims in a bin). It produced
> *valid but suboptimally-split* trees in the serial path (masked — the tests
> check invariants, not specific splits) and a *wrong* parallel histogram. Fixed
> to a proper componentwise corner-min/max union, which is robust to the empty
> sentinel and bit-identical for well-formed boxes. Side effect: `bvh_build`'s
> trees are now correctly SAH-split (slightly different node arrays than before;
> all invariant/equivalence tests still pass).

### Queries — `crd/geometry/bvh/bvh_query.hpp`

- `optional<BvhRayHit> bvh_raycast(const BvhTree&, ConstSpan<AABB3<f32>> prims, const Ray3<f32>&, f32 tmax = ∞)`
  — the nearest primitive whose AABB the ray enters within `[0, tmax]`.
  `BvhRayHit { u32 prim_index; f32 t; }`. Uses the v0f precomputed Williams/Ize
  robust slab (`precompute_ray_aabb` + `intersect_ray_aabb_robust`): hole-free,
  NaN/∞-direction-safe, conservative `tmax` widening (the *entry* `t` is exact —
  not subject to the pad). Ordered traversal — the child on the side the ray
  enters first (per the node's recorded `split_axis`) is visited first, so the
  running `best_t` prunes the far subtree. Returns the *primitive's AABB* hit;
  per-triangle ray-tri refinement inside a leaf is `crd-geometry-mesh` (v4).
- `bvh_overlap(const BvhTree&, ConstSpan<AABB3<f32>> prims, const AABB3<f32>& box, Fn&& on_prim)`
  — invokes `on_prim(u32 prim)` for every primitive whose AABB overlaps `box`,
  in traversal order. A `bvh_overlap(..., Array<u32>& out)` convenience appends
  the hits.
- `bvh_closest_point(const BvhTree&, ConstSpan<AABB3<f32>> prims, const Vec3<f32>& query, f32 max_dist = ∞) → optional<BvhClosestPoint>`
  — the primitive (and the point on its AABB) closest to `query`, considering
  only primitives within `max_dist`. `BvhClosestPoint { u32 prim_index; Vec3<f32>
  point; f32 distance_squared; }`. Branch-and-bound: the per-node AABB distance
  is a lower bound on every leaf below it, so a node whose distance ≥ the current
  best is pruned (re-checked on pop, since the best may have tightened); the
  nearer child is descended first so the best tightens before the far subtree is
  reached. `nullopt` for an empty tree or when nothing is within `max_dist`.
  (Closest point on the primitive *AABB* — per-triangle closest-point inside a
  leaf is `crd-geometry-mesh`, v4. The `Bvh4Tree` / `DynamicBvh` closest-point
  land in the v1i query facade.)

### Refit — `crd/geometry/bvh/bvh_update.hpp`

- `bvh_refit(BvhTree& tree, ConstSpan<AABB3<f32>> prims)` — O(n) bottom-up
  recomputation of every node's `bounds` from the current `prims`. The topology
  (node array, leaf-order permutation, `split_axis`) is left untouched — only
  the bounds change. For dynamic scenes where the set of primitives is fixed but
  their boxes move every frame: far cheaper than a rebuild, and query
  *correctness* is unaffected by how far things moved (only query *efficiency*
  degrades — eventually a rebuild pays off; that's the caller's call). A single
  reverse pass over the node array works because `bvh_build` always pushes a
  node before its children, so children have a higher array index (a debug
  assert checks it). The caller must pass `prims` with the same length as the
  original build (the leaf indices stay valid); changing the count is undefined
  — rebuild. No-op on an empty tree. Catto GDC 2019.

### `DynamicBvh` — `crd/geometry/bvh/dynamic_bvh.hpp`

The incrementally-updatable AABB tree — a *different structure* from the static
`BvhTree` (which is a packed array and can't support arbitrary insert/erase
without a rebuild). A binary tree of *fat* AABBs with parent pointers and a free
list of recycled slots; height-balanced tree rotations on every insert/remove;
a fat-AABB margin so a primitive that moves a little doesn't restructure
anything. The classic dynamic AABB tree from Catto's GDC 2019 *Dynamic Bounding
Volume Hierarchies* / Box2D v3's `b2DynamicTree` — the form `crd-eylem`'s
broadphase (eylem v1c) wraps. Leaves carry a `u32 user_data` (the consumer's
primitive id) and are addressed by a stable `DynamicBvhNodeId` handle.

- `DynamicBvh(IAllocator*, DynamicBvhConfig{fat_margin = 0.1})` — move-only.
- `DynamicBvhNodeId insert(const AABB3<f32>& tight, u32 user_data)` — store
  `tight` inflated by `fat_margin`; greedy best-sibling descent (Box2D-style
  cost; deterministic c1-on-tie), new internal parent, refit + rebalance up to
  the root. Returns a stable handle.
- `void remove(DynamicBvhNodeId)` — splice out the leaf, promote its sibling,
  free the old parent, refit + rebalance up. The handle is invalid afterwards.
- `bool update(DynamicBvhNodeId, const AABB3<f32>& new_tight)` — if the stored
  fat AABB still encloses `new_tight`, a no-op returning false (the margin's
  slack absorbs small motion); otherwise remove + reinsert with a fresh fat AABB
  (the handle stays valid), returning true.
- `query(box, on_leaf)` / `query(box, Array<u32>&)` — visit every leaf whose
  *fat* AABB overlaps `box` (broadphase — the caller refines against its own
  primitive data). `raycast(ray, on_leaf)` — visit every leaf whose fat AABB the
  ray (within [0, ∞)) enters (no nearest-hit ordering; the static `bvh_raycast`
  does nearest over tight boxes).
- Access: `is_empty()`, `leaf_count()`, `user_data(id)`, `fat_aabb(id)`,
  `bounds()` (root fat AABB / identity-empty).
- Diagnostics: `sah_cost()` (Σ interior-node half-areas / root half-area —
  Catto's quality metric), `max_depth()`, `validate()` (debug — full structural
  check: parent/child links, height invariant, AABB enclosure, leaf count).

### `Bvh4Tree` — `crd/geometry/bvh/bvh4.hpp`

The 4-wide topology variant — each interior node holds 2–4 children, so each
traversal step touches one node and tests up to four boxes (fewer fetches, and a
shape the `Vec4f` ray-vs-4-AABB kernel (`bvh4_simd.hpp`) fills in lockstep — as
of v1g, `bvh4_raycast` uses that kernel: per node it transposes the ≤4 children's
bounds into SoA `Vec4f` columns (unused lanes duplicate child 0, harmless) and
does one `ray_vs_4_aabb` (Tavianator min/max slab × 4 + the Ize 2013 conservative
`tmax` widening per lane) instead of four sequential scalar slab tests —
bit-identical (for finite/well-formed inputs) to the scalar form it replaced. The
kernel is out-of-line in `src/bvh4_simd.cpp` (a real TU; it is `Vec4f`/128-bit at
every SIMD level, so the AVX2-`ymm`-expecting `crd-simd-emission-check` doesn't
scan it yet — a 128-bit-aware variant is a follow-up, same as the `simd_batch.cpp`
one). BVH4 is the recommended traversal form for static, query-heavy data
(renderer cull, eylem broadphase static side, mesh-raycast leaves).

- `Bvh4Tree bvh4_collapse(const BvhTree& binary, IAllocator*)` — widen a built
  binary tree: for each interior node, start with its two children and
  repeatedly "open" the largest interior child among them (replace it with its
  two children) until the node has 4 children or can't widen without
  overshooting; recurse top-down. Deterministic — the "open the biggest" rule
  breaks half-area ties on the lower source-node index. The result copies the
  source's leaf-order permutation in (it's self-contained re: leaf order; the
  prims span is still the caller's). Empty source ⇒ empty result; single-leaf
  source ⇒ one node with `child_count == 1`. Never produces more nodes than the
  binary tree (typically ~half for a balanced source).
- `Bvh4Node { AABB3<f32> bounds; Bvh4Child children[4]; u8 child_count; }`;
  `Bvh4Child { AABB3<f32> bounds; u32 first; u16 count; }` — `count > 0` ⇒ leaf
  slot (`first` = start in `prim_indices()`), `count == 0` ⇒ node slot (`first` =
  child node index). Accessors mirror `BvhTree` (`nodes`, `prim_indices`,
  `root`, `bounds`, `is_empty`, …).
- `bvh4_raycast(tree, prims, ray, tmax = ∞) → optional<BvhRayHit>` — the same
  result as `bvh_raycast` over the source binary tree (the collapse changes only
  the fan-out). `bvh4_overlap(tree, prims, box, on_prim)` / `(…, Array<u32>&)` —
  the same result set as `bvh_overlap`. (Stack `k_max_bvh4_stack = 256` —
  a 4-ary DFS pushing all children at once needs ~3·depth.)

## API layers

Today's surface is the typed C++ "Eigen-class" layer (zero-overhead, data-oriented
— `ConstSpan` of boxes, not `Mesh*`). The opt-in cooker/editor handle-based façade
(the same two-layer pattern as `crd-hesap` / `crd-geometry-primitives`) is reserved
for later — nothing here forbids it.

## References

- `docs/decisions/0076-geometry-substrate-architecture.md` — §1 (sub-modules),
  §4 / §5.2 (determinism — SAH split tiebreak), §11 (functional API form).
- `docs/research/cerid-geometry.md` §4.1 (BVH algorithm survey) + §7.2
  (`crd-geometry-bvh` scope) + §8 v1 slice list.
- Wald 2007, *On Fast Construction of SAH-based Bounding Volume Hierarchies* —
  the binned-SAH builder. Catto GDC 2019, *Dynamic Bounding Volume Hierarchies*
  — refit + tree rotations (v1b/v1c). Embree — the BVH4 / SIMD-traversal
  reference (v1d/v1g).
