# Session — 2026-05-13 — Phase 3.1.7 v1i-c: `find_overlapping_pairs(DynamicBvh)` (Catto-style dual-descent) + degenerate-corpus / large-coordinate validation helpers + **`crd-no-non-ascii-test-names` guard correctness fix**

## Goal

Third and closing sub-slice of the v1i split (ADR-0076 §15). v1i-a parked
the unified `queries.hpp` facade and the templated result types; v1i-b
added shapecast. v1i-c lands:

- the **broadphase self-overlap primitive** — `DynamicBvh::find_overlapping_pairs(OutFn)` (Catto GDC 2019 dual-descent, all `(i<j)` fat-AABB-overlapping leaf pairs in one traversal — the all-pairs primitive eylem v1c's broadphase wraps directly), plus the `Array<DynamicBvhPair>` append form and the facade overload;
- the **validation discipline** — a shared header `tests/geometry-primitives/include/test_corpus.hpp` with degenerate-geometry generators (zero-volume AABBs, collinear / zero-area triangles, NaN / ±∞ inputs) and a large-coordinate sweep helper (+1e6 / +1e7 origin shift + per-magnitude ULP-tolerance), and `test_validation.cpp` in **both** test binaries applying the corpora across the existing query suite;
- the **`crd-no-non-ascii-test-names` guard correctness fix** — the third consecutive slice hit by Windows ctest non-ASCII argv mojibake (v0f em-dashes, v1i-b `²` + em-dash, v1i-c `×`) made the recurrence pattern unmistakable; investigation found **two compounding bugs** in `scripts/check_no_non_ascii_test_names.ps1` that had silently turned the guard into a no-op since v0e (~3 days ago).

This closes the v1i cluster (a + b + c).

## What we built / changed

### `engine/geometry-bvh/include/crd/geometry/bvh/dynamic_bvh.hpp` + impl

New POD `DynamicBvhPair { u32 a; u32 b; }` with `a < b` invariant +
`operator==` + `operator<` (so test-side `std::sort` agrees).

New member template `DynamicBvh::find_overlapping_pairs(Fn&& on_pair)` —
Catto-style dual-descent over the live tree:

1. **Outer walk**: DFS over interior nodes; at each node, call the
   cross-walk on `(child1, child2)`, then recurse into each child.
2. **Cross-walk**: at each work pair `(a, b)`, skip if AABBs don't
   overlap; if both leaves, emit `on_pair(min, max)`; if one leaf and one
   interior, expand 1→2 children; if both interior, expand 1→4 children.
3. Both walks use `crd::containers::Array<...>` work stacks allocated
   from the **tree's own allocator** (no `default_allocator()` per
   `feedback_named_allocators_in_tests.md`). Stack peak scales `O(depth)`
   for the outer + `O(4·depth)` for the cross — Array form is unbounded by
   construction, so a pathological dense corpus can't blow a fixed bound.

The non-template Array form `void find_overlapping_pairs(Array<DynamicBvhPair>&)` lives in `dynamic_bvh.cpp` and is one line of forwarding to the template with a `push_back` lambda.

### `engine/geometry-bvh/include/crd/geometry/queries.hpp` — facade

Two new free functions in `crd::geometry`:

```cpp
template <typename Fn> void find_overlapping_pairs(const bvh::DynamicBvh&, Fn&&);
                       void find_overlapping_pairs(const bvh::DynamicBvh&, Array<bvh::DynamicBvhPair>&);
```

Static-tree variants (`BvhTree` / `Bvh4Tree`) are NOT in v1i-c — those trees
don't store `user_data` on leaves, and the broadphase self-overlap use case
is inherently dynamic; if a static-tree all-pairs ever surfaces, a thin
wrapper at the call site can do it via `bvh_overlap` per-prim, or a
dedicated dual-descent can be added without breaking the API. Documented
in the facade comment.

### `tests/geometry-primitives/include/test_corpus.hpp` (new, shared)

Header-only — no link dependency — included from **both**
`crd-geometry-primitives-tests` and `crd-geometry-bvh-tests` (the latter
adds the include dir via `target_include_directories(... PRIVATE
${CMAKE_SOURCE_DIR}/tests/geometry-primitives/include)`).

