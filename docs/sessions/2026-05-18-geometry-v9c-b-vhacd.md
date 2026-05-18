# 2026-05-18 — Phase 3.1.7 v9c-b V-HACD decompose ✅ SHIPPED

**Slice:** Phase 3.1.7 v9c-b — V-HACD recursive convex decomposition (Mamou 2014 §3.2-3.4). Second slice of the v9c `-decomposition` cluster.

**Status:** ✅ shipped same day. **5-config DoD PASS** (`scripts/per-slice-check.ps1 -IncludeRelease -Parallel`).

---

## Algorithm

Recursive plane-search decomposition. Consumes a `VoxelGrid` (from v9c-a `voxelize_mesh`) and emits one `QuickhullResult<f32>` per leaf cluster.

### Outer loop (Mamou §3.2-3.4)

1. **Build root cluster** = all Surface ∪ Inside voxels of grid.
2. **Allocate sidecar** `Array<u32>` of size `grid.voxel_count()` storing per-voxel cluster_id (Outside/Unknown voxels → `kClusterIdNone`).
3. **Iterate** while `clusters.size() < max_parts`:
   a. Pick the worst-concavity cluster.
   b. If `worst_value ≤ min_concavity`: terminate (all clusters convex enough).
   c. Find best split plane (`find_best_split`).
   d. If no valid split: accept the worst cluster as a concave leaf and break.
   e. Apply the split — partition cluster into LEFT (kept) + RIGHT (new), update sidecar, recompute AABBs.
4. **Build hulls** for every final cluster via Quickhull on its surface voxel centres.

### Concavity — voxel-fraction (D129, divergence from Mamou)

```
concavity(C) = 1 - |C_voxels| / |hull_voxels(C)|  clamped to [0, 1]
```

where `hull_voxels(C)` = voxels in the cluster's AABB whose centre is inside `quickhull(cluster_surface_centres(C))`. Computed via existing `contains(ConvexHullView<T>, Vec3<T>)` from `crd-geometry-primitives`.

A perfectly convex cluster (cube, sphere) has concavity = 0 ⇒ the cube → 1 part calibration test fires immediately. Divergence from Mamou's original Hausdorff metric is documented alongside D124: "modern V-HACD implementations universally use voxel-fraction for cooker-budget speed; the V-HACD authors themselves moved off Hausdorff." Pinned for ADR-0076 §24 amendment at v9c-close.

### Cluster surface voxels

A voxel is "cluster surface" iff:
- The grid says it's `Surface`, OR
- Any 6-neighbour is out of the cluster (different cluster id / out of grid / other side of split when scoring a candidate).

Centres of these voxels feed Quickhull.

### Plane search (D130)

For each cluster, evaluate `3 * splits_per_axis` candidate planes (3 axes × N evenly-spaced positions, skipping cluster endpoints). Cost:

```
cost(plane) = concavity(L) + concavity(R)
            + alpha · |size_L - size_R| / (size_L + size_R)
            + beta  · symmetry_penalty
```

`beta = 0` default — true symmetry-axis detection requires principal-axis machinery (PCA / inertia tensor of voxels), deferred to v9c-b-symmetry follow-on. The `beta_symmetry` option stays in the API as a stable knob.

`concavity_of_subset` is the workhorse — given the parent cluster + split predicate, it walks the voxels once, collects surface centres for the subset (with neighbour-checks tracking the implicit "other side" boundary), runs Quickhull, counts hull-voxels in the subset's AABB, and returns the volume-fraction concavity. Each candidate plane costs 2 of these calls.

### Cluster representation (D131)

Per-cluster `Array<u32> voxel_indices` (linear indices into the VoxelGrid) + cached voxel-index-space AABB. Membership is also tracked via the per-voxel cluster_id sidecar for O(1) "is voxel V in cluster C" lookups (essential for the surface-voxel neighbour test). Splits are linear partitions of `voxel_indices` + a sidecar update.

Per advisor: "skip the 8-corners approach — voxel centres give a tight-enough hull and reduce Quickhull point count 8×."

## Output

```cpp
struct VhacdResult {
    Array<QuickhullResult<f32>> parts;   // one per leaf cluster
    u32 total_input_voxels;
    u32 max_recursion_depth_seen;
    f32 max_part_concavity;
    VhacdStatus status;
};
```

Each `QuickhullResult` owns its arrays (per advisor — chosen over concatenated-arrays for lifetime simplicity; cooker-only code, marginal cache-friendliness gain). Caller builds `ConvexHullView<f32>` per part via the existing `crd::geometry::convex::convex_hull_view_of(result.parts[i])` helper.

