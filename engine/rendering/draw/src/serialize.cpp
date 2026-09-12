// crd-draw -- serialize_render_buffer impl (Phase 3.1 v1a-draw d4 stub).
//
// API surface frozen at d4 close (ADR-0066 §19.6); the body is intentionally
// a no-op stub for Phase 7 (replay viewer / capture-tool, ADR-0066 §19.7).
//
// `(void)` casts on the parameters silence -Wunused without #ifdef noise.

#include <crd/draw/serialize.hpp>

#include <crd/draw/render_buffer.hpp>

namespace crd::draw
{
crd::usize serialize_render_buffer_size(const RenderBuffer& buf) noexcept
{
    (void)buf;
    return 0;
}

crd::usize serialize_render_buffer(const RenderBuffer&             buf,
                                   crd::containers::Span<crd::u8>  dst) noexcept
{
    (void)buf;
    (void)dst;
    return 0;
}

} // namespace crd::draw
