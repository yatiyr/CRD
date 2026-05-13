# Session — 2026-05-13 — Phase 3.1.7 v1i-c debt payment: all three debts paid immediately

## Goal

The v1i-c close (`docs/sessions/2026-05-13-geometry-v1i-c-broadphase-pairs-and-validation.md`) deferred three items to `docs/debt.md`:

1. **`find_overlapping_pairs(DynamicBvh)` allocates work-stack `Array`s per call** (added v1i-c). Eylem v1c's broadphase will hit this every physics tick (60–1000 Hz); per-tick alloc churn on the hot path was real overhead.
2. **`Vec4f` inflate-and-slab kernel for `bvh4_shapecast_*`** (added v1i-b). v1i-b's scalar four-sequential-inflate path was correct but missed the SIMD-kernel reuse v1g already shipped for `bvh4_raycast`.
3. **AABB-equivalence shift-invariance test polish** (added v1i-c, advisor flag #3). The `BVH raycast at +1e6 origin` test asserted `payload == payload`, which would surface a future SAH-tiebreak refactor as a false test failure even though the geometry stayed equivalent.

User asked for all three paid immediately rather than carrying them into v1j / v2. This session closes them.

## What we built / changed

### Debt #3 (~15 lines) — AABB-equivalence shift-invariance assertion

`tests/geometry-bvh/test_validation.cpp::BVH raycast is shift-invariant at +1e6 origin` now asserts the named hit AABB at the far origin shifts back to match the named hit AABB at the origin within local ULP tolerance (`tc::ulp_tolerance_for(1e6) * 4 ≈ 0.95`), rather than comparing payload indices. Six min/max component checks + the existing `t` tolerance check. Decoupled from builder permutation: a future SAH-tiebreak refactor that legitimately swaps the chosen prim on a tied hit doesn't break this test as long as the chosen prim has the same geometry.

### Debt #1 — caller-owned scratch for `find_overlapping_pairs`

New public types in `crd/geometry/bvh/dynamic_bvh.hpp`:

```cpp
struct DynamicBvhPairWork { crd::u32 a; crd::u32 b; };  // promoted out of the local scope

struct DynamicBvhPairScratch {
    crd::containers::Array<crd::u32> walk;
    crd::containers::Array<DynamicBvhPairWork> cross;
    explicit DynamicBvhPairScratch(crd::memory::IAllocator* alloc) noexcept;
    void clear() noexcept;   // size() resets; capacity retained
    // move-only; not copyable
};
```

The existing `find_overlapping_pairs(Fn&&)` member template was refactored to a one-line forward to the new scratch-taking overload `find_overlapping_pairs(Fn&&, DynamicBvhPairScratch&)`, which `clear()`s the scratch then runs the same dual-descent walks using `scratch.walk` / `scratch.cross` in place of locally allocated `Array`s. The Array-append form gets a matching scratch-taking overload. Facade in `queries.hpp` mirrors all four shapes.

**Capacity behaviour:** the scratch's `walk` / `cross` Arrays keep their backing storage across `clear()` (Array's `clear()` resets size, not capacity); the second-and-later calls allocate only when the new high-water mark exceeds the previous one. After a few calls the scratch reaches a steady state and the broadphase hot path is alloc-free. Eylem v1c will wrap this directly.

New test case in `test_overlapping_pairs.cpp`: 150-leaf tree, 5 mutation iterations with `update(random leaf)` between, scratch-path and alloc-per-call-path produce bit-identical sorted pair sets after each iteration; final post-reuse run still matches the freshly-allocated reference.

### Debt #2 — `Vec4f ray_vs_4_aabb_inflated` SIMD kernel

New free function in `crd/geometry/bvh/bvh4_simd.hpp`:

```cpp
[[nodiscard]] Ray4AabbResult ray_vs_4_aabb_inflated(
    const Ray3<f32>&, const RayAABBPrecompute<f32>&,
    const Vec4f& bmin_x, ..., const Vec4f& bmax_z,
    f32 pad_x, f32 pad_y, f32 pad_z, f32 t0, f32 t1) noexcept;
```