## Test corpus (advisor TDD — CALIBRATION FIRST)

7 cases / 22 assertions in `test_vhacd.cpp`:

1. **CALIBRATION**: cube voxelized at res=16 → exactly 1 part. Verifies concavity floor. If this fails, the metric is broken and downstream tests are meaningless.
2. **Diagnostics**: EmptyGrid, InvalidOptions (max_parts=0).
3. **Discriminating: dumbbell** (two cubes + thin bar) → ≥ 2 parts. Plane-search must pick the splitting axis.
4. **Discriminating: L-shape** (two perpendicular boxes) → ≥ 2 parts.
5. **Determinism**: identical input → identical part count + max_part_concavity.
6. **Perf budget**: dumbbell @ res=16, FloodFill voxelize + decompose < 5000 ms (generous; tightens at v9c-close after measurement).

## Mid-slice fixes

### Fix 1 — C4702 unreachable-code from placeholder `break`

The initial commit had a placeholder loop body for the pre-plane-search calibration test:

```cpp
break;     // bail without splitting
++worst_iter;  // unreachable
```

`/W4 /WX` rejected this. Fixed by restructuring the placeholder to set the iteration counter to `max_parts` directly (no `break` after a reachable statement). When the plane-search machinery landed, the structure was re-used cleanly without a rewrite.

### Fix 2 — Double-init of crd::jobs

Two of my new tests called `crd::jobs::init()` per case. The `DecompositionJobsListener` (already registered in `test_voxelize.cpp` for the binary lifetime) had already initialised jobs at `testRunStarting`. Result: `WorkerPool::init called twice` assertion. Removed the per-test init/shutdown (same precedent as the `crd-resources-tests` `ResourcesJobsListener` pattern).

## Pinned design decisions for ADR-0076 §24 amendment at v9c-close

- **D129** — Voxel-fraction concavity (`1 - |C|/|hull(C)|`), NOT Mamou's Hausdorff distance. Documented divergence; faster + standard in modern V-HACD impls.
- **D130** — Mamou cost-function form: `concavity(L) + concavity(R) + α·imbalance + β·symmetry`. `β = 0` default; principal-axis detection deferred to follow-on.
- **D131** — Cluster representation = `Array<u32>` voxel indices + per-voxel sidecar `Array<u32>` cluster_id. Sidecar enables O(1) membership for surface-voxel neighbour tests; voxel_indices enables cheap linear partition splits. Both mutate together during `apply_split`.

## Filed follow-ons

- **v9c-b-symmetry** — principal-axis detection (inertia tensor of voxels) feeding the `beta_symmetry` term.
- **v9c-b-perf** — concavity caching (only recompute for the split children, not all clusters).
- **v9c-b-typed** — `vhacd_decompose_typed` strip-compute-retag wrapper for `Length<T>` consumers.

## Files touched

- `engine/geometry-decomposition/include/crd/geometry/decomposition/vhacd.hpp` (NEW — public API: VhacdResult / VhacdOptions / VhacdStatus / `vhacd_decompose`)
- `engine/geometry-decomposition/include/crd/geometry/decomposition/decomposition.hpp` (umbrella now re-exports vhacd)
- `engine/geometry-decomposition/src/vhacd.cpp` (NEW — recursive driver)
- `engine/geometry-decomposition/src/vhacd_internal.hpp` (NEW — VoxelCluster, concavity helpers, split machinery)
- `engine/geometry-decomposition/src/vhacd_internal.cpp` (NEW — impl)
- `engine/geometry-decomposition/CMakeLists.txt` (added `crd-geometry-convex` PUBLIC dep for Quickhull)
- `tests/geometry-decomposition/test_vhacd.cpp` (NEW — 7 cases / 22 assertions)
- `tests/geometry-decomposition/CMakeLists.txt` (auto-globbed; no edit needed)
- `context.md` (Last shipped milestone)
- `docs/ROADMAP.md` (Phase 3.1.7 bullet)
- `docs/phases/phase-3.1.7-geometry.md` (v9c-b row)
- `docs/systems/geometry-decomposition.md` (v9c-b section added)

## Next

🎯 **v9c-close** — cluster wrap: ADR-0076 §24 amendment locking D123-D131 + final system doc pass + 18-config full sweep + eylem v1c stub integration smoke per the per-sub-module practice. ~1 day budget.
