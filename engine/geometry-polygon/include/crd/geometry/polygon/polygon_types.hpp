#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-polygon — Phase 3.1.7 v6a substrate types.
//
// Three planar polygon types form the substrate every v6 algorithm consumes:
//
//   * `Ring2<T>`          non-owning view of a single closed ring (a simple
//                          polygon). Implicit closure: the last vertex IS
//                          connected back to the first; do NOT duplicate it.
//
//   * `PolygonView2<T>`   non-owning view of a possibly-multi-ring polygon
//                          (one outer + N hole rings). All rings share one
//                          vertex array; per-ring offsets pick out each
//                          contiguous span. The view never owns memory.
//
//   * `Polygon2<T>`       owning storage backed by `crd::containers::Array`s
//                          (vertices + offsets). Supports incremental ring
//                          construction via `add_ring`. Convertible to
//                          `PolygonView2<T>` for free.
//
// **Winding convention (LOCKED — ADR-0076 §15 hardening pattern):** Outer ring
// is CCW; hole rings are CW. `signed_area(Ring2)` returns POSITIVE for CCW
// and NEGATIVE for CW. This matches Clipper2 + Vatti 1992 + Eberly 1999 and
// gates v6c/v6d/v6e self-consistency. Builders never silently re-orient — a
// debug `CRD_ASSERT` in `Polygon2::add_ring` enforces the convention. Callers
// with raw input call `ensure_orientation(ring, want_ccw)` (a v6a helper)
// before insertion.
//
// **Indexing.** `Polygon2<T>::ring_offsets()` returns a span of size
// `ring_count() + 1`; entry `r` is the start of ring `r`, entry
// `ring_count()` is the past-the-end index. Ring `r`'s vertex span is
// `vertices().subspan(offsets[r], offsets[r+1] - offsets[r])`. This layout
// is identical to the prefix-sum convention `ConvexHullView::face_vertex_
// offsets` uses (ADR-0076 §15 v1h pin) — caller code can be polymorphic
// across the two with one less indirection.
//
// **Type parameter.** `T = MathScalar` (`f32` / `f64`). The lower (algorithm)
// layer per ADR-0078 §5 D34 — raw scalars only, no Quantity tag inside. The
// typed `Vec2<Length32>` boundary lives one layer up in `*_typed.hpp`
// strip-compute-retag wrappers.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::polygon
{
using crd::math::MathScalar;
using crd::math::Vec2;

// ---- Ring2 — non-owning view of a single closed polygon ring -------------

template <MathScalar T>
struct Ring2
{
    crd::containers::ConstSpan<Vec2<T>> vertices;

    [[nodiscard]] constexpr crd::usize size() const noexcept
    {
        return vertices.size();
    }
    [[nodiscard]] constexpr const Vec2<T>& operator[](crd::usize i) const noexcept
    {
        return vertices[i];
    }
    // Closed-ring next-vertex helper: `next(i) == (i + 1) % size()`, branch-
    // free for hot loops where `size() > 0` is established at entry.
    [[nodiscard]] constexpr crd::usize next(crd::usize i) const noexcept
    {
        const crd::usize n = vertices.size();
        return (i + 1U) >= n ? 0U : (i + 1U);
    }
    [[nodiscard]] constexpr crd::usize prev(crd::usize i) const noexcept
    {
        return i == 0U ? (vertices.size() - 1U) : (i - 1U);
    }
};

// ---- PolygonView2 — non-owning view of a multi-ring polygon -------------

template <MathScalar T>
struct PolygonView2
{
    crd::containers::ConstSpan<Vec2<T>> vertices;
    crd::containers::ConstSpan<crd::u32> ring_offsets;

    // ring_offsets.size() = ring_count + 1; the trailing entry is the past-
    // the-end index into `vertices`. Convention follows ConvexHullView
    // (ADR-0076 §15 v1h pin) so caller code can polymorphically iterate.
    [[nodiscard]] constexpr crd::u32 ring_count() const noexcept
    {
        return ring_offsets.empty() ? 0U : static_cast<crd::u32>(ring_offsets.size()) - 1U;
    }
    [[nodiscard]] constexpr Ring2<T> ring(crd::u32 r) const noexcept
    {
        CRD_ASSERT(r < ring_count());
        const crd::u32 lo = ring_offsets[r];
        const crd::u32 hi = ring_offsets[r + 1U];
        return Ring2<T>{vertices.subspan(lo, static_cast<crd::usize>(hi - lo))};
    }
    [[nodiscard]] constexpr Ring2<T> outer() const noexcept
    {
        CRD_ASSERT(ring_count() >= 1U);
        return ring(0U);
    }
    [[nodiscard]] constexpr crd::u32 hole_count() const noexcept
    {
        const crd::u32 rc = ring_count();
        return rc == 0U ? 0U : (rc - 1U);
    }
};

// ---- Polygon2 — owning storage for a multi-ring polygon ------------------

template <MathScalar T>
class Polygon2
{
public:
    explicit Polygon2(crd::memory::IAllocator* alloc) noexcept
      : m_vertices(alloc), m_ring_offsets(alloc)
    {
        // Empty polygon — `ring_count() == 0`. First `add_ring` seeds the
        // offsets array with its leading-zero entry and trailing-N entry.
    }

    // Append a ring. The first call adds the OUTER ring (must be CCW per the
    // winding convention pinned in the header banner); subsequent calls add
    // HOLE rings (must be CW). Empty rings are rejected. Non-finite vertices
    // are rejected (builder reject — ADR-0076 §15).
    void add_ring(crd::containers::ConstSpan<Vec2<T>> ring)
    {
        CRD_ASSERT(!ring.empty());
        // Builder reject: non-finite vertex ⇒ degenerate input. Queries
        // tolerate, builders reject — engine-wide contract (ADR-0076 §15).
        // The loop body is debug-only via CRD_ASSERT; the iterator binding
        // is unused in NDEBUG, so mark it [[maybe_unused]].
        for ([[maybe_unused]] const auto& v : ring)
        {
            CRD_ASSERT(crd::geometry::primitives::is_finite(v));
        }
        if (m_ring_offsets.empty())
        {
            m_ring_offsets.push_back(0U);
        }
        const crd::u32 base = static_cast<crd::u32>(m_vertices.size());
        m_vertices.reserve(m_vertices.size() + ring.size());
        for (const auto& v : ring)
        {
            m_vertices.push_back(v);
        }
        m_ring_offsets.push_back(base + static_cast<crd::u32>(ring.size()));
    }

    // Append a ring from an iterator pair — convenience for caller code that
    // already has an Array<Vec2>.
    void add_ring(const crd::containers::Array<Vec2<T>>& ring)
    {
        add_ring(crd::containers::ConstSpan<Vec2<T>>{ring.data(), ring.size()});
    }

    // Reset the polygon to empty (preserves backing capacity).
    void clear() noexcept
    {
        m_vertices.clear();
        m_ring_offsets.clear();
    }

    // Accessors mirror PolygonView2 — same shape, same indexing.
    [[nodiscard]] crd::u32 ring_count() const noexcept
    {
        return m_ring_offsets.empty() ? 0U : static_cast<crd::u32>(m_ring_offsets.size()) - 1U;
    }
    [[nodiscard]] crd::u32 hole_count() const noexcept
    {
        const crd::u32 rc = ring_count();
        return rc == 0U ? 0U : (rc - 1U);
    }
    [[nodiscard]] Ring2<T> ring(crd::u32 r) const noexcept
    {
        CRD_ASSERT(r < ring_count());
        const crd::u32 lo = m_ring_offsets[r];
        const crd::u32 hi = m_ring_offsets[r + 1U];
        return Ring2<T>{crd::containers::ConstSpan<Vec2<T>>{m_vertices.data() + lo,
                                                            static_cast<crd::usize>(hi - lo)}};
    }
    [[nodiscard]] Ring2<T> outer() const noexcept
    {
        CRD_ASSERT(ring_count() >= 1U);
        return ring(0U);
    }
    [[nodiscard]] crd::containers::ConstSpan<Vec2<T>> vertices() const noexcept
    {
        return crd::containers::ConstSpan<Vec2<T>>{m_vertices.data(), m_vertices.size()};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> ring_offsets() const noexcept
    {
        return crd::containers::ConstSpan<crd::u32>{m_ring_offsets.data(), m_ring_offsets.size()};
    }
    [[nodiscard]] crd::usize vertex_count() const noexcept
    {
        return m_vertices.size();
    }
    [[nodiscard]] PolygonView2<T> view() const noexcept
    {
        return PolygonView2<T>{vertices(), ring_offsets()};
    }
    // Implicit-conversion shorthand for call sites that want a view.
    [[nodiscard]] operator PolygonView2<T>() const noexcept { return view(); }

private:
    crd::containers::Array<Vec2<T>>  m_vertices;
    crd::containers::Array<crd::u32> m_ring_offsets;
};

// ---- Aliases -------------------------------------------------------------

using Ring2f = Ring2<crd::f32>;
using Ring2d = Ring2<crd::f64>;
using PolygonView2f = PolygonView2<crd::f32>;
using PolygonView2d = PolygonView2<crd::f64>;
using Polygon2f = Polygon2<crd::f32>;
using Polygon2d = Polygon2<crd::f64>;

} // namespace crd::geometry::polygon
