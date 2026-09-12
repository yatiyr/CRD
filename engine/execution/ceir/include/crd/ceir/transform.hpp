#pragma once

// crd-ceir — the transform dialect's find_transform_misuse verifier (CEIR-27a, sec-71). The GENERATED verify_fuse/
// verify_share_storage (gen/transform_ops.hpp) own each directive's STRUCTURAL contract (the required bool `enable`); THIS
// owns the MODULE-WIDE rule: a program-global directive (transform.fuse / transform.share_storage) may appear AT MOST ONCE
// per module -- a second occurrence is DuplicateDirective. Without it a hand-authored schedule asset could set a knob twice
// and the loader plan_options_from_transform is LAST-WRITE-WINS SILENTLY (no diagnostic) -- the 27b .ceir assets make that
// corpus real, so the guard lands with the mechanism. The find_resource/quant_misuse house pattern. ⛔ I6 -- matches op NAME
// (ctx.op_name, const), never op.kind (interning mutates). Program-global this slice (no payload handle) ⇒ "at most once per
// module" IS the whole contract; per-op targeting (sec-71 fine-grained) is named-forward.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/transform_ops.hpp> // register_transform_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::transform
{
enum class TransformMisuseKind : u8
{
    None = 0,
    DuplicateDirective, // a program-global directive (transform.fuse / transform.share_storage / transform.assign_provider) appears more than once
};
[[nodiscard]] containers::StringView transform_misuse_kind_name(TransformMisuseKind k) noexcept;

// The pointing result: the FIRST misuse (pre-order), the offending `op` (the DUPLICATE occurrence). `value` is null -- a
// directive has no value operand, the misuse is the op itself (matches the QuantMisuse attr-misuse convention).
struct TransformMisuse
{
    const Value*        value = nullptr;
    const Operation*    op    = nullptr;
    TransformMisuseKind kind  = TransformMisuseKind::None;
};
// The FIRST transform misuse in module `m` (pre-order), or {None}. ⛔ const Context& -- reads op names only, interns nothing.
[[nodiscard]] TransformMisuse find_transform_misuse(const Context& ctx, const Module& m);
} // namespace crd::ceir::transform
