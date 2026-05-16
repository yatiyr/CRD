# Session log — 2026-05-16 — geometry v5 thread-safety validation

> Cross-cutting tightening pass after v5e UniformGrid close. Locks the
> design principle that emerged across v5d/v5e and applies it
> retroactively to the three "naturally const-safe" backends (v5a KdTree,
> v5b LooseOctree, v5c R*-tree) — adds fiber-jobified concurrent-read
> tests that PROVE the const-safety claim empirically under win-asan
> race detection. **Does NOT add scratch overloads to those backends** —
> matching API to actual need is the elite call.

## The design principle (locked)

The five v5 spatial backends split into two camps based on a single
algorithmic property: **does each stored object live in exactly one
location, or in multiple?**

| Backend | Object lives in | Per-object dedup state | Scratch needed? | Concurrent test |
|---|---|---|---|---|
| **KdTree** (v5a) | exactly one leaf | none | **NO** | ✅ now shipped |
| **LooseOctree** (v5b) | exactly one cell (Ulrich's invariant — the whole point of the loose factor) | none | **NO** | ✅ now shipped |
| **R*-tree** (v5c) | exactly one leaf entry (splits move it but always to one leaf) | none | **NO** | ✅ now shipped |
| **SpatialHash** (v5d) | every cell its AABB overlaps | required | **YES** (v5d-fast) | ✅ shipped |
| **UniformGrid** (v5e) | every cell its AABB overlaps | required | **YES** (day 1) | ✅ shipped |

The first three are *trees* — each object is at exactly one node. Tree
traversal naturally encounters each object at most once → no dedup
state needed → queries are PURELY READ-ONLY → **thread-safe by
construction**.

The last two are *cell-based with multi-cell membership* — an AABB
spans multiple cells, the same `obj_idx` appears in multiple buckets.
Dedup is required. The elite zero-allocation dedup trick mutates per-
object state (`obj.last_query_gen = current_gen`) from inside a `const`
method via `mutable` + `const_cast`. **That mutation is what makes the
const query non-thread-safe**, and what motivated the scratch overload
(moves the dedup state from the tree to the caller).

## The principle: API surface matches actual need

**Adding scratch overloads to KdTree/LooseOctree/R*-tree would be
anti-elite cargo-culting.** Three reasons:

1. **API honesty.** A `scratch` parameter on a query that doesn't need
   one tells callers "you need this for concurrency" — but they don't.
   Misleading API > absent API.
2. **Wasted memory.** A scratch holds dedup state. KdTree/LooseOctree/
   R*-tree don't have dedup state to hold.
3. **API surface bloat.** Two overloads doubles the surface; justified
   when both are needed; pure noise when only one is.

**Adding fiber-jobified concurrent-read tests is the right addition.**
It validates the "const-safe by construction" claim empirically — turns
"we believe it" into "ASan validated 400 fan-out tasks across 4 fibers
under win-asan race detection". Documentation-as-test pattern.

## Scope landed

| Element | Path | Change |
|---|---|---|
| Listener rename (binary-wide) | `tests/geometry-spatial/test_spatial_hash.cpp` | `SpatialHashJobsListener` → `GeometrySpatialJobsListener` (was already binary-wide; rename clarifies intent) |
| KdTree concurrent k-NN test | `tests/geometry-spatial/test_kd_nearest_n.cpp` | +1 case, 400 fan-out tasks via `crd::jobs::parallel_for` |
| LooseOctree concurrent overlap test | `tests/geometry-spatial/test_loose_octree.cpp` | +1 case, 400 fan-out tasks |
| R*-tree concurrent overlap test | `tests/geometry-spatial/test_rtree.cpp` | +1 case, 400 fan-out tasks |

Each test uses the convenience query API directly (no scratch param —
proving that API IS thread-safe for these structures), with each task
holding its own `TlsfAllocator` + output `Array<u32>` (no shared
mutable state on the caller side either). Atomic mismatch counter ==
0 asserted across all fibers.

## Per-slice DoD — 5 configs PASS

| Config | Status | Notes |
|---|---|---|
| win-debug | PASS | 2069 / 2069 ctest |
| win-asan | PASS | full project ctest — race detection clean across ALL 5 backends' fiber-jobified tests (5 × 400 = 2000 fan-out tasks total) |
| win-shipping | PASS | LTCG-clean |
| win-shipping-profile | PASS | 2064 / 2064 ctest |
| win-tidy | PASS | clang-tidy clean |

`scripts/per-slice-check.ps1` gates the first 4; win-shipping-profile run
separately. Total full-project ctest: **2066 → 2069 win-debug** (+3 cases).

## Locked design principle (carries into ADR-0076 §20 v5-close)

**Pin: scratch overloads exist iff per-query dedup state requires them.**

The corollary: any future spatial substrate (v8 voxel-Delaunay, v9 LBVH,
future hybrid-grid backends) must analyse its multi-location-storage
property at design time:
* If **one-object-one-location** (BVH-style, tree-style): no scratch
  needed; queries are naturally const-safe; **fiber-jobified read-only
  test is mandatory** to validate the construction claim under ASan.
* If **multi-location-storage** (hash-style, dense-grid-style): scratch
  overload + caller-supplied dedup state; **concurrent test with per-
  fiber scratch is mandatory** to validate the race-free claim under
  ASan.

Both flavours converge on the same testing discipline: every spatial
backend ships a fiber-jobified concurrent test. The DIFFERENCE is the
API surface — match it to the algorithmic need.

## Cross-substrate observation

Looking at the 5 v5 backends + the precedent set by `crd-geometry-bvh`
(which has `find_overlapping_pairs` with caller-scratch via
`DynamicBvhPairScratch`), Cerid's spatial-substrate thread-safety
contract is now consistent:

* **State-bearing query patterns** (dedup, accumulator, work-stack):
  caller-supplied scratch struct, one per fiber.
* **Pure-read query patterns** (output Array, no internal mutation):
  convenience API only; concurrent safety established by the
  no-`mutable`-member structure + ASan-validated fiber test.

The pattern reusable for `crd-eylem` (broadphase queries) + `crd-renderer`
(spatial cull) + `crd-scene-spatial` (scene index queries).

## Next

Resume Phase 3.1.7 v5 `-spatial` cluster:
* **v5-index-bringup** — realize `scene::SpatialBvhIndex` reserved-shell
  from ADR-0053 (default backend = LooseOctree). The first non-reserved
  spatial index in `crd-scene`. ~2-3 days.
* **v5-queries-extension** — extend `crd/geometry/queries.hpp` compile-
  time-overload facade to dispatch over all 5 v5 backends. ~1 day.
* **v5-close** — ADR-0076 §20 amendment + `docs/systems/geometry-spatial.md`
  + 18-config full sweep. ~1 day.
