## Session 2026-05-17 — Phase 3.1.7 v7 `-mesh-processing` cluster CLOSED

### Goal

Close the `crd-geometry-mesh-processing` sub-module (Phase 3.1.7
sub-module 8 of 11). 8 algorithm slices + 1 substrate + 1 close.
Wrap by amending ADR-0076 §22 with all 72 design decisions, writing
the system overview, running the 18-config full sweep, and syncing
all docs.

### What we shipped (closure deliverables)

- **`docs/systems/geometry-mesh-processing.md`** — system overview
  (status, when-to-use-what matrix, architecture diagram, API stencil,
  determinism contract, robustness contract, performance pins, two-
  layer typed architecture, integration touch-points, open follow-ons,
  references). Same structure pattern as `geometry-polygon.md` etc.
- **ADR-0076 §22 amendment** — slice ledger (v7a-v7h + close), all 72
  design decisions D1-D72 indexed by slice, cluster cross-validation,
  Phase 3.1.7 status update (8 of 11 sub-modules COMPLETE, 59 of 49
  renewed-scope slices = 120%).
- **18-config full sweep** via `scripts/full-sweep.ps1` — PASS on
  retry (elapsed 15:17): 11 Windows configs (debug + relwithdebinfo
  + release + asan + clang-cl + debug-scalar + debug-sse2 + shipping
  + shipping-profile + clang-cl-shipping + tidy) + 7 Linux configs
  (gcc-debug + gcc-relwithdebinfo + gcc-release + gcc-asan +
  gcc-debug-scalar + gcc-debug-sse2 + gcc-shipping). 0 failures.
- **Docs synced** — `context.md`, `docs/ROADMAP.md`,
  `docs/phases/phase-3.1.7-geometry.md` (v7-close ✅), `MEMORY.md`
  + `memory/project_state.md`.

### Cluster summary

| Slice | Date | Engine LOC | Test LOC | Cases | Decisions |
|---|---|---|---|---|---|
| v7a HalfEdgeMesh substrate | 2026-05-16 | ~520 | ~480 | 14 | D1-D8 |
| v7b QEM (Garland-Heckbert 1997) | 2026-05-17 | ~600 | ~400 | 15 | D9-D16 |
| v7c Loop (1987) | 2026-05-17 | ~330 | ~250 | 9 | D17-D25 |
| v7d Isotropic remesh (B-K 2004) | 2026-05-17 | ~620 | ~350 | 9 | D26-D38 |
| v7e Liepa hole-fill §3+§4+§5 (2003) | 2026-05-17 | ~900 | ~500 | 11 | D39-D51 |
| v7f Manifoldness repair | 2026-05-17 | ~440 | ~300 | 9 | D52-D59 |
| v7g Self-intersection removal | 2026-05-17 | ~580 | ~280 | 5 | D60-D66 |
| v7h Taubin (1995) | 2026-05-17 | ~210 | ~270 | 8 | D67-D72 |
| v7-close | 2026-05-17 | — | — | — | — |
| **Total** | — | **~4200** | **~2830** | **80** | **72** |

**Test suite: 80 cases / 1180 assertions.** Cluster total ~7030 LOC
across 8 algorithm slices + 1 substrate.

### Sweep history

Two sweep attempts. The wider corpus (vs the per-slice 4-config DoD)
surfaced two classes of issue the per-slice DoDs didn't catch:

**Sweep 1 (first attempt)** failed with:
1. `win-relwithdebinfo` — MSVC `C1001` LTCG ICE on `<sstream>` (compile
   `test_half_edge_mesh.cpp`). Transient — `feedback_transient_msvc_
   ltcg_ice_accept.md` policy class.
2. `win-clang-cl` — `-Werror,-Wunused-but-set-variable` on `orig_v`
   in `repair_manifoldness.cpp:208`. Real warning; v7f set the
   variable but never used it after I removed pairing logic that
   referenced it.
