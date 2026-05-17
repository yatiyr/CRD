## Session 2026-05-17 — Phase 3.1.7 v8c-pre `insphere_exact` Stage D paydown

### Goal

Close the long-standing debt entry `docs/debt.md::Shewchuk adaptive
predicates` for `insphere`. The previous body of `insphere_exact` in
`engine/geometry-primitives/src/predicates.cpp` was a Stage-A-equivalent
re-expression (f64 multiplication directly through 5 sub-determinants);
on truly cospherical adversarial input it returned f64 noise instead of
exact zero. This blocks Phase 3.1.7 v8c (3D Bowyer-Watson tetrahedrali-
sation) — silent-correctness debt risk per the debt entry's PRIMARY
TRIGGER #2.

### What we built / changed

- **Adversarial test corpus** added to `tests/geometry-primitives/test_predicates.cpp`
  Section (11) — 9 new Catch2 cases under `[stage-d]` tag covering:
  - 5 cospherical-zero cases at increasing radii² (14 / 10000 / 1e6 / 1e10
    / non-axis-aligned r²=1e6) — axis-aligned symmetric configs that
    Stage A happens to handle cleanly via pairwise cancellation.
  - **2 non-symmetric cospherical cases** (`[adversarial]` tag): r²=5e7
    and r²=5e9 with mixed-permutation coords. The r²=5e9 one is the
    discriminator — Stage A returns `-16777216` instead of zero.
  - 1 inside-vs-outside sign correctness test on r²=1e6.
  - 1 orientation-symmetry test (`forward == -swapped` after argument
    swap on first two points).
