// CEIR-19a — the rt dialect: the 6 rt orchestration ops (blas_build/instance_populate/tlas_build/sbt_build/trace/ray_query)
// + the opaque blas/tlas/sbt type-classes + the find_rt_misuse type-chain walk. Device-free (crd-ceir, no gpu-context).
// Proves: (1) a well-formed blas→instance→tlas→sbt→trace (+ ray_query) chain verifies; (2) each mistyped operand is
// REJECTED with its named misuse — INCLUDING ray_query rejecting an SBT (the inline-vs-pipeline line: ray_query takes a
// %tlas ONLY, no %sbt); (3) the geometry_kind closed vocabulary + the instance_count / max_recursion range; (4) the
// DISPATCH-SHAPE checks for trace/ray_query (the find_dispatch_misuse mirror — dims Index-typed, `access` tokens {r|w|rw}
// with arity == the binding count, bindings resource-kinded); (5) the ops are HOST intrinsics (ADR-0110 provider=host).

#include <crd/ceir/rt.hpp>

#include <crd/ceir/context.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/resource_ops.hpp> // register_resource_ops (resource.declare — the typed-value seed)
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;       // NOLINT(google-build-using-namespace)
using namespace crd::ceir; // NOLINT(google-build-using-namespace)
using crd::containers::StringView;

namespace
{
struct RtKit
{
    OpId decl;
    explicit RtKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)rt::register_dialect(ctx);
    }
};
// A main func body block (the ops live here; find_rt_misuse walks the module).
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
// A seed VALUE of type `t` (a resource.declare op with that result type — the mkval pattern; the declare's own kind is
// irrelevant to find_rt_misuse, only the value's TYPE matters for the chain check).
Value* mkval(Context& ctx, const RtKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
// A resource-kinded seed value (a Buffer — the trace/ray_query bindings must be resource-kinded).
Value* mkbuf(Context& ctx, const RtKit& k, Block* b) { return mkval(ctx, k, b, ctx.type_buffer(BufferMode::Plain, ctx.type_f32())); }
} // namespace

TEST_CASE("ceir 19a: a well-formed rt chain verifies (blas->instance->tlas->sbt->trace + ray_query)", "[ceir][rt]")
{
    memory::MallocAllocator root;
    Context                  ctx(&root);
    const RtKit              k(ctx);
    Module* const            m = ctx.create_module();
    Block* const             b = mkmain(ctx, *m);

    Value* const     geom = mkval(ctx, k, b, ctx.type_i32()); // any type — find_rt_misuse does not check blas_build's operand
    Operation* const blas = rt::build_blas_build(ctx, geom, rt::type_blas(ctx));
    b->append(blas);
    Value* const     xf   = mkval(ctx, k, b, ctx.type_i32());
    Operation* const inst = rt::build_instance_populate(ctx, blas->result(0U), xf, ctx.attr_int(4), ctx.type_i32());
    b->append(inst);
    Operation* const tlas = rt::build_tlas_build(ctx, inst->result(0U), rt::type_tlas(ctx));
    b->append(tlas);
    Operation* const sbt = rt::build_sbt_build(ctx, ctx.attr_symbol(StringView("scene_rt_raygen")), rt::type_sbt(ctx));
    b->append(sbt);
    Value* const     dim  = mkval(ctx, k, b, ctx.type_index()); // ⛔ dims/grid MUST be Index-typed (DimNotIndex)
    Value* const     bind = mkbuf(ctx, k, b);                   // ⛔ bindings MUST be resource-kinded (BindingNotResource)
    Operation* const tr =
        rt::build_trace(ctx, dim, dim, dim, tlas->result(0U), sbt->result(0U), bind, ctx.attr_string(StringView("w")));
    b->append(tr);
    Operation* const rq = rt::build_ray_query(ctx, dim, dim, dim, tlas->result(0U), bind,
                                              ctx.attr_symbol(StringView("scene_rt_inline")), ctx.attr_string(StringView("w")));
    b->append(rq);

    CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::None);

    // ⭐ the three handle type-classes are DISTINCT (the ADR-0111 landmine — blas != tlas != sbt even zero-member).
    CHECK(rt::type_blas(ctx) != rt::type_tlas(ctx));
    CHECK(rt::type_tlas(ctx) != rt::type_sbt(ctx));
    CHECK(rt::type_blas(ctx) != rt::type_sbt(ctx));

    // ⭐ ADR-0110: the six ops are HOST intrinsics (op_info.intrinsic + native_provider=host).
    for (const char* nm : {"blas_build", "instance_populate", "tlas_build", "sbt_build", "trace", "ray_query"})
    {
        const OpInfo* const info = ctx.op_info(ctx.intern_op("rt", nm));
        REQUIRE(info != nullptr);
        CHECK(info->intrinsic);
        CHECK(info->native_provider == StringView("host"));
    }
}

