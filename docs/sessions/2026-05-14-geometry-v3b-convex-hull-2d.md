# Session — 2026-05-14 — Phase 3.1.7 v3b — 2D convex hull (Andrew's monotone chain)

## Goal

Ship 2D convex hull computation as the first concrete consumer of v3a's Shewchuk adaptive predicates. The Andrew 1979 monotone chain is the canonical O(n log n) algorithm: lex-sort points, sweep left-to-right for the lower hull, sweep right-to-left for the upper hull, concatenate. Output is the CCW hull boundary.

## What we built

### New header — `engine/geometry-convex/include/crd/geometry/convex/convex_hull_2d.hpp`

Two API forms (both consume the same underlying algorithm):

```cpp
// Primary: output is indices into the input array, CCW order.
template <MathScalar T>
void convex_hull_2d_indices(ConstSpan<Vec2<T>> points, Array<u32>& out_hull_indices);

// Convenience: output is the actual Vec2<T> positions, CCW order.
template <MathScalar T>
void convex_hull_2d_points(ConstSpan<Vec2<T>> points, Array<Vec2<T>>& out_hull_points);
```

Both templated on `T ∈ {f32, f64}`. f32 callers go through f64-adaptive `orient2d` internally (the Shewchuk predicate handles f32 promotion to f64). The points form is implemented as a thin wrapper over the indices form (algorithmic correctness is shared).

### Algorithm structure

Andrew 1979 monotone chain:

