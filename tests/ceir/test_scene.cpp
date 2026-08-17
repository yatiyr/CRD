// CEIR-17a — the scene dialect: the scene.resolve_* intrinsic ops + the opaque resolver-handle type-classes + the
// find_scene_misuse type-chain walk. Device-free (crd-ceir, no gpu-context). Proves: (1) a well-formed
// material→technique→program chain + resolve_geometry verifies; (2) each mistyped operand is REJECTED with its named
// misuse kind (the typed-chain teeth); (3) the phase closed vocabulary; (4) the ops are HOST intrinsics (ADR-0110).

#include <crd/ceir/scene.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp> // register_resource_ops (resource.declare — the typed-value seed)
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::StringView;

namespace
{
struct SceneKit
{
    OpId decl;
    explicit SceneKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)scene::register_dialect(ctx);
    }
};
// A main func body block (the ops live here; find_scene_misuse walks the module).
Block* mkmain(Context& ctx, Module& m)
{
    Block* top = m.body()->first_block();
    if (top == nullptr)
    {
        top = ctx.create_block(0U);
        m.body()->append(top);
    }
    Operation* const f = func::create_func(ctx, m, "main", Visibility::Public, 0U);
    top->append(f);
    return func::func_body_block(f);
}
// A seed VALUE of scene type `t` (a resource.declare op with the scene Extern result type — the mkimage pattern; the
// declare's own kind is irrelevant to find_scene_misuse, only the value's TYPE matters).
Value* mkval(Context& ctx, const SceneKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
} // namespace

TEST_CASE("ceir 17a: a well-formed scene.resolve_* chain verifies (material->technique->program + geometry)", "[ceir][scene]")
{
    memory::GrowableTlsfAllocator root;
    Context                  ctx(&root);
    const SceneKit           k(ctx);
    Module* const            m = ctx.create_module();
    Block* const             b = mkmain(ctx, *m);

    Value* const     draw = mkval(ctx, k, b, scene::type_draw(ctx));
    Operation* const mat  = scene::build_resolve_material(ctx, draw, scene::type_material(ctx));
    b->append(mat);
    Operation* const tech = scene::build_resolve_technique(ctx, mat->result(0U), ctx.attr_string(StringView("opaque")),
                                                           scene::type_technique(ctx));
    b->append(tech);
    Operation* const prog = scene::build_resolve_program(ctx, tech->result(0U), draw, scene::type_program(ctx));
    b->append(prog);
    Operation* const geo = scene::build_resolve_geometry(ctx, draw, scene::type_geometry(ctx));
    b->append(geo);

    CHECK(scene::find_scene_misuse(ctx, *m).kind == scene::SceneMisuseKind::None);

    // ⭐ the five handle type-classes are DISTINCT (the ADR-0111 landmine — draw != material != … even zero-member).
    CHECK(scene::type_draw(ctx) != scene::type_material(ctx));
    CHECK(scene::type_material(ctx) != scene::type_technique(ctx));
    CHECK(scene::type_technique(ctx) != scene::type_program(ctx));
    CHECK(scene::type_program(ctx) != scene::type_geometry(ctx));

    // ⭐ ADR-0110: the four ops are HOST intrinsics (op_info.intrinsic + native_provider promoted at CEIR-7a).
    for (const char* nm : {"resolve_material", "resolve_technique", "resolve_program", "resolve_geometry"})
    {
        const OpInfo* const info = ctx.op_info(ctx.intern_op("scene", nm));
        REQUIRE(info != nullptr);
        CHECK(info->intrinsic);
        CHECK(info->native_provider == StringView("host"));
    }
}

