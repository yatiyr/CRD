#include <crd/ceir/rt.hpp>

#include <crd/ceir/attr.hpp>
#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::rt
{
namespace
{
// An opaque AS/SBT handle's verify hook (CEIR-19a): an rt blas/tlas/sbt Extern carries ZERO members — the handle is
// host-opaque (the ROLE is the CLASS, a distinct TypeClassId; CEIR sees an identity, never the VkAccelerationStructureKHR).
// ⛔ all three classes share this hook — the ROLE is the class, not the member shape (blas != tlas != sbt by TypeClassId).
[[nodiscard]] bool verify_opaque(const Context& /*ctx*/, const Type& t) noexcept { return t.members.empty(); }

[[nodiscard]] TypeId opaque_type(Context& ctx, TypeClassId cls)
{
    const Type p; // zero members — an opaque handle
    return ctx.type_extern(cls, p); // the 8a Extern factory (asserts the class's verify hook)
}

// the CLOSED `geometry_kind` vocabulary (rt.blas_build) — the BLAS geometry the build consumes.
constexpr containers::StringView kGeomKindVocab[] = {containers::StringView("triangles"),
                                                     containers::StringView("procedural"),
                                                     containers::StringView("cluster")};

// Is `v`'s type the rt Extern class `cls`? (A non-Extern / wrong-class / null value ⇒ false.)
[[nodiscard]] bool is_rt_class(const Context& ctx, const Value* v, TypeClassId cls) noexcept
{
    if (v == nullptr) { return false; }
    const Type t = ctx.type_of(v->type());
    return t.kind == TypeKind::Extern && t.type_class == cls;
}

// A CEIR-3c resource TYPE — the kinds a bound descriptor (rt.trace / rt.ray_query binding) may take. ⛔ a FILE-LOCAL COPY of
// context.cpp's scan_dispatch helper (same TU there, anonymous-namespace, not header-exposed) — kept in sync by hand; a NEW
// resource TypeKind (§23 growth) must be added in BOTH. AccelStruct is included (a bound TLAS is a resource too).
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
// (byte compare, no StringView slicing). A FILE-LOCAL COPY of context.cpp's ceir_parse_access — kept in sync by hand.
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

// The dispatch-SHAPE check for rt.trace / rt.ray_query (the find_dispatch_misuse mirror — both are compute.dispatch's RT
// siblings). `fixed` = the count of leading non-binding operands (trace 5: dims+tlas+sbt; ray_query 4: dims+tlas). Checks, in
// contractual order: dims (operands 0-2) Index-typed → `access` kind-fold → tokens → arity (== the variadic-binding count) →
// each binding (operands `fixed`..) resource-kinded. ⛔ standalone-robust (the 12b wrong-kind fold): a raw/deserialized op may
// skip per-op verify, so a non-String `access` folds into AccessTokenInvalid rather than reading clean.
[[nodiscard]] RtMisuse check_dispatch_shape(const Context& ctx, const Operation* op, u32 fixed)
{
    for (u32 i = 0; i < 3U && i < op->num_operands(); ++i)
    {
        if (ctx.type_of(op->operand(i)->type()).kind != TypeKind::Index)
        {
            return {op->operand(i), op, RtMisuseKind::DimNotIndex};
        }
    }
    const u32       bindings = op->num_operands() >= fixed ? op->num_operands() - fixed : 0U;
    u32             tokens   = 0U;
    const AttrValue av       = ctx.attr_value(op->attr(containers::StringView("access")));
    if (av.kind != AttrKind::String) { return {nullptr, op, RtMisuseKind::AccessTokenInvalid}; }
    if (!parse_access(av.s, tokens)) { return {nullptr, op, RtMisuseKind::AccessTokenInvalid}; }
    if (tokens != bindings) { return {nullptr, op, RtMisuseKind::AccessArityMismatch}; }
    for (u32 i = fixed; i < op->num_operands(); ++i)
    {
        if (!is_resource_kind(ctx.type_of(op->operand(i)->type()).kind))
        {
            return {op->operand(i), op, RtMisuseKind::BindingNotResource};
        }
    }
    return {};
}

// The pre-order walk — the FIRST rt misuse, or {None}. Mirrors scan_scene_region: per-op check (by op NAME, never an
// op.kind switch — I6), then recurse regions. The three handle class-ids (blas/tlas/sbt) are PRECOMPUTED by find_rt_misuse
// (interning is non-const), so the recursive walk stays const-clean.
RtMisuse scan_rt_region(const Context& ctx, const Region* r, TypeClassId blas, TypeClassId tlas,
                        TypeClassId sbt) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            // ⛔ rt.blas_build: `geometry_kind` (optional; absent = triangles) in its CLOSED vocabulary.
            if (nm == containers::StringView("rt.blas_build"))
            {
                const AttrId gk = op->attr(containers::StringView("geometry_kind"));
                if (gk.valid()) // absent = the default (triangles) = OK; check only when present
                {
                    const AttrValue gv = ctx.attr_value(gk);
                    bool            ok = gv.kind == AttrKind::String;
                    if (ok)
                    {
                        ok = false;
                        for (const containers::StringView& kk : kGeomKindVocab)
                        {
                            if (gv.s == kk) { ok = true; break; }
                        }
                    }
                    if (!ok) { return {nullptr, op, RtMisuseKind::GeometryKindInvalid}; }
                }
            }
            // ⛔ rt.instance_populate: operand(0) is rt.blas; `instance_count` >= 1.
            else if (nm == containers::StringView("rt.instance_populate"))
            {
                if (op->num_operands() >= 1U && !is_rt_class(ctx, op->operand(0U), blas))
                {
                    return {op->operand(0U), op, RtMisuseKind::BlasTypeMismatch};
                }
                const AttrValue iv = ctx.attr_value(op->attr(containers::StringView("instance_count")));
                if (iv.kind != AttrKind::Int || iv.i < 1) { return {nullptr, op, RtMisuseKind::InstanceCountInvalid}; }
            }
            // ⛔ rt.trace (the PIPELINE path): operand(3) is rt.tlas, operand(4) is rt.sbt; `max_recursion` (opt) >= 1.
            else if (nm == containers::StringView("rt.trace"))
            {
                if (op->num_operands() >= 4U && !is_rt_class(ctx, op->operand(3U), tlas))
                {
                    return {op->operand(3U), op, RtMisuseKind::TlasTypeMismatch};
                }
                if (op->num_operands() >= 5U && !is_rt_class(ctx, op->operand(4U), sbt))
                {
                    return {op->operand(4U), op, RtMisuseKind::SbtTypeMismatch};
                }
                // the dispatch shape (dims Index / access tokens+arity / bindings resource) — fixed=5 (dims+tlas+sbt).
                const RtMisuse d = check_dispatch_shape(ctx, op, 5U);
                if (d.kind != RtMisuseKind::None) { return d; }
                const AttrId mr = op->attr(containers::StringView("max_recursion"));
                if (mr.valid())
                {
                    const AttrValue mv = ctx.attr_value(mr);
                    if (mv.kind != AttrKind::Int || mv.i < 1) { return {nullptr, op, RtMisuseKind::MaxRecursionInvalid}; }
                }
            }
            // ⛔ rt.ray_query (the INLINE path): operand(3) is rt.tlas — and consumes NO sbt (the pipeline-vs-inline line).
            else if (nm == containers::StringView("rt.ray_query"))
            {
                if (op->num_operands() >= 4U && !is_rt_class(ctx, op->operand(3U), tlas))
                {
                    return {op->operand(3U), op, RtMisuseKind::TlasTypeMismatch};
                }
                // the dispatch shape (grid Index / access tokens+arity / bindings resource) — fixed=4 (grid+tlas, NO sbt).
                const RtMisuse d = check_dispatch_shape(ctx, op, 4U);
                if (d.kind != RtMisuseKind::None) { return d; }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const RtMisuse e = scan_rt_region(ctx, op->region(i), blas, tlas, sbt);
                if (e.kind != RtMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = register_rt_ops(ctx); // generated: the rt dialect + its ops (idempotent)
    (void)d->register_type_class("blas", TypeClassSpec{&verify_opaque, 0U}); // idempotent by class
    (void)d->register_type_class("tlas", TypeClassSpec{&verify_opaque, 0U});
    (void)d->register_type_class("sbt", TypeClassSpec{&verify_opaque, 0U});
    return d;
}

TypeClassId blas_class(Context& ctx) { return ctx.intern_type_class("rt", "blas"); }
TypeClassId tlas_class(Context& ctx) { return ctx.intern_type_class("rt", "tlas"); }
TypeClassId sbt_class(Context& ctx) { return ctx.intern_type_class("rt", "sbt"); }

TypeId type_blas(Context& ctx) { return opaque_type(ctx, blas_class(ctx)); }
TypeId type_tlas(Context& ctx) { return opaque_type(ctx, tlas_class(ctx)); }
TypeId type_sbt(Context& ctx) { return opaque_type(ctx, sbt_class(ctx)); }

containers::StringView rt_misuse_kind_name(RtMisuseKind k) noexcept
{
    switch (k)
    {
    case RtMisuseKind::None: return containers::StringView("none");
    case RtMisuseKind::BlasTypeMismatch: return containers::StringView("blas-type-mismatch");
    case RtMisuseKind::TlasTypeMismatch: return containers::StringView("tlas-type-mismatch");
    case RtMisuseKind::SbtTypeMismatch: return containers::StringView("sbt-type-mismatch");
    case RtMisuseKind::InstanceCountInvalid: return containers::StringView("instance-count-invalid");
    case RtMisuseKind::GeometryKindInvalid: return containers::StringView("geometry-kind-invalid");
    case RtMisuseKind::MaxRecursionInvalid: return containers::StringView("max-recursion-invalid");
    case RtMisuseKind::DimNotIndex: return containers::StringView("dim-not-index");
    case RtMisuseKind::AccessTokenInvalid: return containers::StringView("access-token-invalid");
    case RtMisuseKind::AccessArityMismatch: return containers::StringView("access-arity-mismatch");
    case RtMisuseKind::BindingNotResource: return containers::StringView("binding-not-resource");
    }
    return containers::StringView("?");
}

RtMisuse find_rt_misuse(Context& ctx, const Module& m)
{
    // Intern the operand class-ids ONCE here (intern_type_class is non-const) — the recursive walk then stays const.
    return scan_rt_region(ctx, m.body(), blas_class(ctx), tlas_class(ctx), sbt_class(ctx));
}
} // namespace crd::ceir::rt
