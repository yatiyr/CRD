# 2026-05-18 — Phase 3.1.7 v9c `-decomposition` cluster CLOSED

**Slice:** Phase 3.1.7 v9c-close. Final wrap of the v9c V-HACD cluster.

**Status:** ✅ shipped. **18-config full sweep PASS** (`scripts/full-sweep.ps1`). ADR-0076 §24 amendment locked (D123-D131). eylem v1c convex-collider-conditioning **stub integration smoke** PASS. Phase 3.1.7 sub-module 10 of 11 COMPLETE.

---

## What landed

### 1. ADR-0076 §24 amendment

Replaced the §24 planned placeholder with the locked cluster-close amendment:

- **Slice ledger** (v9c-a + v9c-b + v9c-close, ~1330 LOC engine + ~820 LOC tests, 9 design decisions).
- **D123-D131 locked** with rationale at the source-of-truth slice level (per-slice session logs are canonical; §24 indexes them).
- **Cross-validation** — calibration-first TDD worked across both algorithm slices (v9c-a unit-cube 56/8/0 arithmetic-exact + v9c-b cube → 1 part concavity-floor).
- **Phase 3.1.7 status** — 10 of 11 sub-modules COMPLETE.
- **10 follow-on slices filed** (5 v9c-a, 5 v9c-b) — all explicit-scope deferrals, none regressions.

Notable Mamou-divergences locked + documented:
- **D124**: exact SAT, NOT centroid-classification (substrate must serve CAD/SDF too).
- **D129**: voxel-fraction concavity, NOT Hausdorff (modern V-HACD universal; V-HACD authors themselves moved off Hausdorff).

### 2. eylem v1c stub integration smoke

New test case `v9c-close: eylem v1c convex-collider conditioning stub` under tag `[eylem-stub]` in `tests/geometry-decomposition/test_vhacd.cpp`. Exercises the full pipeline as eylem v1c (paused) will call it once it resumes:

```
triangle mesh (L-shape)
   → voxelize_mesh                          (v9c-a)
   → vhacd_decompose                        (v9c-b)
   → per-part convex_hull_view_of           (existing crd-geometry-convex helper)
   → contains(view, centroid)               (eylem GJK-style minimal op)
```

Per-part assertions: ≥ 4 vertices, ≥ 4 faces, NOT coplanar/colinear/coincident, centroid is contained. 15 assertions across the parts produced by the L-shape decomposition. Test runs as part of the geometry-decomposition test binary, NOT in `tests/eylem/` (because eylem v1c doesn't exist yet) — this is the canary that catches API drift before eylem v1c lands. Per `feedback_per_slice_run_ctest.md` per-sub-module eylem-stub integration smoke practice.

### 3. 18-config full sweep

`scripts/full-sweep.ps1` — **18/18 PASS in 24:46**, all 11 Windows + 7 Linux configs green, 2366 tests pass. Required for cluster close per `feedback_full_sweep_required.md`.

```
win-debug                  PASS
win-relwithdebinfo         PASS
win-release                PASS
win-asan                   PASS
win-clang-cl               PASS
win-debug-scalar           PASS
win-debug-sse2             PASS
win-shipping               PASS (build+ctest+sandbox)
win-shipping-profile       PASS (build+ctest+sandbox)
win-clang-cl-shipping      PASS (build+ctest+sandbox)
win-tidy                   PASS (build)
linux-gcc-debug            PASS
linux-gcc-relwithdebinfo   PASS
linux-gcc-release          PASS
linux-gcc-asan             PASS
linux-gcc-debug-scalar     PASS
linux-gcc-debug-sse2       PASS
linux-gcc-shipping         PASS
```

### 4. Docs synced

- `context.md` — Last shipped milestone (cluster CLOSED).
- `docs/ROADMAP.md` — Phase 3.1.7 bullet updated to 10/11 sub-modules.
- `docs/phases/phase-3.1.7-geometry.md` — v9c-close row + cluster-close marker.
- `docs/systems/geometry-decomposition.md` — status moved to v9c-b ✅ + v9c-close ✅; cluster-closed callout added.

## Phase 3.1.7 progress

**10 of 11 sub-modules COMPLETE.** Renewed scope is 49 slices; shipped 73 (149%).

```
✅ -primitives     (v0)            ~3000 LOC
✅ -bvh            (v1)            ~2500 LOC
✅ -convex         (v2 + v3 ext)   ~4500 LOC
✅ -mesh           (v4)            ~1800 LOC
✅ -spatial        (v5)            ~4400 LOC
✅ -polygon        (v6)            ~4350 LOC
✅ -mesh-processing (v7)           ~4200 LOC
✅ -delaunay       (v8)            ~5120 LOC
✅ -decomposition  (v9c)           ~1330 LOC   ← THIS CLUSTER
🚧 -gpu            (v9a, v9b)      planned     ← NEXT, consumes Phase 3.1.7.6
🚧 -shader-helpers (v9e)           planned     ← consumes Phase 3.1.7.6
📋 -curves         (v10)           planned
📋 -primitives transform (v11)     planned
```

## Next

🎯 **v9a-a GPU Morton-code generation** (Karras 2012 LBVH step 1). Consumes Phase 3.1.7.6 `crd-rhi-compute` substrate + v9-prereq-test-harness sanity discipline (ValidationCapture + ulp_compare + gpu_determinism_check + CRD_PERF_BUDGET_LE). First GPU slice; harness fully exercised from day 1. ~3 days, ~400 LOC engine + ~300 LOC tests.

## Files touched

- `docs/decisions/0076-geometry-substrate-architecture.md` — §24 amendment locked.
- `docs/systems/geometry-decomposition.md` — cluster-closed status.
- `docs/phases/phase-3.1.7-geometry.md` — v9c-close row.
- `docs/ROADMAP.md` — Phase 3.1.7 bullet.
- `context.md` — Last shipped milestone.
- `docs/sessions/2026-05-18-geometry-v9c-close.md` — this file.
- `tests/geometry-decomposition/test_vhacd.cpp` — eylem v1c stub smoke added (1 case / 15 assertions).
- `MEMORY.md` — index updated if anything memory-worthy surfaced.

(no engine code touched — close-slice is docs + sweep + smoke only.)
