#pragma once

// crd-draw -- thread-local active buffer + ergonomic wrappers
// (Phase 3.1 v1a-draw d2-curbuf, ADR-0066 sec 19.3).
//
// The canonical API (crd/draw/shapes.hpp) takes an explicit RenderBuffer&
// for stateless safety + per-fiber fan-out emission. This header layers a
// thread-local "active buffer" pointer + ergonomic wrappers on top so dev
// console / editor manipulator / one-liner code can write:
//
//     crd::draw::line(a, b, kRed);
//
// instead of:
//
//     crd::draw::add_line_to(buffer, a, b, kRed);
//
// Consumers install one buffer per thread/fiber via `set_active_buffer(buf)`
// at the top of their render flow; the wrappers route into it. Calling a
// wrapper without an active buffer is a CRD_ASSERT in debug, no-op release.
//
// **Both APIs coexist.** Fan-out emission (eylem v1c+ broadphase per-fiber)
// uses the canonical `*_to(buf, ...)` form; debug-console one-liners use
// the active-buffer form. Mix freely.

#include <crd/core/assert.hpp>
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/shapes.hpp>

namespace crd::draw
{
// Install / read / clear the per-thread active buffer.
//
// Thread-local pointer storage; not thread-safe across threads (each thread
// has its own slot). `clear_active_buffer()` after a render flow is
// recommended hygiene but not required (the pointer is just inert until
// the next set_active_buffer).
void set_active_buffer(RenderBuffer* buf) noexcept;
[[nodiscard]] RenderBuffer* active_buffer() noexcept;
inline void clear_active_buffer() noexcept { set_active_buffer(nullptr); }

// RAII guard: install on construction, restore previous on destruction.
// Useful for nested render flows + exception-safe cleanup.
class ScopedActiveBuffer
{
public:
    explicit ScopedActiveBuffer(RenderBuffer* buf) noexcept
        : m_prev(active_buffer())
    {
        set_active_buffer(buf);
    }
    ~ScopedActiveBuffer() noexcept { set_active_buffer(m_prev); }

