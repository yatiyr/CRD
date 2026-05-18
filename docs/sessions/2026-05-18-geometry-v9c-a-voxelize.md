# 2026-05-18 — Phase 3.1.7 v9c-a V-HACD voxelize ✅ SHIPPED

**Slice:** Phase 3.1.7 v9c-a — first algorithm slice of the v9c V-HACD cluster + first slice using the v9-prereq-test-harness GPU sanity discipline (CPU-mode subset).

**Status:** ✅ shipped same day. **5-config DoD PASS** (`scripts/per-slice-check.ps1 -IncludeRelease -Parallel`, elapsed 00:35) — win-debug + win-asan + win-shipping + **win-release** + win-tidy.

---

## What shipped

New module `engine/geometry-decomposition/` (target `crd-geometry-decomposition`, ns `crd::geometry::decomposition`). 10th `crd-geometry-*` sub-module. Opens the v9c `-decomposition` cluster. Future home for OBB-tree decomposition and other volumetric pre-process primitives.

### Public surface

- `voxel.hpp` — `VoxelState`, `VoxelGrid` (opaque dense storage), `VoxelizationOptions`, `VoxelizationResult<f32>`, `VoxelizationStatus`, `ClassificationMode`
- `voxelize.hpp` — `voxelize_mesh(TriangleMeshViewf, opts, alloc) -> VoxelizationResult<f32>`
- `decomposition.hpp` — umbrella

### Algorithm — two strictly non-overlapping passes (D126)

1. **Surface marking** — Akenine-Möller 2001 13-axis SAT, exact. Dispatched via `crd::jobs::parallel_for` over triangle batches; per-voxel `std::atomic_ref<u8>::fetch_or(Surface)` is idempotent + commutative ⇒ deterministic regardless of thread interleaving. Sequential fallback for tri_count < 256 (fan-out overhead).
2. **Classification** — only writes Outside/Inside into voxels still Unknown. `WindingNumber` (default — Jacobson 2013 via `crd::geometry::mesh::mesh_winding_number`; robust on non-watertight) OR `FloodFill` (6-conn BFS from corner Outside seed; fast, requires watertight).

Pass 2 reads only finalised pass-1 state; no concurrent writers in classification.

### Storage

`VoxelGrid` is dense `Array<u8>` + `std::atomic_ref<u8>` accessors. Plain `Array<std::atomic<u8>>` doesn't compile because `std::atomic` is non-movable. The atomic_ref pattern is C++20-idiomatic and gives the same race-free `fetch_or` semantics. API is **opaque** (no raw buffer accessor) — future bricked/sparse backend (CAM 1024³+ workloads) is non-breaking.

`VoxelState` is `u8` with 2 bits used; 6 reserved for future per-voxel payload (material id, normal, distance estimate).

### Sizing — precedence rule (D125')

Two knobs (`fixed_resolution`, `target_voxel_count`); **precedence**: `fixed_resolution > 0` wins, else `target_voxel_count > 0`. Both zero → InvalidOptions. Was originally "exactly one non-zero" but flipped during testing — that rule forced callers to zero the default `target_voxel_count` before setting `fixed_resolution`, which is user-hostile. Precedence is the elite API: clear mental model ("more specific wins") + forward-compatible with future `voxel_size: Length<T>` (top of the precedence ladder).

### Divergence from Mamou (D124)

Exact SAT surface marking, not Mamou's conservative centroid-classification. The substrate must serve CAD/SDF too, where conservative overlap matters. v9c-b decompose receives strictly-better voxelization than the paper assumes; cost-function tuning may need a one-pass calibration there.

### f32 only at v9c-a

`mesh_winding_number` is f32-only today, so v9c-a ships f32 only. f64 entry as follow-on slice when a real consumer asks.

---

## Adversarial test corpus (calibration-first per advisor TDD)

`test_voxelize.cpp` — 13 cases / 52 assertions.

