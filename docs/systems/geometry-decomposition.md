# crd-geometry-decomposition

Volumetric pre-process substrate for the V-HACD pipeline (Mamou 2014) and future
volumetric decomposition primitives. **Cooker-only — not runtime.** Primary
consumer is eylem v1c convex-collider conditioning; future consumers include
crd-sdf v2 mesh-bake (winding-number sign + grid extraction), CAD pre-process
(boolean intermediates), and CAM toolpath generation (swept-volume material
removal).

> Module path: `engine/geometry-decomposition/`
> Target: `crd-geometry-decomposition`
> Namespace: `crd::geometry::decomposition`
> Opened: Phase 3.1.7 v9c-a (2026-05-18)
> **Status: ✅ v9c CLUSTER CLOSED (2026-05-18) — 18-config full sweep PASS. ADR-0076 §24 amendment locked (D123-D131). 10th of 11 Phase 3.1.7 sub-modules complete.**

## Public surface

| Header | Purpose |
|---|---|
| `crd/geometry/decomposition/decomposition.hpp` | Umbrella — re-exports v9c-a + v9c-b |
| `crd/geometry/decomposition/voxel.hpp`         | `VoxelGrid`, `VoxelState`, `VoxelizationOptions`, `VoxelizationResult<T>`, `VoxelizationStatus`, `ClassificationMode` |
| `crd/geometry/decomposition/voxelize.hpp`      | `voxelize_mesh()` entry point |
| `crd/geometry/decomposition/vhacd.hpp`         | `vhacd_decompose()` + `VhacdResult`, `VhacdOptions`, `VhacdStatus` |

## v9c-a — voxelize

```cpp
namespace crd::geometry::decomposition {

VoxelizationResult<f32> voxelize_mesh(const mesh::TriangleMeshViewf& view,
                                       const VoxelizationOptions& opts,
                                       memory::IAllocator* alloc) noexcept;

} // namespace crd::geometry::decomposition
```

### Algorithm — two strictly non-overlapping passes (pin D126)

| Pass | Operation | Notes |
|---|---|---|
| **1. Surface marking** | Akenine-Möller 2001 13-axis SAT, exact triangle/AABB overlap | `crd::jobs::parallel_for` over triangle batches; per-voxel `std::atomic_ref<u8>::fetch_or(Surface)` is idempotent + commutative ⇒ deterministic regardless of thread interleaving. Skipped for tri_count < 256 (fan-out overhead). |
| **2. Classification** | Writes `Outside` / `Inside` into voxels STILL `Unknown` | `WindingNumber` (default — Jacobson 2013 via `crd::geometry::mesh::mesh_winding_number`; robust on non-watertight) OR `FloodFill` (6-conn BFS from corner Outside seed; fast, requires watertight). |

Pass 2 reads only finalised pass-1 state — no concurrent writers, no atomics
needed for classification.

### Storage

- `VoxelGrid` — dense `crd::containers::Array<u8>` with `std::atomic_ref<u8>`
  accessors. Plain `Array<std::atomic<u8>>` would not compile because
  `std::atomic` is neither copyable nor movable.
- API is **opaque** — no raw buffer accessor. Future bricked / sparse backend
  (e.g. for CAM 1024³ workloads where 1 GiB dense is unaffordable) can replace
  the dense array without breaking consumers. Trigger: ADR-0076 §24 amendment
  at v9c-close.
- `VoxelState` is `u8` with 2 bits used (4 states) and 6 reserved for future
  per-voxel payload (material id, surface normal, distance estimate, …).

### Sizing options — precedence rule

`VoxelizationOptions` carries two sizing knobs; **precedence**:

1. `fixed_resolution > 0` wins — N voxels along the longest mesh axis.
2. Else `target_voxel_count > 0` — derive cubic voxel size from
   `cbrt(volume / target)`; per-axis n via `ceil(extent / voxel_size)`.
3. Both zero → `InvalidOptions`.

Future `voxel_size: Length<T>` mode (CAD/CAM units-typed sizing) lands at the
top of the precedence ladder per ADR-0078 §5 D34 at v9c-b/close.

`padding_voxels` (default 1) adds layers of guaranteed-Outside padding around
the mesh AABB — required for FloodFill seeding from the corner voxel and
useful for downstream BVH-of-voxels traversal.

`max_resolution_per_axis` (default 1024) caps each axis to defend against
pathological aspect ratios.

### Classification modes — when to pick which

| Mode | Speed | Correctness on non-watertight | Default? |
|---|---|---|---|
| `WindingNumber` | O(triangles per voxel) — slower | Robust (Jacobson 2013) | ✅ yes |
| `FloodFill` | O(voxels) — fast | **LEAKS** through any hole; misclassifies entire interior as Outside | no |

