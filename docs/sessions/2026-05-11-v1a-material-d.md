# Session — 2026-05-11 — v1a-material-d — mass derivation (CLOSES v1a-material cluster)

## Goal

Per ADR-0069 §3 + §8 + §11, ship the canonical mass / centre-of-mass /
inertia derivation that produces a body's physical properties from its
collider compound + per-collider material density. Sub-slice d in the
v1a-material cluster (a → b → c → d → cluster close + 14-config sweep).
Without this slice the SI solver (v1e) has no way to translate authoring-
side (geometry + density) into solver-side (`inv_mass`, `inv_inertia`),
so it's the last sub-slice on the v1a critical path before v1b-c wires
the eylem `Component` types into ECS.

## What we built / changed

- **`engine/eylem/include/crd/eylem/mass_properties.hpp`** — new header.
  - `DerivedMassProperties` struct: `{ f32 mass; Vec3f com_local; Vec3f inertia_diagonal; }`.
  - `MaterialAccessor` typedef = `const Material& (*)(void* user_data, MaterialId id)` —
    type-erased closure so the function works against a scene's `MaterialPool`
    OR a cooker's offline material table without instantiating either.
  - `derive_mass_properties(colliders, accessor, user_data) noexcept` free function.
  - Doc block specifies the determinism contract (caller passes colliders
    in ascending ColliderId order; summation runs in that exact order).

- **`engine/eylem/src/mass_properties.cpp`** — implementation.
  - `volume_of(Collider)`:
    - Sphere:  V = (4/3)π r³
    - Box:     V = 8 hx hy hz
    - Capsule: V = π r² (2h) + (4/3)π r³  (cylinder + sphere)
    - ConvexHull / Plane / TriangleMesh / Heightfield / Sdf: V = 0 in v1a;
      v1d-mesh / v1d-hf / v1d-sdf + cooker (v1k) supply pre-computed mass
      properties out-of-band.
  - `inertia_diagonal_local(Collider, mass)`:
    - Sphere:  I = (2/5) m r² · I_3
    - Box:     I_xx = (1/3) m (hy² + hz²) etc. (half-extents form)
    - Capsule (Y-axis): composite cylinder + 2 hemispheres,
      I_yy = (1/2) m_cyl r² + (2/5) m_sph r²
      I_xx = I_zz = (1/12) m_cyl (3r² + 4h²) + (83/320) m_sph r² + m_sph (h + 3r/8)²
      with `m_cyl + m_sph = m_total` split by their volume share.
  - `rotate_inertia(I, R)` — `R · I · R^T` in column-major Mat3 storage;
    used to bring per-collider canonical-orientation diagonals into body
    local frame when `Collider::local_rotation` is non-identity.
  - `parallel_axis_shift(I, m, d)` — `I + m (||d||² · I_3 − d d^T)`; shifts
    inertia tensor from the collider's centroid to the body COM.
  - Two-pass walk: pass 1 = mass + COM (in collider iteration order);
    pass 2 = full 3×3 symmetric inertia tensor about COM. v1a returns the
    diagonal only (`I.c0.x, I.c1.y, I.c2.z`); off-diagonal terms are
    reserved per `RigidBody`'s "v1c-v1e diagonalise at body construction"
    comment (and v1f's planned full-tensor side-channel for asymmetric
    compounds).