1. **Diagnostics (5)**: EmptyMesh / NonFiniteInput / DegenerateAABB / InvalidOptions (both zero) + precedence verification (both set → fixed_resolution wins).
2. **CALIBRATION FIRST** — unit cube `fixed_resolution=4, padding_voxels=0` → exact 4³ grid; assert **56 Surface + 8 Inside + 0 Outside** (arithmetic-exact via SAT: outer shell of 4³ = 64 − 8 = 56 cells touch a face; inner 2³ = 8 Inside).
3. **Padded calibration** — cube `padding_voxels=1` → 6³ grid; assert corner is Outside + core 2³ all Inside + total = 216. (Doesn't pin exact Surface/Outside counts because padding cells ADJACENT to mesh AABB faces are also marked Surface via the shared edge, breaking the naive arithmetic.)
4. **Octahedron volume** — analytic 4/3·r³ vs measured `inside + 0.5·surface` voxel volume, within 10% at res=32.
5. **Open cylinder** — WindingNumber correct (interior > 100 voxels) vs FloodFill leaks (interior = 0; documented behavior). Pins the documented FloodFill limitation as a regression test.
6. **Determinism** — byte-identical voxel state across runs.
7. **Large-coord f32 stability** — 1e4 translation, same surface/inside counts.
8. **Perf budget** — `CRD_PERF_BUDGET_LE("voxelize_octahedron_res32_floodfill", 500.0, ...)` (generous; tightens at v9c-close once measured against a target).

---

## Mid-slice bugs fixed

### Bug 1 — sizing API was user-hostile

Original `validate_options` required "exactly one of fixed_resolution / target_voxel_count non-zero". Default `target_voxel_count = 64³` meant any test setting only `fixed_resolution` tripped "both non-zero → InvalidOptions". 10 / 13 tests failed.

**Fix**: flipped to **precedence rule** — `fixed_resolution > 0` wins, else `target_voxel_count > 0`, both-zero → InvalidOptions. Updated `validate_options`, `choose_resolution`, the InvalidOptions test, and added a precedence verification test (`{fixed=4, target=1M}` → grid is 4³, not 100³).

### Bug 2 — AABB-clamp early-out skipped boundary triangles

The unit-cube calibration got **37 Surface** instead of 56 — exactly the count if back/right/top faces (the three at coordinate = 1.0) were never tested.

Root cause: for a triangle at the upper grid boundary (e.g. y=1.0 on a unit cube with fixed_resolution=4, voxel_size=0.25), `to_iy(1.0) = floor(4.0) = 4 = ny`. The early-out check `raw_iy0 >= ny` fired, returning before any voxel was tested. The triangle TOUCHES voxel iy=3 (whose right edge is at y=1.0), so it should still be tested.

**Fix**: changed early-out from `>= ny` to `> ny` (strict greater), then `std::clamp` both endpoints to `[0, n-1]` for the loop bounds. After the fix: 56 Surface ✓.

### Bug 3 — Padded calibration math was naive

Original test expected `surface_count == 56, outside_count == 152` for the pad=1 case (assuming 56 mesh-induced Surface + 152 padding-shell Outside). Got 91 Outside. The issue: when the mesh AABB faces lie on the shared edge between an interior cell and a padding cell, SAT correctly marks BOTH as Surface. So the simple inclusion-exclusion math doesn't apply.

**Fix**: replaced exact-count assertions with structural invariants — corner Outside, core 2³ Inside, total cells = 216. The pad=0 calibration retains the exact 56/8/0 assertion since there's no padding ambiguity.

### Bug 4 — Non-ASCII test names

I used `→` and `—` in TEST_CASE names. The `crd-no-non-ascii-test-names` CI guard rejected them. Replaced with `->` and `--`. Same precedent as v8e Lloyd CVT slice ([[feedback_per_slice_run_ctest]]).

### Bug 5 — Tidy cleanup pass

Five tidy findings (all minor):
- `crd::u32 nx = 1, ny = 1, nz = 1;` → split per `readability-isolate-declaration`.
- `const T N` (single-uppercase local constant) → renamed to `n_along_longest` per `readability-identifier-naming`.
- Nested conditional `(ex == longest) ? ... : (ey == longest) ? ... : ...` → split to if/else per `readability-avoid-nested-conditional-operator`.
- Test: `using ::VoxelGrid` + `using ::VoxelizationResult` unused → removed (`misc-unused-using-decls`).
- Test: `crd::f32 x0 = ..., x1 = ...` multi-decl lines → split.

---

## Pattern locked

This is the first slice consuming the v9-prereq-test-harness discipline. The pattern works:
- **CPU-only slice** ⇒ no `ValidationCapture` (it needs `VkInstance`).
- **Throughput-tier non-determinism not at stake** ⇒ no `gpu_determinism_check` (the parallel SAT marking IS deterministic by atomic-or design — verified via a same-input → identical-state test).
- **`CRD_PERF_BUDGET_LE`** ✅ used for the perf test.
- **`-IncludeRelease` 5-config DoD** ✅ used for substrate-introduction slice per `feedback_v9_gpu_sanity_harness.md`.

For v9c-b decompose (CPU, more involved algorithm), same pattern + tighter perf budget.

---

## Files touched

- `engine/geometry-decomposition/` (NEW module)
  - `CMakeLists.txt`
  - `include/crd/geometry/decomposition/decomposition.hpp` (umbrella)
  - `include/crd/geometry/decomposition/voxel.hpp` (types)
  - `include/crd/geometry/decomposition/voxelize.hpp` (public entry)
  - `src/voxel.cpp` (VoxelGrid ctor + move impl)
  - `src/voxelize.cpp` (driver + classification)
  - `src/voxelize_internal.hpp` (Akenine-Möller SAT)
- `tests/geometry-decomposition/` (NEW test dir)
  - `CMakeLists.txt`
  - `test_voxelize.cpp` (13 cases / 52 assertions)
- `CMakeLists.txt` (root) — `add_subdirectory(engine/geometry-decomposition)`
- `tests/CMakeLists.txt` — `add_subdirectory(geometry-decomposition)`
- `context.md` (Last shipped milestone)
- `docs/ROADMAP.md` (Phase 3.1.7 bullet)
- `docs/phases/phase-3.1.7-geometry.md` (v9c-a row updated)
- `docs/systems/geometry-decomposition.md` (NEW system doc)

## Pinned for ADR-0076 §24 amendment at v9c-close

D123 (storage), D124 (SAT-not-centroid), D125 (dual classification, WindingNumber default), D125' (precedence sizing), D126 (two non-overlapping passes), D127 (1-voxel padding default), D128 (parallel surface marking, fetch_or union).

## Next

🎯 **v9c-b V-HACD decompose** (Mamou §3.2-3.4) — recursive plane-search convex decomposition; cost = α·∂(R) + β·∂(R̄); per-cluster hull via v3c Quickhull. ~6 days budget, ~1400 LOC engine + ~600 LOC tests. Consumes v9c-a's `VoxelGrid` as input.
