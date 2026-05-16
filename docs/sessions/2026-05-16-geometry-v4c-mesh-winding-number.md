# Session log — 2026-05-16 — geometry v4c: `mesh_winding_number`

> Third slice of Phase 3.1.7 v4 `-mesh` cluster. Generalised winding number
> for robust inside/outside on non-watertight meshes via Jacobson, Kavan,
> Sorkine-Hornung 2013, summing Van Oosterom-Strackee 1983 per-triangle
> solid angles.

## Why this query

`mesh_closest_point` (v4a) and `mesh_raycast` (v4b) answer "where is the
nearest surface point" and "where does this ray hit the surface" — both
**surface** queries. Neither answers the **volume** question:

> Is this point INSIDE the mesh?

Ray-cast parity (count crossings, odd → inside) is the classic answer
but it fails on T-junctions, edge cracks, self-intersections, and
non-watertight input — the kind of meshes that come out of scans,
authoring tools, glTF imports, mesh booleans, etc. Jacobson 2013's
**generalised winding number** is the canonical robust replacement:
it generalises smoothly across imperfect input, giving a continuous
real value that rounds to the topological inside/outside at 0.5.

## Scope landed

| Element | Path |
|---|---|
| Winding header | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_winding_number.hpp` |
| Winding impl   | `engine/geometry-mesh/src/mesh_winding_number.cpp` |
| Typed wrappers | extended `engine/geometry-mesh/include/crd/geometry/mesh/mesh_queries_typed.hpp` |
| Umbrella header | extended `engine/geometry-mesh/include/crd/geometry/mesh/mesh.hpp` |
| Tests | `tests/geometry-mesh/test_mesh_winding_number.cpp` + CMakeLists |

## API surface

```cpp
[[nodiscard]] crd::f32
mesh_winding_number(const TriangleMeshViewf&, const Vec3<f32>& query) noexcept;

[[nodiscard]] inline bool
mesh_is_inside(const TriangleMeshViewf&, const Vec3<f32>& query,
               f32 threshold = 0.5F) noexcept;
```

Typed twin in `mesh_queries_typed.hpp`:

```cpp
template <typename D, typename T>
crd::f32 mesh_winding_number(TriangleMeshViewT<D, T>, ConstSpan<Vec3<T>>,
                             Vec3<Quantity<D, T>> query) noexcept;
template <typename D, typename T>
bool mesh_is_inside(TriangleMeshViewT<D, T>, ConstSpan<Vec3<T>>,
                    Vec3<Quantity<D, T>> query, f32 threshold = 0.5F) noexcept;
