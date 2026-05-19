# Session 2026-05-19 — geometry-v10e frames + viz + sandbox

## Summary

Shipped Phase 3.1.7 v10e: frames + viz adapters + sandbox `CurvesShowcase`
scene. The fifth and final algorithmic slice of the v10 cluster. Closes
the substrate side of `crd-geometry-curves`; only `v10-close` (ADR
amendment + typed-units boundary layer + system doc + 18-config sweep)
remains in the v10 cluster.

## What landed

### `engine/geometry-curves/include/crd/geometry/curves/frames.hpp`

Header-only generic templates over `Curve::scalar_t`:

```cpp
template <crd::math::MathScalar T>
struct CurveFrame { Vec3<T> tangent, normal, binormal; };

template <typename Curve>
Vec3<scalar_t> tangent(const Curve& curve, scalar_t t) noexcept;

template <typename Curve>
Vec3<scalar_t> normal(const Curve& curve, scalar_t t) noexcept;

template <typename Curve>
Vec3<scalar_t> binormal(const Curve& curve, scalar_t t) noexcept;

template <typename Curve>
Array<CurveFrame<scalar_t>> compute_rmf(
    const Curve& curve, u32 n_samples, IAllocator* alloc) noexcept;
```

**Algorithms:**

- `tangent`: `evaluate_derivative(curve, t)` + `try_normalize`. Returns
  +X when the derivative is zero (singular point: cusps, polyline
  self-overlap).
- `normal`: Finite-difference 2nd derivative + Gram-Schmidt against the
  unit tangent. Endpoint-clamped on open curves; modular wrap on closed
  curves (the evaluator handles t-wrap internally).
- `binormal`: `cross(tangent, normal)`.
- `compute_rmf`: Wang 2008 double-reflection walk with **uniform
  closure-twist redistribution** on closed curves.

### Wang 2008 double-reflection step (`detail::wang_step`)

```
v1   = next_point - prev_point
r_L_N = N_prev - 2 * dot(v1, N_prev)/|v1|^2 * v1   // reflect N
r_L_T = T_prev - 2 * dot(v1, T_prev)/|v1|^2 * v1   // reflect T (proxy)
v2   = T_next - r_L_T
N_next = r_L_N - 2 * dot(v2, r_L_N)/|v2|^2 * v2     // reflect again
B_next = T_next x N_next
```

Plus a Gram-Schmidt drift-defence pass (re-orthogonalises N against T
each step; floats accumulate ~1 ULP per step otherwise).

### Closed-curve twist closure (D210)

After the open Wang walk, compute one extra "virtual wrap" frame at
t = 1 == 0, then `theta = atan2(signed projection of wrap.N - frame[0].N
on T_0's perpendicular plane)`. Redistribute by rotating each frame
`k in [1, n-1]` by `-theta * (k / n)` around its own tangent using
`crd::math::deterministic::cos / sin` (CPU<->GPU portable). Frame 0
is unchanged; frame n-1 absorbs (n-1)/n of the twist; the implicit wrap
frame matches frame 0 to within one step's ULP budget.

### `engine/geometry-viz/include/crd/geometry/viz/curves.hpp` + `src/curves.cpp`

```cpp
// Non-template overloads (T = f32, f64).
void draw_polyline(buf, Polyline3View<T>, color, width, flags, lifetime);

// Generic over curve kind.
template <typename Curve>
void draw_curve(buf, curve, n_segments, alloc, color, width, ...);

template <typename Curve>
void draw_tangent_frame(buf, curve, n_samples, axis_len, ...);

void draw_rmf(buf, ConstSpan<CurveFrame<f32>>, ConstSpan<Vec3f>, axis_len, ...);
```

Colour convention pinned at v10e:
- tangent  = red    (matches +X axis)
- normal   = green  (matches +Y axis)
- binormal = blue   (matches +Z axis)

Wire-up: added `crd-geometry-curves` to `crd-geometry-viz`
`target_link_libraries`; `viz.hpp` umbrella includes the new header.

### `sandbox/src/curves_showcase.{hpp,cpp}` + `SandboxScene::CurvesShowcase = 3`

Fourth top-level scene in the existing `SandboxScene` dropdown. ABI pin:
`Physics=0 / GeometryViz=1 / DrawShowcase=2 / CurvesShowcase=3` (append-
only per `feedback_vtable_stability_append_at_end`).

`CurvesShowcaseState` holds:

- Curve-kind picker (9 entries: Polyline / QuadBezier / CubicBezier /
  CubicHermite / CatmullRom / BSpline / CircularArc / EllipseArc /
  Helix). The Helix kind is CatmullRom-backed convenience for the
  canonical cinematic-camera demo curve.
- `n_samples` slider (4 .. 256).
- Frame-mode toggle: Off / Frenet / RMF (Wang 2008).
- `frame_axis_len`, `line_width`, `show_control_points`,
  `control_point_size` sliders.
- Per-kind control-point editor: **ImGui DragFloat3** sliders for each
  control point. The 3D-viewport drag-gizmo path is **future work** --
  recorded in `docs/debt.md` -> "Future cluster -- direct-manipulation
  UX (gizmos / mesh + curve + navmesh editors)".

### Decisions queued for v10-close (ADR-0076 §27)

- **D208** -- frames compose on `evaluate` + `evaluate_derivative`; no
  curve-kind-specific paths leak past the substrate.
- **D209** -- RMF parameter space = parameter-uniform (composes with
  `sample_uniform`). `v10e-arclength-rmf` follow-on for the cinematic-
  camera arc-length-uniform variant.
