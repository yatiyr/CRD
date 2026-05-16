# Session log — 2026-05-16 — geometry v5-queries-extension

> Phase 3.1.7 v5 `-spatial` cluster's second-to-last slice. Extends the
> unified `crd::geometry::{raycast,overlap,closest_point,...}` facade
> (which v1i-a established for `{BvhTree, Bvh4Tree, DynamicBvh, primitive
> shapes}`) to also dispatch over the five v5 backends. **Compile-time
> overload polymorphism only** — no virtual, no IAcceleration vtable.
> Same ADL-resolved namespace = one unified set covering BVH + v5
> backends + primitives.

## Scope landed

| Element | Path |
|---|---|
| New facade header | `engine/geometry-spatial/include/crd/geometry/spatial/queries.hpp` |
| Umbrella update | `engine/geometry-spatial/include/crd/geometry/spatial/spatial.hpp` (added `#include` of new queries.hpp) |
| Tests | `tests/geometry-spatial/test_queries_facade.cpp` (16 cases) |
| CMake update | `tests/geometry-spatial/CMakeLists.txt` (one new file) |

No backend changes — pure forwarding facade.

## Support matrix (locked)

```
                  overlap   raycast   radius   nearest_n   find_pairs
   ──────────────────────────────────────────────────────────────────────
   KdTree          --        --        ✓        ✓           --
   LooseOctree     ✓         ✓         --       --          --
   RTree           ✓         ✓         --       ✓           --
   SpatialHash     ✓         ✓         ✓        --          ✓
   UniformGrid     ✓         ✓         ✓        --          ✓
   ──────────────────────────────────────────────────────────────────────
   BvhTree         ✓         ✓         --       closest_pt  --       (from BVH facade)
   Bvh4Tree        ✓         ✓         --       closest_pt  --
   DynamicBvh      ✓ (fat)   ✓ (fat)   --       closest_pt  ✓
```

`--` = NOT exposed via the facade because the backend doesn't naturally
support that query. **Don't fake what doesn't fit** — KdTree's natural
ops are k-NN + radius over a point cloud (no AABBs stored); LooseOctree
+ RTree are AABB indices with no per-point radius (consumer can do
AABB-overlap-then-filter at the call site if needed).

## Locked design choices (carries into ADR-0076 §20 v5-close amendment)

| # | Decision | Rationale |
|---|---|---|
| 1 | New `crd/geometry/spatial/queries.hpp` in `crd-geometry-spatial` (NOT a modified `crd-geometry-bvh/queries.hpp`) | Avoids reverse-dep edge — `crd-geometry-bvh` is sibling, not upstream, of `crd-geometry-spatial` |
| 2 | Facade overloads live in `crd::geometry` namespace (same as BVH facade) | One ADL set when users include both — `raycast(tree, ray)` resolves regardless of tree type |
| 3 | **No** matching `find_overlapping_pairs` on KdTree/LooseOctree/RTree | These backends don't have native pair queries; faking via O(N²) at the facade layer would mask the right "use the right backend" decision |
| 4 | Result types pass through unchanged (KdRadiusHit, KdNeighbor, RTree::Neighbor, SpatialHashPair, UniformGridPair, RayHit) — first-arg-type overload resolution | Heterogeneous result shapes are an honest API surface; trying to homogenise would lose the distance² / pair-canonical-ordering info |
| 5 | Convenience overloads only — NO scratch overloads on the facade | Scratch is a backend-specific knob that doesn't generalise (KdTree/LooseOctree/RTree don't have scratch). Users who need parallel-query thread-safety call the native scratch-taking overload directly per `feedback_spatial_substrate_thread_safety.md` |
| 6 | Forwards-only impl — facade adds zero state, zero overhead | Preserves backend's documented determinism + thread-safety contract verbatim |
| 7 | Both callback (`template<typename Fn>`) AND Array-sink forms exposed | Matches native APIs' shape; streaming consumers (eylem broadphase) want callback; bulk consumers (renderer cull list) want Array |

## Tests — 16 cases / 5-config DoD PASS

Suite breakdown:

| Backend | Overloads tested | Cases |
|---|---|---|
| LooseOctree | overlap + raycast | 2 |
| RTree | overlap + raycast + nearest_n | 3 |
| SpatialHash | overlap + radius + raycast + find_overlapping_pairs | 4 |
| UniformGrid | overlap + radius + raycast + find_overlapping_pairs | 4 |
| KdTree | radius + nearest_n | 2 |
| Cross-backend uniformity demo | `crd::geometry::overlap(tree, q, out)` called on 4 different tree types, same content → same result | 1 |

Each parity test: build a backend the natural way → run native API to get
reference → run facade overload → assert byte-identical (`sort_arr` for
order-independent set equality, direct field compare for `RayHit` /
`Neighbor` / pair types).

The cross-backend test is the demonstration of the facade's value:
generic code that takes any AABB-spatial-backend and queries it with
ONE function name, no virtual, no template-of-templates gymnastics.

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 2093 / 2093 ctest |
| win-asan | PASS | full project ctest — facade adds no state, no race surface |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 2088 / 2088 ctest under `CRD_ENABLE_PROFILING=ON` + LTCG |
| win-tidy | PASS | clang-tidy clean (one drive-by `readability-isolate-declaration` on a multi-comma line — non-blocking) |

`scripts/per-slice-check.ps1` gates the first 4; win-shipping-profile run
separately. Total full-project ctest: **2077 → 2093 win-debug** (+16
facade cases).

## Phase 3.1.7 v5 `-spatial` cluster — 7/8 slices shipped 2026-05-16

| Slice | Status | LOC engine | LOC tests |
|---|---|---|---|
| v5a KdTree | ✅ | ~700 | ~750 |
| v5b LooseOctree | ✅ | ~750 | ~530 |
| v5c RTree (R*-tree) | ✅ | ~1400 | ~570 |
| v5d SpatialHash + v5d-fast scratch | ✅ | ~850 | ~750 |
| v5e UniformGrid (with scratch from day 1) | ✅ | ~700 | ~700 |
| v5 thread-safety validation pass | ✅ | — | ~150 (3 fiber-jobified concurrent tests) |
| v5-index-bringup (`scene::SpatialBVHIndex`) | ✅ | ~250 | ~430 |
| **v5-queries-extension (facade)** | ✅ | **~250 (header-only)** | **~450** |

**Only `v5-close` remains** — ADR-0076 §20 amendment + `docs/systems/geometry-spatial.md` + 18-config full sweep + ROADMAP/context/MEMORY sync.

## Cross-substrate observation

The facade is the **architectural completion** of the cluster: 6 backends
+ 1 scene-layer adapter all reachable through one consistent overload
set. This is what eylem v3 (multi-backend broadphase), renderer (frustum
cull), scene-spatial queries, editor-spatial-picking, and any future
spatial-feeling consumer will use.

The pattern reusable beyond geometry — any module that ships multiple
backends with overlapping APIs (eylem narrowphase variants, audio
DSP kernels, hesap solver variants) can adopt the same compile-time-
overload-facade-in-shared-namespace pattern.

## Next

v5-close — the final slice:
* ADR-0076 §20 amendment locking all v5 substrate decisions (8 slices' worth)
* `docs/systems/geometry-spatial.md` system overview
* 18-config full sweep (11 Win + 7 Linux WSL)
* ROADMAP / context / MEMORY final sync

After v5-close, Phase 3.1.7 v6 `-polygon` cluster opens (Vatti + CDT +
Bentley-Ottmann + ear clipping).
