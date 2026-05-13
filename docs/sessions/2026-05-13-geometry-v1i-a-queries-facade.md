# Session — 2026-05-13 — Phase 3.1.7 v1i-a: unified query facade + `RayHit<P>` /
# `ClosestPointResult<P>` templated result types + `bvh4_closest_point` +
# `DynamicBvh::closest_point` (ADR-0076 §15, §16 pin #2)

## Goal

The first of three sub-slices for v1i (ADR-0076 §15 — checklist-driven
additions to `crd-geometry`). v1i is sized ~700 LOC engine + ~500 tests over
~3 days; the user split it a/b/c after the design call surfaced a real
tension in §16 pin #2 (templated `RayHit{t, payload}` vs. "distinct concrete
result types per backend"). v1i-a lands:

- the templated result types as the substrate the other backends alias;
- the missing `closest_point` variants for `Bvh4Tree` and `DynamicBvh`
  (today only `BvhTree` ships `bvh_closest_point`, v1e);
- `crd/geometry/queries.hpp` — the unified facade overloads;
- a `test_queries.cpp` cross-verifying every facade overload against the
  underlying backend it forwards to, plus dedicated coverage for the new
  closest-point variants.

v1i-b (shapecast — closed-form TOI) and v1i-c (`find_overlapping_pairs`
dual-descent + validation-corpus helpers) follow.

## What we built / changed

### `engine/geometry-bvh/include/crd/geometry/result_types.hpp` (new)

The substrate header — only the templated result structs, no backend
includes:

```cpp
template <typename Payload> struct RayHit
{
    crd::f32 t{0.0F};
    Payload payload{};
};

template <typename Payload> struct ClosestPointResult
{
    crd::math::Vec3<crd::f32> point{};
    crd::f32 distance_squared{0.0F};
    Payload payload{};
};
```

Field order pinned: `RayHit{t, payload}` and
`ClosestPointResult{point, distance_squared, payload}` (ADR-0076 §16 pin #2,
first sentence). Backends provide their concrete return types via aliases:
`using BvhRayHit = RayHit<u32>;`, `using BvhClosestPoint = ClosestPointResult<u32>;`;
v4 mesh raycast will add `using MeshRayHit = RayHit<MeshHitPayload>;` etc.
Same canonical struct, named per backend at the call site — the alias is the
documentation. No backend-specific bloat in the substrate header.

Lives in `crd-geometry-bvh`'s include tree (top of `crd/geometry/`, not
under `crd/geometry/bvh/`) since `crd-geometry-bvh` is the lowest layer that
already depends on `crd-geometry-primitives` and is depended on by every
later geometry sub-module that'll need the types.

### `bvh_query.hpp` — struct → alias migration

`struct BvhRayHit{prim_index, t}` and `struct BvhClosestPoint{prim_index,
point, distance_squared}` are gone — replaced by:

```cpp
using BvhRayHit       = crd::geometry::RayHit<crd::u32>;
using BvhClosestPoint = crd::geometry::ClosestPointResult<crd::u32>;
```

Field rename `.prim_index` → `.payload`; field-order swap from
`{prim_index, t}` → `{t, payload}` and `{prim_index, point, distance_squared}`
→ `{point, distance_squared, payload}`. Impl-side designated/positional inits
updated:

- `bvh_query.cpp`: `BvhRayHit{best_t, best_prim}`, `BvhClosestPoint{best_point, best_d2, best_prim}`.
- `bvh4.cpp`: `BvhRayHit{best_t, best_prim}`.
- `dynamic_bvh.cpp`: returns `ClosestPointResult<u32>{best_point, best_d2, best_ud}`.

Test-side rename `hit->prim_index` → `hit->payload` across
`test_bvh_raycast.cpp`, `test_bvh_closest.cpp`, `test_bvh4.cpp`,
`test_bvh_refit.cpp` (7 lines). Aggregate inits in the `brute_*` reference
helpers swapped to the new field order.

### `bvh4.cpp` / `bvh4.hpp` — new `bvh4_closest_point`