1. **Sort** by lex `(x, y)` using `crd::containers::stable_sort` (deterministic per ADR-0063).
2. **Dedup** coincident points after sort (the hull never has two consecutive identical vertices).
3. **Lower hull**: iterate sorted points left-to-right; maintain a stack; pop the top while `orient2d(top2, top1, p) ≤ 0` (collinear or right turn — both removed; matches the published Andrew convention so the hull boundary has no degenerate collinear segments).
4. **Upper hull**: iterate sorted points right-to-left with the same rule into a separate stack.
5. **Concatenate**: drop the last point of each stack (they duplicate the other's first point at the lex-min and lex-max extremes).

### Degenerate input handling

Per ADR-0076 §15 (queries-tolerate, builders-reject):
- 0 points → empty hull.
- 1 point → 1-vertex hull.
- 2 distinct points → 2-vertex hull (segment).
- 3+ exactly-collinear → 2-vertex hull (lex-min and lex-max — the segment endpoints).
- All-coincident → 1-vertex hull (the first by stable sort).

Builder-reject: `CRD_ASSERT` finite input in debug. Release path produces a valid-but-degenerate hull on NaN/Inf (the adaptive `orient2d` returns 0 on non-finite, which collinear-collapses the point out).

### Determinism contract

- Sort tiebreak: stable sort with `(point.x, point.y, input_index)` lex comparison. Equal `(x, y)` → lower input index wins.
- "Left turn" decision: strict `orient2d > 0` (Shewchuk adaptive — exact sign on near-collinear input).
- Output is bit-exact across compilers / SIMD widths / OSes.

### Consumer-readiness

- **v3c 3D Quickhull** will use this as the coplanar-input fallback (degenerate 3D hull on the dominant plane).
- **v6 Vatti polygon Boolean** uses 2D hulls for convex envelope shortcuts.
- **v8 2D Delaunay (Bowyer-Watson)** uses the hull boundary as the initial outer loop.

## Tests — `tests/geometry-convex/test_convex_hull_2d.cpp`

14 cases / 58 assertions across 9 categories:

1. **Closed-form hulls**: 4-vertex square, 4-vertex square-with-interior-point, 3-vertex triangle, 6-vertex hexagon-with-10-interior-points.
2. **Degenerate inputs**: empty / single / two-point / collinear (5 points on `y = 2x + 1`) / all-coincident.
3. **CCW polygon invariant**: every consecutive triple in the output is a left turn (or collinear).
4. **Indices form / points form cross-check**: `hull_points[i] == points[hull_indices[i]]` for all `i`.
5. **Containment invariant**: every input point is inside-or-on the output hull polygon (verified via per-edge `orient2d` on every input point — all ≥ 0).
6. **Determinism replay**: two calls with identical input produce identical hull indices.
7. **Adversarial near-collinear** (the classic Quickhull torture): 4 points on `y=0` plus an outlier at `(1.5, 1e-14)`. The adaptive `orient2d` must detect the tiny tilt and place the outlier on the hull; naïve float `orient2d` would have missed it.
8. **Large-coordinate** (scale 1e6): 4-corner square at origin + 1e6 with interior point — adaptive predicate stays robust.
9. **f32 input**: 4-corner square with interior point produces 4-vertex hull (f32 promotes to f64 adaptive internally).

## Tests / verification

- **win-debug**: 14 v3b cases / 58 assertions ✅. Full convex suite: **160 cases / 20678 assertions** (was 146 / 20620 before v3b; +14 cases / +58 assertions).
- **win-asan**: 14 v3b cases / 58 assertions ✅.
- **win-shipping**: 14 v3b cases / 58 assertions ✅.
- **win-tidy**: green on the new files (`test_predicates.cpp` and `test_convex_hull_2d.cpp`); pre-existing tidy warnings in v2 test files (`test_gjk_distance.cpp`, `test_hull_queries.cpp`, `test_simd_support.cpp`) are NOT mine to fix in v3b — they're carry-over debt from v2.
- Full 17-config sweep deferred to v3-close per user directive.

## Bugs caught during v3b

1. **Upper hull stack logic broken on first draft** — initially tried a single-stack implementation with a `lower_hull_size` threshold preventing pops from crossing back into the lower hull; the boolean conditional was incorrect (`stack.size() > lower_hull_size + (stack.size() > lower_hull_size ? 0 : 1)` was a brain-tangle). Fix: restructured to two separate stacks (`lower` + `upper`), concatenate at the end with the standard "drop last of each" rule. Cleaner code, easier to verify against Andrew's published pseudocode.

## Clang-tidy fixes (per the new "tidy after every slice" policy)

1. **`readability-isolate-declaration`** in `predicates.cpp` — multi-variable declarations (`crd::f64 a, b;`) flagged. Fixed in v3a retroactively + v3b by splitting onto individual lines.
2. **`modernize-use-std-numbers`** in `test_convex_hull_2d.cpp` — `3.14159265358979323846` literal flagged; replaced with `std::numbers::pi_v<f64>`.
3. **`readability-identifier-naming.LocalConstexprVariable`** in `test_convex_hull_2d.cpp` + `test_predicates.cpp` — `constexpr f64 R = 1.0e6;` flagged. The `.clang-tidy` config requires `kCamelCase` for local constexpr (CLAUDE.md says "lower_case" but `.clang-tidy` is authoritative). Renamed to `kOrigin`.

## Decisions made

- **Two API forms (indices + points)** — indices is primary (caller owns association); points is convenience built on top. Both share the algorithm.
- **Two separate stacks for lower/upper hull** — cleaner than the threaded single-stack form; matches Andrew's published pseudocode 1:1.
- **`crd::containers::stable_sort`** — ADR-0063 deterministic sort. Provides natural tiebreak on `(x, y)` ties (lower input index wins).
- **Lex tiebreak in comparator** — even though stable sort would give us the equivalent, making the comparator explicitly tiebreak on `input_index` means we work correctly with non-stable sort too (future ABI flexibility).
- **Output is CCW** — standard convention for outward-facing polygons.
- **No `ConvexHullView` integration in v3b** — 2D hulls produce `Array<Vec2>` / `Array<u32>`, not `ConvexHullView` (which is 3D-specific with face_vertex_indices/offsets). v3c 3D Quickhull is where `ConvexHullView` enters via `to_convex_hull_view_owning(QuickhullResult)`.

## Files touched

- **New**: `engine/geometry-convex/include/crd/geometry/convex/convex_hull_2d.hpp` (~250 LOC)
- **New**: `tests/geometry-convex/test_convex_hull_2d.cpp` (~330 LOC, 14 cases / 58 assertions)
- **Edited**: `engine/geometry-convex/include/crd/geometry/convex/convex.hpp` (added include for the new header)
- **Edited**: `tests/geometry-convex/CMakeLists.txt` (added `test_convex_hull_2d.cpp`)
- **Edited** (retroactive v3a tidy fix): `engine/geometry-primitives/src/predicates.cpp` (split multi-var declarations) + `tests/geometry-primitives/test_predicates.cpp` (renamed `R` → `kOrigin`)

## Next session starts with

**v3c — 3D Quickhull (Barber-Dobkin-Huhdanpaa 1996).** Honest 1500-LOC sizing per Q3. The substrate of the v3 sub-phase. Consumes v3a `orient3d` for face-side tests + v3b 2D hull as the coplanar fallback. Output `QuickhullResult<T>` that converts to `ConvexHullView` via `convex_hull_view_of(...)` for v2 GJK/EPA binding.

## Notes for future-me

- Pre-existing tidy warnings in v2 test files (`test_gjk_distance.cpp`, `test_hull_queries.cpp`, `test_simd_support.cpp`) are NOT in scope for v3b. v3-close (or a dedicated tidy-debt-paydown slice) can address them. Carry-over.
- The `_constexpr LocalConstexprVariable kCamelCase` rule is a divergence from CLAUDE.md's "Constexpr var lower_case". The `.clang-tidy` file is authoritative; CLAUDE.md is documentation drift. Note for the next docs pass.
