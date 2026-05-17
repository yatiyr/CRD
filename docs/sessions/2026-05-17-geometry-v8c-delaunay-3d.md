## Session 2026-05-17 — Phase 3.1.7 v8c 3D Bowyer-Watson Delaunay tetrahedralisation

### Goal

Ship v8c of the `crd-geometry-delaunay` cluster — the FIRST consumer of
v8c-pre's full Stage D `insphere_exact`. Pure 3D Delaunay tetrahedrali-
sation of a point set via Bowyer-Watson incremental insertion. Foundation
for FEA tet meshes, scientific visualization, 3D Voronoi (v8d-3d), Lloyd
CVT (v8e), Sibson NNI (v8f), Ruppert refinement (v8g, 3D extension), and
sliver removal (v8h).

### What we built / changed

- **New `engine/geometry-delaunay/src/delaunay_3d_internal.hpp`**
  (namespace `crd::geometry::delaunay::detail3d`):
  - `Tet { u32 v[4]; u32 nbr[4]; u8 alive }` ≤ 40-byte slot (D90,
    static_assert pinned).
  - `face_vertices[4][3]` permutation table giving canonical outward-
    oriented faces (D92):
    - face_vertices[0] = {1, 3, 2}  // opposite v0
    - face_vertices[1] = {0, 2, 3}  // opposite v1
    - face_vertices[2] = {0, 3, 1}  // opposite v2
    - face_vertices[3] = {0, 1, 2}  // opposite v3
    Verified via transposition parity such that `orient3d(face, v[i]) > 0`
    for every positively-oriented tet.
  - `TetPool` free-list LIFO allocator (mirror of `TriPool`).
  - `find_face_with_vertices` — locate which face slot of a tet matches a
    given unordered vertex triple (for outer-neighbour rewiring).
  - `is_finite_vec` — NaN/Inf rejection helper.
  - `build_super_tet<T>` — 1000× bbox scale super-tet, **D94** explicit
    ordering: base (s0, s1, s2) in z = cz - scale plane CW-from-+z view +
    s3 above. Shewchuk convention `orient3d > 0 iff d below abc plane`
    means CW-from-+z base + s3 above gives positive orient3d.
  - `locate_tet<T>` jump-walk — apex-side `orient3d` per face; cross
    face with negative sign; deterministic lowest-face-index tiebreak.
  - `insert_point_3d<T>` — full cavity BFS + boundary face collection +
    star-shape defensive check (D91) + free + fan + O(K²) edge-match
    neighbour wiring.
- **New public `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay_3d.hpp`**:
  - `DelaunayStatus3` enum with `Ok` / `TooFewPoints` / `NonFiniteInput` /
    `DuplicatePoint` / `Coplanar` / `InternalInvariant`. **`Coplanar`** is
    new vs 2D (D93 — every-4-tuple-orient3d-zero detection).
  - `DelaunayResult3<T>` with telemetry: `tet_indices` (4 per tet,
    positively oriented) + `tet_count` + `cavity_max_size` +
    `super_tet_stripped` + `status`.
  - `delaunay_3d<T>(ConstSpan<Vec3<T>>, IAllocator*) -> DelaunayResult3<T>`
    entry.
- **New `engine/geometry-delaunay/src/delaunay_3d.cpp`** — owns input
  validation, lex-sort `(x, y, z, original_index)`, coplanar diagnostic,
  augmented-points construction, super-tet seed + `CRD_ASSERT` positive
  orientation, per-point insert loop driving the shared core, strip-time
  tet emission filtering super-tet vertices.
- **Umbrella `delaunay.hpp`** re-exports v8c alongside v8a + v8b.
- **`engine/geometry-delaunay/CMakeLists.txt`** docstring updated.
- **`tests/geometry-delaunay/CMakeLists.txt`** adds `test_delaunay_3d.cpp`.
- **`tests/geometry-delaunay/test_delaunay_3d.cpp`** — 12 cases / 33
  assertions including the cospherical-pathology validator.

### Plain-English explanation

3D Bowyer-Watson works identically to 2D: insert points one at a time,
each insertion finds the "bad" tets (those whose circumsphere contains
the new point), removes them leaving a polyhedral cavity, and re-fans
new tets from the new point to each cavity boundary face.

The 3D structure has more bookkeeping than 2D:
- 4 vertices per tet (vs 3 per triangle).
- 4 neighbour pointers per tet (one per face).
- The "circumsphere contains point" test is `insphere` — a 5×5 lifted
  determinant. Stage D matters here: v8c-pre's paydown made this exact
  on cospherical input.
- The neighbour rewiring needs an outward-oriented face table since each
  face is a 3-vertex triangle that has TWO orientations (CCW from one
  side, CW from the other). The face table picks the canonical CCW
  ordering relative to the tet's outward normal so that the new tet
  built on it inherits positive orientation automatically.