- **`engine/eylem/include/crd/eylem/physics_scene.hpp`** — added
  `[[nodiscard]] virtual DerivedMassProperties derive_body_mass(BodyId) const = 0;`
  with doc block on the `inv_mass = 0` sentinel handshake (caller's policy).

- **`engine/eylem/src/null_physics_scene.cpp`**:
  - `StoredCollider` now carries `BodyId body` (so we can filter the per-
    body collider list — the null impl's `m_colliders` is a flat array
    across all bodies). Storage order = ascending ColliderId order, since
    `m_colliders` is append-only and `ColliderId.index() = position + 1`.
  - `derive_body_mass(BodyId)` impl: `has_body` check → invalid id returns
    zero-init result; otherwise collect this body's colliders into a local
    `Array<Collider>` (preserving order), define a captureless lambda that
    forwards `MaterialId` lookups through `m_materials.get`, call the free
    function. Uses `+[]` unary-plus idiom for captureless-lambda → function-
    pointer conversion (MSVC parse-tested).

- **`engine/eylem/include/crd/eylem/eylem.hpp`** — umbrella include for
  the new header.

- **`tests/eylem/test_v1a_interface.cpp`** — 5 new TEST_CASEs / 30 new assertions:
  - Sphere matches analytic V + isotropic inertia (steel density 7850).
  - Box at origin: 1m³ unit-density → 8000 kg + diagonal = (16000/3) all axes.
  - Two-box compound shifts COM along +X to midpoint; parallel-axis works
    out as expected on the diagonal (I_xx = 1000/3, I_yy = I_zz = 1000/3 + 2000).
  - Plane-only / empty input returns zeros (no asserts; safe for static-
    body compounds).
  - `NullPhysicsScene::derive_body_mass` walks per-body colliders +
    resolves through the scene's `MaterialPool`; invalid `BodyId::null()`
    returns zero-init.

## Plain-English explanation

Authors define bodies in the natural way: "this character has a 0.4 m
sphere head, a 0.5 m capsule torso, two 0.3 m capsule arms," each
referencing a material from the pool ("rubber", "steel"). The solver
needs `inv_mass` (1 / total kg) + `inv_inertia` (the diagonal of
1 / I about the body's centre of mass). v1a-material-d is the bridge:
walk the geometry, compute volume × density per collider, sum into mass,
shift the inertia tensors to the body's centre of mass, hand back a
ready-to-apply `DerivedMassProperties`.

Determinism is the loud part. FP `+` is commutative but not associative
— shuffling the order of additions can flip the last bits of the result.
ADR-0063 §4's "fixed-position write" protocol means we pin the iteration
order via ColliderId. The null impl achieves this for free (its
`m_colliders` is append-only); the v1b-c real impl (eylem-rigid3d) will
sort by ColliderId before calling. The free function trusts the caller
to pass colliders in that order (documented contract, not runtime check).

The inertia tensor is the subtle part. For a sphere it's isotropic
((2/5) m r² on every axis). For a box it's the standard `(1/3) m (hi² + hj²)`.
For a capsule the math is a composite cylinder + two hemispheres with a
parallel-axis correction for the hemisphere centroids being offset by
3r/8 from the flat cap. Once each collider's tensor is rotated from its
local canonical frame into the body's local frame and parallel-axis-
shifted to the body COM, we sum across colliders to get the body-frame
tensor about the body COM. v1a returns the diagonal — full diagonalisation
(via Jacobi eigendecomposition) lands at v1c when the SI solver needs to
handle asymmetric compounds; v1f stores the full tensor in a side channel
when it lands.

## Decisions made

- **Free function is the canonical implementation; scene method is a thin
  wrapper.** Lets the cooker (v1k) and editor (Phase 7) compute mass
  properties without instantiating a scene.
- **Type-erased `MaterialAccessor` callback** rather than templating on
  the lookup. Two reasons: (1) keeps the function out of headers (compile
  time, ODR), (2) the cooker's lookup table is shaped differently than
  the scene's MaterialPool — the void* boundary lets both work.
- **v1a returns the diagonal of inertia only.** `RigidBody::inv_inertia`
  is a Vec3f; full tensor would force a struct change. The `RigidBody`
  comment already pins this: "Off-diagonal entries are reserved for v1f
  (asymmetric collider compounds); v1c-v1e diagonalise at body
  construction." Honoured.
- **Capsule axis = Y by convention.** Matches Bullet, PhysX, Jolt;
  ColliderCapsule's `half_height` is along Y. v1a-material-d's inertia
  formula assumes Y; if v5+ adds a `capsule_axis` enum (no current
  evidence we need it — most engines fix Y), the formula shape doesn't
  change, only which Vec3f component holds the longitudinal value.
- **`volume_of` returns 0 for ConvexHull / Plane / TriangleMesh /
  Heightfield / Sdf in v1a.** The cooker (v1k) is the right place to
  compute mesh-based volumes (Gauss-Bonnet integral over triangles for
  closed meshes; user-supplied for SDF/heightfield). Static colliders
  (Plane, often Heightfield) legitimately have V = 0; mass-derivation
  silently treats them as massless contributors, which is correct.
- **Two-pass structure** (mass+COM, then inertia about COM). Required
  because parallel-axis shift needs the COM. The alternative (track
  inertia about origin, then subtract `m·||COM||² I − m·COM·COM^T`)
  produces identical FP results; chose two-pass for clarity.
- **Empty input returns zero-init** (no assert). A body with no colliders
  is structurally static; `mass = 0` matches RigidBody's `inv_mass = 0`
  sentinel for "infinite mass / static". Safe boundary behaviour.

## Files touched

- `engine/eylem/include/crd/eylem/mass_properties.hpp` — new (~80 LOC, doc-heavy)
- `engine/eylem/src/mass_properties.cpp` — new (~190 LOC including helpers)
- `engine/eylem/include/crd/eylem/physics_scene.hpp` — +14 LOC (virtual + doc)
- `engine/eylem/src/null_physics_scene.cpp` — +35 LOC (StoredCollider+BodyId, derive impl, include)
- `engine/eylem/include/crd/eylem/eylem.hpp` — +1 line (umbrella)
- `tests/eylem/test_v1a_interface.cpp` — +175 LOC (5 cases / 30 assertions + tiny test_accessor helper)

## Tests / verification

- Built: ✅ whole engine + eylem-rigid3d clean.
- `crd-eylem-tests`: **236 assertions across 31 test cases pass** (was 206/26; +30/+5).
- `ctest --preset win-debug`: **1041/1041 pass** (was 1036; +5).
- Sweep cadence: per project policy, `scripts/full-sweep.ps1` runs at
  v1a-material cluster close (a/b/c/d all ✅). **Full 14-config sweep
  PASS:** Win × 8 (debug / relwithdebinfo / release / asan / clang-cl /
  debug-scalar / tidy / shipping) all green via `scripts/full-sweep.ps1`;
  Linux × 6 (debug / relwithdebinfo / release / asan / debug-scalar /
  shipping) verified directly via wsl + per-config ctest because the PS
  full-sweep.ps1 wsl-stderr handling tripped on a CMake deprecation
  warning emitted by zstd's `cmake_minimum_required` (cosmetic; same
  brittleness pattern noted in the v0e post-mortem). Test counts:
  win-debug 1041/1041, linux-gcc-debug 1041/1041, linux-gcc-release
  1038/1038 (release excludes `#if CRD_ENABLE_ASSERTS` debug-only cases).

### Cluster-close drive-by fix — clang-cl + scalar build (v1b-rigid3d)

Pre-existing latent issue surfaced by the cluster-close sweep: `body_pool.cpp`
+ `collider_pool.cpp` declared two `put_lane` overloads (one for `Vec8f`
columns, one for `Vec4f`). Each build only invokes one — Lane=8 (AVX2)
uses Vec8f, Lane=4 (scalar) uses Vec4f. clang-cl's
`-Werror,-Wunused-function` flags whichever overload isn't called in the
current build. The original code dodged this on MSVC because MSVC doesn't
warn on unused-function-in-anon-namespace — clang-cl is stricter.

**Fix:** replaced the two overloads with a single `template<typename ColT>
put_lane` that auto-deduces from the column type. Exactly one specialisation
gets emitted per build (no overload-set whatsoever), so neither compiler
has anything unused to warn about. Buffer size derived as
`sizeof(ColT) / sizeof(crd::f32)` — works uniformly for Vec4f (16/4 = 4)
and Vec8f (32/4 = 8). Files: `engine/eylem-rigid3d/src/body_pool.cpp`,
`engine/eylem-rigid3d/src/collider_pool.cpp`. Single-path discipline:
no maybe_unused, no #ifdef — one template, one path.

## v1a-material cluster recap (a → b → c → d, 4 sub-slices, one day)

| Sub-slice | What | LOC | New tests | Assertions/cases after |
|---|---|---|---|---|
| **v1a-material-a** | 64-byte `Material` + `FrictionModel` (6 values) + `RestitutionModel` (3) + `MaterialId` + `GeometricMean` combine mode | ~150 | +5 cases / +44 | 161 / 19 |
| **v1a-material-b** | `MaterialPool` + `IPhysicsScene::create_material/update_material/material(id)/has_material(id)` + `NullPhysicsScene` impl | ~280 | +4 cases / +32 | 193 / 23 |
| **v1a-material-c** | Per-collider `Collider::material` field + 2-arg canonical `add_collider` + 3-arg NVI convenience overload | ~110 | +3 cases / +13 | 206 / 26 |
| **v1a-material-d** | `derive_mass_properties` free function + `IPhysicsScene::derive_body_mass` + sphere/box/capsule analytic V + I + COM + parallel-axis | ~320 | +5 cases / +30 | 236 / 31 |
| **CLUSTER TOTAL** | ~860 LOC + 17 cases / 119 assertions | ~860 | +17 / +119 | 236 / 31 |

ADR-0069 estimated v1a-material at ~700 LOC + ~17 tests; we landed at
~860 LOC + 17 tests / 119 assertions. The LOC overrun is primarily in
v1a-material-d's inertia-tensor math (rotate + parallel-axis shift +
analytic capsule formula) which the ADR underspecified — the elite-tier
physics derivation was honest engineering scope.