Defaulting `WindingNumber` is the safe choice for arbitrary input meshes. Game
collision proxies, CAD models, and downstream eylem v1c colliders are routinely
non-watertight; the slow-but-correct default protects unwary callers.
`FloodFill` is the fast-path opt-in for known-watertight input — voxelize_mesh
documents the leak in the option doc + corpus has a regression test pinning the
leak behaviour.

### Divergence from Mamou (pin D124)

Mamou's original V-HACD uses centroid-in-mesh classification per voxel (one
point test per voxel against the mesh). v9c-a chooses exact SAT for surface
marking + classification cascade because the substrate must also serve CAD and
SDF generation, where conservative overlap matters. The v9c-b decompose stage
receives a strictly-better voxelization than the paper assumes; cost-function
tuning may need a one-pass calibration there.

### f32 only at v9c-a

The WindingNumber oracle (`mesh_winding_number`) is f32-only today, so v9c-a
ships f32 only. f64 entry as a follow-on slice when a real f64 consumer asks
(e.g. CAD precision modeling at orbital scale).

## Determinism

`VoxelGrid` contents are bit-identical across runs given identical input,
regardless of thread interleaving:

- Surface marking uses `fetch_or(Surface)` — commutative + idempotent.
- Classification pass 2 runs only after pass 1 has fully joined.
- WindingNumber is a deterministic floating-point function of voxel centre.
- FloodFill walks neighbours in fixed order from a fixed seed.

## v9c-b — decompose

```cpp
namespace crd::geometry::decomposition {

VhacdResult vhacd_decompose(const VoxelGrid& grid,
                              const primitives::AABB3<f32>& grid_aabb,
                              const math::Vec3<f32>& voxel_size_world,
                              const VhacdOptions& opts,
                              memory::IAllocator* alloc) noexcept;

} // namespace crd::geometry::decomposition
```

Recursive plane-search convex decomposition per Mamou §3.2-3.4. Consumes a
`VoxelGrid` (from v9c-a) and emits one `QuickhullResult<f32>` per leaf cluster
into `VhacdResult.parts`. Caller builds `ConvexHullView<f32>` per part via the
existing `crd::geometry::convex::convex_hull_view_of(parts[i])` helper.

### Algorithm

1. **Build root cluster** = all Surface ∪ Inside voxels of grid.
2. **Per-voxel sidecar** `Array<u32>` of cluster_ids (Outside/Unknown →
   `kClusterIdNone`); root cluster_id = 0.
3. **Iterate** while `clusters.size() < max_parts`:
   a. Pick worst-concavity cluster.
   b. Terminate if `worst_value ≤ min_concavity`.
   c. `find_best_split` — 3 axes × `splits_per_axis` evenly-spaced positions;
      pick min-cost.
   d. `apply_split` — partition `voxel_indices` into LEFT (kept) + RIGHT (new
      cluster); update sidecar; recompute AABBs.
4. **Build hulls** — Quickhull on each leaf cluster's surface voxel centres.

### Concavity (D129) — voxel-fraction, NOT Hausdorff

```
concavity(C) = 1 - |C_voxels| / |hull_voxels(C)|   clamped to [0, 1]
```