TEST_CASE("ceir 17a: the scene type-chain REJECTS every mistyped operand + a bogus phase", "[ceir][scene]")
{
    memory::GrowableTlsfAllocator root;

    // MaterialTypeMismatch: resolve_technique fed a DRAW (not a material).
    {
        Context        ctx(&root);
        const SceneKit k(ctx);
        Module* const  m = ctx.create_module();
        Block* const   b = mkmain(ctx, *m);
        Value* const   draw = mkval(ctx, k, b, scene::type_draw(ctx));
        Operation* const t  = scene::build_resolve_technique(ctx, draw, ctx.attr_string(StringView("opaque")),
                                                            scene::type_technique(ctx));
        b->append(t);
        CHECK(scene::find_scene_misuse(ctx, *m).kind == scene::SceneMisuseKind::MaterialTypeMismatch);
    }
    // TechniqueTypeMismatch: resolve_program's operand(0) is a MATERIAL (not a technique).
    {
        Context        ctx(&root);
        const SceneKit k(ctx);
        Module* const  m = ctx.create_module();
        Block* const   b = mkmain(ctx, *m);
        Value* const   draw = mkval(ctx, k, b, scene::type_draw(ctx));
        Value* const   mat  = mkval(ctx, k, b, scene::type_material(ctx));
        Operation* const p  = scene::build_resolve_program(ctx, mat, draw, scene::type_program(ctx));
        b->append(p);
        CHECK(scene::find_scene_misuse(ctx, *m).kind == scene::SceneMisuseKind::TechniqueTypeMismatch);
    }
    // DrawTypeMismatch: resolve_material fed a MATERIAL (not a draw).
    {
        Context        ctx(&root);
        const SceneKit k(ctx);
        Module* const  m = ctx.create_module();
        Block* const   b = mkmain(ctx, *m);
        Value* const   mat = mkval(ctx, k, b, scene::type_material(ctx));
        Operation* const r = scene::build_resolve_material(ctx, mat, scene::type_material(ctx));
        b->append(r);
        CHECK(scene::find_scene_misuse(ctx, *m).kind == scene::SceneMisuseKind::DrawTypeMismatch);
    }
    // DrawTypeMismatch: resolve_program's operand(1) is a TECHNIQUE (not a draw).
    {
        Context        ctx(&root);
        const SceneKit k(ctx);
        Module* const  m = ctx.create_module();
        Block* const   b = mkmain(ctx, *m);
        Value* const   tech = mkval(ctx, k, b, scene::type_technique(ctx));
        Operation* const p  = scene::build_resolve_program(ctx, tech, tech, scene::type_program(ctx));
        b->append(p);
        CHECK(scene::find_scene_misuse(ctx, *m).kind == scene::SceneMisuseKind::DrawTypeMismatch);
    }
    // PhaseInvalid: a well-typed resolve_technique with a NON-vocabulary phase.
    {
        Context        ctx(&root);
        const SceneKit k(ctx);
        Module* const  m = ctx.create_module();
        Block* const   b = mkmain(ctx, *m);
        Value* const   mat = mkval(ctx, k, b, scene::type_material(ctx));
        Operation* const t = scene::build_resolve_technique(ctx, mat, ctx.attr_string(StringView("bogus")),
                                                           scene::type_technique(ctx));
        b->append(t);
        CHECK(scene::find_scene_misuse(ctx, *m).kind == scene::SceneMisuseKind::PhaseInvalid);
    }
    // every phase in the closed vocabulary is ACCEPTED.
    for (const char* ph : {"opaque", "transparent", "shadow", "depth", "velocity"})
    {
        Context        ctx(&root);
        const SceneKit k(ctx);
        Module* const  m = ctx.create_module();
        Block* const   b = mkmain(ctx, *m);
        Value* const   mat = mkval(ctx, k, b, scene::type_material(ctx));
        Operation* const t = scene::build_resolve_technique(ctx, mat, ctx.attr_string(StringView(ph)),
                                                           scene::type_technique(ctx));
        b->append(t);
        CHECK(scene::find_scene_misuse(ctx, *m).kind == scene::SceneMisuseKind::None);
    }
}