Splats `pad_{x,y,z}` to `Vec4f`, inflates the input columns (`bmin − pad`, `bmax + pad`), forwards to the existing `ray_vs_4_aabb` slab kernel. `bvh4_inflated_raycast` (the shapecast traversal in `bvh_shapecast.cpp`) now transposes ≤4 child bounds into SoA `Vec4f` columns and does one `ray_vs_4_aabb_inflated` per node, mirroring v1g's `bvh4_raycast` pattern. Leaf-prim scalar inflate-and-slab is unchanged (per-leaf SIMD is `-mesh` v4g territory).

New test case `BVH4: ray_vs_4_aabb_inflated (Vec4f shapecast kernel) lane-by-lane vs scalar inflated slab` in `test_bvh4.cpp` — 2000 random ray × 4-box trials with varying pad (zero / small isotropic / larger anisotropic, since `bvh4_shapecast` hits all of those in practice). For each trial, the SIMD kernel's `hit_mask` and `t_enter` for every lane must bit-match four sequential scalar `intersect_ray_aabb_robust(inflate(box, pad), ...)` calls. **Bit-identical for finite/well-formed inputs** — the `min`/`max` chain is associative + commutative on finites and the inflation is the same arithmetic in both paths.

**Performance shape (not measured yet — micro-bench is future work):** scalar form does 4 × {6 subtractions + 6 multiplications + 5 min/max chain + 6 cmp + 4 robust-pad mul + boolean} per node; SIMD form does 1 × {6 lane-broadcast + 6 sub/add + 6 mul + 5 min/max chain (vector) + 1 cmp + 1 robust-pad mul (vector)}. The per-instruction comparison suggests a ~2-3× per-node ratio for the test itself; the per-call speedup on `bvh4_shapecast_*` depends on the leaf-scalar / interior-SIMD work mix (sparse trees with few interior nodes will see less benefit than dense ones) and is unmeasured. The `tests/bench/test_bench_bvh.cpp` v1f benchmark already exercises the kernel shape via `bvh4_raycast`; a dedicated shapecast benchmark is future work when a real consumer needs the number.

### Drive-by cleanup

`tests/geometry-bvh/test_validation.cpp` had two unused `using crd::geometry::bvh::Bvh4Tree;` / `bvh4_collapse;` declarations from when I drafted it. Clang-tidy `misc-unused-using-decls` flagged them; removed.

## Decisions made

- **Promote `CrossWork` out of the function scope.** The previous form defined `struct CrossWork` *inside* `find_overlapping_pairs`'s template body, which made it impossible to express its type from a public caller-owned scratch struct. The new `DynamicBvhPairWork` is a top-level public type. Same 2-`u32` POD shape; no behaviour change in the existing call site.
- **Scratch is move-only.** Sharing a scratch across threads is a footgun (the eylem broadphase will be fiber-friendly per ADR-0033 but each fiber should own its scratch); preventing copy construction at the type level surfaces the intent. Move-construct + move-assign explicit-defaulted so a vector of scratches works.
- **`scratch.clear()` happens inside `find_overlapping_pairs`, not the caller.** The caller's mental model is "call the function; the scratch is internal state I lend you." Putting the clear at the function entry hides the implementation detail. The capacity-retention property is documented in the scratch struct's comment.
- **`ray_vs_4_aabb_inflated` forwards to `ray_vs_4_aabb` rather than duplicating the slab chain.** One source of truth for the Tavianator / Ize-2013 logic. The cost is one extra `Vec4f` add per axis (the inflation) — bought back by the kernel call being inlined out at `-O2`/release.
- **Leaf-prim path stays scalar.** `bvh4_inflated_raycast` SIMD-transposes only the ≤4 *children* per node; leaf primitives still get one-at-a-time scalar `inflate` + `intersect_ray_aabb_robust`. Per-leaf SIMD (8 prims × `Vec8f` Möller-Trumbore-style) is the `-mesh` v4g pattern, NOT v1i-c debt. The leaf-prim scalar path was already correct in v1i-b; no need to touch it.
- **`pad` is passed as three `f32` scalars, not `Vec4f`.** The kernel splats inside, and the caller (e.g. `bvh4_inflated_raycast`) already has the pad as `Vec3<f32>`. Three scalars is the natural form at the call boundary; broadcasting is the kernel's concern.
- **No new benchmark this slice.** The 2-3× per-node throughput estimate is from the per-instruction comparison, not measured. A `tests/bench/test_bench_shapecast.cpp` would be the right home for a measured number, but adds out-of-scope work — `tests/bench/test_bench_bvh.cpp` already covers the `bvh4_raycast` shape this mirrors. Debt entry would be premature given v4g per-leaf SIMD will dominate the shapecast benchmark surface.

