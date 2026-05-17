## Session 2026-05-17 — Phase 3.1.7 v8 `-delaunay` cluster CLOSED

### Goal

Close the `crd-geometry-delaunay` sub-module (Phase 3.1.7 sub-module 9
of 11). 11 algorithm slices + 1 cluster-close, all shipped in a single
day. Wrap by amending ADR-0076 §23 with all 50 design decisions D73-D122,
writing the system overview, running the 18-config full sweep, and
syncing all docs.

### What we shipped (closure deliverables)

- **`docs/systems/geometry-delaunay.md`** — system overview (status,
  when-to-use-what matrix, architecture diagram, module dep graph, API
  stencil, determinism contract, robustness contract, performance pins,
  two-layer typed architecture, integration touch-points, open follow-
  ons, references). Same structure pattern as `geometry-mesh-processing.md`
  / `geometry-polygon.md` etc.
- **ADR-0076 §23 amendment** — slice ledger (v8a-v8h + v8c-pre +
  v8-close), all 50 design decisions D73-D122 indexed by slice, cluster
  cross-validation, Phase 3.1.7 status update (9 of 11 sub-modules
  COMPLETE, 70 of 49 renewed-scope slices = 143%), filed follow-on
  slices (v8g-perf, v8e-3d-clip, v8h-exude, v8c-hilbert,
  v6c-consume-v8a, v8*-typed).
- **18-config full sweep** via `scripts/full-sweep.ps1` — **17/18 PASS,
  1 BUILD-FAIL on `win-shipping-profile` due to MSVC C1001 internal
  compiler error at `engine/platform/src/window.cpp:181` during LTCG
  codegen of `crd-sandbox.exe`** (see "Infrastructure pothole" below).
  Fix shipped + re-verified → **final result 18/18 PASS, 2381 tests on
  shipping-profile, all sandbox smokes green.**
- **Docs synced** — `context.md` (Last shipped → v8-close milestone),
  `docs/ROADMAP.md` (9 of 11 sub-modules complete; 70 of 49 slices =
  143%), `docs/phases/phase-3.1.7-geometry.md` (v8-close ✅ row added),
  `MEMORY.md` + `memory/project_state.md`.

### Cluster summary

| Slice | Date | Engine LOC | Test LOC | Cases | Decisions |
|---|---|---|---|---|---|
| v8a 2D Bowyer-Watson (Bowyer 1981 / Watson 1981) | 2026-05-17 | ~600 | ~400 | 10 | D73-D80 |
| v8b 2D Hilbert-sort BW (delaunator-style) | 2026-05-17 | ~550 | ~350 | 12 | D81-D84 |
| v8c-pre `insphere_exact` Stage D (Shewchuk 1997) | 2026-05-17 | ~150 | ~150 | 9 | D85-D89 |
| v8c 3D Bowyer-Watson tetrahedralisation | 2026-05-17 | ~720 | ~340 | 12 | D90-D94 |
| v8d-2d 2D Voronoi extraction | 2026-05-17 | ~450 | ~370 | 11 | D95-D97 |
| v8d-3d 3D Voronoi cells + ConvexHullView helper | 2026-05-17 | ~600 | ~360 | 14 | D98-D101 |
| v8e Lloyd CVT 2D + 3D (Lloyd 1982) | 2026-05-17 | ~700 | ~440 | 22 | D102-D108 |
| v8f Sibson NNI 2D (Sibson 1981) | 2026-05-17 | ~430 | ~390 | 11 | D109-D113 |
| v8g Ruppert 2D refinement (Ruppert 1995) | 2026-05-17 | ~410 | ~350 | 10 | D114-D118 |
| v8h 3D dihedral-bounded refinement (3D-Ruppert) | 2026-05-17 | ~510 | ~340 | 11 | D119-D122 |
| v8-close | 2026-05-17 | — | — | — | — |
| **Total** | — | **~5120** | **~3490** | **112** | **50 (D73-D122)** |

**Test suite: 112 cases / 1163 assertions.** Cluster total ~8610 LOC
across 11 algorithm slices. Plus 2 new `crd-geometry-primitives` helpers
(`circumcenter_2d` added at v8d-2d, `circumcenter_3d` added at v8d-3d) —
reusable by v8g Ruppert + future tet meshers.

### Key milestones within the v8 cluster

1. **Stage D `insphere_exact` paydown at v8c-pre** — closes the long-
   standing `docs/debt.md::Shewchuk adaptive predicates` entry. Literal
   port of Shewchuk's `insphereexact` (predicates.c v4.0.0 lines 3346-
   3601): 10 pairwise → 10 trio → 5 quad → 5 lifted dets → cascaded
   5760-element final sum. Thread-local static buffers (~170 KB per
   thread). The discriminating r²=5e9 cospherical test where Stage A
   returns `-16777216` proved Stage D actually works.

2. **TDD-FIRST pattern paid off** across the cluster — adversarial
   discriminating tests written BEFORE implementation in every slice
   that has a defining mathematical property:
   - v8c-pre: r²=5e9 cospherical discriminator.
   - v8c: 9-pt cospherical-pathology mixture (validates v8c-pre).
   - v8d-2d / v8d-3d: cospherical-pathology validators carried
     forward.
   - v8f: linear-function reproduction at error < 1e-8 (Sibson's
     hallmark).
   - v8h: **CALIBRATION FIRST** — regular tetrahedron returns 6
     dihedrals all = arccos(1/3) within 1e-9. Anchored the dihedral
     formula before anything else depended on it. **v8h shipped with
     zero mid-slice algorithm bugs** — only tidy fixes at slice close.

