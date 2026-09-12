#pragma once

// crd-draw -- serialize_render_buffer (Phase 3.1 v1a-draw d4, ADR-0066 §19.7).
//
// API-surface stub for the Phase 7 replay viewer / capture-tool pipeline.
// The d4 close FREEZES the public crd::draw API forever (ADR-0066 §19.6),
// so the *signature* of `serialize_render_buffer` and its size-query peer
// must exist now even though the real implementation is deferred. Keeping
// the surface stable from day one means Phase 7's capture pipeline can be
// designed against the final shape today.
//
// Contract (when Phase 7 fills in the body):
//   - serialize_render_buffer_size(buf) returns the byte count needed to
//     pack `buf` into the wire format. Pure function of buf's contents.
//   - serialize_render_buffer(buf, dst) writes the wire-format bytes into
//     `dst`. If `dst.size() < required_size` no bytes are written and 0 is
//     returned. On success returns the number of bytes written, equal to
//     serialize_render_buffer_size(buf).
//
// Wire format will be a versioned packed-binary blob -- a compact dump of
// the SoA point/line/triangle/text spans plus the master flags + theme
// snapshot. Versioned by `kDrawApiVersion` so capture files cleanly fail
// the load when the engine API moves forward.
//
// Today: both functions return 0 (stub). Loading a capture written by the
// real impl from a future engine version will be a forward-compatibility
// concern Phase 7 deals with explicitly.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::draw
{
class RenderBuffer;

// Number of bytes `serialize_render_buffer` would write for `buf`.
// Stub: returns 0.
[[nodiscard]] crd::usize serialize_render_buffer_size(const RenderBuffer& buf) noexcept;

// Pack `buf` into `dst`. Returns the number of bytes written, or 0 on
// short-buffer / stub-impl. See header doc for the full contract.
[[nodiscard]] crd::usize serialize_render_buffer(const RenderBuffer&         buf,
                                                  crd::containers::Span<crd::u8> dst) noexcept;

} // namespace crd::draw
