# crd-geometry-spatial — system overview

> Phase 3.1.7 v5 cluster. The spatial-acceleration substrate beyond the
> per-primitive BVH in `crd-geometry-bvh`. Five backends + unified
> compile-time-overload facade + `crd-scene::SpatialBVHIndex` consumer.
> Sixth of 11 `crd-geometry` sub-modules; closed 2026-05-16.

## Status

- v5a `KdTree` ✅ shipped 2026-05-16
- v5b `LooseOctree` ✅ shipped 2026-05-16
- v5c `RTree` (R*-tree) ✅ shipped 2026-05-16
- v5d `SpatialHash` + v5d-fast scratch overloads ✅ shipped 2026-05-16
- v5e `UniformGrid` (with scratch from day 1) ✅ shipped 2026-05-16
- v5 thread-safety validation pass ✅ shipped 2026-05-16
- v5-index-bringup (`scene::SpatialBVHIndex`) ✅ shipped 2026-05-16
- v5-queries-extension (facade) ✅ shipped 2026-05-16
- v5-close ✅ shipped 2026-05-16 — this doc + ADR-0076 §20 amendment + 18-config sweep.

**Cluster totals:** 8 slices · ~4900 LOC engine + ~4330 LOC tests · 20
locked substrate decisions in ADR-0076 §20. Full project ctest 1952 to
**2093** win-debug across the cluster (+141 cases).

## When to use what

Five backends, each chosen for a distinct workload. **Use the right one;
don't paper over with a wrong-fit backend.**

| Workload | Backend | Why |
|---|---|---|
| Point-cloud k-NN, radius search (lidar, particles, mesh vertices) | **KdTree** | Native k-NN + radius; no AABB overhead |
| Dynamic AABB broadphase, scene cull, eylem rigid body | **LooseOctree** | Update fast-path absorbs ~90%+ small motions; Ulrich invariant ⇒ naturally const-safe |
| Static / cooked-level AABBs, k-NN over AABBs | **RTree (R*)** | STR bulk-load for static; Hjaltason-Samet k-NN; better packing than octree |
| Particle / swarm broadphase, soft-body neighbour search, eylem v3 XPBD | **SpatialHash** | O(1) hash lookup; native `find_overlapping_pairs`; scratch overload for parallel queries |
| Bounded uniform-density domain (voxel scenes, CFD coarse cells, pathfinding) | **UniformGrid** | Dense flat array, no hash collisions, predictable O(cell_count) memory |
| Spatial culling over ECS entities | **SpatialBVHIndex** (in `crd-scene`) | Wraps LooseOctree; fires on storage events; ADR-0053-compliant |

## Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│  Unified facade — crd::geometry::* (compile-time overload set)     │
│  raycast(tree, ray) / overlap(tree, box, out) / radius(tree, p, r) │
│  nearest_n(tree, p, k, out) / find_overlapping_pairs(tree, out)    │
│  Header-only crd/geometry/spatial/queries.hpp + crd-geometry-bvh's │
│  crd/geometry/queries.hpp — both in crd::geometry, ADL unifies.    │
└────────────────────────────────────────────────────────────────────┘
                            │  ADR-0076 §16 pin #1, §20 pin #17
                            ▼
┌────────────────────────────────────────────────────────────────────┐
│  Typed Quantity wrappers (one per backend)                         │
│  kd_queries_typed.hpp / octree_queries_typed.hpp /                 │
│  rtree_queries_typed.hpp / hash_queries_typed.hpp /                │
│  grid_queries_typed.hpp — bridges AABB3<Length32> ↔ raw f32.       │
└────────────────────────────────────────────────────────────────────┘
                            │  ADR-0078 §5 D34 (two-layer typed)
                            ▼
┌────────────────────────────────────────────────────────────────────┐
│  Backend implementations (raw f32 algorithm bodies)                │
│  kd_tree.hpp + kd_nearest_n.hpp + kd_radius.hpp + kd_range_aabb    │
│  loose_octree.hpp                                                  │
│  rtree.hpp                                                         │
│  spatial_hash.hpp                                                  │
│  uniform_grid.hpp                                                  │
└────────────────────────────────────────────────────────────────────┘
                            │
                            ▼
        crd-geometry-primitives (AABB3, Ray3, Vec3, is_finite)
        crd-containers (Array, HashMap, sort, push_heap, pop_heap)
        crd-memory (IAllocator, TlsfAllocator)

