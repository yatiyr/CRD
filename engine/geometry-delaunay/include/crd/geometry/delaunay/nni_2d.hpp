#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8f 2D Sibson Natural Neighbour Interpolation
//                          (Sibson 1981).
//
// **Idea**. Given scattered input sites with values, interpolate a value at
// any query point inside the convex hull. Sibson's method:
//   1. "Insert" the query into the Voronoi diagram hypothetically.
//   2. The query's new cell `V_q` steals area from each NEIGHBOURING cell
//      `V_i`.
//   3. Sibson weight `w_i = stolen_area_i / total_stolen_area`.
//   4. Interpolated value = `sum(w_i * value_i)`.
//
// **Properties** (textbook):
//   - C¹ continuous everywhere INSIDE the convex hull except at the sites.
//   - Reproduces linear functions exactly.
//   - Bounded: result is always a convex combination of natural-neighbour
//     values (no overshoot).
//   - Local: only natural neighbours contribute.
//   - **Crushes** inverse-distance-weighted (IDW) for terrain reconstruction
//     and scientific-field interpolation.
//
// **Algorithm (Bowyer-Watson cavity / Belikov-Semenov 1997 form)**:
//   Build (cached for many queries):
//     1. Run `delaunay_2d(sites)` to get triangle indices.
//     2. Rebuild triangle adjacency via sort-and-scan over 3T half-edges.
//     3. Compute circumcentre per Delaunay triangle (cached for query reuse).
//
//   Per query `q`:
//     1. Locate a containing Delaunay triangle (jump-walk via `orient2d`).
//        Outside convex hull -> `OutsideHull`. Exact-site match ->
//        `OnSite` + return that site's value.
//     2. BFS the "cavity" of triangles whose circumcircle contains `q`
//        (Shewchuk `incircle` adaptive — Stage D paydown from v8a).
//     3. Cavity boundary forms a polygon; vertices are the **natural
//        neighbours** of q (in CCW order).
//     4. For each natural neighbour `n_i`:
//        - Walk cavity triangles incident to `n_i` in CCW order; their
//          circumcentres are the "OLD" Voronoi vertices of `n_i`'s contribution.
//        - Compute two "NEW" Voronoi vertices: circumcentre of (q, n_{i-1},
//          n_i) and circumcentre of (q, n_i, n_{i+1}).
//        - Stolen polygon = [new_left, old_C0, old_C1, ..., new_right]
//          where the old C_k are the cavity-triangle circumcentres in walk
//          order from n_{i-1} to n_{i+1}.
//        - Stolen area via signed-area formula.
//     5. Weights = stolen_i / sum_stolen.
//     6. Interpolated = sum(w_i * value[n_i]).
//
// **Determinism contract**: Delaunay output deterministic; cavity BFS in
// monotonic tri-id order; cavity boundary walk in CCW order via Delaunay
// neighbour info (the same direction convention as v8d-2d D99 mirror).
// Byte-identical interpolated value across compilers given byte-identical
// input.
//
// **Robustness contract**:
//   - Diagnostic statuses from Delaunay (`TooFewPoints` / `NonFiniteInput`
//     / `DuplicatePoint`) propagated.
//   - Non-finite query -> `QueryNonFinite`.
//   - Query outside convex hull -> `OutsideHull` (NNI undefined there).
//   - Query coincident with a site -> `OnSite` + that site's value.
//   - `InternalInvariant` on any walk-step inconsistency (defense-in-depth).
//
// **Two-layer typing** (ADR-0078 §5 D34): raw `<MathScalar T>` body; typed
// wrappers in `nni_2d_typed.hpp` added at slice close on first typed
// consumer.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class NniStatus : crd::u8
{
    Ok                = 0,
    TooFewPoints      = 1, // < 3 input sites
    NonFiniteInput    = 2,
    DuplicatePoint    = 3,
    QueryNonFinite    = 4,
    OutsideHull       = 5, // query outside convex hull
    OnSite            = 6, // query coincident with an input site
    NotInitialized    = 7, // interpolator was not successfully constructed
    InternalInvariant = 8,
};

template <crd::math::MathScalar T>
struct NniResult
{
    T          value  = static_cast<T>(0);
    NniStatus  status = NniStatus::Ok;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == NniStatus::Ok || status == NniStatus::OnSite;
    }
};

// Cached Sibson interpolator. Builds Delaunay + triangle adjacency +
// per-triangle circumcentres once; supports many subsequent `interpolate()`
// queries. Cheaper than the one-shot functional form below when querying
// many points on the same input.
template <crd::math::MathScalar T>
class NniInterpolator2
{
public:
    // Build from input sites + values. Both spans must have the same size.
    NniInterpolator2(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
                      crd::containers::ConstSpan<T>                  values,
                      crd::memory::IAllocator*                        alloc);

    [[nodiscard]] NniStatus build_status() const noexcept { return m_build_status; }

    // Interpolate at a query point.
    [[nodiscard]] NniResult<T> interpolate(const crd::math::Vec2<T>& query) const;

private:
    crd::memory::IAllocator*                    m_alloc;
    crd::containers::Array<crd::math::Vec2<T>>  m_sites;
    crd::containers::Array<T>                   m_values;
    crd::containers::Array<crd::u32>            m_tri_indices;       // 3 per tri
    crd::containers::Array<crd::u32>            m_tri_neighbours;    // 3 per tri (k_null_nbr for hull)
    crd::containers::Array<crd::math::Vec2<T>>  m_circumcentres;     // 1 per tri
    crd::u32                                    m_tri_count   = 0;
    NniStatus                                   m_build_status = NniStatus::NotInitialized;
};

// One-shot functional form. Builds and discards the cached structure per
// call. Use the class form for many queries on the same input.
template <crd::math::MathScalar T>
[[nodiscard]] NniResult<T>
sibson_interpolate_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
                       crd::containers::ConstSpan<T>                  values,
                       const crd::math::Vec2<T>&                      query,
                       crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
