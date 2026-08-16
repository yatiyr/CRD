// CEIR-20a — the work dialect: the 4 work ops (queue_alloc/produce/consume/compact) + the opaque queue/record type-classes
// + the find_work_misuse type-chain walk. Device-free (crd-ceir, no gpu-context). Proves: (1) a well-formed
// queue_alloc→produce→consume→compact chain verifies; (2) each mistyped %queue operand is REJECTED with QueueTypeMismatch —
// INCLUDING consume op(0), compact op(0)+op(1), produce op(3); (3) the capacity/record_stride range; (4) the DISPATCH-SHAPE
// checks (the find_dispatch_misuse mirror — produce's dims Index-typed [consume/compact have NO host grid — indirect],
// `access` tokens {r|w|rw} with arity == the binding count, bindings resource-kinded); (5) the ops are HOST intrinsics
// (ADR-0110 provider=host). ⛔ NOT ceir.async — work is DEVICE-side work generation (see the dialect header).

#include <crd/ceir/work.hpp>

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
struct WorkKit
{
    OpId decl;
    explicit WorkKit(Context& ctx) : decl(ctx.intern_op("resource", "declare"))
    {
        (void)func::register_dialect(ctx);
        (void)resource::register_resource_ops(ctx);
        (void)work::register_dialect(ctx);
    }
};
// A main func body block (the ops live here; find_work_misuse walks the module).
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
// A seed VALUE of type `t` (a resource.declare op with that result type — only the value's TYPE matters to find_work_misuse).
Value* mkval(Context& ctx, const WorkKit& k, Block* b, TypeId t)
{
    Operation* const d = ctx.create_operation(k.decl, {}, 1U, t);
    b->append(d);
    return d->result(0U);
}
// A resource-kinded seed value (a Buffer — the produce/consume/compact bindings must be resource-kinded).
Value* mkbuf(Context& ctx, const WorkKit& k, Block* b) { return mkval(ctx, k, b, ctx.type_buffer(BufferMode::Plain, ctx.type_f32())); }
// A queue-typed seed value (a work.queue Extern — a produce/consume/compact %queue operand).
Value* mkq(Context& ctx, const WorkKit& k, Block* b) { return mkval(ctx, k, b, work::type_queue(ctx)); }
} // namespace

TEST_CASE("ceir 20a: a well-formed work chain verifies (queue_alloc->produce->consume->compact)", "[ceir][work]")
{
    memory::MallocAllocator root;
    Context                  ctx(&root);
    const WorkKit            k(ctx);
    Module* const            m = ctx.create_module();
    Block* const             b = mkmain(ctx, *m);

    Operation* const q1 = work::build_queue_alloc(ctx, ctx.attr_int(64), ctx.attr_int(16), work::type_queue(ctx));
    b->append(q1);
    Operation* const q2 = work::build_queue_alloc(ctx, ctx.attr_int(64), ctx.attr_int(16), work::type_queue(ctx));
    b->append(q2);
    Value* const     dim  = mkval(ctx, k, b, ctx.type_index()); // ⛔ grid MUST be Index-typed (DimNotIndex)
    Value* const     bind = mkbuf(ctx, k, b);                   // ⛔ bindings MUST be resource-kinded (BindingNotResource)
    Operation* const prod = work::build_produce(ctx, dim, dim, dim, q1->result(0U), bind,
                                                ctx.attr_symbol(StringView("wf_raygen")), ctx.attr_string(StringView("w")));
    b->append(prod);
    Operation* const cons = work::build_consume(ctx, q1->result(0U), bind, ctx.attr_symbol(StringView("wf_trace")),
                                                ctx.attr_string(StringView("w")));
    b->append(cons);
    Operation* const comp = work::build_compact(ctx, q1->result(0U), q2->result(0U), bind,
                                                ctx.attr_symbol(StringView("wf_compact")), ctx.attr_string(StringView("w")));
    b->append(comp);

    CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::None);

    // ⭐ the two handle type-classes are DISTINCT (the ADR-0111 landmine — queue != record even zero-member).
    CHECK(work::type_queue(ctx) != work::type_record(ctx));

    // ⭐ ADR-0110: the four ops are HOST intrinsics (op_info.intrinsic + native_provider=host).
    for (const char* nm : {"queue_alloc", "produce", "consume", "compact"})
    {
        const OpInfo* const info = ctx.op_info(ctx.intern_op("work", nm));
        REQUIRE(info != nullptr);
        CHECK(info->intrinsic);
        CHECK(info->native_provider == StringView("host"));
    }
}

