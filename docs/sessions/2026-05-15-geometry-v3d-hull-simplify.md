# 2026-05-15 — Phase 3.1.7 v3d: hull simplification

## What shipped

**v3d hull simplification** — last v3 slice before v3-close. Greedy
vertex-removal hull simplifier with a shrinkage-distance cost metric,
convexity guard via v1h `k_distance_epsilon`, and a multi-domain
locked-vertex constraint (`keep_vertex_indices`).

**Files added:**
- `engine/geometry-convex/include/crd/geometry/convex/hull_simplify.hpp` (~135 LOC public API + design comment)
- `engine/geometry-convex/src/hull_simplify.cpp` (~440 LOC implementation, f32+f64 explicit instantiations)
- `tests/geometry-convex/test_hull_simplify.cpp` (~440 LOC, 16 test cases)

**Files updated:**
- `engine/geometry-convex/include/crd/geometry/convex/convex.hpp` (umbrella include + new `quickhull.hpp` include line)
- `tests/geometry-convex/CMakeLists.txt` (added new test file)
- `docs/phases/phase-3.1.7-geometry.md` (marked v3d row as shipped + updated consumer mapping)
- `context.md` (current-focus paragraph)
- Memory `project_state.md`

## API

```cpp
template <MathScalar T> struct HullSimplifyOptions
{
    u32 target_vertex_count = 0;                 // 0 = no target
    T   max_error_threshold = 0;                  // 0 = no threshold
    ConstSpan<u32> keep_vertex_indices = {};     // LOCKED vertices
};

template <MathScalar T>
QuickhullResult<T> simplify_hull(const QuickhullResult<T>& source,
                                   IAllocator* alloc,
                                   const HullSimplifyOptions<T>& opts = {});
```

## Algorithm

Greedy vertex removal:
1. Per-vertex incident-face index built once.
2. **Per-vertex cost = max distance from removed vertex to any fan-triangle plane** that would replace it. Fan pivot = lowest-index ring vertex (deterministic per ADR-0076 §4 pin #11).
3. Linear scan finds cheapest non-locked, non-tombstoned vertex.
4. **Convexity guard**: every new fan face's outward plane must have all OTHER live hull vertices below within `k_distance_epsilon`. If violated, tombstone the candidate and try the next-cheapest.
5. On admit: drop incident faces, append fan triangles, update adjacency, clear tombstones (topology changed).
6. Stop when `target_vertex_count` reached OR `max_error_threshold` exceeded OR all candidates exhausted.
7. Compact into fresh `QuickhullResult<T>`; output vertices in increasing-original-input-index order; face planes re-derived from final vertex positions.

## Bug caught + fixed during first test run

Initial `build_ring` used `(k+1)%3` to pick the walk-start ring vertex
(the vertex "after" v in face F0's CCW order). This walks CW around v
when looking from outside the hull, producing an inverted fan
triangulation — the convexity guard rejected every removal.

**Fix:** use `(k+2)%3` (the vertex that comes BEFORE v in F0's CCW
order). This walks CCW around v from outside, matches the outward
normal direction of the cap. After the fix, all 16 test cases passed
first try.

Concrete trace (pyramid example, v at apex, 4 base vertices r_0..r_3
CCW from above): old code produced ring `[r_0, r_3, r_2, r_1]` (CW
around v); fixed code produces `[r_1, r_2, r_3, r_0]` (CCW around v
from outside). The fan triangle winding correctly produces outward
normals via `(b - a) × (c - a)`.

## Drive-by: non-ASCII test name fix

While running win-debug ctest, 8 pre-existing v3b + v3c tests reported
"Failed" with mojibaked names — the same Windows-ctest-argv mojibake
bug that triggered the `crd-no-non-ascii-test-names` guard creation in
v1i-c. The guard's scope apparently doesn't reach `tests/geometry-convex/`
or v3b/v3c shipped past it.

Fixed all `→` → `->` and `—` → `--` in:
- `tests/geometry-convex/test_convex_hull_2d.cpp` (5 TEST_CASE names)
- `tests/geometry-convex/test_quickhull.cpp` (14 TEST_CASE names)
- `tests/geometry-convex/test_hull_simplify.cpp` (1 — my own)

Filing as a v3-close item: revisit the guard scope to catch this
pre-emptively.

## Verification

| Config | Build | ctest |
|---|---|---|
| win-debug | ✓ | **1546/1546** |
| win-asan | ✓ | **1546/1546** |
| win-shipping | ✓ | **1541/1541** |
| win-tidy | ✓ (clean on my files) | **1546/1546** |

Convex suite alone: **198 cases / 21270 assertions** (was 182 / 21079
at v3c-c close, +16 cases / +191 assertions).

Full 17-config `scripts/full-sweep.ps1` deferred to v3-close per user
directive (per `feedback_full_sweep_required.md`: sweep is the close
gate, not the per-slice gate).

## Test coverage (16 cases)

1. Empty source → empty result
2. Tetrahedron cannot be reduced below 4 vertices
3. Both opts zero → identity copy
4. target_vertex_count >= source size → identity copy
5. Coplanar source → identity (flat hulls already minimal)
6. Cube → 4 vertices reduces, stays convex, output ⊆ input
7. Octahedron → 4 vertices reduces, stays convex, output ⊆ input
8. Locked diagonal-corner vertices survive on cube
9. All-locked input → no-op
10. Tiny error threshold → no removal
11. Huge error threshold → full reduction, stays convex
12. Determinism replay (identical input → identical output)
13. Random cloud (200 points) → reduces and stays convex
14. f32 cube → reduces and stays convex
15. Output AABB ⊆ input AABB (random cloud)
16. Locked vertices preserved in random cloud

Each test uses TlsfAllocator per `feedback_named_allocators_in_tests.md`
(no `default_allocator()`).

## Performance

Linear scan over candidates each iteration → O(n²) total time. Fine
for the working hull sizes (~5k vertex cap per v1j-b sandbox).
Upgradable to a min-heap if a perf consumer surfaces. Performance
benchmark (`tests/bench/test_bench_hull_simplify.cpp`) deferred to
v3-close.

## Next

**v3-close** — tiebreak conformance sweep + degenerate corpus + perf
bench (`test_bench_hull_simplify.cpp`) + full 17-config
`scripts/full-sweep.ps1` + ADR-0076 §18 amendment + revisit the
`crd-no-non-ascii-test-names` guard scope so it actually scans
`tests/geometry-convex/` next time.

Then **v4 `-mesh` cluster** (TriangleMeshView + half-edge + mesh
closest-point + Möller-Trumbore raycast + Jacobson 2013 winding number
+ v4g per-leaf SIMD + v4-validate formal mesh validation).