- The boundary face collection now produces triangular faces instead of
  edges; each shared edge between two new tets (the ones built on
  adjacent cavity boundary faces sharing an edge) becomes a face of
  both new tets. We find these matches via O(K²) edge scan where K = #
  of cavity boundary faces (typically 20-50 — quadratic is fine).

The Shewchuk convention surprise: `orient3d(a, b, c, d) > 0` iff d is
**below** the plane abc viewed with the right-hand rule. My initial
super-tet ordering had the base CCW-from-+z + apex above, which gave
the OPPOSITE sign. Swapping two base vertices flipped to CW-from-+z
(= CCW-from-below) which matches the convention.

### Decisions made (D90-D94, pinned for ADR-0076 §23 amendment at v8-close)

- **D90.** **Internal `Tet` slot layout** `{ u32 v[4]; u32 nbr[4]; u8 alive }`,
  ≤ 40 bytes static_assert-pinned. Mirror of D73 for the 3D case.
- **D91.** **Star-shape defensive check** at cavity boundary collection:
  every boundary face must satisfy `orient3d(face_v0, face_v1, face_v2, q) > 0`.
  With Stage D `insphere` (v8c-pre) this MUST hold for valid input. A
  failure means input is degenerate beyond what predicates can resolve;
  we return `DelaunayStatus3::InternalInvariant` rather than ship a
  corrupt mesh. Defense-in-depth — never tripped on the test corpus.
- **D92.** **Face vertex permutation table** in the internal header
  giving the canonical outward-oriented face opposite each vertex. The
  permutations chosen so that `orient3d(face_v0, face_v1, face_v2, v[i]) > 0`
  for every positively-oriented tet — lets the new tet (face, q) inherit
  positive orientation when q is on the same side as the original apex
  (which it is, since q is inside the cavity).
- **D93.** **Coplanar diagnostic** (new in 3D — no 2D analog): if all N
  input points are coplanar, no 3D tetrahedralisation exists. Detected
  by scanning sorted prefix for any 4-tuple with non-zero `orient3d`;
  if none found, report `DelaunayStatus3::Coplanar`. O(1) typical, O(N)
  worst-case.
- **D94.** **Super-tet ordering** explicitly chosen to match Shewchuk's
  `orient3d > 0 iff d below abc plane` convention. Base (s0, s1, s2)
  CW-from-+z view + s3 at +z gives positive orient3d. Verified by
  `CRD_ASSERT` at init.

### Files touched

- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay_3d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — re-exports v8c.
- `engine/geometry-delaunay/src/delaunay_3d_internal.hpp` — NEW.
- `engine/geometry-delaunay/src/delaunay_3d.cpp` — NEW.
- `engine/geometry-delaunay/CMakeLists.txt` — docstring updated.
- `tests/geometry-delaunay/CMakeLists.txt` — added test source.
- `tests/geometry-delaunay/test_delaunay_3d.cpp` — NEW (12 cases / 33 assertions).

### Tests / verification

- **12 Catch2 cases / 33 assertions on v8c suite**:
  - 4 diagnostics (TooFewPoints / NonFiniteInput / DuplicatePoint /
    Coplanar).
  - single tetrahedron (4 pts → 1 tet).
  - tet + center (5 pts → 4 tets).
  - cube (8 pts → 5-12 tets, exact count compiler-dependent; Delaunay
    invariants verified).
  - 24-pt random cloud — `verify_delaunay_3d` helper checks orient3d > 0
    AND empty circumsphere on EVERY output tet.
  - **cospherical-pathology 9-pt mixture** (`[cospherical]` tag): 5
    cospherical points on r²=5e9 sphere (the v8c-pre Stage D
    discriminator) + 4 non-cospherical points. **Without Stage D
    insphere this test would produce inverted tets**; with it the
    Delaunay invariants hold.
  - insertion-order determinism (shuffled input → canonicalised tet
    sets identical).
  - large-coord f32 stability (1e6 scale).
  - f64 precision tier.
- v8a + v8b + v8c combined run: 34 cases / 614 assertions, all green.
- **Mid-slice bug fix**: initial super-tet ordering caused `CRD_ASSERT`
  failure at first run (super_orient < 0). Shewchuk's `orient3d > 0 iff
  d below abc` convention required CW-from-+z base + apex-above instead
  of CCW-from-+z + apex-above; swapped s1↔s2 to fix. No other bugs —
  faces table, cavity BFS, fan, neighbour rewiring all worked first try
  after the super-tet fix.
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel` (backgrounded).

### Next session starts with

v8d-2d — 2D Voronoi diagram extraction. Geometric dual of v8a/v8b. Each
Delaunay triangle's circumcentre is a Voronoi vertex; perpendicular
bisector segments between adjacent circumcentres form Voronoi edges;
cells emitted as `Polygon2<T>` with bounded/unbounded flag for boundary
cells. DCEL face traversal for cell topology. Powers Worley/cellular
noise, biome partitioning, NNI (v8f).
