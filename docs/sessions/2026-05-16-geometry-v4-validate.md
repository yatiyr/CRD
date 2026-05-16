# Session log — 2026-05-16 — geometry v4-validate: formal mesh validation

> Renewed-scope addition to Phase 3.1.7 v4 `-mesh` cluster (ADR-0076 §15
> review 2026-05-13). Surfaces every structural defect that breaks
> downstream queries / cookers / FEA prep — so the cooker / editor / runtime
> can refuse-or-warn known-broken meshes.

## Scope landed

| Element | Path |
|---|---|
| Validation header | `engine/geometry-mesh/include/crd/geometry/mesh/mesh_validate.hpp` |
| Validation impl   | `engine/geometry-mesh/src/mesh_validate.cpp` |
| Umbrella header   | extended `mesh.hpp` |
| Tests             | `tests/geometry-mesh/test_mesh_validate.cpp` + CMakeLists |

## API surface

```cpp
enum class MeshDefectKind : u8 {
    OutOfBoundsIndex,        // triangle a refers to vertex b ≥ vertex_count
    DegenerateTriangle,      // triangle a has repeated vertex (i0==i1, etc.)
    ZeroAreaTriangle,        // triangle a's area < area_epsilon
    NonManifoldEdge,         // canonical edge (a, b) shared by ≥3 triangles
    BoundaryEdge,            // canonical edge (a, b) shared by 1 triangle
    InconsistentOrientation, // triangles a and b share an edge in SAME direction
};

struct MeshDefect { MeshDefectKind kind; u32 a; u32 b; };

struct MeshValidationOptions {
    Area32 area_epsilon{1.0e-12F};  // TYPED — boundary surface per ADR-0078 §5
    bool   check_edges          = true;
    bool   check_orientation    = true;
    bool   report_boundary_edges = true;
};

struct MeshValidationReport {
    Array<MeshDefect> defects;
    u32  triangle_count, vertex_count;
    u32  non_manifold_edge_count, boundary_edge_count, manifold_edge_count;
    bool well_formed;  // no critical defects
    bool watertight;   // well_formed + 0 boundary edges + triangle_count > 0
};

[[nodiscard]] MeshValidationReport
validate_triangle_mesh(const TriangleMeshViewf&, IAllocator*,
                       const MeshValidationOptions& opts = {});
```

`area_epsilon` is `Area32` (Length²) at the options surface, stripped to
raw `f32` inside the algorithm per the two-layer rule (ADR-0078 §5 D34).
Caller authors `Area32{1e-12F}` (= 1 µm² in SI); the algorithm body sees
`opts.area_epsilon.value`.

## Algorithm

Three passes, each deterministic.

### Pass 1 — Triangle-level (single linear scan)

For each triangle index 0 → triangle_count - 1:
1. **Out-of-bounds check** — emit `OutOfBoundsIndex` if any vertex index ≥
   `vertex_count`; skip the rest of this tri's checks (its vertex reads
   would be unsafe).
2. **Degenerate check** — emit `DegenerateTriangle` if any two of the
   three indices are equal; skip area check.
3. **Zero-area check** — compute `|edge1 × edge2|²` and compare against
   `4 × area_epsilon²` (skip the sqrt). Emit `ZeroAreaTriangle` on fail.
   Zero-area is an *authoring smell*, NOT a critical defect — it doesn't
   break surface topology, but downstream normalization may divide-by-zero.

### Pass 2 — Edge map build + sort

For each non-defective triangle, push 3 `EdgeRec{v_lo, v_hi, tri, dir}`
entries — canonical key `(min(va, vb), max(va, vb))`, with the original
winding direction preserved in `dir` (0 = `(lo→hi)`, 1 = `(hi→lo)`).
Then `std::sort` by canonical key.

### Pass 3 — Edge classification (single scan over sorted edges)

Walk the sorted edge list. A "run" of identical `(v_lo, v_hi)` is one
canonical undirected edge:

- **count == 1**: `BoundaryEdge`. Increment counter; emit defect iff
  `opts.report_boundary_edges`.
- **count == 2**: a manifold edge. If `opts.check_orientation` AND the
  two `dir` fields are equal → `InconsistentOrientation` (the two
  adjacent triangles traverse the edge in the same direction, breaking
  CCW-outward consistency).
- **count ≥ 3**: `NonManifoldEdge`. Increment counter; emit defect.

### Verdict booleans

- `well_formed`: no critical defects (`OutOfBoundsIndex`,
  `DegenerateTriangle`, `NonManifoldEdge`, `InconsistentOrientation`).
  Boundary edges + zero-area are NOT critical — they're informational.
- `watertight`: `well_formed && boundary_edge_count == 0 && triangle_count > 0`.

Empty mesh: `well_formed = true`, `watertight = false` (vacuous well-formed,
not a closed surface).

## Determinism

- **Pass 1 order**: ascending triangle index.
- **Pass 3 order**: ascending canonical edge key. Defects within a run
  emit at the run's first index.
- **Edge canonical key**: orientation-independent `(min, max)` pair —
  bit-exact across compilers / input windings.
