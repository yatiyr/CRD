#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6b ear-clipping triangulation w/ hole support.
//
// Classical ear-clipping (Meisters 1975 + de Berg ch. 3) driven by Shewchuk
// `orient2d` adaptive predicates for adverse-input robustness. Polygons-with-
// holes are reduced to a single simple polygon via Eberly 1999 cut-and-join
// bridges; the bridged polygon is then ear-clipped uniformly.
//
// **Algorithmic complexity.** O(n²) per ring (classical bound: each ear pop
// reclassifies its two neighbours then we re-scan the candidate set). Held
// 2001's FIST speedup to O(n log n) via reflex-vertex spatial buckets is a
// follow-on optimization (see open follow-ons in `docs/systems/geometry-
// polygon.md` once it ships). For typical font / UI / sketch / navmesh-cell
// polygons (n ≤ ~1k vertices), O(n²) lands well below 1 ms.
//
// **Determinism contract (ADR-0063 + ADR-0076 §4 pin #11).** Every
// orientation decision uses Shewchuk `orient2d` adaptive precision; no naive
// cross-product fallback. Ear-pick order is by SMALLEST CURRENT vertex
// index (lex-tuple tiebreak engine-wide). Hole-bridge target picks the
// rightmost vertex (max x, max y tie, min idx tie) — both choices are
// byte-identical across MSVC / GCC / clang.
//
// **Builder reject / query tolerate.** Input polygon's vertices are required
// finite at construction (`Polygon2::add_ring` asserts in debug); the
// triangulator additionally short-circuits with `ok = false` on:
//   * outer ring size < 3 (no polygon)
//   * any ring is self-intersecting
//   * the bridged polygon is self-intersecting (degenerate input — rare,
//     usually means the original polygon has a hole touching its outer ring
//     at a single point, or hole-on-hole overlap)
//   * Eberly visibility test failed for a hole (no visible outer vertex —
//     typically means the hole is OUTSIDE the outer ring)
//
// **Two-layer typed architecture (ADR-0078 §5 D34).** Algorithm body operates
// on raw `MathScalar T` (`f32`/`f64`); typed `Vec2<Length32>` callers ride
// the `triangulate_typed.hpp` strip-compute-retag wrapper one layer up.
//
// **Output convention.** `triangle_indices` is a flat `Array<u32>` of size
// `3 * triangle_count`; consecutive triples are CCW-ordered triangles (since
// the outer ring is CCW by the v6 winding convention, and ear-clipping
// preserves orientation). For a polygon with N total vertices (outer + all
// holes) and H holes, the triangle count is `N + 2*H - 2` (each bridge adds
// two coincident edges which contribute one extra ear). Indices refer to
// the input polygon's flat `vertices()` array.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::polygon
{

// Options gate future tuning knobs (quality-aware ear pick, FIST spatial
// acceleration). v6b defaults are correctness + determinism.
struct TriangulateOptions
{
    // Reserved — FIST quality-aware pick lands as a v6 follow-on. The lex-
    // tuple deterministic pick is engine-wide-mandate-grade today.
    bool prefer_high_quality_ears = false;
};

enum class TriangulateStatus : crd::u8
{
    Ok                    = 0,
    EmptyPolygon          = 1, // outer ring has < 3 vertices
    NonSimpleOuter        = 2, // outer ring self-intersects
    NonSimpleHole         = 3, // some hole self-intersects
    HoleBridgingFailed    = 4, // Eberly visibility test couldn't bridge a hole
    BridgedSelfIntersect  = 5, // post-bridging polygon self-intersects (rare)
    NonFiniteInput        = 6, // a polygon vertex was non-finite
};

template <crd::math::MathScalar T>
struct TriangulationResult
{
    // Flat triangle index buffer — `3 * triangle_count` entries, three
    // consecutive indices per CCW triangle. Indices reference the INPUT
    // polygon's `vertices()` array.
    crd::containers::Array<crd::u32> triangle_indices;
    crd::u32                          triangle_count = 0;
    TriangulateStatus                 status         = TriangulateStatus::Ok;

    explicit TriangulationResult(crd::memory::IAllocator* alloc)
      : triangle_indices(alloc)
    {
    }

    [[nodiscard]] bool ok() const noexcept { return status == TriangulateStatus::Ok; }
};

// Public entry — triangulates a simple polygon ring (no holes).
template <crd::math::MathScalar T>
[[nodiscard]] TriangulationResult<T>
triangulate_ear_clip(Ring2<T> ring, crd::memory::IAllocator* alloc,
                     TriangulateOptions opts = {});

// Public entry — triangulates a polygon-with-holes via Eberly cut-and-join
// bridges + ear-clipping.
template <crd::math::MathScalar T>
[[nodiscard]] TriangulationResult<T>
triangulate_ear_clip(PolygonView2<T> poly, crd::memory::IAllocator* alloc,
                     TriangulateOptions opts = {});

} // namespace crd::geometry::polygon