- **`insphere_exact` body rewritten** as literal port of Shewchuk's
  `insphereexact` (predicates.c v4.0.0 lines 3346-3601):
  - **Step 1**: 10 pairwise (x, y) 2D minors via existing `two_minor`
    (ab, bc, cd, de, ea, ac, bd, ce, da, eb — 4 elts each).
  - **Step 2**: 10 trio cofactor expansions via existing
    `triangle_cofactor(m1, s1, m2, s2, m3, s3)` (abc, bcd, cde, dea, eab,
    abd, bce, cda, deb, eac — 24 elts each).
  - **Step 3**: 5 quad 96-element expansions via local `build_quad` lambda
    (`a's quad = cde + bce - deb - bcd` pattern, cycled for b/c/d/e).
  - **Step 4**: 5 lifted 1152-element dets via local `build_lifted_det`
    lambda (double-scale each quad by point's x/y/z, sum the x/y/z parts).
  - **Step 5**: cascaded final 5760-element sum via three
    `linear_expansion_sum` calls.
  - Returns `deter[deterlen - 1]` (highest-magnitude term = exact sign).
- **Buffer split**: small intermediates (4/8/16/48/96/192-element arrays)
  stay on stack; the 5 large lifted-det buffers + final cascade buffers
  (384/768/1152/2304/3456/5760 elts) live in `thread_local static` storage.
  Total stack per call ~15 KB; per-thread TLS ~170 KB one-time cost.
  Trade-off chosen to avoid 175+ KB stack frames + zero allocator
  dependency from `predicates.cpp`.
- **`docs/debt.md`** entry marked CLOSED 2026-05-17 with a CLOSED-banner
  preceding the historical entry.

### Plain-English explanation

The "in-sphere" predicate answers: given a tetrahedron formed by 4 points
in 3D, does the 5th point lie inside, outside, or exactly on the
circumscribed sphere? It's a 5×5 determinant of points lifted to 4D by
adding `x²+y²+z²` as a 4th coordinate — Voronoi/Delaunay's defining
test.

f64 (double-precision) arithmetic loses ~half the digits per
multiplication. For our adversarial case (5 cospherical points on a
sphere of radius²=5×10⁹, integer coords ~50000), the intermediate
cofactor products are of order `lift × cofactor ≈ 5×10⁹ × 10^13 = 5×10²²`
— that's 23 decimal digits of intermediate, vs f64's ~16-digit precision.
Stage A truncates and produces noise. Even though the algebraic answer is
exactly zero, Stage A returns `-16777216` (= `-2²⁴`).

Shewchuk's solution: represent each intermediate as an **expansion** of
non-overlapping f64 components that together sum to the exact value.
`two_product(a, b)` returns the exact 2-component representation of a×b;
`linear_expansion_sum` adds two expansions exactly; `scale_expansion`
multiplies an expansion by a scalar exactly. Every arithmetic op preserves
the exact value. At the end the largest-magnitude component of the final
expansion gives the exact sign.

The cost scales: the 5×5 lifted-determinant expansion balloons to up to
5760 f64 components total. We allocate that as `thread_local static`
buffer (one per thread, ~170 KB) so calling `insphere_exact` doesn't
hammer the stack.

### Decisions made (D85-D89, pinned for ADR-0076 §23 amendment at v8-close)

- **D85.** **Port Shewchuk literally rather than re-derive.** Advisor
  flagged that re-deriving a 5-sub-det4 decomposition (my initial sketch)
  would be silent-correctness debt of its own. Shewchuk's published
  `insphereexact` decomposition (10 pairwise minors → 10 trios → 5 quads
  → 5 lifted dets → cascaded sum) is validated by CGAL, Triangle, and
  decades of production use. Used existing `triangle_cofactor` helper
  which exactly matches Shewchuk's per-trio pattern.
- **D86.** **`thread_local static` for large buffers**. The 5 lifted-det
  buffers (1152 elts) + final cascade (2304+2304+3456+5760 = ~14000 elts)
  would blow a 1 MB stack if local. `thread_local` is one-time per-thread
  TLS cost, no allocator dependency in `predicates.cpp`. Small buffers
  (≤192 elts) stay on stack for cache locality.
- **D87.** **Adversarial corpus FIRST, port SECOND.** Built the
  `[stage-d][adversarial]` test corpus and ran against the OLD code to
  confirm at least one case discriminated (r²=5e9 returned `-16777216`).
  Then ported. Then re-ran. Validates the test corpus actually tests what
  it claims.
- **D88.** **Axis-aligned symmetric tests are NOT discriminating** — the
  4 cofactor products cancel pairwise in f64 exactly because they're
  structurally equal. A discriminating test requires non-symmetric
  cospherical coords where the 4 cofactor products are algebraically
  cancelling but f64-distinct. Pinned via the r²=5e7/5e9 mixed-permutation
  configs.
- **D89.** **API surface unchanged**. `insphere_exact(Vec3<f64>, ..., Vec3<f64>) -> f64`
  signature identical; body-only replacement. Callers (`insphere_adapt`'s
  fallthrough at predicates.cpp:1133) untouched.

### Files touched

- `engine/geometry-primitives/src/predicates.cpp` — `insphere_exact` body
  replaced (~150 LOC removed, ~110 LOC added). Header comments rewritten
  to document the Shewchuk-port algorithm + thread_local buffer split.
- `tests/geometry-primitives/test_predicates.cpp` — Section (11) added,
  9 cases / 17 assertions under `[stage-d]` tag.
- `docs/debt.md` — Shewchuk entry marked CLOSED with banner.

### Tests / verification

- **9 new Stage D cases / 17 assertions — all pass on new code; the
  r²=5e9 case fails on old code** (returns `-16777216` instead of 0).
- **Full predicates suite regression check: 170 cases / 65630 assertions
  PASS** — no regression on the broader orient2d/orient3d/incircle/
  insphere corpus.
- 4-config DoD via `scripts/per-slice-check.ps1 -Parallel` (backgrounded).

### Memory recorded

No new memory entries. The vcvars/build/test patterns from
[[reference-build-test-workflow]] applied unchanged.

### Next session starts with

v8c — 3D Bowyer-Watson tetrahedralisation. With `insphere_exact` now full
Stage D, v8c can ship its tet-flip logic against truly cospherical input
without silent-correctness risk. New `delaunay_3d(ConstSpan<Vec3<T>>) -> DelaunayResult3<T>`
entry; 4-tuple-per-tet index buffer + neighbour table; super-tet at
1000× bbox; jump-walk via `orient3d` apex-side; cavity BFS via the now-
exact `insphere`; re-tetrahedralisation by fanning from inserted vertex.
