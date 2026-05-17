## Session 2026-05-17 — Phase 3.1.7 v8a 2D Bowyer-Watson Delaunay

### Goal

Open Phase 3.1.7 sub-module 9 of 11 — `crd-geometry-delaunay`. v8a
ships the substrate primitive: pure 2D Delaunay triangulation via
Bowyer 1981 / Watson 1981 incremental insertion. The standalone
`delaunay_2d(points)` API that v8b (divide-and-conquer), v8d-2d
(Voronoi extraction), v8e (Lloyd CVT), v8f (NNI), v8g (Ruppert
refinement) all build on.

### What we built / changed

- **New `engine/geometry-delaunay/` module** (9th `crd-geometry-*`
  sub-module). Depends PUBLIC on `crd-core`, `crd-containers`,
  `crd-memory`, `crd-math`, `crd-units`, `crd-geometry-primitives`
  (for Shewchuk adaptive predicates).
- **`delaunay_2d.hpp`** — API: `DelaunayStatus` enum, `DelaunayResult2<T>`
  struct, `delaunay_2d(points, alloc)` entry. Explicit `f32` + `f64`
  instantiations.
- **`delaunay_2d.cpp`** — Bowyer-Watson incremental implementation
  with internal `Tri` slot (3 vertex ids + 3 neighbour-tri ids +
  alive flag), `TriPool` free-list allocator, super-triangle at
  1000× bbox scale, lex-sorted insertion, jump-walk point location,
  cavity BFS using Shewchuk `incircle`, re-triangulation by fanning
  from inserted vertex, neighbour-pointer rewiring, super-tri
  vertex stripping at strip-time.
- **Root `CMakeLists.txt`** — `add_subdirectory(engine/geometry-delaunay)`.
- **`tests/CMakeLists.txt`** — `add_subdirectory(geometry-delaunay)`.
- **`tests/geometry-delaunay/CMakeLists.txt`** — new test target.
- **`tests/geometry-delaunay/test_delaunay_2d.cpp`** — 10 cases.

### Plain-English explanation

The Bowyer-Watson algorithm builds a Delaunay triangulation one point
at a time. The key invariant of a Delaunay triangulation is the
"empty circumcircle" property: every triangle's circumcircle contains
no OTHER input point. To maintain this, when you insert a new point,
you find every existing triangle whose circumcircle ALREADY contains
the new point (those are the "bad" triangles), delete them all, and
re-triangulate the resulting hole by connecting the new point to each
boundary edge of the hole.

The robustness magic comes from Shewchuk's adaptive precision
predicates — specifically `incircle(a, b, c, d)` returning the EXACT
sign of "is d inside the circumcircle of CCW triangle (a, b, c)?"
without floating-point roundoff error. Without this, naive
implementations break on cocircular points or near-cocircular
configurations. With it, the algorithm just works.

Determinism is achieved by lex-sorting input points before insertion
— `(x, y, original_index)` tiebreak — so the cavity expansions
happen in the same order every run.

### Decisions made (D73-D80, pinned for ADR-0076 §23 amendment at v8-close)

- **D73.** Internal `Tri` slot layout `{ u32 v[3]; u32 nbr[3]; u8
  alive; }`. `v[i]` is vertex id (input index, or N+0/1/2 for super-
  triangle). `nbr[i]` is the triangle adjacent across edge `(v[i],
  v[(i+1)%3])`, or `k_null_tri` if outer-boundary. Free-list pop is
  LIFO (deterministic given deterministic edit sequence).
- **D74.** Super-triangle scale = 1000× max(bbox.width, bbox.height).
  Matches Triangle / CGAL empirically-stable scaling. Three vertices
  placed at large enough offsets that no input point can fall on or
  near a super-edge.
- **D75.** Lex-sort `(x, y, original_index)` for insertion order.
  Original-index tiebreak resolves coincident-coordinate ties
  deterministically (though `DuplicatePoint` status fires before
  sort — exact-coord duplicates rejected upfront).
- **D76.** Jump-walk via apex-side `orient2d`. For triangle T =
  (a, b, c) and query q, compute signs of `orient2d` for each edge.
  All ≥ 0 → inside. Else cross the edge with most-negative sign;
  deterministic tiebreak prefers edge 0, then 1, then 2.
- **D77.** Cavity BFS in monotonic FIFO order via small deque +
  visited bitmap. On visit: `incircle(v0, v1, v2, q) > 0` →
  triangle bad, push neighbours.
- **D78.** Cavity boundary edges = edges of the bad-triangle cluster
  with non-bad (or null) neighbour-tri. Each emitted as `(v[k],
  v[(k+1)%3], outer_nbr_id)`.
- **D79.** Re-triangulation: for each cavity edge `(a, b, nbr)`,
  alloc new triangle `(a, b, q)`. Wire `new.nbr[0] = nbr`; outer's
  neighbour-pointer updated to point back. Second-pass wires
  `nbr[1]` and `nbr[2]` between consecutive new triangles by edge
  match (O(K²) where K = cavity size, typically ~6-10).
- **D80.** Strip-time triangle emission: walk pool in slot order;
  for each alive triangle whose 3 vertices are all `< N`, emit
  `(v[0], v[1], v[2])`. Triangle order matches triangle-id order —
  byte-identical across compilers.

### Files touched

- `engine/geometry-delaunay/CMakeLists.txt` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — NEW (umbrella).
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay_2d.hpp` — NEW.
- `engine/geometry-delaunay/src/delaunay_2d.cpp` — NEW.
- `CMakeLists.txt` — added `add_subdirectory(engine/geometry-delaunay)`.
- `tests/CMakeLists.txt` — added `add_subdirectory(geometry-delaunay)`.
- `tests/geometry-delaunay/CMakeLists.txt` — NEW.
- `tests/geometry-delaunay/test_delaunay_2d.cpp` — NEW (10 cases).

### Tests / verification

- **10 Catch2 cases / 29 assertions.** Coverage:
  - `TooFewPoints` diagnostic.
  - `NonFiniteInput` diagnostic.
  - `DuplicatePoint` diagnostic.
  - Single triangle (3 pts).
  - Square (4 pts, 2 triangles).
  - Regular pentagon (5 pts, 3 triangles).
  - 32-pt deterministic random cloud — verifies orient2d > 0 (CCW)
    AND empty-circumcircle invariant (incircle ≤ 0 for every other
    input point) on EVERY output triangle.
  - Insertion-order determinism (5-point square+centre with two
    different input orders — output triangulations equivalent under
    vertex-position canonicalisation).
  - Large-coord f32 stability (1e6 scale square+centre → 4 triangles).
  - f64 precision tier (square+centre → 4 triangles).
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS first try**
  (elapsed 03:44): win-debug + win-asan + win-shipping + win-tidy.
- One mid-build error fixed: missing `<array>` include in test file
  (used `std::array` for the determinism canonicalisation helper).

### Next session starts with

v8b 2D divide-and-conquer Delaunay (Guibas-Stolfi 1985) — O(n log n)
bulk-build alternative to v8a's O(n^1.5) incremental form. Faster
for 1000+ point sets. Edge-flip merge step + recursive split + lex-
sort. First step: `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay_2d_divide.hpp`
with the `delaunay_2d_divide(points)` entry point.
