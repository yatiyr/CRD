# Session 2026-05-19 — geometry-v10c arc-length system

## Summary

Shipped Phase 3.1.7 v10c: the arc-length system for `crd-geometry-curves`.
Drives constant-speed curve traversal — the foundation cinematic-camera
dolly + robot trajectory + animation keyframe-timing-remap consumers will
pull on.

## What landed

### Public API (`engine/geometry-curves/include/crd/geometry/curves/arclength.hpp`)

```cpp
inline constexpr crd::u32 k_arclength_default_samples = 64U;

template <crd::math::MathScalar T> struct ArclengthSample {
    T t;        // curve parameter, [0, 1]
    T distance; // cumulative arc length from t=0
};

template <crd::math::MathScalar T> struct ArclengthTable {
    crd::containers::Array<ArclengthSample<T>> samples;
    T                                          total_length;
    bool                                       closed;
};

template <typename Curve>
ArclengthTable<typename Curve::scalar_t> build_arclength_table(
    const Curve& curve, crd::u32 n_samples, IAllocator* alloc) noexcept;

template <typename Curve>
ArclengthTable<typename Curve::scalar_t> build_arclength_table(
    const Curve& curve, IAllocator* alloc) noexcept; // n = 64

template <crd::math::MathScalar T>
T length_of(const ArclengthTable<T>& table) noexcept;

template <typename Curve>
typename Curve::scalar_t length_of(
    const Curve& curve, crd::u32 n_samples, IAllocator* alloc) noexcept;

template <crd::math::MathScalar T>
T t_at_distance(const ArclengthTable<T>& table, T distance) noexcept;

template <crd::math::MathScalar T>
T distance_at_t(const ArclengthTable<T>& table, T t) noexcept;
```

### Algorithm

`build_arclength_table` samples the curve at `n+1` uniform t values
covering t ∈ [0, 1] inclusive, then accumulates chord lengths. Open
and closed curves share the SAME table layout (D200) — closed curves
use the same n+1 entries; the chord from t=(n-1)/n to t=1 IS the
wrap-back chord and `total_length` includes it.

`t_at_distance` and `distance_at_t` use binary search + linear
interpolation (D201). Open curves clamp out-of-range inputs; closed
curves modular-reduce them via floor-based reduction (D202).

## Decisions queued for v10-close (ADR-0076 §27)

- **D198** — Uniform-t chord-length table, NOT analytic arc length.
  Same infrastructure across all curve kinds. Analytic specialisations
  (CircularArc = r·θ, Bezier via Gauss-Legendre 5-pt) filed as
  `v10c-analytic-arc-length` follow-on.
- **D199** — Default `n_samples = 64`. Cinematic-quality resolution
  at ~2 KB f32 table size.
- **D200** — Open + closed share the same `n+1`-entry layout. Uniform
  binary search across both. Closed's wrap chord is the last segment.
- **D201** — Binary search + linear interpolation. Higher-order
  interpolation would not improve accuracy because the table is
  itself piecewise-linear chord approximation.
- **D202** — Closed-curve modular reduction: floor-based on `t` (mod
  1.0) and `distance` (mod `total_length`). Handles negative inputs.

## Typed-units boundary layer — DEFERRED to v10-close

User decision (2026-05-19): the typed-units boundary layer
(`Vec3<Length<T>>` / `Length<T>` API surface) ships at **v10-close** as
a single coherent layer covering ALL of v10, not piecemeal in v10c
alone. The v10 substrate stays raw-scalar throughout (v10a/b/c/d/e)
for internal consistency; the typed wrapper is the boundary at the
public API surface.

This pins **D187** (originally an v10a deferred slot) into v10-close.

Reasoning:

1. **Consistency**: typing arc-length alone while control points stay
   raw creates a half-baked API surface ("Length of *what*?").
2. **Right unit of work**: the typed wrapper covers the WHOLE v10
   surface; doing it once at v10-close is one slice (~150 LOC + ~150
   tests), one ADR pin, one coherent docs update.
3. **Consumer-pull discipline**: no v10 consumer is breathing on the
   substrate yet — animation paths / cinematic camera / robot
   trajectory all live downstream of Phase 3.1.7 close.
4. **The wrapper is mechanical**: strip-compute-retag wrappers
   following the `crd-geometry-primitives::queries_typed.hpp` pattern
   from v0d.

The v10-close row in `docs/phases/phase-3.1.7-geometry.md` was added
to capture this; see that row for the full list of typed wrappers.

## Tests (`tests/geometry-curves/test_curves_arclength.cpp`)

13 cases / 196 assertions:

| Test | What it verifies |
|---|---|
| `build_arclength_table returns n+1 entries` | Sample count + boundary entries (t=0,d=0) and (t=1,d=total). |
| `default n_samples is 64` | `build_arclength_table(curve, alloc)` matches D199. |
| `straight-line Polyline length` | `length_of` matches sum-of-chords on a 3-4-5 triangle (= 7.0). |
| `CircularArc length ≈ r·θ` | Half-circle of r=2 ⇒ table length within 1e-3 of analytic π·2 at n=256. |
| `length_of convenience matches table-based` | Both APIs return identical totals. |
| `t_at_distance boundaries open` | d=0 → t=0; d=total_length → t=1. |
| `distance_at_t boundaries open` | t=0 → d=0; t=1 → d=total_length. |
| `open-curve clamps out-of-range` | Negative + over-range inputs clamp correctly. |
| `t_at_distance ∘ distance_at_t open` | Round-trip within 1e-5 abs at 21 sample points. |
| `distance_at_t ∘ t_at_distance open` | Inverse round-trip within 1e-4 abs at 21 sample points. |
| `arc length monotone in t` | Every consecutive sample pair: distance non-decreasing, t strictly increasing. |
| `closed curve modular wrap` | `t > 1` wraps; `t < 0` wraps; `d > L` wraps; `d < 0` wraps. |
| `f64 instantiations` | Full template instantiates + round-trip works for `double`. |

Module total: **61 cases / ~2 445 assertions PASS** (was v10b 48 /
2 249; +13 cases / +196 assertions).

## Files added

- `engine/geometry-curves/include/crd/geometry/curves/arclength.hpp`
- `tests/geometry-curves/test_curves_arclength.cpp`

## Files changed

- `engine/geometry-curves/include/crd/geometry/curves/curves.hpp` —
  added `#include "arclength.hpp"` to the umbrella.
- `tests/geometry-curves/CMakeLists.txt` — added
  `test_curves_arclength.cpp`.
- `docs/phases/phase-3.1.7-geometry.md` — v10c row marked ✅; v10-close
  row ADDED with the typed-units-layer scope.

## What's next

- **v10d** — Curve queries: AABB / closest-point Newton / ray
  intersection. Hooks into `crd/geometry/queries.hpp` facade.
- **v10e** — Frames + viz + sandbox showcase.
- **v10-close** — ADR-0076 §27 + typed-units boundary layer
  (`queries_typed.hpp`) + 18-config sweep + system doc.
