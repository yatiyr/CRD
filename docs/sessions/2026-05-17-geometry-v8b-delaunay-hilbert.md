## Session 2026-05-17 — Phase 3.1.7 v8b 2D Hilbert-sort Bowyer-Watson Delaunay

### Goal

Ship v8b of the `crd-geometry-delaunay` cluster. Original plan was
Guibas-Stolfi 1985 divide-and-conquer. User scope-check chose option B:
**Hilbert + delaunator-style** (Mapbox delaunator / CGAL `spatial_sort` /
Sandia / libigl approach). Same Bowyer-Watson core as v8a; the only
difference is the **insertion order**. Lex-sort gives strict determinism
but linear scan-line walks. Hilbert sort gives spatial locality so each
jump-walk from the prior insertion is O(1) average steps — total
O(n log n) dominated by the sort itself.

### What we built / changed

- **Extracted shared Bowyer-Watson core** to `engine/geometry-delaunay/src/delaunay_2d_internal.hpp`
  (namespace `crd::geometry::delaunay::detail`): `Tri` slot + `TriPool`
  free-list + `build_super_triangle<T>` + `locate_triangle<T>` jump-walk +
  `insert_point<T>` cavity-BFS-and-fan. Pattern matches
  `engine/geometry-bvh/src/bvh_build_internal.hpp` from v1f. Header-only
  + templates so both consumer TUs emit their own instantiations.
- **Refactored `delaunay_2d.cpp`** (v8a) to consume the internal header.
  Now ~150 LOC of lex-sort + validation + driver loop. Same algorithm,
  same output, same explicit instantiations.
- **New `delaunay_2d_hilbert.cpp`** (v8b) — ~250 LOC: bbox compute →
  Skilling 2004 iterative `xy2d` per point on a 2^16 × 2^16 grid →
  sort `(hilbert_index, original_index)` → driver loop consuming the
  shared core.
- **New public `delaunay_2d_hilbert.hpp`** — re-exports `DelaunayResult2` +
  `DelaunayStatus` from `delaunay_2d.hpp`; adds the new entry.
- **Umbrella `delaunay.hpp`** updated to include both headers.
- **`tests/geometry-delaunay/CMakeLists.txt`** adds `test_delaunay_2d_hilbert.cpp`.
- **`scripts/per-slice-check.ps1`** gained a `-Parallel` flag (user request,
  same session) — spawns each requested config in its own `Start-Job` with
  `ninja -j NumProc/NumJobs`, logs to `scripts\.per-slice-logs\<preset>.log`.

### Plain-English explanation

Bowyer-Watson always works the same way: build a giant super-triangle that
contains every input point, then insert each point one at a time. For each
point you find the triangle that contains it (jump-walk), then find every
existing triangle whose circumcircle contains the new point (those are
"bad" — they violate the Delaunay empty-circumcircle property), delete
them, and re-triangulate the hole by fanning new triangles from the new
point to each boundary edge of the hole.

The cost is dominated by the jump-walk. From the previous insertion's
triangle, you have to walk through the mesh to find the new point's
containing triangle. If consecutive insertions are FAR apart (lex-sort's
left-to-right scan-line), you walk a lot — maybe √n steps per insertion,
n^1.5 total. If consecutive insertions are NEAR each other (Hilbert curve
order), you walk a constant — n total.

The Hilbert curve is a space-filling curve that visits every cell in a
2D grid such that consecutive cells are 2D-near. It's been around since
1891. Computing the Hilbert index for a 2D point is the Skilling 2004
iterative bit-by-bit method: 16 iterations gives 32-bit indices on a
65536 × 65536 grid.

Mapbox `delaunator` (the de-facto JS triangulator) uses this trick.
CGAL's `spatial_sort` uses it. Sandia uses it. libigl uses it. It's the
standard production approach.

### Decisions made (D81-D84, pinned for ADR-0076 §23 amendment at v8-close)

- **D81.** Hilbert grid resolution = 2^16 (65 536 cells per axis). Matches
  CGAL `spatial_sort` and Mapbox delaunator. 16-bit indices fit in `u32`
  Hilbert codes (32 bits when interleaved). Sub-cell resolution is
  irrelevant since `original_index` tiebreaks coincident-cell points at
  D83.
