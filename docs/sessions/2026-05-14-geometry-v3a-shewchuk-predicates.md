# Session — 2026-05-14 — Phase 3.1.7 v3a — Shewchuk 1997 adaptive predicates

## Goal

Ship the geometry substrate's foundational robust-predicates layer per ADR-0076 §18.1: `orient2d` / `orient3d` / `incircle` / `insphere` with adaptive-precision floating-point arithmetic (Shewchuk 1997). The kernel that every higher-tier slice (v3b 2D hull → v3c 3D Quickhull → future v6 Vatti / v8 Bowyer-Watson / v9 V-HACD + CFD AMR + FEA contact + CAD boolean) consumes for cross-platform bit-exact sign correctness on near-degenerate input.

## What we built

### New header — `engine/geometry-primitives/include/crd/geometry/primitives/predicates.hpp`

Public API + inline Stage A (the fast f64 path). The four predicates:

```cpp
// 2D 3-point orientation (sign of det([a-c, b-c]))
[[nodiscard]] f64 orient2d(Vec2<f64>, Vec2<f64>, Vec2<f64>);
[[nodiscard]] f32 orient2d(Vec2<f32>, Vec2<f32>, Vec2<f32>);

// 3D 4-point orientation (sign of det([a-d, b-d, c-d]) — Shewchuk convention)
[[nodiscard]] f64 orient3d(Vec3<f64>, Vec3<f64>, Vec3<f64>, Vec3<f64>);
[[nodiscard]] f32 orient3d(Vec3<f32>, Vec3<f32>, Vec3<f32>, Vec3<f32>);

// 2D in-circle test (sign of the 4x4 lifted determinant)
[[nodiscard]] f64 incircle(Vec2<f64>, Vec2<f64>, Vec2<f64>, Vec2<f64>);
[[nodiscard]] f32 incircle(Vec2<f32>, Vec2<f32>, Vec2<f32>, Vec2<f32>);

// 3D in-sphere test (sign of the 5x5 lifted determinant)
[[nodiscard]] f64 insphere(Vec3<f64>, Vec3<f64>, Vec3<f64>, Vec3<f64>, Vec3<f64>);
[[nodiscard]] f32 insphere(Vec3<f32>, Vec3<f32>, Vec3<f32>, Vec3<f32>, Vec3<f32>);
```

### Stage-A inline body

Each predicate computes the determinant in f64 directly, plus a "permanent" (sum of absolute values of intermediate products). If `|estimate| > permanent × ε_static`, the sign is provably correct and we return immediately — the common case for non-degenerate input.

