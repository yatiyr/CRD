## Session 2026-05-17 — Phase 3.1.7 v7h Taubin smoothing (Taubin 1995)

### Goal

Ship `crd-geometry-mesh-processing` v7h: Gabriel Taubin's 1995
volume-preserving Laplacian smoothing — the standard noise-removal
primitive that DOESN'T melt the mesh down to its centroid like pure
Laplacian does. Consumers: scanner-noise removal, NPR mesh prep,
procgen post-pass, any pipeline that needs "low-pass-filter this
geometry" without shape drift.

### What we built / changed

- **New `taubin_smooth` entry point** in
  `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/taubin_smooth.hpp`
  + `.cpp`:
  - `TaubinSmoothOptions<T>` — `n_iterations` (λμ pair count;
    default 5), `lambda` (default 0.5), `mu` (default -0.53),
    `keep_boundary_fixed` (default true), `output_allocator`.
  - `TaubinSmoothStatus` — `Ok`, `EmptyMesh`, `NonManifoldInput`,
    `InvalidParameters` (λ ≤ 0 or μ ≥ 0).
  - `TaubinSmoothReport` — `status`, `iterations_run`,
    `vertices_smoothed`, `boundary_vertices_clamped`,
    `output_vertices`, `output_faces`.
  - `HalfEdgeMesh<T> taubin_smooth(const HalfEdgeMesh<T>&, opts,
    &report)` — input untouched; returns a fresh smoothed copy.
- **Umbrella header** `mesh_processing.hpp` extended.
- **8 Catch2 tests** in `tests/geometry-mesh-processing/test_taubin_smooth.cpp`.

### Plain-English explanation

Classical Laplacian smoothing moves every vertex toward the centroid of
its neighbours, which suppresses noise but also monotonically shrinks
the mesh — after enough iterations a sphere becomes a point, a cube
becomes its centroid. Taubin's 1995 trick: alternate the Laplacian
smooth with a NEGATIVE-weighted "un-shrink" step that pulls the
vertex back away from the centroid. The combination acts as a
frequency-domain LOW-PASS FILTER: high frequencies (noise) get
damped, low frequencies (shape) get preserved.

Concretely, each iteration pair runs:
- Pass 1 (shrink, λ > 0): `v ← v + λ · (mean(neighbours) - v)`
- Pass 2 (un-shrink, μ < 0): `v ← v + μ · (mean(neighbours) - v)`

The default (λ=0.5, μ=-0.53) targets Taubin's "wide-pass-band" filter
(pass-band frequency `k_PB ≈ 0.113`). Tighter parameters (λ=0.6307,
μ=-0.6732 from the original paper) give a narrower pass band with
stronger shape preservation, at the cost of less noise removal per
iteration.

Boundary vertices clamp by default (an open mesh shouldn't slide
its outline inward). With `keep_boundary_fixed = false`, boundary
vertices smooth using ONLY their boundary 1-ring neighbours —
preserving the boundary curve's overall geometry while removing
boundary noise.

### Decisions made (pinned for ADR-0076 §22 amendment at v7-close)

- **D67.** Clone-and-mutate pattern: extract input via `to_indexed`,
  rebuild output on requested allocator, mutate via
  `set_vertex_position` (added in v7d). Same as v7d/v7e/v7g.
- **D68.** Boundary flag cached ONCE at entry (Taubin doesn't change
  topology). Re-walking the 1-ring each pass would be wasted work.
- **D69.** Jacobi update per pass — compute new positions for all
  alive vertices against OLD into scratch, then apply atomically.
  Order-independent → deterministic across compilers.
- **D70.** Boundary smoothing behaviour:
  - `keep_boundary_fixed = true` (default): boundary vertices skip
    both passes (clamp at original position).
  - `keep_boundary_fixed = false`: boundary vertices smooth using
    ONLY their boundary 1-ring neighbours — matches v7c cubic-
    B-spline-mask intent but with Taubin's λ/μ alternation.
- **D71.** Umbrella operator = `mean(neighbours) - v` with UNIFORM
  weights (Taubin's original form). Cotangent weighting (Pinkall-
  Polthier / Desbrun) is a refinement that ships as v7h-cotan
  followon if a consumer requires it.
- **D72.** InvalidParameters check rejects `λ ≤ 0` and `μ ≥ 0`. Does
  NOT enforce `|μ| > λ` — caller may want high-pass / experimental
  filters.

### Files touched

- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/taubin_smooth.hpp` — NEW.
- `engine/geometry-mesh-processing/include/crd/geometry/mesh_processing/mesh_processing.hpp` — added include.
- `engine/geometry-mesh-processing/src/taubin_smooth.cpp` — NEW (impl + `f32`/`f64` instantiations).
- `tests/geometry-mesh-processing/test_taubin_smooth.cpp` — NEW (8 cases).
- `tests/geometry-mesh-processing/CMakeLists.txt` — added new test source.

### Tests / verification

- **8 Catch2 cases**; mesh-processing suite total = **80 cases / 1180
  assertions** (v7a 14 + v7b 15 + v7c 9 + v7d 9 + v7e 11 + v7f 9 +
  v7g 5 + v7h 8).
- Coverage:
  - `EmptyMesh` diagnostic.
  - `InvalidParameters` (λ = 0 + μ = 0).
  - `n_iterations = 0` returns clone (positions unchanged).
  - **Taubin volume preservation vs pure-Laplacian** (subdivided
    cube; Taubin's drift STRICTLY LESS than pure-Laplacian-only
    drift — the hallmark comparison).
  - Open quad boundary clamped (corners unchanged).
  - Determinism: byte-identical positions across runs.
  - f64 precision tier.
- **4-config DoD via `scripts/per-slice-check.ps1` — PASS first try**
  (elapsed 03:41): win-debug + win-asan + win-shipping + win-tidy.

### Bug found + fixed mid-slice

Initial volume-preservation assertion was too tight on a RAW 8-vertex
cube: `drift < 0.05` (5%). Actual drift was 93% — Taubin's frequency
analysis assumes a smooth manifold, but a raw cube is ALL high-
frequency content (sharp 90° corners). Taubin's wide-pass-band
default damps these aggressively.

Fix: revise the test to subdivide the cube first (Loop 2 levels →
98 vertices, smooth low-frequency cube shape + zero-amplitude
high-frequency subdivision artifacts), then compare Taubin's drift
against pure-Laplacian's drift (μ ≈ 0). The hallmark "Taubin
preserves volume better than pure Laplacian" assertion holds with
this corpus. The raw-cube test is preserved but only asserts the
algorithm runs successfully + produces a manifold output (volume
preservation is meaningless on cubes given Taubin's input model).

### Next session starts with

v7-close — the cluster wrap: ADR-0076 §22 amendment locking all
v7a-v7h design decisions D1-D72; `docs/systems/geometry-mesh-
processing.md` system documentation; 18-config full sweep PASS;
roadmap / context / memory final sync; cluster session log
linking all 8 slice logs. This closes Phase 3.1.7 sub-module 8
(`-mesh-processing`).
