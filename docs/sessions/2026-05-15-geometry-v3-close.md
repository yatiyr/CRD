# 2026-05-15 — Phase 3.1.7 v3-close

## What shipped

**v3-close** closes the v3 cluster (`-convex` hull construction +
simplification). All v3 slices now ✅ shipped: v3a Shewchuk adaptive
predicates + v3b 2D convex hull + v3c 3D Quickhull (a+b+c) + v3d hull
simplification + v3-close (this slice).

**New files:**
- `tests/geometry-convex/test_v3_close.cpp` (~360 LOC, 9 cases / 243 assertions)
- `tests/bench/test_bench_quickhull.cpp` (~170 LOC, 8 benchmarks)

**Files modified:**
- `engine/geometry-primitives/src/predicates.cpp` — `two_two_sum` marked `[[maybe_unused]]` to fix clang-cl `-Werror=unused-function`
- `tests/geometry-convex/CMakeLists.txt` + `tests/bench/CMakeLists.txt` (wire new files)
- `docs/decisions/0076-geometry-substrate-architecture.md` (§18.5 amendment)
- `docs/phases/phase-3.1.7-geometry.md` (v3-close row marked ✅)
- `context.md`
- Memory `project_state.md`

## Test corpus (9 cases / 243 assertions)

1. **Quickhull shuffled-input preserves hull vertex SET** — 5 perms × 80-point cloud. Same SET, possibly different `vertices` array indices (because input-index tiebreaks differ under shuffle).
2. **2D convex_hull shuffled-input preserves hull vertex SET** — same invariant for v3b.
3. **Quickhull large-coord 1e6 origin shift** — vertex count + face count match origin reference.
4. **Quickhull large-coord 1e7 origin shift** — same invariants at orbital scale.
5. **3D Quickhull coplanar fallback cross-check** — coplanar input on Z=0 produces flat hull whose vertex set matches v3b 2D monotone-chain on the dominant plane (exact equality of vertex sets).
6. **v3d threshold-respect** — tiny `max_error_threshold` (1e-9) prevents any removal; generous threshold (1.5) admits removals.
7. **v3d all-locked + huge threshold** — no removal (locked vertices excluded from candidate selection).
8. **v3d large-coord 1e6 simplification** — stays convex + locked vertices preserved.
9. **v3d shuffled-input simplification stays valid** — weak invariants (vertex count ≤ source, ≥ 4, face count ≥ 4); strict bit-equality not expected because greedy trajectory depends on hull vertex order.