3. **Project-wide tooling improvements landed mid-cluster**:
   - `scripts/per-slice-check.ps1 -Parallel` (added at v8b): runs all 4
     configs in `Start-Job` parallel, ~3× speedup (3:25 → 1:04 → as
     low as 00:25 in subsequent runs).
   - `.clang-tidy` `WarningsAsErrors: '*'` flip (v8d-2d slice): tidy
     warnings are now build failures. Cleanup agent fixed 129 source
     files + new `tests/.clang-tidy` exclusion policy.
   - CI clang-tidy version pinned to LLVM 20.1.8 (after two iterations
     to find the right action tag + version syntax — `KyleMayes/install-
     llvm-action@v2` resolved to an older v2.0.x without LLVM 20
     support; pinned `@v2.0.9` + `version: '20.1.8'` for determinism).

### Infrastructure pothole: MSVC C1001 LTCG ICE at window.cpp:181

When the 18-config full sweep ran, 17 configs were green and
`win-shipping-profile` failed at link with MSVC fatal error C1001
(internal compiler error) at `engine/platform/src/window.cpp(181)`
during the LTCG codegen pass for `crd-sandbox.exe`. Line 181 is the
innocuous `Window::Window() noexcept : m_impl(std::make_unique<Impl>())
{}` private ctor. `window.cpp` was last touched three commits ago
(`c84c8d5 platform implementation`), so this is not a v8 regression;
v8 perturbed the LTCG call graph enough that the existing MSVC
PGO/LTCG bug surfaced in this preset.

**Fix:** marked `Window::Window()` with `CRD_NOINLINE` in
`engine/platform/include/crd/platform/window.hpp`. Same precedent as
the `evict_block_locked` / `try_evict_to_budget` fixes in
`crd-resources` (see CLAUDE.md *Troubleshooting* → *win-release: Tests
fail due to MSVC LTCG interprocedural analysis bug*). One include
addition (`<crd/core/platform.hpp>` for the macro). Rebuild +
re-ctest of `win-shipping-profile` after the fix: **build clean, 2381
tests PASS in 29.96s, EXIT=0.**

Lesson: when full-sweep flags a single config BUILD-FAIL with an ICE
in untouched code, the right play is the `CRD_NOINLINE` precedent
rather than a clean rebuild — MSVC LTCG ICEs in `std::make_unique`
constructors are deterministic w.r.t. call graph, so they will not
"flake out" of existence.

### Scope-honest deferrals (filed as follow-on slices, not regressions)

- **v8g-perf** — incremental Bowyer-Watson + segment-protected cavity
  for Ruppert (currently full CDT rebuild per iter; ~500-800 LOC).
- **v8e-3d-clip** — 3D polyhedron-vs-bbox halfspace clipper for Lloyd
  3D ClipToBbox (currently returns `BboxClipNotSupported3D`; ~200-300
  LOC).
- **v8h-exude** — true sliver exudation (Cheng-Dey-Edelsbrunner-Facello-
  Teng 2000 weighted-Delaunay perturbation; ~1500+ LOC; substantially
  different algorithm from v8h's dihedral-bounded refinement).
- **v8c-hilbert** (latent) — 3D Hilbert-sort variant of `delaunay_3d`.
- **v6c-consume-v8a** — refactor v6c CDT to consume v8a internally
  (cleanup; v6c output unchanged).
- **v8*-typed** — `*_typed.hpp` wrappers per ADR-0078 §5 D34 at first
  typed consumer.

### Phase 3.1.7 progress

**9 of 11 sub-modules COMPLETE**: primitives ✅ + bvh ✅ + convex ✅ +
v3 convex-hull-extension ✅ + mesh ✅ + spatial ✅ + polygon ✅ +
mesh-processing ✅ + **delaunay ✅**. **70 of the renewed-scope 49
slices shipped (143%)** — v6 expanded 4→6, v7 expanded 7→9, v8 expanded
9→11 (substrate decomposition + v8c-pre paydown + close); nothing cut.
Next sub-modules: v9 GPU LBVH + V-HACD + REPL + v9e shader-helpers
emit → v10 `-curves` → v11 transform-aware query helpers in
`-primitives`.

### Next session starts with

**Phase 3.1.7 v9** — `-gpu` (GPU LBVH builder Karras 2012 + GPU BVH
refit) + `-decomposition` (V-HACD Mamou 2014 convex decomposition) +
REPL bindings + v9e GLSL/HLSL `signed_distance.hpp` shader-helper emit.
~8000 LOC engine + ~4000 LOC cooker-emitted GLSL/HLSL, ~3 weeks.

After v9 closes: v10 `-curves` (Bezier / Hermite / Catmull-Rom /
B-spline / arcs + sampling + arc-length + closest-point + Frenet + RMF)
→ v11 transform-aware query helpers in `-primitives`. Then Phase 3.1.7
fully closes and the engineering-platform pivot resumes with
`crd-hesap-dense` v0 (BLAS L1/L2/L3 + LAPACK direct) per the Strategic
Execution Plan, then eylem v1c resumes.
