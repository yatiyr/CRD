#include <crd/ceir/work.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::work
{
namespace
{
// An opaque queue/record handle's verify hook (CEIR-20a): a work queue/record Extern carries ZERO members — the handle is
// host-opaque (the ROLE is the CLASS, a distinct TypeClassId; CEIR sees an identity, never the device buffer + counter).
// ⛔ both classes share this hook — the ROLE is the class, not the member shape (queue != record by TypeClassId).
[[nodiscard]] bool verify_opaque(const Context& /*ctx*/, const Type& t) noexcept { return t.members.empty(); }

[[nodiscard]] TypeId opaque_type(Context& ctx, TypeClassId cls)
{
    const Type p; // zero members — an opaque handle
    return ctx.type_extern(cls, p); // the 8a Extern factory (asserts the class's verify hook)
}

// Is `v`'s type the work Extern class `cls`? (A non-Extern / wrong-class / null value ⇒ false.)
[[nodiscard]] bool is_work_class(const Context& ctx, const Value* v, TypeClassId cls) noexcept
{
    if (v == nullptr) { return false; }
    const Type t = ctx.type_of(v->type());
    return t.kind == TypeKind::Extern && t.type_class == cls;
}

// A CEIR-3c resource TYPE — the kinds a bound descriptor (a work op binding) may take. ⛔ a FILE-LOCAL COPY of context.cpp's
// scan_dispatch helper (kept in sync by hand with rt.cpp's copy; a NEW resource TypeKind must be added in all three).
[[nodiscard]] bool is_resource_kind(TypeKind k) noexcept
{
    switch (k)
    {
    case TypeKind::Buffer:
    case TypeKind::Image:
    case TypeKind::Tensor:
    case TypeKind::SparseTensor:
    case TypeKind::Sampler:
    case TypeKind::ResourceTable:
    case TypeKind::AccelStruct:
    case TypeKind::VideoFrame:
    case TypeKind::AudioBuffer:
    case TypeKind::ExternalResource:
    case TypeKind::View:
        return true;
    default:
        return false;
    }
}

// Parse the `access` string: comma-separated tokens, each EXACTLY "r" | "w" | "rw", one per binding in operand order (an
// empty string = zero bindings). Sets `count`, returns false on any malformed/empty token. ⛔ the find_dispatch_misuse mirror
// (byte compare, no StringView slicing). A FILE-LOCAL COPY of context.cpp/rt.cpp's ceir_parse_access — kept in sync by hand.
[[nodiscard]] bool parse_access(containers::StringView s, u32& count) noexcept
{
    count = 0U;
    if (s.size() == 0U) { return true; }
    usize start = 0U;
    for (usize i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == ',')
        {
            const usize len = i - start;
            const char* t   = s.data() + start;
            const bool  ok  = (len == 1U && (t[0] == 'r' || t[0] == 'w')) || (len == 2U && t[0] == 'r' && t[1] == 'w');
            if (!ok) { return false; }
            ++count;
            start = i + 1U;
        }
    }
    return true;
}

// The dispatch-SHAPE check for the work dispatch ops (the find_dispatch_misuse mirror). `fixed` = the count of leading
// non-binding operands (produce 4: grid+queue; consume 1: queue; compact 2: src+dst). `has_grid` = the op carries a host
// launch grid at operands 0-2 (produce only; consume/compact are INDIRECT — the grid is the device count). Checks, in
// contractual order: dims (0-2) Index-typed if has_grid → `access` kind-fold → tokens → arity (== the variadic-binding count)
// → each binding (operands `fixed`..) resource-kinded. ⛔ standalone-robust (the 12b wrong-kind fold): a non-String `access`
// folds into AccessTokenInvalid rather than reading clean.
[[nodiscard]] WorkMisuse check_work_shape(const Context& ctx, const Operation* op, u32 fixed, bool has_grid)
{
    if (has_grid)
    {
        for (u32 i = 0; i < 3U && i < op->num_operands(); ++i)
        {
            if (ctx.type_of(op->operand(i)->type()).kind != TypeKind::Index)
            {
                return {op->operand(i), op, WorkMisuseKind::DimNotIndex};
            }
        }
    }
    const u32       bindings = op->num_operands() >= fixed ? op->num_operands() - fixed : 0U;
    u32             tokens   = 0U;
    const AttrValue av       = ctx.attr_value(op->attr(containers::StringView("access")));
    if (av.kind != AttrKind::String) { return {nullptr, op, WorkMisuseKind::AccessTokenInvalid}; }
    if (!parse_access(av.s, tokens)) { return {nullptr, op, WorkMisuseKind::AccessTokenInvalid}; }
    if (tokens != bindings) { return {nullptr, op, WorkMisuseKind::AccessArityMismatch}; }
    for (u32 i = fixed; i < op->num_operands(); ++i)
    {
        if (!is_resource_kind(ctx.type_of(op->operand(i)->type()).kind))
        {
            return {op->operand(i), op, WorkMisuseKind::BindingNotResource};
        }
    }
    return {};
}

// The pre-order walk — the FIRST work misuse, or {None}. Mirrors scan_rt_region: per-op check (by op NAME, never an op.kind
// switch — I6), then recurse regions. The queue class-id is PRECOMPUTED by find_work_misuse (interning is non-const), so the
// recursive walk stays const-clean.
WorkMisuse scan_work_region(const Context& ctx, const Region* r, TypeClassId queue) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            // ⛔ work.queue_alloc: `capacity` >= 1, `record_stride` >= 1.
            if (nm == containers::StringView("work.queue_alloc"))
            {
                const AttrValue cv = ctx.attr_value(op->attr(containers::StringView("capacity")));
                if (cv.kind != AttrKind::Int || cv.i < 1) { return {nullptr, op, WorkMisuseKind::CapacityInvalid}; }
                const AttrValue sv = ctx.attr_value(op->attr(containers::StringView("record_stride")));
                if (sv.kind != AttrKind::Int || sv.i < 1) { return {nullptr, op, WorkMisuseKind::RecordStrideInvalid}; }
            }
            // ⛔ work.produce (the grid-dispatch appender): operand(3) is work.queue; dims (0-2) Index; access/bindings.
            else if (nm == containers::StringView("work.produce"))
            {
                if (op->num_operands() >= 4U && !is_work_class(ctx, op->operand(3U), queue))
                {
                    return {op->operand(3U), op, WorkMisuseKind::QueueTypeMismatch};
                }
                const WorkMisuse d = check_work_shape(ctx, op, 4U, /*has_grid=*/true); // grid+queue = 4 fixed
                if (d.kind != WorkMisuseKind::None) { return d; }
            }
            // ⛔ work.consume (the INDIRECT dispatch): operand(0) is work.queue; NO host grid (device count drives it).
            else if (nm == containers::StringView("work.consume"))
            {
                if (op->num_operands() >= 1U && !is_work_class(ctx, op->operand(0U), queue))
                {
                    return {op->operand(0U), op, WorkMisuseKind::QueueTypeMismatch};
                }
                const WorkMisuse d = check_work_shape(ctx, op, 1U, /*has_grid=*/false); // queue = 1 fixed, no grid
                if (d.kind != WorkMisuseKind::None) { return d; }
            }
            // ⛔ work.compact (stream-compaction): operand(0) AND operand(1) are work.queue (src, dst); access/bindings.
            else if (nm == containers::StringView("work.compact"))
            {
                if (op->num_operands() >= 1U && !is_work_class(ctx, op->operand(0U), queue))
                {
                    return {op->operand(0U), op, WorkMisuseKind::QueueTypeMismatch};
                }
                if (op->num_operands() >= 2U && !is_work_class(ctx, op->operand(1U), queue))
                {
                    return {op->operand(1U), op, WorkMisuseKind::QueueTypeMismatch};
                }
                const WorkMisuse d = check_work_shape(ctx, op, 2U, /*has_grid=*/false); // src+dst = 2 fixed, no grid
                if (d.kind != WorkMisuseKind::None) { return d; }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const WorkMisuse e = scan_work_region(ctx, op->region(i), queue);
                if (e.kind != WorkMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = register_work_ops(ctx); // generated: the work dialect + its ops (idempotent)
    (void)d->register_type_class("queue", TypeClassSpec{&verify_opaque, 0U});  // idempotent by class
    (void)d->register_type_class("record", TypeClassSpec{&verify_opaque, 0U}); // queue != record (distinct TypeIds)
    return d;
}

TypeClassId queue_class(Context& ctx) { return ctx.intern_type_class("work", "queue"); }
TypeClassId record_class(Context& ctx) { return ctx.intern_type_class("work", "record"); }

TypeId type_queue(Context& ctx) { return opaque_type(ctx, queue_class(ctx)); }
TypeId type_record(Context& ctx) { return opaque_type(ctx, record_class(ctx)); }

containers::StringView work_misuse_kind_name(WorkMisuseKind k) noexcept
{
    switch (k)
    {
    case WorkMisuseKind::None: return containers::StringView("none");
    case WorkMisuseKind::QueueTypeMismatch: return containers::StringView("queue-type-mismatch");
    case WorkMisuseKind::CapacityInvalid: return containers::StringView("capacity-invalid");
    case WorkMisuseKind::RecordStrideInvalid: return containers::StringView("record-stride-invalid");
    case WorkMisuseKind::DimNotIndex: return containers::StringView("dim-not-index");
    case WorkMisuseKind::AccessTokenInvalid: return containers::StringView("access-token-invalid");
    case WorkMisuseKind::AccessArityMismatch: return containers::StringView("access-arity-mismatch");
    case WorkMisuseKind::BindingNotResource: return containers::StringView("binding-not-resource");
    }
    return containers::StringView("?");
}

WorkMisuse find_work_misuse(Context& ctx, const Module& m)
{
    // Intern the operand class-id ONCE here (intern_type_class is non-const) — the recursive walk then stays const.
    return scan_work_region(ctx, m.body(), queue_class(ctx));
}
} // namespace crd::ceir::work