```

Return is bare `f32` because the winding number is dimensionless
(rotations / 4π). Only the query point carries the dim tag.

## Algorithm

**Per-triangle solid angle** (Van Oosterom-Strackee 1983):

```
a = v0 - p, b = v1 - p, c = v2 - p
numerator   = a · (b × c)
denominator = |a||b||c| + (a·b)|c| + (b·c)|a| + (c·a)|b|
Ω = 2 · atan2(numerator, denominator)         (signed)
```

**Winding number**: `w(p) = (1/4π) · Σ_triangles Ω(p, tri)`.

For closed watertight manifolds → `w ∈ {0 (outside), 1 (inside)}` exactly.
For non-watertight meshes → continuous real that rounds to topological
inside/outside at 0.5 (Jacobson's key claim).

**Degenerate handling**:
- `p` coincident with a vertex → `|a|·|b|·|c| = 0` → that contribution
  short-circuited to `0.0F`. The vertex's local solid angle accumulates
  from the surrounding triangles.
- Zero-area triangle → numerator = 0, denominator > 0 → `atan2(0, +) = 0`.
  Contributes nothing; harmless.
- Empty mesh → `0.0F` (caller's `mesh_is_inside` returns `false`).

**Complexity**: O(N). The hierarchical treecode (Jacobson 2013 §4) for
O(log N) average queries requires per-BVH-node dipole moments + an
adaptive descent criterion — reserved as **v4c-fast** follow-on once
a consumer needs it (eylem volumetric inside-checks at scale; editor
"fill" tool over millions of triangles).

## Determinism notes

- **Summation order**: ascending triangle index, naive sum. Kahan
  compensation reserved for v4c-precision if a real test corpus
  surfaces drift at the 0.5 threshold (unlikely; threshold has
  comfortable margin).
- **`atan2` / `sqrt`**: not bit-exact across libm implementations.
  Documented in the header. The inside/outside answer is robust to
  1-2 ULP drift per contribution — the aggregate threshold check
  hits the {0, 1} attractors with wide margin.

## Test corpus

8 cases / **281 assertions**:

1. **Empty mesh** → `w = 0`, `is_inside = false`.
2. **Watertight cube — origin inside / far point outside** — `w_inside ≈ 1.0`
   within 1e-3, `w_outside ≈ 0.0`; `is_inside` agrees.
3. **125 interior queries on a watertight cube** — all return `w ≈ 1.0`
   within 1e-2 and `is_inside = true`.
4. **8 exterior queries on a watertight cube** — all return `w ≈ 0` and
   `is_inside = false`.
5. **Jacobson robustness — open cube (+X face removed) interior point**
   → still classified inside (`w ≈ 5/6 ≈ 0.833`, above the 0.5 threshold).
   The +X solid-angle contribution (≈ 4π/6 sr) is lost, but the inside
   answer survives.
6. **Open cube — far exterior points** → still `is_inside = false`.
7. **Translation invariance** — winding for `(mesh, query)` and
   `(mesh + Δ, query + Δ)` agrees within 1e-2 across a +100/-200/+50
   translation.
8. **Typed Quantity wrapper** — `Vec3<Length32>` inside/outside queries
   on a cube return the expected booleans.

## Decisions locked

- **O(N) direct algorithm at v4c-base.** The hierarchical Jacobson
  treecode is real follow-on work (per-BVH-node moments + descent
  criterion + correctness tests). Direct sum is the reference; the
  fast path consumes it as oracle in tests.
- **Threshold at 0.5 by default.** Matches Jacobson 2013. Caller
  overridable for advanced segmentation (e.g. "show me cells where
  `0.3 < w < 0.7`" → meshes with ambiguous topology).
- **Return is bare `f32` even on typed call sites.** Winding is
  dimensionless — wrapping it in a `Quantity<Dimensionless, T>`
  would be ceremonial; the type tells the call site nothing more
  than `f32` already does. Same call as `bool mesh_is_inside`.
- **No BVH dependence on the v4c-base path.** Direct O(N) walks
  `view.indices` directly. The hierarchical variant (v4c-fast) will
  use the v4a `TriangleMeshBvh` for per-node moments.

## 5-config DoD

| Config | Build | CTest |
|---|---|---|
| win-debug | clean | **1935/1935** (+8 from v4c) |
| win-asan | clean | 1935/1935 |
| win-shipping | clean | 1848/1848 |
| win-shipping-profile | clean | 1930/1930 |
| win-tidy | clean | — |

Full project ctest 1927 (v4b close) → **1935** after v4c.

## Open follow-ups inside v4

- **v4d per-leaf SIMD Möller-Trumbore** — `Vec8f` over 8 triangles per
  BVH leaf. Replaces v4b's per-tri Woop inner loop with a batched SoA
  variant. Requires leaf-storage repack inside `TriangleMeshBvh`
  (probably a v4d-only sidecar to avoid touching v4a/b layout).
- **v4-validate** — formal mesh validation pipeline stage
  (manifoldness, orientation, area-zero, vertex-duplication,
  edge-non-manifold).
- **v4c-fast (hierarchical treecode)** — Jacobson 2013 §4 BVH-accelerated
  evaluation. ~2× the algorithm-code of v4c-base + precompute pass over
  the BVH at build time. Defer until a real consumer surfaces.
- **v4-close** — ADR-0076 §17 amendment + system doc + 18-config full
  sweep + ONE sandbox viz session demonstrating all four queries
  (closest_point + raycast + winding + SIMD perf).

## References

- ADR-0076 §4 pin #11 — determinism rules (with caveat: atan2/sqrt
  libm drift accepted, threshold margin absorbs it).
- ADR-0078 §5 D27/D32-D36 — two-layer typing; strip-compute-retag.
- Alec Jacobson, Ladislav Kavan, Olga Sorkine-Hornung, "Robust
  inside-outside segmentation using generalized winding numbers"
  (ACM TOG / SIGGRAPH 2013). The paper.
- A. Van Oosterom, J. Strackee, "The Solid Angle of a Plane
  Triangle" (IEEE TBME 1983). Per-triangle solid angle formula.
- Preceding session logs: `2026-05-16-geometry-v4a-mesh-closest-point.md`,
  `2026-05-16-geometry-v4b-mesh-raycast.md`.