┌────────────────────────────────────────────────────────────────────┐
│  ECS consumer (Phase 3.1.7 v5-index-bringup)                       │
│  crd-scene::SpatialBVHIndex                                        │
│  Two-state index (unconfigured = no-op shell; configured = real)   │
│  IAabbExtractor pattern + UPSERT-only update contract              │
│  Wraps LooseOctree backend; ADR-0053 §6 day-one promise preserved  │
└────────────────────────────────────────────────────────────────────┘
```

## API at a glance — five backends, one facade

```cpp
#include <crd/geometry/spatial/spatial.hpp>           // all backends + facade
#include <crd/geometry/queries.hpp>                   // BVH facade (optional)

namespace cg = crd::geometry;
namespace cgs = crd::geometry::spatial;

// --- KdTree (point cloud) ---
auto kd = cgs::kd_build<f32>(points, &alloc);
Array<cgs::KdNeighbor<f32>> knn{&alloc};
cg::nearest_n(kd, points, query_pt, k, knn);                   // unified facade
Array<cgs::KdRadiusHit<f32>> in_radius{&alloc};
cg::radius(kd, points, query_pt, 0.5f, in_radius);

// --- LooseOctree (dynamic AABB) ---
cgs::LooseOctree<f32> oct{&alloc, cgs::OctreeBuildOptions<f32>{world_bounds}};
auto h = oct.insert(aabb, payload);
oct.update(h, new_aabb);  // fast-path absorbs no-op moves
oct.remove(h);
Array<u32> hits{&alloc};
cg::overlap(oct, query_box, hits);
auto rh = cg::raycast(oct, ray);

// --- RTree (R*) ---
cgs::RTree<f32> rt{&alloc};
rt.bulk_load(aabbs, payloads, handles);  // STR — ~7× faster than per-insert
cg::overlap(rt, query_box, hits);
Array<typename cgs::RTree<f32>::Neighbor> knn{&alloc};
cg::nearest_n(rt, query_pt, k, knn);

// --- SpatialHash (particle broadphase) ---
cgs::SpatialHash<f32> sh{&alloc, cgs::SpatialHashConfig<f32>{1.0f, 1024U}};
sh.insert(aabb, payload);
cg::overlap(sh, query_box, hits);
cg::radius(sh, query_pt, 0.5f, hits);
Array<cgs::SpatialHashPair> pairs{&alloc};
cg::find_overlapping_pairs(sh, pairs);
// Parallel-query path: caller-supplied scratch.
cgs::SpatialHashScratch scratch{&alloc};
sh.overlap(query_box, scratch, hits);  // race-free across fibers

// --- UniformGrid (bounded dense) ---
cgs::UniformGrid<f32> ug{&alloc, cgs::UniformGridConfig<f32>{bounds, 1.0f}};
ug.insert(aabb, payload);
cg::overlap(ug, query_box, hits);
cg::raycast(ug, ray);  // Amanatides-Woo + grid-bounds clip
cgs::UniformGridScratch ug_scratch{&alloc};
ug.overlap(query_box, ug_scratch, hits);  // parallel-safe

// --- crd-scene SpatialBVHIndex ---
World world{&alloc};
world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});
auto* idx = world.find_index<SpatialBVHIndex>();
MyAabbExtractor extractor{};  // implements IAabbExtractor
idx->configure(&extractor, OctreeBuildOptions<f32>{world_bounds});
// Now on_insert / on_update / on_entity_destroyed maintain the index.
Array<EntityId> ents{&alloc};
idx->overlap(query_box, ents);
auto hit = idx->raycast(ray);
```

## Support matrix (facade)

```
                  overlap   raycast   radius   nearest_n   find_pairs
   ──────────────────────────────────────────────────────────────────────
   KdTree          --        --        ✓        ✓           --
   LooseOctree     ✓         ✓         --       --          --
   RTree           ✓         ✓         --       ✓           --
   SpatialHash     ✓         ✓         ✓        --          ✓
   UniformGrid     ✓         ✓         ✓        --          ✓
   ──────────────────────────────────────────────────────────────────────
   BvhTree         ✓         ✓         --       closest_pt  --
   Bvh4Tree        ✓         ✓         --       closest_pt  --
   DynamicBvh      ✓ (fat)   ✓ (fat)   --       closest_pt  ✓