    ScopedActiveBuffer(const ScopedActiveBuffer&)            = delete;
    ScopedActiveBuffer(ScopedActiveBuffer&&)                 = delete;
    ScopedActiveBuffer& operator=(const ScopedActiveBuffer&) = delete;
    ScopedActiveBuffer& operator=(ScopedActiveBuffer&&)      = delete;

private:
    RenderBuffer* m_prev;
};

// ---------------------------------------------------------------------------
// Convenience wrappers -- each forwards to the canonical *_to(buf, ...)
// function with `buf = active_buffer()`. Asserts in debug if no active
// buffer is set. Release builds early-out cleanly (no crash).
// ---------------------------------------------------------------------------

namespace detail_active
{
inline RenderBuffer* checked_active() noexcept
{
    auto* buf = active_buffer();
    CRD_ASSERT(buf != nullptr); // must call set_active_buffer first
    return buf;
}
} // namespace detail_active

inline void line(crd::math::Vec3f a, crd::math::Vec3f b, Color c = kWhite,
                 crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                 crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        add_line_to(*buf, a, b, c, width_px, flags, lifetime_s);
}

inline void point(crd::math::Vec3f p, Color c = kWhite, crd::f32 size_px = 4.0F,
                  PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        add_point_to(*buf, p, c, size_px, flags, lifetime_s);
}

inline void triangle(crd::math::Vec3f a, crd::math::Vec3f b, crd::math::Vec3f c,
                     Color color = kWhite, PrimFlags flags = kDefaultFlags,
                     crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        add_triangle_to(*buf, a, b, c, color, flags, lifetime_s);
}

inline void box_wire(const crd::math::Mat4f& world, crd::math::Vec3f half_extents,
                     Color color = kWhite, crd::f32 width_px = 1.0F,
                     PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        box_wire_to(*buf, world, half_extents, color, width_px, flags, lifetime_s);
}

inline void box_solid(const crd::math::Mat4f& world, crd::math::Vec3f half_extents,
                      Color color = kWhite, PrimFlags flags = kDefaultFlags,
                      crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        box_solid_to(*buf, world, half_extents, color, flags, lifetime_s);
}

inline void aabb_wire(crd::math::Vec3f min_corner, crd::math::Vec3f max_corner,
                      Color color = kAabb, crd::f32 width_px = 1.0F,
                      PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        aabb_wire_to(*buf, min_corner, max_corner, color, width_px, flags, lifetime_s);
}

inline void sphere_wire(crd::math::Vec3f center, crd::f32 radius, Color color = kWhite,
                        crd::u32 segments_long = 16, crd::u32 segments_lat = 8,
                        crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                        crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        sphere_wire_to(*buf, center, radius, color, segments_long, segments_lat,
                       width_px, flags, lifetime_s);
}

inline void sphere_solid(crd::math::Vec3f center, crd::f32 radius, Color color = kWhite,
                         PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        sphere_solid_to(*buf, center, radius, color, flags, lifetime_s);
}

inline void capsule_wire(crd::math::Vec3f a, crd::math::Vec3f b, crd::f32 radius,
                         Color color = kWhite, crd::u32 segments = 16,
                         crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                         crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        capsule_wire_to(*buf, a, b, radius, color, segments, width_px, flags, lifetime_s);
}

inline void capsule_solid(crd::math::Vec3f a, crd::math::Vec3f b, crd::f32 radius,
                          Color color = kWhite, crd::u32 segments = 16,
                          PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        capsule_solid_to(*buf, a, b, radius, color, segments, flags, lifetime_s);
}

inline void arrow(crd::math::Vec3f origin, crd::math::Vec3f dir, crd::f32 length,
                  Color color = kWhite, crd::f32 head_size_ratio = 0.2F,
                  crd::f32 head_radius_ratio = 0.4F, crd::f32 width_px = 2.0F,
                  PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        arrow_to(*buf, origin, dir, length, color, head_size_ratio,
                 head_radius_ratio, width_px, flags, lifetime_s);
}

inline void axis_triad(const crd::math::Mat4f& transform, crd::f32 length = 1.0F,
                       crd::f32 width_px = 2.0F, PrimFlags flags = kDefaultFlags,
                       crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        axis_triad_to(*buf, transform, length, width_px, flags, lifetime_s);
}

inline void arc(crd::math::Vec3f center, crd::math::Vec3f axis, crd::math::Vec3f zero_dir,
                crd::f32 radius, crd::f32 angle_min, crd::f32 angle_max,
                Color color = kWhite, crd::u32 segments = 24, crd::f32 width_px = 1.0F,
                PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        arc_to(*buf, center, axis, zero_dir, radius, angle_min, angle_max,
               color, segments, width_px, flags, lifetime_s);
}

inline void cross_3d(crd::math::Vec3f center, crd::f32 size, Color color = kWhite,
                     crd::f32 width_px = 1.0F, PrimFlags flags = kDefaultFlags,
                     crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        cross_3d_to(*buf, center, size, color, width_px, flags, lifetime_s);
}

inline void grid(crd::math::Vec3f origin, crd::math::Vec3f right, crd::math::Vec3f forward,
                 crd::u32 cells_x, crd::u32 cells_z, crd::f32 cell_size,
                 Color color = kGrey, crd::f32 width_px = 1.0F,
                 PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        grid_to(*buf, origin, right, forward, cells_x, cells_z, cell_size,
                color, width_px, flags, lifetime_s);
}

inline void frustum(const crd::math::Mat4f& view_proj, Color color = kYellow,
                    crd::f32 clip_z_min = 0.0F, crd::f32 width_px = 1.0F,
                    PrimFlags flags = kDefaultFlags, crd::f32 lifetime_s = 0.0F)
{
    if (auto* buf = detail_active::checked_active())
        frustum_to(*buf, view_proj, color, clip_z_min, width_px, flags, lifetime_s);
}

} // namespace crd::draw