Surface, in `crd::geometry::test_corpus`:

- `degenerate_aabbs()` — point AABB (`min == max`), the empty sentinel
  (`min = +∞, max = -∞` — the v1f `aabb_empty()` non-finite identity,
  EXEMPT from the §15 `all_finite` contract), NaN min, NaN max,
  half-infinite boxes (`min.x = -∞` / `max.x = +∞`).
- `degenerate_triangles()` — coincident vertices (zero-area, zero-edge),
  collinear non-coincident, "needle" (two coincident), NaN vertex, ∞ vertex.
- `degenerate_spheres()` — zero radius, negative radius, NaN center, NaN
  radius, ∞ radius, ∞ center.
- `degenerate_rays()` — zero direction, NaN origin / direction, ∞ origin
  / direction.
- Large-coordinate helpers: `shift(...)` for each primitive +
  `k_far_origin_modest = 1e6F`, `k_far_origin_stress = 1e7F` constants +
  `ulp_tolerance_for(magnitude)` that returns the conservative ULP at
  that magnitude (`m / 2^22` for `m > 1`, `1e-6` floor). Callers use this
  as the comparison slack for shifted-scene queries — not `1e-6`, because
  at +1e7 ULP is `O(1.0)`.

### `tests/geometry-bvh/test_overlapping_pairs.cpp` (new)

7 cases / +1378 assertions:

1. **Empty tree** emits no pairs.
2. **Single leaf** emits no pairs.
3. **Hand-built 3-box scene** — A and B overlap, C isolated → exactly one
   pair `(10, 20)` emitted.
4. **Matches brute force on random corpora** — 5 trials, varying world
   size (10..50, so pair density varies from dense to sparse) and leaf
   count (30..250); brute-force O(n²) reference over the fat AABBs (the
   BVH self-overlap is over fat AABBs — the inflated AABB that `insert`
   stores); pair sets compared as sorted vectors. Pin: every emitted pair
   has `a < b`.
5. **Callback form == Array form** — same tree, same pair sequence
   bit-identical.
6. **Deterministic across re-invocations** — two calls on the same tree
   state produce bit-identical pair sequences.
7. **n=10000 dense-corpus soak** (advisor recommendation — eylem-v1c
   blocker check) — 10k leaves in a small (size 20) world so overlaps
   are dense; cross-stack `Array<CrossWork>` grows up to 4× per pop; pin
   that the TLSF allocator (64 MB headroom) doesn't OOM and the brute
   force count matches. Catches a pathological cross_stack growth before
   eylem v1c's broadphase hits it.
8. **Tracks insert / remove / update** — the broadphase keeps step with
   tree mutations: insert (3 leaves, 1 pair) → update one away (0 pairs)
   → update another into overlap (1 pair, different IDs) → remove the
   anchor (0 pairs).

### `tests/geometry-primitives/test_validation.cpp` (new)