TEST_CASE("ceir 20a: the work type-chain REJECTS every mistyped operand + bad counts", "[ceir][work]")
{
    memory::MallocAllocator root;

    // QueueTypeMismatch: produce fed a non-queue op(3).
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  notq = mkval(ctx, k, b, ctx.type_i32());
        Value* const  dim  = mkval(ctx, k, b, ctx.type_index());
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const p = work::build_produce(ctx, dim, dim, dim, notq, bind, ctx.attr_symbol(StringView("k")),
                                                 ctx.attr_string(StringView("w")));
        b->append(p);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::QueueTypeMismatch);
    }
    // QueueTypeMismatch: consume fed a non-queue op(0).
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  notq = mkval(ctx, k, b, ctx.type_i32());
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const c = work::build_consume(ctx, notq, bind, ctx.attr_symbol(StringView("k")), ctx.attr_string(StringView("w")));
        b->append(c);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::QueueTypeMismatch);
    }
    // QueueTypeMismatch: compact's dst op(1) is not a queue (op(0) is a valid queue, so the check reaches op(1)).
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  src  = mkq(ctx, k, b);
        Value* const  notq = mkval(ctx, k, b, ctx.type_i32());
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const c = work::build_compact(ctx, src, notq, bind, ctx.attr_symbol(StringView("k")), ctx.attr_string(StringView("w")));
        b->append(c);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::QueueTypeMismatch);
    }
    // CapacityInvalid: queue_alloc capacity = 0.
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Operation* const q = work::build_queue_alloc(ctx, ctx.attr_int(0), ctx.attr_int(16), work::type_queue(ctx));
        b->append(q);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::CapacityInvalid);
    }
    // RecordStrideInvalid: queue_alloc record_stride = 0 (capacity valid so the check reaches record_stride).
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Operation* const q = work::build_queue_alloc(ctx, ctx.attr_int(64), ctx.attr_int(0), work::type_queue(ctx));
        b->append(q);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::RecordStrideInvalid);
    }
    // DimNotIndex: produce dim op(0) is i32, not Index (queue op(3) valid so the shape check is reached).
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  q    = mkq(ctx, k, b);
        Value* const  bad  = mkval(ctx, k, b, ctx.type_i32()); // dim NOT Index
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const p = work::build_produce(ctx, bad, bad, bad, q, bind, ctx.attr_symbol(StringView("k")),
                                                 ctx.attr_string(StringView("w")));
        b->append(p);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::DimNotIndex);
    }
    // AccessTokenInvalid: a consume `access` token outside {r,w,rw}.
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  q    = mkq(ctx, k, b);
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const c = work::build_consume(ctx, q, bind, ctx.attr_symbol(StringView("k")), ctx.attr_string(StringView("x")));
        b->append(c);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::AccessTokenInvalid);
    }
    // AccessArityMismatch: consume with 1 binding but a 2-token `access` ("w,w").
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  q    = mkq(ctx, k, b);
        Value* const  bind = mkbuf(ctx, k, b);
        Operation* const c = work::build_consume(ctx, q, bind, ctx.attr_symbol(StringView("k")), ctx.attr_string(StringView("w,w")));
        b->append(c);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::AccessArityMismatch);
    }
    // BindingNotResource: the compact binding op(2) is an i32, not a resource (arity matches so the resource check is hit).
    {
        Context ctx(&root);
        const WorkKit k(ctx);
        Module* const m = ctx.create_module();
        Block* const  b = mkmain(ctx, *m);
        Value* const  src = mkq(ctx, k, b);
        Value* const  dst = mkq(ctx, k, b);
        Value* const  bad = mkval(ctx, k, b, ctx.type_i32()); // binding NOT a resource
        Operation* const c = work::build_compact(ctx, src, dst, bad, ctx.attr_symbol(StringView("k")), ctx.attr_string(StringView("w")));
        b->append(c);
        CHECK(work::find_work_misuse(ctx, *m).kind == work::WorkMisuseKind::BindingNotResource);
    }
}