- **D210** -- closed-curve RMF closure twist via uniform redistribution.
- **D211** -- zero-curvature normal fallback: `frenet_fallback_normal`
  projects +Y onto the tangent's perpendicular plane; falls back to +Z
  if tangent parallel to +Y.
- **D212** -- degenerate-tangent fallback: scalar `tangent()` returns
  +X; `compute_rmf` retains last-good tangent.
- **D213** -- Frenet 2nd-derivative step size: h = 1e-3 default (safe
  for both analytic and finite-difference 1st-derivative kinds). Per-
  kind h = 1e-4 trait override filed as `v10a-cr-analytic-2nd-
  derivative` + `v10a-bspline-analytic-2nd-derivative` follow-ons.

### Tests

**`tests/geometry-curves/test_curves_frames.cpp` (9 cases / 387 assertions):**

1. `tangent` unit-length on every curve kind (Bezier / Hermite / Arc).
2. `tangent` on a straight polyline = constant chord direction.
3. `normal` perpendicular to `tangent`; `binormal = cross(T, N)`.
4. **Planar circular arc RMF reduces to Frenet** -- all normals
   coplanar with the arc plane; all binormals = plane normal up to a
   single global sign. **This is the Wang reflection-sign discriminator
   -- a wrong sign produces alternating binormals**.
5. **Helix RMF** -- orthonormal frames at every sample; adjacent
   tangents satisfy `dot(T_i, T_{i+1}) > 0` (minimal-twist).
6. **Closed-circle RMF closure** -- the would-be wrap frame's normal
   matches `frame[0].normal` to within ~1/n radian after redistribution.
7. Zero-curvature line: normal falls back to deterministic fallback
   (+Y or +Z based on tangent orientation).
8. Degenerate-tangent scalar API returns +X.
9. f64 instantiations work end-to-end.

**`tests/geometry-viz/test_curves_viz.cpp` (5 cases / 5 assertions):**

1. `draw_polyline` open: emits n-1 lines.
2. `draw_polyline` closed: emits n lines.
3. `draw_curve` 16-segment Bezier emits 16 lines.
4. `draw_tangent_frame` 8 samples on an arc = 9 frames * 3 axes = 27 lines.
5. `draw_rmf` 3 frames = 9 lines.

**`tests/sandbox/test_showcase.cpp` (+2 cases):**

1. Every curve kind renders into a non-empty buffer (9 kinds covered).
2. Frame-mode toggle adds at least 3 * (n_samples + 1) hair lines
   relative to Off.

Also extends the `SandboxScene` enum-stability `static_assert` to pin
`CurvesShowcase == 3`.

**Module totals:**
- `crd-geometry-curves-tests`: **81 cases / 2921 assertions PASS**
  (was v10d 72 / 2534 -> +9 cases / +387 assertions).
- `crd-geometry-viz-tests`: previous-cluster total + 5 new cases / 5
  assertions.
- `crd-sandbox-showcase-tests`: **9 cases / 57 assertions PASS** (was 7
  -> +2 cases).

## What's next

- **v10-close** -- ADR-0076 §27 amendment locking D182-D213; typed-
  units boundary layer (`queries_typed.hpp` covering the WHOLE v10
  surface: evaluate / sample / arclength / queries / frames); system
  doc `docs/systems/geometry-curves.md`; 18-config full sweep.

## Files added

- `engine/geometry-curves/include/crd/geometry/curves/frames.hpp`
- `engine/geometry-viz/include/crd/geometry/viz/curves.hpp`
- `engine/geometry-viz/src/curves.cpp`
- `sandbox/src/curves_showcase.hpp`
- `sandbox/src/curves_showcase.cpp`
- `tests/geometry-curves/test_curves_frames.cpp`
- `tests/geometry-viz/test_curves_viz.cpp`
- `docs/sessions/2026-05-19-geometry-v10e-frames-viz-sandbox.md`

## Files changed

- `engine/geometry-curves/include/crd/geometry/curves/curves.hpp` --
  umbrella + `frames.hpp` include.
- `engine/geometry-viz/include/crd/geometry/viz/viz.hpp` -- umbrella +
  `curves.hpp` include.
- `engine/geometry-viz/CMakeLists.txt` -- link `crd-geometry-curves`.
- `sandbox/src/geometry_showcase.hpp` -- `SandboxScene::CurvesShowcase = 3`.
- `sandbox/src/sandbox_layer.hpp` -- include `curves_showcase.hpp`,
  add `m_curves_showcase` state.
- `sandbox/src/sandbox_layer.cpp` -- Scene dropdown extended, render +
  ImGui panel dispatched to curves showcase.
- `sandbox/CMakeLists.txt` -- add `curves_showcase.cpp` source; link
  `crd-geometry-curves`.
- `tests/geometry-curves/CMakeLists.txt` -- add `test_curves_frames.cpp`.
- `tests/geometry-viz/CMakeLists.txt` -- add `test_curves_viz.cpp`;
  link `crd-geometry-curves`.
- `tests/sandbox/CMakeLists.txt` -- add `curves_showcase.cpp` + link
  `crd-geometry-curves`.
- `tests/sandbox/test_showcase.cpp` -- +2 cases + enum-stability pin.
- `docs/phases/phase-3.1.7-geometry.md` -- v10e row flipped to done.
- `docs/debt.md` -- added "Future cluster -- direct-manipulation UX"
  entry (gizmos / mesh + curve + navmesh editors); user-flagged
  high-priority future workstream.
- `MEMORY.md` index + `project_gizmos_direct_manipulation_cluster.md`
  -- mirror the gizmos workstream so the agent remembers across
  sessions.