```cpp
[[nodiscard]] std::optional<BvhClosestPoint> bvh4_closest_point(
    const Bvh4Tree& tree, ConstSpan<AABB3<f32>> prims, const Vec3<f32>& query,
    f32 max_dist = std::numeric_limits<f32>::infinity());
```

Branch-and-bound DFS over the 4-wide tree; same algorithmic shape as
`bvh_closest_point` over the source binary tree (collapse changes only
fan-out, never the chosen prim's distance²). Per-popped node:

1. Re-check the node's AABB against the (possibly tightened) `best_d2`;
   prune the whole subtree if the AABB lower-bound ≥ best.
2. Score every live child by its AABB squared distance; insertion-sort into
   *descending*-`d2` order — push then pop visits the nearest child first
   (tightens `best_d2` before the far subtrees are reached). Tiebreak on
   lower child index — deterministic.
3. Process leaf children directly (no recursion); push interior children to
   stack.

Squared throughout (no `sqrt` on the hot path); `max_dist²` stored as the
cutoff. Stack size = `k_max_bvh4_stack` (= 256, matches the existing 4-ary
DFS bound).

### `dynamic_bvh.hpp` / `dynamic_bvh.cpp` — new `DynamicBvh::closest_point`

```cpp
[[nodiscard]] std::optional<crd::geometry::ClosestPointResult<crd::u32>>
closest_point(const crd::math::Vec3<crd::f32>& query,
              crd::f32 max_dist = std::numeric_limits<crd::f32>::infinity()) const;
```

Member function (matches the existing `query` / `raycast` member style on
`DynamicBvh` — and needs access to the private `Node` array; no
public-API workaround). Broadphase semantics: closest leaf by *fat* AABB
distance (the fat AABB is what the tree stores; the caller's narrowphase
refines against tight prim data). Reports the leaf's `user_data` in the
payload, the point on its fat AABB, and the squared distance. Same B&B
shape: per-node lower-bound pruning, near-child-first push order, squared
throughout, `max_dist²` cutoff.

### `engine/geometry-bvh/include/crd/geometry/queries.hpp` (new)

The facade — free-function overloads in `crd::geometry`, resolved at
compile time over `{primitive shapes, BvhTree, Bvh4Tree, DynamicBvh}`. Zero
overhead: every overload `inline`-forwards to the backend function it
delegates to.

```cpp
// nearest-hit raycast
std::optional<bvh::BvhRayHit> raycast(const bvh::BvhTree&,  …);
std::optional<bvh::BvhRayHit> raycast(const bvh::Bvh4Tree&, …);
// (DynamicBvh's raycast is intentionally callback-only — broadphase form.)

// overlap (callback + Array append)
void overlap(const bvh::BvhTree&,  …);  // BvhTree     / Bvh4Tree / DynamicBvh
void overlap(const bvh::Bvh4Tree&, …);
void overlap(const bvh::DynamicBvh&, …);

// closest-point
std::optional<bvh::BvhClosestPoint> closest_point(const bvh::BvhTree&,  …);
std::optional<bvh::BvhClosestPoint> closest_point(const bvh::Bvh4Tree&, …);
std::optional<ClosestPointResult<u32>> closest_point(const bvh::DynamicBvh&, …);
```

Primitive overloads (`closest_point(Sphere, Vec3)` → `Vec3`,
`distance(Shape, Vec3)` → `T`, `intersects(A, B)` → `bool`,
`intersect_ray_aabb(...)` → `bool`, `signed_distance(Plane, Vec3)` /
`sd_sphere(p, c, r)` / …) are NOT redefined here — they already live in
`primitives/closest_point.hpp` / `primitives/intersect.hpp` /
`primitives/signed_distance.hpp` in the same `crd::geometry::primitives`
namespace. `queries.hpp` simply *includes* them so a caller of the facade
gets the full unified surface via the one header.

Pinned out-of-scope for v1i-a (the v1i-b/c slices):
- `find_overlapping_pairs(const DynamicBvh&, OutFn)` (dual-descent
  self-overlap) — v1i-c.
- Shapecast (`cast_ray` / `cast_sphere` / `cast_box` closed-form TOI) —
  v1i-b.
- The degenerate-corpus + large-coordinate validation sweep — v1i-c.

### `tests/geometry-bvh/test_queries.cpp` (new)

10 TEST_CASEs, 13,142 assertions, all green on win-debug + win-asan +
win-shipping:

1. **Layout pins** — `static_assert` that `BvhRayHit` IS `RayHit<u32>` and
   `BvhClosestPoint` IS `ClosestPointResult<u32>`; field order
   `{t, payload}` and `{point, distance_squared, payload}` pinned.
2. **`raycast(BvhTree)` == `bvh_raycast`** — facade and direct calls return
   bit-identical hits on a random 300-prim corpus, 500 rays each (incl.
   finite + ∞ `tmax`).
3. **`raycast(Bvh4Tree)` == `bvh4_raycast` == `raycast(BvhTree)`** — the
   collapse changes only fan-out; same `t` on every hit.
4. **`overlap(BvhTree) / overlap(Bvh4Tree)` match brute force** — sorted
   prim-index sets agree across binary, BVH4, and the brute reference;
   callback form == Array form.
5. **`overlap(DynamicBvh)`** — visits fat AABBs by `user_data` (broadphase
   form); 3-leaf scene confirms only the expected `user_data`s appear.
6. **`closest_point(BvhTree) == bvh_closest_point`** — facade and direct
   match on a 250-prim random corpus, 300 queries each, finite/inf `max_dist`.
7. **`bvh4_closest_point` matches brute force AND BvhTree closest_point** —
   on a random corpus across 3 trials (varying `max_leaf_prims` 1..6),
   200 queries per trial: squared distance bit-matches the brute force; on
   a tie the chosen prim may differ but each named prim genuinely realises
   that distance + the point is on its AABB. Cross-backend agreement: BVH4
   == binary tree distance² (the collapse doesn't change the chosen leaf).
8. **`bvh4_closest_point`: empty / single-prim / `max_dist` cutoff** — the
   three edge cases that aren't worth threading through the random sweep.
9. **`DynamicBvh::closest_point`** — matches a brute-force reference over
   the fat AABBs on a 250-leaf corpus (300 queries, varying `max_dist`);
   ties: payload may differ but every named leaf realises that distance.
10. **`DynamicBvh::closest_point`: empty / inside / cutoff** — empty tree →
    `nullopt`; query inside a fat AABB → `distance_squared == 0` at the
    query point with the right `user_data`; query beyond `max_dist` →
    `nullopt`.

`crd-geometry-bvh-tests` is now **50 cases / 65 549 assertions** on
win-debug (was 40 / 13 197 after v1g).

## Plain-English explanation

Before v1i-a, every BVH-style query function had its own *result struct*
hand-defined where it lived: `BvhRayHit` in `bvh_query.hpp`, `BvhClosestPoint`
likewise. When `crd-geometry-mesh` v4 adds ray-vs-triangle (with `{tri_index,
u, v, t}` payload) and the scene raycast adds `{entity_id, t}`, that pattern
would grow into a sprawl of one-off structs. v1i-a parks one canonical
templated type — `RayHit<Payload>` — at the top of `crd/geometry/` and lets
each backend alias-and-name it (`BvhRayHit` for the `u32` prim index,
`MeshRayHit` for the triangle barycentric payload, `SceneRayHit` for the
entity id). The alias *is* the documentation: the field name `payload`
admits "I am whatever the backend says I am," and `BvhRayHit hit.payload`
reads as "the BVH leaf index for this hit." No vtable, no `std::variant`,
no fat union. The two backends that were missing closest-point — BVH4 and
DynamicBvh — get them now, with the same Catto-style branch-and-bound the
binary tree had (`max_dist²` cutoff, nearer-child-first, lower-bound
pruning, squared throughout). Finally, the `queries.hpp` header is the one
file callers `#include` to see the whole unified surface — `raycast` /
`overlap` / `closest_point` / `contains` / `distance` over every backend
and every primitive shape — without learning the per-backend
function-naming convention (`bvh_raycast` vs `bvh4_raycast` vs
`DynamicBvh::raycast` vs `intersect_ray_aabb`).

## Decisions made

- **A. Templated `RayHit<P>` + alias, not B. distinct concrete structs**
  (user pick, in front of the advisor-surfaced ADR-0076 §16 pin #2 split —
  the pin's first sentence says templated, the last sentence says "clarity
  over genericity"). Migration cost — ~10 sites — is paid in this slice; the
  v4 mesh / scene raycast types will follow the same pattern as
  `RayHit<MeshHitPayload>` / `RayHit<EntityId>`.
- **Result-types file separate from facade file** (`result_types.hpp` vs
  `queries.hpp`). The types are the substrate the BVH header aliases — they
  must NOT include backend headers (`bvh_query.hpp` already includes
  `result_types.hpp`; if the types lived in `queries.hpp` we'd have a
  cycle). The facade is a separate concern: it pulls in every backend
  header + every primitive header + exposes the unified overloads.
- **`queries.hpp` lives in `crd-geometry-bvh`** (`engine/geometry-bvh/include/
  crd/geometry/queries.hpp` — top of `crd/geometry/`, NOT under
  `crd/geometry/bvh/`). User pick. `crd-geometry-bvh` is the lowest layer
  that already depends on both `crd-geometry-primitives` and the BVH types.
  No new module, no new CMake target. Later sub-modules (`-convex`, `-mesh`,
  `-spatial`) will add their own overloads to the same facade.
- **`DynamicBvh::closest_point` is a member function**, not a free function
  like `bvh4_closest_point`. Mirrors the existing `query` / `raycast`
  members on `DynamicBvh` and is the cleanest path to the private `Node`
  array (the alternative — adding a public `nodes()` accessor — would leak
  free-list internals). The facade `closest_point(DynamicBvh, …)` is the
  uniform call surface; the member is the implementation.
- **Primitive overloads NOT redefined in `queries.hpp`** — `intersect.hpp`,
  `closest_point.hpp`, `signed_distance.hpp`, `barycentric.hpp` already ship
  `intersects` / `closest_point` / `distance` / `contains` / `signed_distance`
  in `crd::geometry::primitives`. `queries.hpp` includes them; callers get
  them via ADL. No shadowing, no risk of accidentally redirecting a
  primitive call through a wrong overload.
- **`bvh4_closest_point` insertion-sorts children by distance, push-order
  descending → pop-order ascending**. With ≤4 children, an in-place
  insertion sort is 6-7 compares max — cheaper than a `std::sort` call and
  zero-allocation. Tiebreak on lower child index — deterministic; agrees
  with `bvh4_raycast`'s walk on tied AABBs.
- **`raycast(DynamicBvh, …)` is deliberately NOT a facade overload.** The
  native form on `DynamicBvh` is a *visit-every-hit* callback (broadphase
  semantics); there is no nearest-hit single-result API on it today. Eylem
  v1c's broadphase wraps `DynamicBvh::raycast` directly; if a unified
  nearest-hit form is ever wanted, it lands in a later slice with the v1c
  shapecast surface.

## Files touched

- New: `engine/geometry-bvh/include/crd/geometry/result_types.hpp`,
  `engine/geometry-bvh/include/crd/geometry/queries.hpp`,
  `tests/geometry-bvh/test_queries.cpp`, this session log.
- Modified: `engine/geometry-bvh/include/crd/geometry/bvh/bvh_query.hpp`
  (struct → alias), `engine/geometry-bvh/include/crd/geometry/bvh/bvh4.hpp`
  (added `bvh4_closest_point` declaration),
  `engine/geometry-bvh/include/crd/geometry/bvh/dynamic_bvh.hpp` (added
  `closest_point` member + `<crd/geometry/result_types.hpp>` + `<optional>`
  includes), `engine/geometry-bvh/src/bvh_query.cpp` (field-order swap),
  `engine/geometry-bvh/src/bvh4.cpp` (field-order swap + `bvh4_closest_point`
  impl), `engine/geometry-bvh/src/dynamic_bvh.cpp` (closest_point impl),
  `tests/geometry-bvh/test_bvh_raycast.cpp`,
  `tests/geometry-bvh/test_bvh_closest.cpp`,
  `tests/geometry-bvh/test_bvh4.cpp`,
  `tests/geometry-bvh/test_bvh_refit.cpp` (each: `.prim_index` →
  `.payload` rename + aggregate-init field-order swap on `brute_*`
  reference helpers), `tests/geometry-bvh/CMakeLists.txt` (added
  `test_queries.cpp`), and the docs (this slot).

## Tests / verification

Per the in-flight `-bvh` verification directive (win-debug + win-asan +
win-shipping + win-tidy build per slice; full 17-config `scripts/full-sweep.ps1`
deferred to v1 cluster close after v1j):

- **win-debug**: full build ✅; ctest **1232/1232 PASS** (was 1227 before
  v1i-a — +5 new TEST_CASEs in test_queries.cpp registered under
  `[geometry][queries][...]`; the layout-pin case + the 4 facade-equivalence
  cases). Total time ~30 s.
- **win-asan**: full build ✅; ctest **1232/1232 PASS** (62 s — ASan
  overhead). No use-after-free / heap-buffer-overflow flagged by my
  template-result-type changes or the new closest-point variants.
- **win-shipping**: full build ✅ (full LTO, MSVC); ctest **1227/1227 PASS**
  (debug-only tests gated `#if CRD_ENABLE_ASSERTS` correctly skipped).
- **win-tidy**: build ✅ (clang-tidy gate; my new files produced zero new
  warnings — the pre-existing `readability-inconsistent-ifelse-braces` in
  `sandbox/src/sandbox_layer.cpp` are from old code, unchanged).
- **CI guards**: `crd-no-std-math-check` + `crd-no-std-sort-check` +
  `crd-no-non-ascii-test-names` green. `crd-simd-emission-check` requires
  `dumpbin` (vcvars) — passes when run as part of full `ctest --preset
  win-debug` (which is how the win-debug count of 1232 is the *all green*
  number).

`crd-geometry-bvh-tests` directly: **50 cases / 65 549 assertions** on
win-debug (was 40 / 13 197 after v1g).

Full 17-config `scripts/full-sweep.ps1` — **deferred to v1 cluster close**
per the user (after v1j ships).

## Next session starts with

- **Phase 3.1.7 v1i-b** — shapecast (closed-form TOI). `crd/geometry/
  queries.hpp` gains `cast_ray` / `cast_sphere` / `cast_box` overloads
  against the primitive shapes *and* the BVH backends. Closed-form
  Minkowski-sum reductions for the closed-form shapes (sphere-cast vs AABB
  = ray-vs-AABB-grown-by-r; box-cast vs AABB = ray-vs-Minkowski-AABB;
  sphere-cast vs sphere = quadratic; capsule-cast vs triangle). BVH
  variants traverse with the swept-shape AABB. The *general* convex-cast
  (GJK-cast) lives in `-convex` v2. `test_shapecast.cpp`: TOI bit-matches
  brute-force; zero-radius sphere-cast recovers `cast_ray`. ~250 + ~150
  LOC, ~1 day.
- After v1i-b: **v1i-c** — `find_overlapping_pairs(const DynamicBvh&,
  OutFn)` (Catto-style dual-descent self-overlap) + reusable test-corpus
  helper header (degenerate corpus + large-coordinate sweep) +
  `test_validation.cpp` per module applying the corpora.
- After v1i closes: **v1j** (`crd-geometry-viz` companion module) → v1
  cluster close → run the full 17-config sweep → on to v2 (`-convex`: GJK
  + EPA + SAT + Quickhull).