TEST_CASE("ceir 19a: the rt type-chain REJECTS every mistyped operand + bad vocab/counts", "[ceir][rt]")
{
    memory::MallocAllocator root;

    // BlasTypeMismatch: instance_populate fed a non-blas op(0).
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  notblas = mkval(ctx, k, b, ctx.type_i32());
        Value* const  xf      = mkval(ctx, k, b, ctx.type_i32());
        Operation* const inst = rt::build_instance_populate(ctx, notblas, xf, ctx.attr_int(4), ctx.type_i32());
        b->append(inst);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::BlasTypeMismatch);
    }
    // TlasTypeMismatch: trace fed a non-tlas op(3). (Checked BEFORE dims — i32 dims here do not shadow the tlas verdict.)
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  nottlas = mkval(ctx, k, b, ctx.type_i32());
        Value* const  sbt     = mkval(ctx, k, b, rt::type_sbt(ctx));
        Value* const  dim     = mkval(ctx, k, b, ctx.type_i32());
        Operation* const tr = rt::build_trace(ctx, dim, dim, dim, nottlas, sbt, dim, ctx.attr_string(StringView("w")));
        b->append(tr);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::TlasTypeMismatch);
    }
    // SbtTypeMismatch: trace with a valid tlas but a non-sbt op(4).
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  tlas   = mkval(ctx, k, b, rt::type_tlas(ctx));
        Value* const  notsbt = mkval(ctx, k, b, ctx.type_i32());
        Value* const  dim    = mkval(ctx, k, b, ctx.type_i32());
        Operation* const tr = rt::build_trace(ctx, dim, dim, dim, tlas, notsbt, dim, ctx.attr_string(StringView("w")));
        b->append(tr);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::SbtTypeMismatch);
    }
    // TlasTypeMismatch: ray_query fed an SBT where a TLAS is expected — the inline path takes a %tlas ONLY, no %sbt.
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  sbt = mkval(ctx, k, b, rt::type_sbt(ctx));
        Value* const  dim = mkval(ctx, k, b, ctx.type_i32());
        Operation* const rq =
            rt::build_ray_query(ctx, dim, dim, dim, sbt, dim, ctx.attr_symbol(StringView("k")), ctx.attr_string(StringView("w")));
        b->append(rq);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::TlasTypeMismatch);
    }
    // InstanceCountInvalid: instance_count = 0.
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  blas = mkval(ctx, k, b, rt::type_blas(ctx));
        Value* const  xf   = mkval(ctx, k, b, ctx.type_i32());
        Operation* const inst = rt::build_instance_populate(ctx, blas, xf, ctx.attr_int(0), ctx.type_i32());
        b->append(inst);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::InstanceCountInvalid);
    }
    // GeometryKindInvalid: blas_build with a bogus geometry_kind (an optional attr set post-build).
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  geom = mkval(ctx, k, b, ctx.type_i32());
        Operation* const blas = rt::build_blas_build(ctx, geom, rt::type_blas(ctx));
        b->append(blas);
        ctx.set_attr(blas, StringView("geometry_kind"), ctx.attr_string(StringView("bogus")));
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::GeometryKindInvalid);
    }
    // MaxRecursionInvalid: trace with max_recursion = 0 — ⛔ needs VALID dims/bindings so the dispatch-shape checks pass
    // first (else DimNotIndex/BindingNotResource would shadow the max_recursion verdict; the checks run before it).
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  tlas = mkval(ctx, k, b, rt::type_tlas(ctx));
        Value* const  sbt  = mkval(ctx, k, b, rt::type_sbt(ctx));
        Value* const  dim  = mkval(ctx, k, b, ctx.type_index());
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const tr = rt::build_trace(ctx, dim, dim, dim, tlas, sbt, bind, ctx.attr_string(StringView("w")));
        b->append(tr);
        ctx.set_attr(tr, StringView("max_recursion"), ctx.attr_int(0));
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::MaxRecursionInvalid);
    }

    // ── the dispatch-shape negatives (the find_dispatch_misuse mirror; tlas/sbt are valid so the shape check is reached) ──

    // DimNotIndex: trace dim op(0) is i32, not Index.
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  tlas = mkval(ctx, k, b, rt::type_tlas(ctx));
        Value* const  sbt  = mkval(ctx, k, b, rt::type_sbt(ctx));
        Value* const  bad  = mkval(ctx, k, b, ctx.type_i32()); // dim NOT Index
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const tr = rt::build_trace(ctx, bad, bad, bad, tlas, sbt, bind, ctx.attr_string(StringView("w")));
        b->append(tr);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::DimNotIndex);
    }
    // AccessTokenInvalid: a trace `access` token outside {r,w,rw}.
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  tlas = mkval(ctx, k, b, rt::type_tlas(ctx));
        Value* const  sbt  = mkval(ctx, k, b, rt::type_sbt(ctx));
        Value* const  dim  = mkval(ctx, k, b, ctx.type_index());
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const tr = rt::build_trace(ctx, dim, dim, dim, tlas, sbt, bind, ctx.attr_string(StringView("x")));
        b->append(tr);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::AccessTokenInvalid);
    }
    // AccessArityMismatch: 1 binding but a 2-token `access` ("w,w").
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  tlas = mkval(ctx, k, b, rt::type_tlas(ctx));
        Value* const  sbt  = mkval(ctx, k, b, rt::type_sbt(ctx));
        Value* const  dim  = mkval(ctx, k, b, ctx.type_index());
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const tr = rt::build_trace(ctx, dim, dim, dim, tlas, sbt, bind, ctx.attr_string(StringView("w,w")));
        b->append(tr);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::AccessArityMismatch);
    }
    // BindingNotResource: the ray_query binding op(4) is an i32, not a resource (arity matches so the resource check is hit).
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  tlas = mkval(ctx, k, b, rt::type_tlas(ctx));
        Value* const  dim  = mkval(ctx, k, b, ctx.type_index());
        Value* const  bad  = mkval(ctx, k, b, ctx.type_i32()); // binding NOT a resource
        Operation* const rq =
            rt::build_ray_query(ctx, dim, dim, dim, tlas, bad, ctx.attr_symbol(StringView("k")), ctx.attr_string(StringView("w")));
        b->append(rq);
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::BindingNotResource);
    }

    // every geometry_kind in the closed vocabulary is ACCEPTED.
    for (const char* gk : {"triangles", "procedural", "cluster"})
    {
        Context ctx(&root);
        const RtKit   k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  geom = mkval(ctx, k, b, ctx.type_i32());
        Operation* const blas = rt::build_blas_build(ctx, geom, rt::type_blas(ctx));
        b->append(blas);
        ctx.set_attr(blas, StringView("geometry_kind"), ctx.attr_string(StringView(gk)));
        CHECK(rt::find_rt_misuse(ctx, *m).kind == rt::RtMisuseKind::None);
    }
}