## Files touched

- Modified:
  - `engine/geometry-bvh/include/crd/geometry/bvh/dynamic_bvh.hpp` — `DynamicBvhPairWork` POD + `DynamicBvhPairScratch` (move-only, allocator-constructed, `clear()`); `find_overlapping_pairs` member template split into scratch-taking + alloc-per-call overloads.
  - `engine/geometry-bvh/src/dynamic_bvh.cpp` — `find_overlapping_pairs(Array<DynamicBvhPair>&, DynamicBvhPairScratch&)` impl.
  - `engine/geometry-bvh/include/crd/geometry/queries.hpp` — two new scratch-taking facade overloads.
  - `engine/geometry-bvh/include/crd/geometry/bvh/bvh4_simd.hpp` + `src/bvh4_simd.cpp` — new `ray_vs_4_aabb_inflated` kernel.
  - `engine/geometry-bvh/src/bvh_shapecast.cpp` — `bvh4_inflated_raycast` now uses the SIMD kernel for the per-node child test; includes `<crd/geometry/bvh/bvh4_simd.hpp>`.
  - `tests/geometry-bvh/test_overlapping_pairs.cpp` — added `find_overlapping_pairs: caller-owned scratch reuse matches alloc-per-call` (5-iter mutation loop).
  - `tests/geometry-bvh/test_bvh4.cpp` — added `BVH4: ray_vs_4_aabb_inflated (Vec4f shapecast kernel) lane-by-lane vs scalar inflated slab` (2000 trials, varying pad).
  - `tests/geometry-bvh/test_validation.cpp` — replaced payload-index comparison with AABB-equivalence ULP comparison; removed two unused `using` decls flagged by tidy.
  - `docs/debt.md` — three entries struck through with PAID status.

## Tests / verification

Per the in-flight `-bvh` directive (full 17-config sweep deferred to v1 cluster close after v1j):

- **win-debug**: full build ✅; ctest **1275/1275 PASS** (was 1273 after v1i-c initial — +2 cases from scratch-reuse + SIMD-inflated-kernel tests).
- **win-asan**: full build ✅; ctest **1275/1275 PASS** (~65 s). No use-after-free / heap-overflow flagged by the scratch lifecycle or the SIMD kernel.
- **win-shipping**: full build ✅ (full LTO, MSVC); ctest **1270/1270 PASS**.
- **win-tidy**: build ✅; the two `misc-unused-using-decls` warnings introduced by my draft test_validation.cpp are removed; pre-existing `bugprone-unchecked-optional-access` notes on Catch2-`REQUIRE.has_value()` patterns unchanged.
- CI guards: `crd-no-std-math-check` / `crd-no-std-sort-check` / `crd-no-non-ascii-test-names` / `crd-simd-emission-check` all green.

`crd-geometry-bvh-tests`: **82 cases / 76 581 assertions** (was 80 / 68 563 after v1i-c initial — +2 cases, +8018 assertions; the SIMD-inflated lane-by-lane test contributes ~8000 from its 2000 trials × 4 lanes).

## Next session starts with

- **Phase 3.1.7 v1j** — `crd-geometry-viz` companion module. Unchanged from prior plan; the debt payment doesn't reshape the surface. ~500 LOC + ~200 tests / ~3 days.
- After v1j: v1 cluster closes → **full 17-config `scripts/full-sweep.ps1`** → v2 (`-convex`: GJK + EPA + SAT + Quickhull, including GJK-cast).
- **No new debt** from this payment session.