- **D82.** Hilbert mapping = Skilling 2004 iterative `xy2d`. Standard
  bit-by-bit Lam-Shapiro 1994 form but reformulated by Skilling for
  constant work per bit. 16 iterations per point → 16 × N bit ops.
- **D83.** Sort key = `(hilbert_index, original_index)`. Original-index
  tiebreak resolves coincident-grid-cell points deterministically. Same
  input → same Hilbert+orig keys → same insertion order → byte-identical
  output (modulo super-tri stripping which is also deterministic in
  triangle-id order). Output WILL differ from v8a (different insertion
  order produces different triangle-id allocation order) but is internally
  deterministic.
- **D84.** Bbox padding factor = 1.0 (no pad). Degenerate (all-coincident)
  bbox falls back to unit extent for sort purposes — Hilbert sort then
  degenerates to original-index tiebreak, exactly what we want.

### Files touched

- `engine/geometry-delaunay/src/delaunay_2d_internal.hpp` — NEW (~300 LOC).
- `engine/geometry-delaunay/src/delaunay_2d.cpp` — refactored to consume
  the internal header (now ~150 LOC).
- `engine/geometry-delaunay/src/delaunay_2d_hilbert.cpp` — NEW (~250 LOC).
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay_2d_hilbert.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — now re-exports v8b too.
- `engine/geometry-delaunay/CMakeLists.txt` — docstring updated (sources globbed).
- `tests/geometry-delaunay/CMakeLists.txt` — added `test_delaunay_2d_hilbert.cpp`.
- `tests/geometry-delaunay/test_delaunay_2d_hilbert.cpp` — NEW (12 cases).
- `scripts/per-slice-check.ps1` — `-Parallel` + `-ParallelJobs` flags added.

### Tests / verification

- **12 Catch2 cases / 552 assertions on the v8b suite:**
  - 3 diagnostics (TooFewPoints / NonFiniteInput / DuplicatePoint).
  - single triangle (3 pts) / square (4 pts, 2 tris) / pentagon
    (5 pts, 3 tris).
  - 64-pt random cloud — orient2d > 0 + empty-circumcircle invariant on
    every output triangle.
  - insertion-order determinism — shuffled inputs canonicalise to same
    triangle set.
  - large-coord f32 stability (1e6 scale).
  - f64 precision tier.
  - **equivalence with v8a** — 48-pt random cloud, canonical triangle
    sets compared byte-for-byte against `delaunay_2d` output.
  - **1024-point scale test** — 32 × 32 jittered grid completes cleanly,
    ≤ 2N triangles, Delaunay invariants hold.
- v8a + v8b combined run: 22 cases / 581 assertions, all pass.
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS first try**
  (sequential mode, elapsed 03:25): win-debug + win-asan + win-shipping +
  win-tidy.

### Parallel-mode tooling (user request, same session)

`scripts/per-slice-check.ps1` now accepts `-Parallel` and `-ParallelJobs N`.
In parallel mode each requested config runs in its own `Start-Job`; the
per-job ninja parallelism defaults to `NumProc / NumJobs` so total CPU
pressure is bounded; per-config logs go to `scripts\.per-slice-logs\`.

The 4 configs have independent `build/<preset>/` directories so they don't
fight over filesystem state. The biggest wall-time win is overlapping
win-shipping (LTO-link-bound, single-threaded) with the CPU-bound
debug/asan/tidy builds.

Sequential mode (current default) still runs in ~3:25 for a small module
change. Parallel mode is the new tool when iterating on cluster closes
or hunting flakes.

### Memory recorded

- `feedback_iterate_local_test_only.md` — during iteration build + run
  only the affected module's tests; reserve `per-slice-check.ps1` for
  slice close.
- `reference_build_test_workflow.md` — vcvars sourcing, single-target
  build, single-test-binary run, per-slice DoD gate, parallel-mode hooks.

### Next session starts with

v8c-pre `insphere_exact` Stage D paydown — mandatory before v8c 3D
Bowyer-Watson. `insphere` is currently Stage A/B (re-expression only,
not the full adaptive cascade). v8c-pre upgrades it to full Shewchuk
1997 expansion arithmetic with ULP-conformance test against Shewchuk's
published reference results. Filed in `docs/debt.md::Shewchuk adaptive
predicates`. Blocking for v8c — silent-correctness debt risk if v8c
ships without it.
