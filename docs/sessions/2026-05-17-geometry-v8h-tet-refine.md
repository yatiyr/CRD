## Session 2026-05-17 — Phase 3.1.7 v8h 3D Delaunay quality refinement

### Goal

Ship the LAST algorithm slice of the v8 cluster — 3D Delaunay quality
refinement for FEA / FVM 3D mesh preprocessing. With v8h, all 11
algorithm slices of v8 are complete; only v8-close remains.

### Scope honesty (D119, pinned per advisor)

The phase doc described this as "Shewchuk-style sliver removal", but
true sliver exudation is the Cheng-Dey-Edelsbrunner-Facello-Teng 2000
algorithm using *weighted* Delaunay with per-vertex weight perturbations
— substantially different machinery. **v8h ships dihedral-bounded
refinement** (a 3D-Ruppert analog) that inserts Steiner points at
bad-tet circumcentres. Sliver exudation is a v8h-exude follow-on.

**Known limitation**: Steiner-insertion-only refinement does NOT
provably remove all slivers — slivers have good radius-edge ratio and
bad dihedral, and their circumcentres often land back inside another
sliver or outside the input domain. 3D termination is NOT guaranteed
for arbitrary α (no analog to Ruppert 2D's α ≤ 20.7° bound).
Adversarial inputs can return `NotConverged`; that's a valid outcome.

### What we built / changed

- **New public `engine/geometry-delaunay/include/crd/geometry/delaunay/tet_refine_3d.hpp`**:
  - `TetRefineStatus` enum (Ok / TooFewPoints / NonFiniteInput /
    DuplicatePoint / Coplanar / InvalidAngle / NotConverged /
    InternalInvariant).
  - `TetRefineOptions<T>` (min_dihedral_degrees default 10,
    max_iterations default 5000, max_steiner default 50000).
  - `TetRefineResult<T>` (vertices + tet_indices + telemetry).
  - `tet_refine_3d<T>(points, opts, alloc)` entry.
  - `min_dihedral_of_tet_rad<T>(v0..v3)` public helper.
- **New `engine/geometry-delaunay/src/tet_refine_3d.cpp`** implementing:
  - **Six-dihedral enumeration** via edge table
    `{(0,1,2,3), (0,2,1,3), (0,3,1,2), (1,2,0,3), (1,3,0,2), (2,3,0,1)}`
    where the tuple is `(i, j, k, l)` with edge `(vi, vj)` and off-edge
    vertices `(vk, vm)` (vm renamed from vl to dodge `misc-confusable-
    identifiers` v1↔vl trip).
  - Dihedral formula: `cos = dot(n1, n2) / (|n1| |n2|)` where
    `n1 = (vj-vi) × (vk-vi)`, `n2 = (vj-vi) × (vm-vi)`. Calibration:
    regular tet returns arccos(1/3) ≈ 70.5288°.
  - Refinement loop:
    - Initial Delaunay via v8c.
    - Bbox + 10% pad + bbox-scaled eps² = (bbox_diag × 1e-6)² (D122).
    - Per iteration: scan tets, find first with min-dihedral < α.
      Compute circumcentre. Skip if outside `[bbox±pad]` (D121) or
      near-duplicate. Insert if actionable; re-Delaunay.
    - Halt as `NotConverged` if no actionable bad tet OR limits hit.
- **Umbrella `delaunay.hpp`** re-exports v8h.
- **`engine/geometry-delaunay/CMakeLists.txt`** docstring updated.
- **`tests/geometry-delaunay/CMakeLists.txt`** adds `test_tet_refine_3d.cpp`.
- **`tests/geometry-delaunay/test_tet_refine_3d.cpp`** — 11 cases / 52
  assertions; **calibration test FIRST** per advisor TDD recommendation.

### Decisions made (D119-D122, pinned for ADR-0076 §23 at v8-close)

- **D119.** Scope honest: dihedral-bounded refinement, NOT sliver
  exudation. Sliver exudation = v8h-exude follow-on.
- **D120.** Six dihedrals per tet via explicit edge-tuple enumeration.
  Calibration: regular tet returns arccos(1/3).
- **D121.** Out-of-domain skip: bad-tet circumcentre outside input
  bbox+pad → skip and continue scanning. Don't extend the mesh past
  the input domain.
- **D122.** Bbox-scaled near-duplicate eps: `eps² = (bbox_diag × 1e-6)²`.
  Auto-scales with input coord magnitude. v8g's absolute 1e-12 would be
  too tight for FEA-scale 3D inputs (10³ to 10⁵ coord magnitudes).

### Files touched

- `engine/geometry-delaunay/include/crd/geometry/delaunay/tet_refine_3d.hpp` — NEW.
- `engine/geometry-delaunay/include/crd/geometry/delaunay/delaunay.hpp` — re-exports v8h.
- `engine/geometry-delaunay/src/tet_refine_3d.cpp` — NEW.
- `engine/geometry-delaunay/CMakeLists.txt` — docstring updated.
- `tests/geometry-delaunay/CMakeLists.txt` — added test source.
- `tests/geometry-delaunay/test_tet_refine_3d.cpp` — NEW (11 cases / 52 assertions).

### Tests / verification

- **11 cases / 52 assertions on v8h suite**:
  - **CALIBRATION FIRST** (advisor's pin): regular tet returns 6
    dihedrals all = arccos(1/3) ≈ 70.5288° within 1e-9; flat-tet
    returns near-zero (anchor for the dihedral formula).
  - 4 diagnostics (TooFewPoints / NonFiniteInput / Coplanar /
    InvalidAngle).
  - Regular-tet + centre converges in ≤ 1 iter at α=30°.
  - Cube + interior converges at α=10°.
  - **Sliver-rich input may-return-NotConverged** (4 near-coplanar +
    1 above; both Ok and NotConverged are valid; algorithm must not
    crash) — advisor's "3D termination NOT guaranteed" pin verified.
  - Determinism (same input → byte-identical output).
  - f32 precision tier + exposed `min_dihedral_of_tet_rad` helper.
- Combined delaunay suite: **112 cases / 1163 assertions** all green.
- **No mid-slice bugs in algorithm logic** — the calibration-first TDD
  pattern paid off. Only tidy fixes needed at slice-close:
  - `vl` confusable with `v1` (`misc-confusable-identifiers`) → renamed
    to `vm`.
  - Multi-decl `T xmin = ..., ymin = ..., zmin = ...;` → split.
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel`: PASS in 00:25.

### Next session starts with

**v8-close** — final cluster wrap. 11/11 algorithm slices complete
(v8a, v8b, v8c-pre, v8c, v8d-2d, v8d-3d, v8e, v8f, v8g, v8h, plus
v8-close itself). Cluster wrap = ADR-0076 §23 amendment locking
D73-D122 + `docs/systems/geometry-delaunay.md` system doc + 18-config
full sweep + roadmap/context/MEMORY final sync + cluster session log.
After v8-close, Phase 3.1.7 sub-module 9 of 11 closed (delaunay), and
the remaining sub-modules are v9 (`-mesh-processing` extensions GPU
LBVH + V-HACD + REPL + shader-helpers) and v10 (`-curves`) + v11
(transform-aware queries) before Phase 3.1.7 fully closes.