3. `linux-gcc-*` (all 7 configs) — three classes of error:
   a. `-Wfloat-conversion` on `T{1e-10}` / `T{-0.53}` /
      `T{1.41421356}` default initializers in 3 hpp option structs.
   b. `-Wcomment` on `loop_subdivide.hpp:36` ASCII-art line ending in
      `\` (multi-line C++ comment continuation).
   c. The `-Wconversion`-as-error path doesn't fire on per-slice
      DoD (win-debug + win-asan + win-shipping + win-tidy) — those
      use MSVC which is more lenient on narrowing double→float
      defaults.

**Sweep 2 (post-fix #1)** caught additional issues:
4. `linux-gcc-*` — `T{1e-20}` narrowing in 5 sites across 4 `.cpp`
   files (caught only after the hpp fixes above unblocked compilation
   far enough to reach them). Plus `kPi` / `kTwoPi` constexpr
   transcendentals.
5. `win-tidy` — clang-tidy access violation on `test_taubin_smooth.cpp`
   (transient per `feedback_transient_clang_tidy_crash.md` policy).

**Sweep 3 (post-fix #2)**: PASS 18/18 elapsed 15:17.

### Bugs fixed mid-close (real, not transient)

1. **`repair_manifoldness.cpp:208`** — removed unused `orig_v`
   (clang-cl `-Wunused-but-set-variable -Werror`).
2. **`qem_decimate.hpp:92`** — `T singular_det_epsilon = T{1e-10}`
   → `static_cast<T>(1e-10)` (gcc `-Wfloat-conversion`).
3. **`taubin_smooth.hpp:93`** — `T mu = T{-0.53}` →
   `static_cast<T>(-0.53)`.
4. **`fill_holes.hpp:115`** — `T refine_alpha = T{1.41421356}` →
   `static_cast<T>(1.41421356)`.
5. **`quadric.hpp:136`** — `T det_epsilon = T{1e-10}` →
   `static_cast<T>(1e-10)` (preemptive).
6. **`remove_self_intersections.hpp:118`** — `T dedup_epsilon =
   T{1e-6}` → `static_cast<T>(1e-6)` (preemptive).
7. **`loop_subdivide.hpp:36`** — ASCII-art diagram with trailing
   `\` (multi-line comment) → replaced with text description.
8. **`fill_holes.cpp:109,370`** — `T{1e-20}` (×2) →
   `static_cast<T>(1e-20)`.
9. **`isotropic_remesh.cpp:307`** — `T{1e-20}` →
   `static_cast<T>(1e-20)`.
10. **`qem_decimate.cpp:113,162`** — `T{1e-20}` (×2) →
    `static_cast<T>(1e-20)`.
11. **`fill_holes.cpp:378`** — `kPi = T{3.14159265358979323846}` →
    `static_cast<T>(3.14159265358979323846)`.
12. **`loop_subdivide.cpp:156`** — `kTwoPi = T{6.28318530717958647692}`
    → `static_cast<T>(6.28318530717958647692)`.

The core lesson: `T{double_literal}` triggers gcc
`-Wfloat-conversion -Werror` for any literal not exactly representable
in `T` when `T = f32`. The 4-config per-slice DoD (MSVC-only) doesn't
catch this — gcc-as-error in the linux sweep does. **Use
`static_cast<T>(literal)` for non-exact-representable defaults +
constexpr in this module from now on.**

### Phase 3.1.7 progress

**8 of 11 sub-modules COMPLETE**: primitives ✅ + bvh ✅ + convex ✅
+ v3 convex-hull-extension ✅ + mesh ✅ + spatial ✅ + polygon ✅ +
**mesh-processing ✅**. **59 of the renewed-scope 49 slices shipped
(120%)** — v6 expanded 4→6 (substrate separation), v7 expanded 7→9
(substrate + close), nothing cut.

### Next session starts with

**Phase 3.1.7 v8 `-delaunay`** — 2D Bowyer-Watson + Voronoi-from-
Delaunay + 3D Bowyer-Watson + insphere_exact Stage D paydown at
v8c-pre (silent-correctness debt risk if shipped without). Plan in
`docs/phases/phase-3.1.7-geometry.md` slice table.

First step: re-read the phase doc's v8 row, surface any scope deltas
upfront (per the v7e scope-check lesson in `feedback_scope.md` sub-
rule), then dive into v8a 2D Bowyer-Watson.