11 cases / +60 assertions exercising the **NaN/Inf contract** (ADR-0076
§16 pin #3 — "queries tolerate, builders reject") at the primitive layer:
every primitive query function (`intersect_ray_aabb` /
`intersect_ray_sphere` / `intersect_ray_triangle` / `intersects(AABB,
AABB)` / `distance_squared(...)` / SDFs) called over the degenerate
corpus must complete without UB. Plus the large-coordinate sweep:
ray-vs-AABB and ray-vs-sphere at +1e6 origin agree with the same query at
origin within the local ULP tolerance; AABB-vs-AABB intersects is
shift-invariant at +1e7. Also pins `is_finite` correctly accepts the
finite-but-extreme corpus (point AABB, zero-radius sphere) and rejects
the non-finite one.

### `tests/geometry-bvh/test_validation.cpp` (new)

7 cases / +25 assertions at the BVH-query layer: raycast / overlap /
closest_point / shapecast tolerate degenerate ray / AABB / point inputs;
`DynamicBvh::find_overlapping_pairs` tolerates an empty tree. Plus
shift-invariance for the BVH queries — raycast at +1e6 returns the same
prim payload within local ULP; overlap at +1e7 returns the same prim set
(sorted comparison). Builders REJECT non-finite *prim* inputs (the §15
contract — the asserts are pinned in `bvh_build` / `dynamic_bvh.insert` /
`bvh4_collapse` from v1h); we don't try to build a BVH from a NaN
corpus — that's a different test surface.

### **`scripts/check_no_non_ascii_test_names.ps1` — guard correctness fix**

The recurrence pattern — v0f em-dashes, v1i-b `²` + em-dash, v1i-c `×` —
made the diagnosis unavoidable: the guard had been a silent no-op since
it was added in v0e (~3 days ago). Two compounding bugs:

1. **Broken `$RepoRoot` default** — the param block was
   ```powershell
   param([string] $RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path)
   ```
   PowerShell evaluates a param's default expression in a scope where
   `$PSScriptRoot` is **empty**, not the script's own `$PSScriptRoot`.
   So `Resolve-Path "/.."` evaluated to `D:\` (the working drive root).
   The script then computed `$testsDir = "D:\tests"`, which doesn't
   exist, and exited PASS via the "no tests directory yet" early return.
   The guard never scanned any test file.
2. **Wrong regex** — the pattern was `[^ -]`. PowerShell's .NET regex
   parses the `-` between space and the class-close `]` as a *range*
   operator, not a literal. So `[^ -]` is equivalent to `[^\x20-\x2D]`
   — "not in space-to-dash" — which matches every letter, digit, and
   Unicode codepoint. If the script had ever scanned a test file, this
   regex would have flagged every TEST_CASE line, including ASCII-clean
   ones — drowning the real signal in noise. (The first bug masked
   this second one for the entire silent-no-op period.)

**Fix** (`scripts/check_no_non_ascii_test_names.ps1`):
- Initialize `$RepoRoot` in the script body via an `if
  ([string]::IsNullOrEmpty($RepoRoot))` check, AFTER `$PSScriptRoot` is
  in scope. The CMake-test invocation passes no args, so the body
  defaults take over.
- Replace the regex with a **per-character codepoint loop**:
  ```powershell
  foreach ($ch in $line.ToCharArray()) {
      if ([int]$ch -gt 0x7F) { $hasNonAscii = $true; break }
  }
  ```
  Can't be misparsed by regex-class quirks. Each comment block in the
  script names the bug, the date, and the slices it bit, so the next
  reader sees the recurrence story inline.

**Verification of the fix**: seeded `tests/_corruption_test/test_x.cpp`
with a TEST_CASE name containing `×`, ran the guard standalone, got
`FAIL: 1 TEST_CASE name(s)`. Removed the seed; re-ran on the (cleaned)
real tree, got `PASS - all TEST_CASE names are ASCII-only`. Guard does
what its name says, finally.

The Linux `.sh` mirror used `LC_ALL=C grep -rEn ... $'[\x80-\xff]'` —
correct byte-range expansion via bash ANSI-C quoting. Linux side was
fine; only the Windows side was broken.

## Plain-English explanation

This slice closes the v1i unified-query story. **Broadphase pair-finding**
is the operation that says, "given a scene full of moving boxes, give me
every pair whose bounding boxes are touching — efficiently." The naive
form is N×N (every box against every other box). Catto's dual-descent
walks the spatial tree once and finds them all in `O(n + |pairs|)` —
~100× faster than naive at n=10000 in a dense scene, and what eylem's
collision broadphase will use directly.

**Validation discipline** is what catches the bugs we don't have specific
tests for. A `raycast` against a NaN ray, a `closest_point` query at
coordinate +1e7 — these are the cases a user (or another sub-module)
might genuinely throw at the substrate. The slice adds a shared corpus
header that every later test can pull in, and applies it to the existing
query surface to confirm nothing UBs and the answers stay correct at
large coordinates. Future sub-modules (`-convex`, `-mesh`, `-spatial`)
extend the same corpus rather than reinventing it.

The **guard fix** is the one that mattered most this slice. The same bug
class kept biting (Windows ctest can't run a test whose name contains
non-ASCII bytes), and each slice "fixed" the one offending test name and
moved on. v1i-c was the third time. Investigation found the supposed
guardrail had been silently doing nothing since v0e — broken `param`
default scope + broken regex class. Fixed both, verified with a seeded
non-ASCII test case that the guard now flags it correctly, and the
recurrence is closed.

## Decisions made

- **`find_overlapping_pairs` is a `DynamicBvh` member function**, not a
  free function. Mirrors `query` / `raycast` / `closest_point` (also
  members, also need private `Node` array access). The facade
  `crd::geometry::find_overlapping_pairs(tree, ...)` is the uniform call
  surface.
- **Cross-stack uses `Array<CrossWork>`, not a fixed-size stack.** The
  outer walk is `O(depth)` but the cross-walk's worst case is hard to
  bound a priori. The Array form (allocated from the tree's allocator,
  one allocation amortised across the call) trades a small constant cost
  for safety on pathological dense corpora. Validated by the n=10000
  soak test.
- **No static-tree variants (`BvhTree` / `Bvh4Tree`) for find_pairs.**
  Static trees don't store `user_data` on leaves — the broadphase use
  case is inherently dynamic. If a static-tree all-pairs operation ever
  surfaces, it's a different shape (probably `find_overlapping_pairs(tree, prims, OutFn)` returning prim-index pairs) and can be added without
  breaking the API. Documented in the facade.
- **Corpus header lives in `tests/geometry-primitives/include/`** as a
  shared resource. Both test binaries add it as a private include dir
  in their CMakeLists. Header-only; no link surface. Each sub-module's
  next slice (`-convex` v2, `-mesh` v4, etc.) extends the corpus rather
  than reinventing it.
- **Validation tests are scoped to "doesn't crash, doesn't UB, agrees
  within local ULP"**, not exhaustive correctness. Exhaustive
  property-based testing of every primitive query function under every
  degenerate input is a different (much larger) effort; this slice adds
  the *discipline* and the corpus to build on.
- **Guard scope kept ASCII-only.** The corrected regex / loop scans for
  any codepoint `> 0x7F`. Justified non-ASCII (`crd-lint-allow-non-ascii-test-name` marker on the same line) is the explicit opt-out per the
  original v0e design. No such opt-outs exist today.
- **Linux .sh guard not touched** — the bash form (`LC_ALL=C grep
  $'[\x80-\xff]'`) is correct. Only the Windows .ps1 was broken.
- **Advisor flag #3 (shift-invariance assertion checks payload index
  instead of AABB equivalence)** deferred to debt as polish. The current
  test correctly catches what it's meant to catch (shift-invariance of
  the chosen hit); a future builder-tiebreak refactor that legitimately
  swaps the chosen prim on a tied hit would surface as a fail here, but
  that's information not noise.

## Files touched

- New: `engine/geometry-bvh/include/crd/geometry/bvh/dynamic_bvh.hpp`
  (`DynamicBvhPair` POD + `find_overlapping_pairs` member template +
  declarations),
  `engine/geometry-bvh/include/crd/geometry/queries.hpp`
  (facade overloads),
  `tests/geometry-primitives/include/test_corpus.hpp`
  (shared validation helpers),
  `tests/geometry-primitives/test_validation.cpp`,
  `tests/geometry-bvh/test_validation.cpp`,
  `tests/geometry-bvh/test_overlapping_pairs.cpp`, this session log.
- Modified: `engine/geometry-bvh/src/dynamic_bvh.cpp` (Array form impl),
  `engine/geometry-bvh/include/crd/geometry/bvh/dynamic_bvh.hpp`
  (`DynamicBvhPair` + declarations),
  `scripts/check_no_non_ascii_test_names.ps1` (both bug fixes + inline
  postmortem comments),
  `tests/geometry-bvh/CMakeLists.txt` (new test files + include dir),
  `tests/geometry-primitives/CMakeLists.txt` (new test file + include
  dir), docs (session log + phase doc + context + system doc).

## Tests / verification

Per the in-flight `-bvh` directive (full 17-config sweep deferred to v1
cluster close — which is **now**, after v1j; v1i-c is the second-to-last
v1 slice):

- **win-debug**: full build ✅; ctest **1273/1273 PASS** (was 1247 after
  v1i-b — +26 new TEST_CASEs across test_overlapping_pairs.cpp + the two
  test_validation.cpp files, registered under `[geometry][pairs]` and
  `[geometry][validation]`).
- **win-asan**: full build ✅; ctest **1273/1273 PASS** (~65 s — ASan
  overhead). The n=10000 soak runs clean — no use-after-free / heap
  overflow on the larger work stacks.
- **win-shipping**: full build ✅ (full LTO, MSVC); ctest **1268/1268
  PASS** (debug-only tests correctly gated). The +1e7 large-coordinate
  test stresses the slab kernel's precision; passes the local-ULP
  comparison cleanly.
- **win-tidy**: build ✅ (clang-tidy gate); zero new warnings on the
  v1i-c files. The pre-existing `bugprone-unchecked-optional-access`
  notes on `test_queries.cpp` lines 452/453 (from v1i-a's
  `DynamicBvh::closest_point` empty/inside/cutoff test) are unchanged
  — same Catch2-`REQUIRE(opt.has_value())` pattern that tidy can't see
  through.
- **CI guards**: `crd-no-std-math-check` + `crd-no-std-sort-check` +
  `crd-simd-emission-check` green; `crd-no-non-ascii-test-names` **now
  actually verifies** that all 187+ TEST_CASE lines in `tests/**/*.cpp`
  are ASCII-only (instead of silently passing — see the postmortem above).
  Seeded a test file with `×` in a TEST_CASE name and confirmed the
  fixed guard flags it; removed the seed; the real tree passes clean.

`crd-geometry-bvh-tests`: **80 cases / 68 563 assertions** (was 65 /
67 500 after v1i-b — well, 68 383 after the v1i-b strengthening).
`crd-geometry-primitives-tests`: **114 cases / 64 467 assertions** (was
103 / 64 407 after v1h — +11 cases from test_validation.cpp).

Full 17-config `scripts/full-sweep.ps1` — still deferred to v1 cluster
close (after v1j). Per the user, CI is surprisingly green at this point
so the sweep amortises once at the end.

## Next session starts with

- **Phase 3.1.7 v1j** — `crd-geometry-viz` companion module (NEW
  module — debug-only; depends `crd-geometry-*` + `crd-draw`;
  `crd-geometry` itself never depends on `crd-draw`, mirroring
  `crd-eylem` / `crd-eylem-viz`). Pure functions emitting
  `crd::draw::RenderBuffer` primitives: `draw_aabb` / `draw_obb` /
  `draw_sphere` / `draw_capsule` / `draw_frustum` / `draw_triangle` /
  `draw_ray`; `draw_ray_hit` (point + normal arrow + the t-segment);
  `draw_closest_point` (segment query → closest); `draw_normals`
  (per-face/per-vertex normal hairs); `draw_bvh(BvhTree | Bvh4Tree |
  DynamicBvh, depth_limit)` (node AABBs colour-keyed by depth);
  `draw_frustum_cull(Frustum, BvhTree)`; `draw_overlap_pairs(DynamicBvh)`
  (lines between overlapping leaf centroids — direct visualisation of
  what v1i-c just shipped). ~500 LOC + ~200 tests / ~3 days.
- **After v1j**: v1 cluster closes (v1a → v1j). **Then full 17-config
  `scripts/full-sweep.ps1`** — the deferred sweep that's been amortising
  through v1c..v1j. Then on to **v2** (`-convex`: GJK + EPA + SAT +
  Quickhull, including GJK-cast = the exact-corners answer that
  v1i-b's conservative `cast_sphere(Sphere, AABB)` defers to).
- **Pinned debt:**
  1. `Vec4f` inflate-and-slab kernel for `bvh4_shapecast_*` (v1i-b
     follow-up, ~40 LOC — mirror of v1g pattern).
  2. Advisor #3 polish on `test_validation.cpp:test_BVH_raycast_at_1e6`
     — assert AABB equivalence not prim-index for shift-invariance,
     so a future SAH-tiebreak refactor doesn't surface as a false test
     failure here.
