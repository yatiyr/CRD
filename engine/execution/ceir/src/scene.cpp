#include <crd/ceir/scene.hpp>

#include <crd/ceir/dialect.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/type.hpp>

namespace crd::ceir::scene
{
namespace
{
// An opaque resolver-handle's verify hook (CEIR-17a): a scene draw/material/technique/program/geometry Extern carries
// ZERO members — the handle is host-opaque (the ROLE is the CLASS, a distinct TypeClassId; CEIR sees an identity, never
// the payload). Any member (a wrongly-parameterized handle) rejects. ⛔ all five classes share this hook — the ROLE is
// the class, not the member shape.
[[nodiscard]] bool verify_opaque(const Context& /*ctx*/, const Type& t) noexcept { return t.members.empty(); }

[[nodiscard]] TypeId opaque_type(Context& ctx, TypeClassId cls)
{
    const Type p; // zero members — an opaque handle
    return ctx.type_extern(cls, p); // the 8a Extern factory (asserts the class's verify hook)
}

// the CLOSED `phase` vocabulary (scene.resolve_technique) — the render pass a technique is resolved FOR.
constexpr containers::StringView kPhaseVocab[] = {containers::StringView("opaque"), containers::StringView("transparent"),
                                                  containers::StringView("shadow"), containers::StringView("depth"),
                                                  containers::StringView("velocity")};

// Is `v`'s type the scene Extern class `cls`? (A non-Extern / wrong-class / null value ⇒ false.)
[[nodiscard]] bool is_scene_class(const Context& ctx, const Value* v, TypeClassId cls) noexcept
{
    if (v == nullptr) { return false; }
    const Type t = ctx.type_of(v->type());
    return t.kind == TypeKind::Extern && t.type_class == cls;
}

// The pre-order walk — the FIRST scene misuse, or {None}. Mirrors scan_render_region: per-op check, then recurse regions.
// The three operand class-ids (draw/material/technique) are PRECOMPUTED by find_scene_misuse (interning is non-const), so
// the recursive walk stays const-clean.
SceneMisuse scan_scene_region(const Context& ctx, const Region* r, TypeClassId draw, TypeClassId mat,
                              TypeClassId tech) // NOLINT(misc-no-recursion)
{
    if (r == nullptr) { return {}; }
    for (Block* b = r->first_block(); b != nullptr; b = b->next_in_region())
    {
        for (Operation* op = b->first_op(); op != nullptr; op = op->next_in_block())
        {
            const containers::StringView nm = ctx.op_name(op->kind());
            // ⛔ operand(0) must be scene.draw: resolve_material / resolve_geometry.
            if (nm == containers::StringView("scene.resolve_material")
                || nm == containers::StringView("scene.resolve_geometry"))
            {
                if (op->num_operands() >= 1U && !is_scene_class(ctx, op->operand(0U), draw))
                {
                    return {op->operand(0U), op, SceneMisuseKind::DrawTypeMismatch};
                }
            }
            else if (nm == containers::StringView("scene.resolve_technique"))
            {
                if (op->num_operands() >= 1U && !is_scene_class(ctx, op->operand(0U), mat))
                {
                    return {op->operand(0U), op, SceneMisuseKind::MaterialTypeMismatch};
                }
                // the CLOSED phase vocabulary (structural verify already required the String attr's PRESENCE).
                const AttrId ph = op->attr(containers::StringView("phase"));
                const AttrValue pv = ctx.attr_value(ph);
                bool phase_ok = pv.kind == AttrKind::String;
                if (phase_ok)
                {
                    phase_ok = false;
                    for (const containers::StringView& v : kPhaseVocab)
                    {
                        if (pv.s == v) { phase_ok = true; break; }
                    }
                }
                if (!phase_ok) { return {nullptr, op, SceneMisuseKind::PhaseInvalid}; }
            }
            else if (nm == containers::StringView("scene.resolve_program"))
            {
                if (op->num_operands() >= 1U && !is_scene_class(ctx, op->operand(0U), tech))
                {
                    return {op->operand(0U), op, SceneMisuseKind::TechniqueTypeMismatch};
                }
                if (op->num_operands() >= 2U && !is_scene_class(ctx, op->operand(1U), draw))
                {
                    return {op->operand(1U), op, SceneMisuseKind::DrawTypeMismatch};
                }
            }
            for (u32 i = 0; i < op->num_regions(); ++i)
            {
                const SceneMisuse e = scan_scene_region(ctx, op->region(i), draw, mat, tech);
                if (e.kind != SceneMisuseKind::None) { return e; }
            }
        }
    }
    return {};
}
} // namespace

Dialect* register_dialect(Context& ctx)
{
    Dialect* const d = register_scene_ops(ctx); // generated: the scene dialect + its ops (idempotent)
    (void)d->register_type_class("draw", TypeClassSpec{&verify_opaque, 0U});      // idempotent by class
    (void)d->register_type_class("material", TypeClassSpec{&verify_opaque, 0U});
    (void)d->register_type_class("technique", TypeClassSpec{&verify_opaque, 0U});
    (void)d->register_type_class("program", TypeClassSpec{&verify_opaque, 0U});
    (void)d->register_type_class("geometry", TypeClassSpec{&verify_opaque, 0U});
    return d;
}

TypeClassId draw_class(Context& ctx) { return ctx.intern_type_class("scene", "draw"); }
TypeClassId material_class(Context& ctx) { return ctx.intern_type_class("scene", "material"); }
TypeClassId technique_class(Context& ctx) { return ctx.intern_type_class("scene", "technique"); }
TypeClassId program_class(Context& ctx) { return ctx.intern_type_class("scene", "program"); }
TypeClassId geometry_class(Context& ctx) { return ctx.intern_type_class("scene", "geometry"); }

TypeId type_draw(Context& ctx) { return opaque_type(ctx, draw_class(ctx)); }
TypeId type_material(Context& ctx) { return opaque_type(ctx, material_class(ctx)); }
TypeId type_technique(Context& ctx) { return opaque_type(ctx, technique_class(ctx)); }
TypeId type_program(Context& ctx) { return opaque_type(ctx, program_class(ctx)); }
TypeId type_geometry(Context& ctx) { return opaque_type(ctx, geometry_class(ctx)); }

containers::StringView scene_misuse_kind_name(SceneMisuseKind k) noexcept
{
    switch (k)
    {
    case SceneMisuseKind::None: return containers::StringView("none");
    case SceneMisuseKind::DrawTypeMismatch: return containers::StringView("draw-type-mismatch");
    case SceneMisuseKind::MaterialTypeMismatch: return containers::StringView("material-type-mismatch");
    case SceneMisuseKind::TechniqueTypeMismatch: return containers::StringView("technique-type-mismatch");
    case SceneMisuseKind::PhaseInvalid: return containers::StringView("phase-invalid");
    }
    return containers::StringView("?");
}

SceneMisuse find_scene_misuse(Context& ctx, const Module& m)
{
    // Intern the operand class-ids ONCE here (intern_type_class is non-const) — the recursive walk then stays const.
    return scan_scene_region(ctx, m.body(), draw_class(ctx), material_class(ctx), technique_class(ctx));
}
} // namespace crd::ceir::scene