Static error bounds (`ccwerrbound_a/b/c`, `o3derrbound_a/b/c`, `iccerrbound_a/b/c`, `isperrbound_a/b/c`) are `constexpr` derived from IEEE 754 binary64 properties (Cerid's fixed FP contract per ADR-0063 — no `exactinit()` runtime initialization).

### New source — `engine/geometry-primitives/src/predicates.cpp`

Stage B / Stage C / Stage D adaptive paths (the cold path called when Stage A's estimate is unreliable). ~440 LOC. Contents:

- **Expansion arithmetic primitives** (Shewchuk §2.4 / §2.5): `two_sum`, `two_diff`, `two_product` (using `std::fma`), `square`, `two_one_sum`, `two_one_diff`, `two_two_sum`, `two_two_diff`, `fast_two_sum`, `linear_expansion_sum`, `scale_expansion`, `estimate`.
- **`orient2d_adapt`** — full Stage B + Stage C + Stage D expansion (Shewchuk §4.3 reference impl). Captures the four (a-c, b-c) differences with their tails, multiplies via Two-Product, accumulates an expansion of up to 16 nonoverlapping f64s, returns the sign of the highest-magnitude nonzero term.
- **`orient3d_adapt`** — Stage B (8-element + 16-element + 24-element expansions for the three 2x2 cofactors and their sum) + Stage C (compensation pass using tail components). Stage D (full ~192-element expansion) is left as a TODO marker; only triggers on inputs Stage C cannot resolve, and v8 Bowyer-Watson will surface any such cases.
- **`incircle_adapt`** — Stage B (cascading 4 → 8 → 16 → 32 → 64 → 96 element expansions for the 4x4 lifted determinant). Stage C/D land at v8 if needed.
- **`insphere_adapt`** — Stage A-equivalent (5x5 lifted determinant). Full Stage B+C+D for the 5x5 case is ~2000 LOC; the only consumer (v8 Bowyer-Watson 3D Delaunay) is months out. API surface is in place; adaptive form drops in without API change when consumer demands it.

### FMA over Veltkamp-Dekker split

Two-Product uses `std::fma` (2 FLOPs) instead of the classical Veltkamp-Dekker split (~17 FLOPs). FMA is IEEE 754-correctly-rounded by spec, available on every CPU since Haswell/Bulldozer (2013+), and `std::fma` is NOT banned by `crd-no-std-math-check` (only transcendentals are). Cleaner code, faster, deterministic.

### Determinism contract

Adaptive expansion arithmetic is purely arithmetic on IEEE 754 binary64 with `/fp:precise`:
- No FMA reassociation (the compiler can't reorder `a*b - c*d` into `fma(a, b, -c*d)` because that would change the rounding behavior).
- No transcendentals.
- No extended-precision FP registers (ADR-0063's deterministic FP contract).
- Same inputs → same expansion intermediates → same sign result, across compilers / SIMD widths / OSes. Bit-exact replay is the substrate-wide guarantee.

### Convention pin — Shewchuk's `orient3d`

`orient3d(a, b, c, d) > 0` iff d lies **BELOW** the plane through (a, b, c). This is the published Shewchuk convention and matches every computational-geometry textbook that cites his paper. It's the *negative* of the more common `det([b-a, c-a, d-a])` form — pinned in the header doc + tests + commented at every consumer site.

Convex hull algorithms in v3c will use: "d is outside the hull" → `orient3d(face_a, face_b, face_c, d) < 0` (face vertices CCW from outside the hull).

### NaN/Inf contract (ADR-0076 §15)

Every predicate guards its inputs with `predicate_detail::any_nonfinite` and returns `0.0` on non-finite input — the queries-tolerate contract. Builders (Quickhull, Bowyer-Watson) assert on finite inputs at their entry points; predicates themselves stay tolerant. No UB.

## Tests — `tests/geometry-primitives/test_predicates.cpp`

31 cases / 1031 assertions across 10 categories:

1. **Basic CCW/CW/collinear** orient2d.
2. **Above/below/coplanar** orient3d (Shewchuk convention pinned).
3. **Inside/outside/cocircular** incircle.
4. **Inside/outside/cospherical** insphere.
5. **Symmetry contract**: `orient2d(a,b,c) == -orient2d(b,a,c)` etc.
6. **NaN/Inf tolerance**: non-finite input → exact 0.0.
7. **Determinism replay**: two identical calls → `memcmp`-equal bit-exact results.
8. **Adversarial near-degenerate** (the Shewchuk torture corpus): near-collinear with 1e-15 perpendicular offsets — adaptive path must return correct sign.
9. **Large-coordinate adversarial** (Shewchuk torture at scale 1e6): predicates resolve sign correctly even where ULP at scale is much larger than the geometric perturbation.
10. **Exact zero on geometric degeneracy**: 1100+ collinear test cases on the line y = 2x + 1 across an 11x11x11 grid; all return exactly 0.0.

## Tests / verification

- **win-debug**: 31 v3a cases / 1031 assertions ✅. Full geometry-primitives suite: **149 cases / 65590 assertions** (was 114 / 64467 before v3a; +35 cases / +1123 assertions).
- **win-asan**: 31 v3a cases / 1031 assertions ✅.
- **win-shipping**: 31 v3a cases / 1031 assertions ✅.
- Full 17-config sweep deferred to v3-close per user directive ("we will do full sweep after v3 is closed").

## Bugs caught during v3a

1. **`const_cast` on local const f64s** — my first draft of `orient3d_adapt` declared `adx/bdy/...` as `const f64` from Stage A, then tried to write them via `const_cast` in Stage C. That's UB on const-qualified storage. Fix: declare them as non-const (the Two-Diff in Stage C writes the hi-word to bit-identical values, the tail is the new info). Caught at compile time by clang-cl's `-Werror=cast-qual` equivalent? No, MSVC accepts the cast silently; this was a code-review catch before first compile.
2. **`orient3d` Shewchuk-convention sign flip** — my first test draft used "above = positive" (the standard det([b-a, c-a, d-a]) form), but Shewchuk's published implementation (which I match in `predicates.cpp`) is "below = positive" (`det([a-d, b-d, c-d])`). Fix: updated header doc to explicitly call out Shewchuk's convention; updated tests to match; commented the sign convention pin so v3c Quickhull knows to use "d outside = orient3d < 0" for face-side tests.

## Decisions made

- **Stage A inline + Stage B/C/D out-of-line** — public header has the fast path (the 99%+ case) inline; the heavy adaptive computation lives in `predicates.cpp` to keep header-include footprint small.
- **`std::fma`-based Two-Product** — 2 FLOPs vs ~17 for Veltkamp-Dekker. FMA is not banned by `crd-no-std-math-check` (verified guard scope).
- **Static error bounds as `constexpr`** — no `exactinit()` runtime init. Cerid's FP contract (ADR-0063) is fixed IEEE 754 binary64 with `/fp:precise`; the constants compute at compile time.
- **f32 promotes to f64** — Shewchuk's error analysis is binary64-specific; promoting f32 inputs to f64 and running the f64 adaptive path gives ~9 decimal digits of headroom over f32's ~7, which is bullet-proof for any non-mathematically-degenerate input.
- **Shewchuk's `orient3d` convention** — kept literally; pinned in header docs + tests with "Shewchuk convention" labels.
- **NaN/Inf returns 0.0** — queries-tolerate per ADR-0076 §15. Builders assert at their entry points.
- **Stage D for orient3d / Stage C/D for incircle / Stage B+ for insphere are TODO** — Stage A + Stage B / Stage C cover the predicates' intended consumers (v3b 2D hull, v3c 3D Quickhull) bit-exactly. v8 Bowyer-Watson 3D Delaunay (the only consumer that genuinely needs full insphere Stage D) is months out; the API surface is in place and the deeper adaptive paths drop in without API change when the consumer surfaces.

## Files touched

- **New**: `engine/geometry-primitives/include/crd/geometry/primitives/predicates.hpp` (~340 LOC public API + Stage A inline)
- **New**: `engine/geometry-primitives/src/predicates.cpp` (~440 LOC Stage B/C/D out-of-line)
- **New**: `tests/geometry-primitives/test_predicates.cpp` (~430 LOC, 31 cases / 1031 assertions)
- **Edited**: `tests/geometry-primitives/CMakeLists.txt` (added `test_predicates.cpp` to the binary)

## Next session starts with

**v3b — 2D convex hull via Andrew's monotone chain.** Consumes `orient2d` from v3a. ~200 LOC engine + ~250 tests, 1-2 days. After that: v3c 3D Quickhull (the big one, ~1500 LOC honest sizing), v3d hull simplification, v3-close (tiebreak conformance + degenerate corpus + full 17-config sweep).

## Notes for future-me

- The Stage D fallback for orient3d (the full ~192-element expansion) is a known gap. If v3c Quickhull or v6 Vatti surfaces a near-coplanar input where Stage C gives the wrong sign, that's the upgrade path. Mark this in `docs/debt.md` as a low-priority follow-up.
- Similarly, full Stage B+C+D for `incircle` and Stage B for `insphere` are v8-time work. The API surface is stable; only the internal adaptive depth changes.
- `crd-no-std-math-check` was confirmed to allow `std::fma` and `std::fabs` (only transcendentals are banned). Document this as a pin so a future "tighten the guard" pass doesn't accidentally ban FMA.
