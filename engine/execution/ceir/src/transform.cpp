#include <crd/ceir/transform.hpp>

#include <crd/ceir/ir.hpp>

namespace crd::ceir::transform
{
namespace
{
using containers::StringView;

// The pre-order walk -- the FIRST duplicate directive, or {None}. Per-op by op NAME (never op.kind -- I6); const Context.
// A program-global directive may appear at most once; `seen_fuse`/`seen_share` thread the module-wide state across nested
// regions (there are none this slice, but the walk is region-recursive to match the house find_*_misuse mold).
TransformMisuse scan_transform_region(const Context& ctx, const Region* r, bool& seen_fuse, // NOLINT(misc-no-recursion)
                                      bool& seen_share, bool& seen_assign, bool& seen_constrain, bool& seen_place)
{
    if (r == nullptr) { return {}; }
    for (const Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (const Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const StringView nm = ctx.op_name(op->kind());
            if (nm == StringView("transform.fuse"))
            {
                if (seen_fuse) { return {nullptr, op, TransformMisuseKind::DuplicateDirective}; }
                seen_fuse = true;
            }
            else if (nm == StringView("transform.share_storage"))
            {
                if (seen_share) { return {nullptr, op, TransformMisuseKind::DuplicateDirective}; }
                seen_share = true;
            }
            else if (nm == StringView("transform.assign_provider")) // CEIR-29a-3b: the provider pin, program-global (at most once)
            {
                if (seen_assign) { return {nullptr, op, TransformMisuseKind::DuplicateDirective}; }
                seen_assign = true;
            }
            else if (nm == StringView("transform.constrain_provider_class")) // CEIR-29c-3b: the class filter, program-global (at most once)
            {
                if (seen_constrain) { return {nullptr, op, TransformMisuseKind::DuplicateDirective}; }
                seen_constrain = true;
            }
            else if (nm == StringView("transform.place_mesh")) // CEIR-30c: the mesh placement, program-global (at most once)
            {
                if (seen_place) { return {nullptr, op, TransformMisuseKind::DuplicateDirective}; }
                seen_place = true;
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const TransformMisuse e =
                    scan_transform_region(ctx, op->region(i), seen_fuse, seen_share, seen_assign, seen_constrain, seen_place);
                if (e.kind != TransformMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

TransformMisuse find_transform_misuse(const Context& ctx, const Module& m)
{
    bool seen_fuse      = false;
    bool seen_share     = false;
    bool seen_assign    = false;
    bool seen_constrain = false;
    bool seen_place     = false;
    return scan_transform_region(ctx, m.body(), seen_fuse, seen_share, seen_assign, seen_constrain, seen_place);
}

StringView transform_misuse_kind_name(TransformMisuseKind k) noexcept
{
    switch (k)
    {
    case TransformMisuseKind::None: return StringView("none");
    case TransformMisuseKind::DuplicateDirective: return StringView("duplicate-directive");
    }
    return StringView("?");
}
} // namespace crd::ceir::transform