## Decision deltas vs ADR-0069

- ADR-0069 §8 specified "mass = Σ collider_volume · material.density" —
  shipped exactly as specified.
- ADR-0069 §11 specified the v1a-material-d sub-slice — shipped on plan.
- Refinement (NOT in ADR): also derive `com_local` + `inertia_diagonal`
  in the same call. ADR §8 only mentioned mass; without inertia the SI
  solver (v1e) cannot consume the result, so the elite-tier scope ships
  the full mass-properties derivation in this slice.
- Refinement (NOT in ADR): type-erased `MaterialAccessor` callback rather
  than templating. Cooker reuse motivation as documented.

## Next session starts with

- **v1b-c**: ECS components (`RigidBodyComponent`, `ColliderComponent`)
  + `EylemSystem` registration. Wires the v1a interface + v1b storage
  pools into the scene/Schedule machinery so the sandbox can spawn
  bodies as ECS entities. Per phase plan §v1b-c. ~180 LOC + ~5 tests.

## Doc updates this slice (per per-slice doc-sync discipline)

- `context.md` — "Coming up next" arrow chain updated: v1a-material a/b/c/d ✅
  marked, cluster CLOSED 2026-05-11 noted, v1e-material + v1k-material-cooker
  + v1k-material-bench arrows added downstream where they ship.
- `docs/phases/phase-3.1-eylem.md` — header status line updated to
  "v1a-material a/b/c/d ✅ — material substrate cluster CLOSED 2026-05-11".
  Each of v1a-material-{b,c,d} rows promoted to ✅ shipped 2026-05-11
  with full scope summary + test counts + session log link.
- This session log + the v1a-material-{b,c} session logs constitute the
  per-slice paper trail.
