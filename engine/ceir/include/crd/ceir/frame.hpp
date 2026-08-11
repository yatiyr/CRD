#pragma once

// crd-ceir — the FRAME dialect (CEIR-15a, §39): the authorable frame graph as CEIR. The OPS are generated
// (`register_frame_ops`); this header adds the `draw_list` type-class + its factory + a combined `register_dialect`.
// A `frame.draw_list` produces a draw-list VALUE — a marker Extern type-class (an ECS query handle; the host resolves
// it to pre-resolved DrawItems at execute, so the IR carries NO scene type — the frame-cook ⊥ crd-scene invariant).
// The frame graph's RESOURCES are ordinary `resource.declare`/`import` values (NO frame.resource op — the CEIR-12b/c/d
// planner plans declare's resources); HISTORY rides declare's lifetime=history + `frame.history` for the prev-frame read.
// ⛔ Callers use THIS `register_dialect` (not the raw generated `register_frame_ops`) so the draw_list type exists for
// `frame.draw_list`'s result type + `find_frame_misuse`.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/frame_ops.hpp> // register_frame_ops (the generated ops)
#include <crd/ceir/id.hpp>

namespace crd::ceir::frame
{
// Register the `frame` dialect: its generated ops (register_frame_ops) + the `draw_list` marker type-class. Idempotent.
Dialect* register_dialect(Context& ctx);

// The interned draw-list type-class = intern_type_class("frame", "draw_list"). Works for a not-yet-registered context
// too (the id is a content hash); register_dialect gives it its verify hook.
[[nodiscard]] TypeClassId draw_list_class(Context& ctx);

// Build the frame `draw_list` VALUE type — a marker Extern (zero members: an ECS-query handle). The class must be
// registered (type_extern asserts its hook).
[[nodiscard]] TypeId type_draw_list(Context& ctx);
} // namespace crd::ceir::frame