where `hull_voxels(C)` = voxels in cluster's AABB whose centre is inside
`quickhull(cluster_surface_centres(C))`. A perfectly convex cluster (cube,
sphere) has concavity = 0. Empty / degenerate hull ⇒ concavity = 0 ("as
convex as it can be" — safer termination than ∞).

**Divergence from Mamou's original Hausdorff**: documented alongside D124.
Modern V-HACD implementations (the reference impl itself) universally use the
voxel-fraction form for cooker-budget speed.

### Cost function (D130) — Mamou §3.3

```
cost(plane) = concavity(L) + concavity(R)
            + alpha · |size_L - size_R| / (size_L + size_R)
            + beta  · symmetry_penalty
```

`beta = 0` default — true symmetry-axis detection requires principal-axis
machinery (PCA / inertia tensor of voxels), deferred to v9c-b-symmetry
follow-on. The `beta_symmetry` option stays in the API as a stable knob.

### Cluster representation (D131)

Per-cluster `Array<u32> voxel_indices` (linear indices into the VoxelGrid) +
cached voxel-index-space AABB + global per-voxel cluster_id sidecar. Sidecar
gives O(1) membership for the "any neighbour out of cluster?" surface-voxel
predicate; voxel_indices array gives a cheap linear partition for `apply_split`.

Voxel CENTRES (not 8 corners) feed Quickhull — tight-enough hull, 8× fewer
input points (advisor's call: "skip the 8-corners approach").

### Options + result

```cpp
struct VhacdOptions {
    f32 min_concavity     = 0.05F;
    u32 max_parts         = 32U;
    u32 max_depth         = 16U;
    u32 splits_per_axis   = 16U;
    f32 alpha_imbalance   = 0.05F;
    f32 beta_symmetry     = 0.05F;   // reserved; β=0 in v9c-b
};

struct VhacdResult {
    Array<convex::QuickhullResult<f32>> parts;
    u32  total_input_voxels;
    u32  max_recursion_depth_seen;
    f32  max_part_concavity;
    VhacdStatus status;
};
```

`VhacdStatus`: Ok / EmptyGrid / InvalidOptions / OutOfMemory / HullBuildFailed
/ InternalInvariant.

## Pinned design decisions (carried for ADR-0076 §24 amendment at v9c-close)

- **D123** — Dense `Array<u8>` + `std::atomic_ref` storage with monotonic-union
  write semantics. Opaque accessor API. Sparse/bricked deferred until consumer
  demand.
- **D124** — Exact SAT (Akenine-Möller 2001) surface marking, not Mamou's
  conservative centroid-classification. Divergence note: v9c-b sees strictly
  better input than the paper assumes.
- **D125** — Dual classification: `WindingNumber` default + `FloodFill` opt-in.
  WindingNumber is the safer choice for arbitrary input meshes.
- **D125'** — **Precedence** sizing rule (`fixed_resolution` wins, else
  `target_voxel_count`, else InvalidOptions), not "exactly one non-zero". No
  zero-out ritual for callers; forward-compatible with future `voxel_size`.
- **D126** — Two strictly non-overlapping passes: surface-mark fully completes
  (jobs join) before classification starts. Pass 2 only writes into still-
  Unknown voxels.
- **D127** — 1-voxel padding default + corner-seed FloodFill outside assumption.
  Also enables future BVH-of-voxels with safe boundary cells.
- **D128** — Parallel surface marking via `crd::jobs::parallel_for` over
  triangle batches with `fetch_or(Surface)` race-free union. Sequential fallback
  for tri_count < 256.
- **D129** — Voxel-fraction concavity, not Hausdorff. `1 - |C|/|hull_voxels(C)|`
  clamped to [0,1]. Documented divergence from Mamou; faster + standard in
  modern V-HACD impls.
- **D130** — Mamou cost-function form: `concavity(L)+concavity(R) + α·imbalance
  + β·symmetry`. β=0 default; principal-axis detection deferred.
- **D131** — Cluster repr = `Array<u32>` voxel_indices + global per-voxel
  cluster_id sidecar. Sidecar enables O(1) membership for surface-voxel
  neighbour tests; voxel_indices enables cheap linear-partition splits.

## Filed follow-ons

**v9c-a follow-ons:**
- `v9c-a-bvh-winding` — wire BVH-accelerated `mesh_winding_number_fast` for
  classification at 256³+ resolutions (current O(triangles) brute force is the
  bottleneck above 64³).
- `v9c-a-f64` — f64 voxelize entry once a CAD/CAM consumer asks.
- `v9c-a-voxel-size-units` — `Length<T> voxel_size` sizing knob (top of the
  precedence ladder).
- `v9c-a-sparse-backend` — bricked / sparse VoxelGrid storage for CAM 1024³+
  workloads.
- `v9c-a-typed-wrapper` — `voxelize_mesh_typed` strip-compute-retag boundary
  wrapper for `Vec3<Length32>` consumers.

**v9c-b follow-ons:**
- `v9c-b-symmetry` — principal-axis detection (inertia tensor of voxels)
  feeding the `beta_symmetry` term.
- `v9c-b-perf` — concavity caching (only recompute split children, not all
  clusters every iter).
- `v9c-b-typed` — `vhacd_decompose_typed` for `Length<T>` consumers.
- `v9c-b-bvh-contains` — BVH-accelerated point-in-hull for the hull-voxel count
  loop (current O(faces) per point can dominate at 256³ workloads).
- `v9c-b-parallel` — parallel candidate plane evaluation via
  `crd::jobs::parallel_for` (currently sequential per cluster).

## Phase plan

Plan: `docs/phases/phase-3.1.7-geometry.md`. ADR-0076 §24 amendment locks v9c
cluster decisions at v9c-close.

| Slice | Status | Summary |
|---|---|---|
| v9c-a   voxelize_mesh         | ✅ 2026-05-18 | SAT surface mark + 2-mode classify. |
| v9c-b   vhacd_decompose       | ✅ 2026-05-18 | Recursive plane-search decomp; voxel-fraction concavity. |
| v9c-close                     | 📋 planned    | §24 amendment + eylem v1c stub integration smoke. |
