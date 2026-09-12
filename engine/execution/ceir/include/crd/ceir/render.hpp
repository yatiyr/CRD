#pragma once

// crd-ceir — the RENDER dialect's ATTACHMENT type-classes (CEIR-14a, §41). Attachments are the RAH-1 general typed
// model: a COLOR or DEPTH attachment is an 8a Extern TYPE-CLASS over ONE underlying image member — the ROLE (color vs
// depth) rides the TYPE (the 12a one-source-of-truth doctrine), NOT a string attr. `render.color_attachment` /
// `render.depth_attachment` (the TOML-generated ops) PRODUCE these values; `render.scope` consumes them through its
// variadic tail. Because two different type-classes with identical params are DIFFERENT TypeIds (the ADR-0111
// landmine), a color attachment != a depth attachment — which is exactly "at most one depth attachment" made a type
// property the moment `find_render_misuse` looks (CEIR-14a). The OPS are generated (`register_render_ops`); this header
// adds the type-classes + factories + a combined `register_dialect`. A plugin renderer can mint its own attachment-like
// class with zero central edits (the open-world proof — the `time` precedent).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/render_ops.hpp> // register_render_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::render
{
// Register the `render` dialect: its generated ops (register_render_ops) + the color/depth attachment type-classes.
// Idempotent (re-registration is a no-op). ⛔ Callers use THIS (not the raw generated register_render_ops) so the
// attachment types exist for the attachment-declare ops' result types + find_render_misuse.
Dialect* register_dialect(Context& ctx);

// The interned attachment type-classes = intern_type_class("render", "<color|depth>_attachment"). Work for a
// not-yet-registered context too (the id is a content hash); register_dialect gives them their verify hook.
[[nodiscard]] TypeClassId color_attachment_class(Context& ctx);
[[nodiscard]] TypeClassId depth_attachment_class(Context& ctx);

// Build a COLOR / DEPTH attachment TYPE over `image` (a CEIR-3c Image type, or a resource.view subresource of one — the
// §41 mip/layer/aspect view case). The Extern type carries the class, so a color attachment is a DISTINCT TypeId from a
// depth attachment. `image` becomes the underlying member. The class must be registered (type_extern asserts its hook).
[[nodiscard]] TypeId type_color_attachment(Context& ctx, TypeId image);
[[nodiscard]] TypeId type_depth_attachment(Context& ctx, TypeId image);
} // namespace crd::ceir::render