```

`--` = NOT exposed via the facade. **Don't fake what doesn't fit** —
KdTree's natural ops are k-NN + radius over a point cloud (no AABBs
stored); LooseOctree + RTree are AABB indices with no per-point radius.

## Thread-safety contract (locked at v5 cluster close)

Cerid's spatial substrate ships **two API surfaces** depending on the
backend's algorithmic property:

* **One-object-one-location** (KdTree, LooseOctree, RTree): queries are
  PURELY READ-ONLY by construction — no per-object dedup state, no
  `mutable` member, no `const_cast` write during query. Concurrent
  `const` queries on the same tree are race-free; each thread needs
  only its own output `Array`.

* **Multi-location-storage** (SpatialHash, UniformGrid): AABB spans
  multiple cells, same `obj_idx` appears in multiple buckets, dedup
  state MUST be mutated during queries → const queries are NOT
  thread-safe → **scratch overload mandatory**. Each worker fiber /
  thread holds its own `SpatialHashScratch` / `UniformGridScratch`
  (per-scratch generation counter); no shared mutation, race-free.

**`find_overlapping_pairs`** is always purely read-only — already
thread-safe by construction; no scratch needed.

### Concurrency validation discipline

Every backend ships a **fiber-jobified concurrent-read test** via
`crd::jobs::parallel_for` (same listener pattern as the
`SpatialHashJobsListener`-renamed-to-`GeometrySpatialJobsListener`
binary-wide hook). 400 fan-out tasks across 16 jobs / 4 worker fibers,
per-task isolated `TlsfAllocator` + (scratch if needed) + output Array,
atomic mismatches counter == 0 under win-asan race detection.

Total cross-backend ASan race coverage: **2000 fan-out tasks** (5
backends × 400 each).

Documented as the reusable design rule in
`feedback_spatial_substrate_thread_safety.md`.

## Determinism contract

Inherits ADR-0063 + ADR-0076 §4 pin #11 + §15 builder-reject /
query-tolerate. Cross-backend pins applied at v5:

- **Lex-tuple comparators** eliminate equal-key ordering hazards (any
  partition / sort over numeric keys uses `(coord, original_index)` or
  `(axis_lo, axis_hi, payload)`).
- **`crd::containers::nth_element` / `push_heap` / `pop_heap` / `sort`
  over `std::*`** — bit-exact across MSVC / GCC / clang on x64 / ARM64.
- **Lowest-payload tiebreak** on equal distance (k-NN) / equal `t`
  (raycast) / equal distance² (closest-point) — §4 pin #11 engine-wide.
- **NaN/Inf contract**: builders REJECT non-finite (debug
  `CRD_ASSERT`); queries TOLERATE non-finite (defensive `is_finite`
  short-circuit returns empty result; no crash). Zero-direction ray
  short-circuits in raycast.
- **Update fast-path pattern**:
  - LooseOctree: AABB-fit-only (Ulrich invariant; the
    correctness-critical "guaranteed-fast-path target-depth formula"
    `(loose - 1) × R / extent` ensures off-center objects still fit
    their cell's loose AABB).
  - SpatialHash + UniformGrid: same-cell-range detection.
- **Amanatides-Woo voxel traversal** for SpatialHash + UniformGrid
  raycast — with ALL-tied-axes-advance corner-grazing safety.
- **Grid-bounds-clipped Amanatides-Woo** for UniformGrid raycast — slab
  ray-vs-grid-AABB at entry, walk from clipped start.

## Performance pins

- **KdTree** — branch-and-bound k-NN with strict-`>` lower-bound
  pruning. Caller-heap (max-heap-by-distance) via
  `crd::containers::push_heap` / `pop_heap`.
- **LooseOctree** — same-AABB-fit-only update fast-path absorbs ~90%+
  of small motions at zero rebucketing cost. Lazy-allocated 64 B cell
  nodes (`static_assert`-pinned) + free-list pool with stable handle.
- **RTree** — STR bulk-load is **~7× faster than per-insert** for
  static cooked-level data. Forced reinsertion (Beckmann §4.3) avoids
  cascading splits on dynamic inserts. Hjaltason-Samet 1999 incremental
  k-NN gives optimal asymptotic.
- **SpatialHash** — POW2 bucket count + bit-mask modulo; per-query
  generation counter dedup is **zero-allocation** (no seen-set). Same-
  cell-range update fast-path. AABB-stored-to-all-overlapping-cells
  with cross-cell dedup at query time.
- **UniformGrid** — flat `Array<Array<u32>>` indexed by
  `(z·ny + y)·nx + x` — branchless lookup. 256M cell sanity cap
  (debug `CRD_ASSERT`) — past that point, switch to SpatialHash.
- **All backends** — scratch overloads (where shipped) maintain
  zero-allocation per query after first warmup (the scratch capacity
  amortises across calls).

## Integration with `crd-scene` (v5-index-bringup)

The `crd-scene::SpatialBVHIndex` realises the ADR-0053 v1i reserved
shell — the FIRST non-reserved spatial index. Two-state design preserves
the day-one promise:

1. **Unconfigured** — every storage event is a no-op; queries return
   empty. Identical observable behaviour to the v1i shell it replaced.
   Existing Phase-3.0 user code that did
   `register_component<X>(SpatialBVH{})` continues running with no
   changes.
2. **Configured** via
   `world.find_index<SpatialBVHIndex>()->configure(extractor, opts)` —
   `on_insert` / `on_update` / `on_entity_destroyed` maintain the
   backing LooseOctree. Queries return real entities.

The user supplies an `IAabbExtractor` that bridges storage events to
world-space AABBs. **Canonical signature**:
`extract(EntityId, ComponentId, const void* data)` — the `data` pointer
IS the freshly-installed component bytes (per `IStorageEventSink`
contract). Naive `world.get_component<T>(e)` from inside `on_insert`
returns nullptr mid-archetype-migration; the `data` pointer avoids the
hazard.

**UPSERT-only update contract**: `world.add_component(e, new_value)`
fires per-entity `on_update`; `world.get_component_mut<T>(e)` only
bumps chunk-version (ChangeDetect's hint-grade signal), does NOT fire
the storage event. Documented prominently in the index header.

This pattern (extractor interface + two-state design + LooseOctree
backing + concurrent-jobs test) is the **template for future spatial
extensions**: `LightInfluenceIndex` (Phase 3.5), `OcclusionIndex`
(3.5+), `AudioOcclusionIndex` (3.4 ray-traced acoustics).

## Open follow-ons (post-close)

None blocking. Backlog of opportunistic enhancements per the per-
session logs:

- **k-NN scratch sharing** across backends (SpatialHash / UniformGrid
  callers passing pre-sized output Array).
- **STR build parallelism** (RTree) — the sorts and per-slab packing
  parallelise cleanly; defer until a consumer benchmark surfaces the
  need.
- **SpatialHash + UniformGrid u64 generation overflow protection**
  (already shipped — listed here for visibility): pre-wrap detection
  resets every object's `last_query_gen` to 0 + restarts counter at 1.
  Cosmic linear-time scan; one-time cost.
- **Per-component last-out tracking in SpatialBVHIndex** — currently
  on_remove per-component is conservative no-op (entity may carry
  other watched components). v5-bringup MVP behaviour; cleanup happens
  on `on_entity_destroyed` (definitive) or on subsequent `on_update`
  (extractor refresh).
- **DimRoot<>-typed reductions** for kd-tree distance — when ADR-0078
  follow-up lands. Today's typed wrapper returns dimensionless
  `distance_squared` per ADR-0078 §5 D34.

## References

- ADR-0076 §20 — locked v5 substrate decisions
- ADR-0076 §16 — compile-time overload polymorphism (facade pattern)
- ADR-0053 §6 — SpatialBVHIndex slot (originally reserved Phase 3.5,
  promoted at Phase 3.1.7 v5-index-bringup)
- ADR-0078 §5 — two-layer typed architecture (typed wrappers)
- `feedback_spatial_substrate_thread_safety.md` — scratch ↔ multi-
  location-storage rule + mandatory fiber-jobified concurrent test
- 8 session logs `docs/sessions/2026-05-16-geometry-v5*.md`