- **Area test**: squared cross-product magnitude vs squared threshold,
  no `sqrt`. Bit-exact.

Cross-call reproducibility verified by the "deterministic across repeated
runs" test (asserts identical defect list across two consecutive validation
passes on the same mesh).

## Test corpus

10 cases / 46 assertions in `tests/geometry-mesh/test_mesh_validate.cpp`:

1. **Empty mesh** — well_formed=true, watertight=false (empty not closed),
   zero defects.
2. **Canonical CCW-outward unit cube** — 12 tris, 8 verts, 18 manifold
   edges (12·3/2), zero boundary, zero non-manifold, watertight=true.
3. **Out-of-bounds index** — `OutOfBoundsIndex` defect, well_formed=false.
4. **Degenerate triangle (i1==i2)** — `DegenerateTriangle` defect.
5. **Zero-area (collinear-x verts)** — `ZeroAreaTriangle` defect; well_formed
   remains true (authoring smell, not critical).
6. **Non-manifold edge** — 3 triangles share canonical edge (0, 1):
   `NonManifoldEdge` defect, well_formed=false.
7. **Open mesh (single triangle)** — 3 boundary edges flagged.
8. **Inconsistent orientation** — two adjacent tris traverse shared edge
   in same direction: `InconsistentOrientation` defect.
9. **`check_edges=false`** — short-circuits pass 2/3 so manifold /
   boundary / non-manifold counts stay zero; tri-level checks still run.
10. **Determinism** — same mesh validated twice yields bit-identical
    defect lists.

## Decisions locked

- **Sorted-edge classification, not hashmap.** O(E log E) deterministic
  sort vs `unordered_map<{lo, hi}, vector<tri>>` non-deterministic
  bucket order. The sort form is bit-exact across compilers; the hashmap
  form would need custom seeded hash + sorted-bucket iteration to match.
- **`Area32` at the options surface; raw `f32` inside the algorithm.**
  ADR-0078 §5 D34 — typed boundary, raw inner loop.
- **Zero-area triangles do NOT fail `well_formed`.** They're an authoring
  smell (caller may want to remove or warn about them), but they don't
  break topological consistency. CSG / SDF flood-fill / collision queries
  all handle zero-area triangles correctly.
- **Boundary edges emit per-edge defects by default.** `report_boundary_edges`
  toggle lets cookers / editors silence the noise on legitimately-open
  meshes (terrain heightfields, fan strips, mocap geometry).
- **Inconsistent orientation = critical defect.** Inconsistent winding
  breaks `cull_back`, breaks signed-distance computation, breaks normals.
  Better to refuse the mesh than ship a silent bug.
- **Empty mesh is well-formed, NOT watertight.** Empty surface isn't a
  closed surface (no triangle to be "inside of"). Watertight bool requires
  `triangle_count > 0`.

## 5-config DoD

| Config | Build | CTest |
|---|---|---|
| win-debug | clean | **1952/1952** (+10 from v4-validate) |
| win-asan | clean | 1952/1952 |
| win-shipping | clean | 1865/1865 |
| win-shipping-profile | clean | 1947/1947 |
| win-tidy | clean | — |

Full project ctest 1942 (v4d close) → **1952** after v4-validate.

## Phase 3.1.7 v4 cluster — ALL SLICES SHIPPED

| Slice | Surface | Tests | Status |
|---|---|---|---|
| v4a | `mesh_closest_point` (Ericson + BVH branch-bound) | 6 / 145 | ✅ |
| v4b | `mesh_raycast` (Woop watertight + Williams/Ize slab) | 8 / 16 | ✅ |
| v4c | `mesh_winding_number` (Jacobson + Van Oosterom-Strackee) | 8 / 281 | ✅ |
| v4d | `mesh_raycast_simd` (AVX2 Möller-Trumbore) | 7 / 15 | ✅ |
| v4-validate | `validate_triangle_mesh` (6 defect classes) | 10 / 46 | ✅ |

Net: 39 cases / 503 assertions across the v4 cluster. New
`engine/geometry-mesh/` module with one umbrella + 6 logical headers + 5
.cpp files. Typed wrapper layer per ADR-0078 §5 covers closest_point +
raycast + winding (validate stays raw — its consumers are
cooker / editor / debug tools, not real-time pipelines that need typed
APIs). All five slices 5-config DoD-green.

## Open follow-ups inside v4

- **v4-close**: ADR-0076 §17 amendment + `docs/systems/geometry-mesh.md` +
  18-config full sweep + ONE sandbox-viz session demonstrating all five
  v4 queries together on a picked mesh.

After v4-close: v5 `-spatial` cluster + ECS index unlock.

## References

- ADR-0076 §15 (renewed-scope review 2026-05-13) — the v4-validate slice
  addition.
- ADR-0078 §5 D32-D36 — two-layer typing; `area_epsilon: Area32` at the
  boundary.
- ADR-0076 §4 pin #11 — determinism rules (sorted-edge classification
  follows).
- Preceding session logs: v4a / v4b / v4c / v4d.
