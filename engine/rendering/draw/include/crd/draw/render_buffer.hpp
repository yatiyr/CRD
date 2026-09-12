#pragma once

// crd-draw -- RenderBuffer (Phase 3.1 v1a-draw, ADR-0066).
//
// Retained per-frame primitive store, PhysX-style (`PxRenderBuffer`). The
// immediate-mode API (`crd::draw::line()` etc.) appends to a buffer; the
// frame-graph overlay pass (`add_draw_overlay_pass()`) reads from one.
//
// Lifecycle: typically `clear()` at frame begin, append during frame,
// rendered + serialised at frame end. Multi-buffer use is fine for
// fan-out emission (per-thread buffers + `append()` to merge before
// rendering, in deterministic order).
//
// String storage for DebugText: the buffer holds a `const char*` to a
// caller-owned string. For dynamic strings, the consumer keeps the
// strings alive for the lifetime of the buffer (typically one frame).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/draw/types.hpp>
#include <crd/math/vec.hpp>

namespace crd::draw
{
class RenderBuffer
{
public:
    RenderBuffer() noexcept = default;
    explicit RenderBuffer(crd::memory::IAllocator* alloc) noexcept
        : m_points(alloc), m_lines(alloc), m_triangles(alloc), m_texts(alloc) {}

    // Wipes all four primitive arrays. O(N); does not release the
    // underlying capacity (subsequent appends reuse it).
    void clear() noexcept
    {
        m_points.clear();
        m_lines.clear();
        m_triangles.clear();
        m_texts.clear();
    }

    // Append every primitive from `other` to this buffer. Used to merge
    // per-thread buffers in deterministic order at end-of-fan-out. O(N).
    void append(const RenderBuffer& other)
    {
        for (const auto& p : other.m_points)    { m_points.push_back(p); }
        for (const auto& l : other.m_lines)     { m_lines.push_back(l); }
        for (const auto& t : other.m_triangles) { m_triangles.push_back(t); }
        for (const auto& t : other.m_texts)     { m_texts.push_back(t); }
    }

    // Translate every primitive by `delta`. For floating-origin worlds:
    // when the engine shifts origin, call `shift(-old_origin_in_new_frame)`
    // to keep debug primitives co-located with the scene. O(N).
    void shift(crd::math::Vec3f delta) noexcept
    {
        for (auto& p : m_points)    { p.pos = p.pos + delta; }
        for (auto& l : m_lines)     { l.a   = l.a + delta;   l.b = l.b + delta; }
        for (auto& t : m_triangles) { t.a   = t.a + delta;   t.b = t.b + delta; t.c = t.c + delta; }
        for (auto& t : m_texts)     { t.pos = t.pos + delta; }
    }

    // ---- Accessors (consumed by the renderer + serialiser) -----------------
    [[nodiscard]] containers::ConstSpan<DebugPoint>    points()    const noexcept
    {
        return containers::make_span(m_points.data(), m_points.size());
    }
    [[nodiscard]] containers::ConstSpan<DebugLine>     lines()     const noexcept
    {
        return containers::make_span(m_lines.data(), m_lines.size());
    }
    [[nodiscard]] containers::ConstSpan<DebugTriangle> triangles() const noexcept
    {
        return containers::make_span(m_triangles.data(), m_triangles.size());
    }
    [[nodiscard]] containers::ConstSpan<DebugText>     texts()     const noexcept
    {
        return containers::make_span(m_texts.data(), m_texts.size());
    }

    [[nodiscard]] crd::usize point_count()    const noexcept { return m_points.size(); }
    [[nodiscard]] crd::usize line_count()     const noexcept { return m_lines.size(); }
    [[nodiscard]] crd::usize triangle_count() const noexcept { return m_triangles.size(); }
    [[nodiscard]] crd::usize text_count()     const noexcept { return m_texts.size(); }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_points.empty() && m_lines.empty() && m_triangles.empty() && m_texts.empty();
    }

    // ---- Mutators (called by immediate-mode API + per-shape generators) ----
    void add_point(const DebugPoint& p)       { m_points.push_back(p); }
    void add_line(const DebugLine& l)         { m_lines.push_back(l); }
    void add_triangle(const DebugTriangle& t) { m_triangles.push_back(t); }
    void add_text(const DebugText& t)         { m_texts.push_back(t); }

    // Capacity-management hints for high-volume callers (eylem v1c+
    // broadphase, sdf cell viz). Nothing forces these -- appends grow
    // automatically -- but pre-reserving avoids allocator churn in steady state.
    void reserve_lines(crd::usize n)     { m_lines.reserve(n); }
    void reserve_triangles(crd::usize n) { m_triangles.reserve(n); }
    void reserve_points(crd::usize n)    { m_points.reserve(n); }

private:
    crd::containers::Array<DebugPoint>    m_points;
    crd::containers::Array<DebugLine>     m_lines;
    crd::containers::Array<DebugTriangle> m_triangles;
    crd::containers::Array<DebugText>     m_texts;
};

} // namespace crd::draw