Note: case 9 was originally written stronger (expected equal vertex
counts across shuffled inputs) but failed because greedy simplification
is sensitive to the starting hull's vertex array order. Relaxed to weak
invariants — this is correct: greedy algorithms aren't permutation-
deterministic at the simplified-vertex-count level. Strict bit-
identity of `simplify_hull` output holds only when the input
`QuickhullResult.vertices` array is itself bit-identical (covered by
v3d's standalone determinism test).

## Bench (`tests/bench/test_bench_quickhull.cpp`)

8 benchmarks (not in ctest; run via `crd-bench.exe "[!benchmark]"`):

- Quickhull build n=100 / n=1000 / n=10000 (f64)
- 2D monotone chain n=1000 / n=10000 (f64)
- simplify_hull 200-source → 8 vertices
- simplify_hull 500-source → 16 vertices

Targets per ADR-0076 §4.1: linear in N for monotone-chain after sort,
sub-millisecond Quickhull build for N ≤ 10k. Actual numbers captured
in bench output; reference baselines tracked in `docs/bench/` once a
perf-regression CI lane lands.

## Drive-by debts paid

### 1. `predicates.cpp::two_two_sum` unused on live paths

clang-cl `-Werror=unused-function` failed both `win-clang-cl` and
`win-clang-cl-shipping` because `two_two_sum` (a Shewchuk-primitive
adder helper) is defined but only `two_two_diff` is reached on the
live code paths.

This was latent v3a debt — MSVC was lenient about unused static
functions, clang-cl is strict. The v3a slice never ran the
`win-clang-cl` config (per-slice verification was win-debug + win-asan
+ win-shipping + win-tidy per `feedback_clang_tidy_after_every_slice.md`)
so it slipped through until v3-close ran the full sweep.

**Fix:** `[[maybe_unused]]` with a doc comment noting the function
stays as a Shewchuk-expansion helper for the future Stage D `insphere`
consumer (v8c-pre paydown per `docs/debt.md`).

### 2. 19 non-ASCII TEST_CASE names in v3b/v3c

`→` and `—` in TEST_CASE names mojibake'd through Windows ctest argv
(Active Code Page CP1254 on Turkish Windows → `ÔåÆ` and `ÔÇö`). The
`crd-no-non-ascii-test-names` guard was created in v1i-c to catch
exactly this — and the guard was wired correctly into ctest at that
time. **The gap was in v3b/v3c's per-slice verification protocol**:
those slices verified via the test binary directly (not via ctest), so
the guard never ran during v3b/v3c slice closure. v3-close ran the
full ctest, exposed the failures, and 19 TEST_CASE names were
mechanically replaced (`→` → `->`, `—` → `--`) across
`test_convex_hull_2d.cpp` + `test_quickhull.cpp` + `test_hull_simplify.cpp`.

The guard is now green across `tests/geometry-convex/`. **Lesson for
the per-slice protocol:** run ctest (not just the test binary) at
slice close — the guard tests like `crd-no-non-ascii-test-names`
register as ctest tests and don't appear in the test binary's case
list, so binary-direct verification can miss them.

## Verification — Full 17-config `scripts/full-sweep.ps1` PASS

| Lane | Configs | Result |
|---|---|---|
| Windows | win-debug + win-relwithdebinfo + win-release + win-asan + win-clang-cl + win-debug-scalar + win-debug-sse2 + win-shipping + win-clang-cl-shipping + win-tidy (10) | **10/10 PASS** |
| Linux | linux-gcc-debug + relwithdebinfo + release + asan + debug-scalar + debug-sse2 + shipping (7) | **7/7 PASS** |
| **Total** | | **17/17 PASS** |

Sweep elapsed: ~13:50 (Win 6:22 first attempt + 7:21 retry + Linux 6:28).

Convex suite final count: **207 cases / 21513 assertions** (was 198 /
21270 at v3d close, +9 cases / +243 assertions from `test_v3_close.cpp`).

## ADR-0076 §18.5 amendment

Locks the Q1–Q4 substrate decisions from the 2026-05-14 user-approved
recommendation set, validated in flight:

- **Q1 (`ConvexHullViewOwning<T>` type?)** — NO. `QuickhullResult<T>` is the owning form; `convex_hull_view_of(...)` builds non-owning view inline.
- **Q2 (Honest Quickhull LOC sizing?)** — YES, 1500 LOC budget; actual came in at ~1020 LOC across v3c-a + v3c-b + v3c-c (per-seam discipline + advisor design pass).
- **Q3 (Shewchuk adaptive predicates as substrate foundation?)** — YES; `crd-geometry-primitives::predicates.hpp` ships full Stage D `orient3d` + `incircle`; `insphere` Stage-D upgrade reserved for v8c-pre.
- **Q4 (`keep_vertex_indices` from day 1?)** — YES; v3d ships full multi-domain integration (eylem / CAD / FEA / robotics).

## Phase 3.1.7 progress snapshot

| Sub-module | Status |
|---|---|
| `-primitives` v0a–v0f + v1h | ✅ shipped |
| `-bvh` v1a–v1g | ✅ shipped |
| unified queries v1i-a/b/c | ✅ shipped |
| `-viz` v1j | ✅ shipped |
| `-convex` v2a–v2-close | ✅ shipped 2026-05-14 |
| `-convex` v3a–v3-close (hull construction + simplification) | ✅ shipped 2026-05-15 |
| `-mesh` v4 | ⏳ NEXT |
| `-spatial` v5 | ⏳ |
| `-polygon` v6 | ⏳ |
| `-mesh-processing` v7 | ⏳ |
| `-delaunay` v8 (with v8c-pre `insphere` Stage-D paydown) | ⏳ |
| `-gpu` v9 + V-HACD + REPL + v9e shader emit | ⏳ |
| Renewed-scope additions: v4-validate + v10 `-curves` + v11 transform-aware | ⏳ |

**Roughly 32 of the renewed-scope 49 slices shipped (~65%).**

## Next

**v4 `-mesh` cluster** — TriangleMeshView + half-edge topology + mesh
closest-point + Möller-Trumbore mesh raycast + Jacobson 2013 winding
number + v4g per-leaf SIMD Möller-Trumbore over 8 triangles + the
renewed-scope **v4-validate** formal mesh-validation pass
(duplicate-vertex / degenerate-triangle / non-manifold-edge /
inverted-normal / self-intersection / open-boundary / disconnected-
component — cooker + editor-import gate, runtime never re-validates).

Consumers waiting on v4: eylem v1d-mesh (mesh collider), `crd-sdf` v2
mesh-bake (winding-number sign test + BVH closest-point), `crd-renderer`
Phase 3.5+ mesh frustum cull, future `crd-cad-feature` 3.1.9 mesh-from-
brep export, future `crd-fea` 3.1.12 mesh validation as pre-flight gate.
